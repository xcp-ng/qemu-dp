#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/display/vgpu.h"
#include "hw/xen/xen_common.h"
#include "ui/console.h"

#include <sys/socket.h>
#include <netinet/in.h>

typedef struct shared_surface {
    uint32_t    offset;
    uint32_t    linesize;
    uint32_t    width;
    uint32_t    height;
    uint32_t    depth;
    uint32_t    update;
    uint64_t    vram_addr;
} QEMU_PACKED shared_surface_t;

typedef struct VGPUState {
    SysBusDevice sysdev;

    QemuConsole *con;
    shared_surface_t *shared;

    uint32_t surface_offset;
    uint32_t surface_linesize;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_depth;
    uint32_t surface_update;

    uint8_t *surface_buffer;

    int surface_fd;
} VGPUState;

#define DEMU_FIFO_PATH     "/run/xen/vgpu-fifo-%d"
#define DEMU_FIFO_TIMEOUT   (60 * 5)
#define DEMU_SURFACE_PATH    "/xen.vgpu.%d"

#define VGPU(obj) \
    OBJECT_CHECK(VGPUState, (obj), TYPE_VGPU)

static bool out_of_time = false;

static void fifo_open_alarm_sig_handler(int sig)
{
    (void)(sig);
    out_of_time = true;
}

static int create_fifo(char *path, int mode, int timeout)
{
    int fd;
    int rc;
    struct sigaction sa;
    struct sigaction old_sa;
    sigset_t unblock;
    sigset_t oldblock;

    /* Create fifo, if doest already exist */
    rc = mkfifo(path, S_IRUSR | S_IWUSR);

    if (rc == -1 && errno != EEXIST) {
        error_report("Error, mkfifo: %s\n", strerror(errno));
        return -errno;
    }

    info_report("Fifo %s!\n", rc ? "exists" : "created");

    /* set alarm */
    sa.sa_handler = fifo_open_alarm_sig_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, &old_sa)) {
        fd = -errno;
        error_report("Error waiting on SIGALRM %d, %s\n", errno, strerror(errno));
        goto unlink_and_exit;
    }
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &unblock, &oldblock);

    alarm(timeout);

    /* open fifo - will block until other end connects */
    do {
        fd = open(path, mode);
        if (fd == -1)
            fd = -errno;
    } while (fd == -EINTR && !out_of_time);

    /* unset alarm */
    alarm(0);
    pthread_sigmask(SIG_SETMASK, &oldblock, NULL);
    if (sigaction(SIGALRM, &old_sa, NULL)) {
        fd = -errno;
        error_report("Error restoring old  SIGALRM %d, %s\n", errno, strerror(errno));
    }

    if (fd == -EINTR)
        fd = -ETIME;

unlink_and_exit:
    unlink(path);

    if (fd < 0)
        error_report("create_fifo failed with %s\n", strerror(-fd));
    else
        info_report("Rendezvous successful");
    return fd;
}

static int open_surface_shm(shared_surface_t **shared)
{
    int rc = 0;
    int shm_fd;
    void *ptr;
    char *sh_name;

    if (asprintf(&sh_name, DEMU_SURFACE_PATH, xen_domid) < 0)
        return -errno;

    /* open the shared memory segment */
    shm_fd = shm_open(sh_name, O_RDONLY, 0666);
    if (shm_fd < 0) {
        rc = -errno;
        error_report("shm_open error %s\n", strerror(-rc));
        goto out;
    }

    /* now map the shared memory segment in the address space of the process */
    ptr = mmap(0, sizeof(shared_surface_t), PROT_READ, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        rc = -errno;
        error_report("mmap error %s\n", strerror(-rc));
    } else {
        *shared = ptr;
    }

    if (close(shm_fd)) {
        if (!rc)
            rc = -errno;
        error_report("close error %s\n", strerror(errno));
    }

    if (shm_unlink(sh_name)) {
        if (!rc)
            rc = -errno;
        error_report("shm_unlink error %s\n", strerror(errno));
    }
out:
    free(sh_name);
    return rc;
}

static int create_demu_fifo(void)
{
    char *buffer;
    int rc, fd, flags;

    rc = asprintf(&buffer, DEMU_FIFO_PATH, xen_domid);
    if (rc < 0)
        return -errno;

    fd = create_fifo(buffer, O_WRONLY, DEMU_FIFO_TIMEOUT);

    free (buffer);
    if (fd < 0)
        return fd;

    flags = fcntl(fd, F_GETFL, 0);
    rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return  rc ? rc : fd;
}


#define BLOCK_SUPPRESS_LIMIT 200

static void vgpu_fb_update(void *opaque)
{
    static int block_count = 0;
    VGPUState *s = VGPU(opaque);
    DisplaySurface *surface;
    pixman_format_code_t format;
    ssize_t rc;
    char buf = 'S';

    rc = write(s->surface_fd, &buf, 1);
    if (rc < 0) {
        if (errno == EAGAIN) {
            block_count++;
            if (block_count == BLOCK_SUPPRESS_LIMIT)
                error_report("%s: write() was blocked %d times without success",
                             __func__, block_count);
        } else {
            error_report("%s: write() failed: %d, %s",
                         __func__, errno, strerror(errno));
        }
    } else {
        if (block_count >= BLOCK_SUPPRESS_LIMIT)
            info_report("%s: write() was unblocked after %d attempts",
                        __func__, block_count);
        block_count = 0;

        if (rc != 1)
            error_report("%s: write() wrote %ld, expected 1",
                         __func__, rc);
    }

    if (s->surface_offset != s->shared->offset ||
        s->surface_linesize != s->shared->linesize ||
        s->surface_width != s->shared->width ||
        s->surface_height != s->shared->height ||
        s->surface_depth != s->shared->depth) {

        s->surface_offset = s->shared->offset;
        s->surface_linesize = s->shared->linesize;
        s->surface_width = s->shared->width;
        s->surface_height = s->shared->height;
        s->surface_depth = s->shared->depth;

        info_report("%s: %dx%dx%d @ %x (linesize = %x)\n", __func__,
                s->surface_width, s->surface_height, s->surface_depth,
                s->surface_offset, s->surface_linesize);

        format = qemu_default_pixman_format(s->surface_depth, true);
        surface = qemu_create_displaysurface_from(s->surface_width,
                                                  s->surface_height,
                                                  format,
                                                  s->surface_linesize,
                                                  s->surface_buffer + s->surface_offset);
        dpy_gfx_replace_surface(s->con, surface);
    }

    if (s->surface_update != s->shared->update) {
        s->surface_update = s->shared->update;

        dpy_gfx_update(s->con, 0, 0,
                       s->surface_width, s->surface_height);
    }
}

#define SURFACE_RESERVED_SIZE       0x01000000

static const GraphicHwOps vgpu_ops = {
    .gfx_update  = vgpu_fb_update
};

static void vgpu_fb_realize(DeviceState *dev, Error **errp)
{
    VGPUState *s = VGPU(dev);
    const int n = SURFACE_RESERVED_SIZE >> TARGET_PAGE_BITS;
    xen_pfn_t pfn[n];
    int fifo_fd, i, rc;
    uint64_t address;

    s->con = graphic_console_init(dev, 0, &vgpu_ops, s);

    fifo_fd = create_demu_fifo();
    if (fifo_fd < 0) {
        error_report("fifo failed\n");
        exit(1);
    }

    rc = open_surface_shm(&s->shared);
    if (rc) {
        error_report("Failed to open shared surface.  %s", strerror(-rc));
        exit(1);
    }

    address = s->shared->vram_addr;

    for (i = 0; i < n; i++)
        pfn[i] = (address >> TARGET_PAGE_BITS) + i;

    s->surface_buffer = xenforeignmemory_map(xen_fmem, xen_domid,
                                             PROT_READ | PROT_WRITE,
                                             n, pfn, NULL);
    if (s->surface_buffer == NULL) {
        error_report("mmap failed. address = 0x%jx\n", address);
        exit(1);
    }

    s->surface_fd = fifo_fd;
}

static void vgpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = vgpu_fb_realize;
    /* we might want to have some properties here later */
    dc->props = NULL;
    dc->user_creatable = true;
    dc->hotpluggable = false;
}

static const TypeInfo vgpu_info = {
    .name          = TYPE_VGPU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VGPUState),
    .class_init    = vgpu_class_init,
};

static void vgpu_register_types(void)
{
    type_register_static(&vgpu_info);
}

type_init(vgpu_register_types)
