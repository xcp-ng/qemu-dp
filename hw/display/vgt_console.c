/*
 * QEMU vGT/XenGT Console support
 *
 * Copyright (c) Citrix Systems, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/display/vgt_vga.h"
#include "vga_int.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "hw/xen/xen.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <libdrm/drm.h>
#include <libdrm/i915_drm.h>
#include <xf86drm.h>
#include <sys/time.h>

#include <libdrm/intel_bufmgr.h>
#include <libdrm/drm_fourcc.h>
#include "intel-tools/intel_batchbuffer.h"
#include "intel-tools/intel_chipset.h"
#include "qemu/host-utils.h"

typedef struct xengt_surface {
    QemuConsole *con;
    drm_intel_bo *bo;
    uint32_t linesize;
    PixelFormat pf;
} xengt_surface_t;

int drm_fd;
static bool xengt_enabled;
static drm_intel_bufmgr *gem_vgt_bufmgr;
static struct intel_batchbuffer *gem_vgt_batchbuffer;
static xengt_surface_t xengt_surface;

bool xengt_is_enabled(void)
{
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    int rc;

    if (!vgt_vga_enabled)
        return 0;

    if (xengt_enabled)
        goto done;

    memset(&gem_vgtbuffer, 0, sizeof (gem_vgtbuffer));

    gem_vgtbuffer.plane_id = I915_VGT_PLANE_PRIMARY;
    gem_vgtbuffer.vmid = xen_domid;
    gem_vgtbuffer.flags = I915_VGTBUFFER_QUERY_ONLY;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0)
        goto done;

    xengt_enabled = !!gem_vgtbuffer.start;

    if (xengt_enabled)
        qemu_log("vGT: enabled\n");

done:
    return xengt_enabled;
}

#define	P2ROUNDUP(_x, _a) -(-(_x) & -(_a))

static void xengt_destroy_display_surface(void)
{
    xengt_surface_t *surface = &xengt_surface;
    QemuConsole *con = surface->con;
    DisplaySurface *old_ds, *ds;
    int width;
    int height;

    if (surface->con == NULL)
        return;

    qemu_log("vGT: %s\n", __func__);

    old_ds = qemu_console_surface(surface->con);
    width = surface_width(old_ds);
    height = surface_height(old_ds);

    ds = qemu_create_displaysurface(width, height);
    dpy_gfx_replace_surface(con, ds);
    surface->con = NULL;

    drm_intel_bo_unmap(surface->bo);
    drm_intel_bo_unreference(surface->bo);
    surface->bo = NULL;
}

static void xengt_create_display_surface(QemuConsole *con,
                                         struct drm_i915_gem_vgtbuffer *gem_vgtbuffer,
                                         PixelFormat *pf)
{
    xengt_surface_t *surface = &xengt_surface;
    uint32_t width = P2ROUNDUP(gem_vgtbuffer->width, 16);
    uint32_t linesize = width * gem_vgtbuffer->bpp / 8;
    DisplaySurface *ds;
    pixman_format_code_t format;

    surface->linesize = linesize;
    surface->bo = drm_intel_bo_alloc(gem_vgt_bufmgr, "vnc",
                                     P2ROUNDUP(gem_vgtbuffer->height * linesize,
                                               4096),
                                     4096);
    if (surface->bo == NULL) {
        qemu_log("vGT: %s: failed to allocate buffer", __func__);
        return;
    }

    drm_intel_bo_map(surface->bo, 1);

    qemu_log("vGT: %s: w %d h %d, bbp %d , stride %d, fmt 0x%08x\n", __func__,
             width,
             gem_vgtbuffer->height,
             gem_vgtbuffer->bpp,
             linesize,
             gem_vgtbuffer->drm_format);

    format = qemu_pixman_get_format(pf);
    ds = qemu_create_displaysurface_from(width,
                                                  gem_vgtbuffer->height,
                                                  format,
                                                  linesize,
                                                  surface->bo->virtual);
    dpy_gfx_replace_surface(con, ds);
    surface->pf = *pf;

    surface->con = con;
}

typedef struct xengt_fb {
    int64_t created;
    int64_t used;
    uint64_t epoch;
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    drm_intel_bo *bo;
} xengt_fb_t;

#define XENGT_NR_FB 16

static xengt_fb_t xengt_fb[XENGT_NR_FB];

static void xengt_close_object(uint32_t handle)
{
    struct drm_gem_close gem_close;

    memset(&gem_close, 0, sizeof (gem_close));
    gem_close.handle = handle;

    (void) drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
}

static unsigned int fb_count = 0;

static void xengt_release_fb(unsigned int i, const char *reason)
{
    xengt_fb_t *fb = &xengt_fb[i];

    if (fb->gem_vgtbuffer.handle == 0)
        return;

    qemu_log("vGT: %s %u (%s)\n", __func__, i, reason);

    if(fb->bo)
         drm_intel_bo_unreference(fb->bo);

    xengt_close_object(fb->gem_vgtbuffer.handle);

    memset(fb, 0, sizeof(*fb));
    --fb_count;

    if (fb_count == 0)
      xengt_destroy_display_surface();
}

QEMUTimer *drm_timer;

#define XENGT_TIMER_PERIOD 1000 /* ms */

/* Timeout to release a vgtbuffer object after last use */
#define XENGT_VGTBUFFER_EXPIRE 5000 /* ms */

static void xengt_timer(void *opaque)
{
    int64_t now;
    int i;

    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    for (i = 0; i < XENGT_NR_FB; i++) {
        xengt_fb_t *fb = &xengt_fb[i];

        if (fb->gem_vgtbuffer.handle == 0)
            continue;

        if ((now - fb->used) > XENGT_VGTBUFFER_EXPIRE)
            xengt_release_fb(i, "unused");
    }

    timer_mod(drm_timer, now + XENGT_TIMER_PERIOD);
}

void xengt_drm_init(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, errno);
        exit(-1);
    }

    qemu_log("vGT: %s opened drm\n", __func__);

    gem_vgt_bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
    if (gem_vgt_bufmgr == NULL) {
        qemu_log("vGT: %s: drm_intel_bufmgr_gem_init failed\n", __func__);
        exit(-1);
    }

    drm_intel_bufmgr_gem_enable_reuse(gem_vgt_bufmgr);

    qemu_log("vGT: %s initialized bufmgr\n", __func__);

    gem_vgt_batchbuffer = intel_batchbuffer_alloc(gem_vgt_bufmgr, intel_get_drm_devid(drm_fd));
    if (gem_vgt_batchbuffer == NULL) {
        qemu_log("vGT: %s: intel_batchbuffer_alloc failed\n", __func__);
        exit(-1);
    }

    qemu_log("vGT: %s initialized batchbuffer\n", __func__);

    drm_timer = timer_new_ms(QEMU_CLOCK_REALTIME, xengt_timer, NULL);
    timer_mod(drm_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + XENGT_TIMER_PERIOD);

    qemu_log("vGT: %s created timer\n", __func__);
}

static int gem_bo_globalize(uint32_t fd, uint32_t handle,  uint32_t* ghandle)
{
    int ret;
    struct drm_gem_flink flink;

    memset(&flink, 0, sizeof(flink));
    flink.handle = handle;

    ret = drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink);
    if (ret != 0)
        return -errno;

    *ghandle = flink.name;
    return 0;
}

static xengt_fb_t *xengt_new_fb(struct drm_i915_gem_vgtbuffer *gem_vgtbuffer)
{
    xengt_fb_t *fb;
    static uint64_t epoch = 1;
    uint64_t oldest_epoch;
    unsigned int i, oldest;
    int rc;
    uint32_t global_handle = 0;

    oldest_epoch = epoch;
    oldest = XENGT_NR_FB;

    for (i = 0; i < XENGT_NR_FB; i++) {
        fb = &xengt_fb[i];

        if (fb->epoch < oldest_epoch) {
            oldest_epoch = fb->epoch;
            oldest = i;
        }
    }
    assert(oldest < XENGT_NR_FB);

    i = oldest;
    fb = &xengt_fb[i];

    xengt_release_fb(i, "spill");

    fb->used = fb->created = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    fb->epoch = epoch++;
    fb->gem_vgtbuffer = *gem_vgtbuffer;

    rc = gem_bo_globalize(drm_fd, gem_vgtbuffer->handle, &global_handle);
    if (rc) {
        qemu_log("vGT: %s: Failed to link from handle %x!\n", __func__, gem_vgtbuffer->handle);
        return NULL;
    }

    fb->bo = drm_intel_bo_gem_create_from_name(gem_vgt_bufmgr, "src", global_handle);
    if (!fb->bo) {
         qemu_log("vGT: %s: Failed to create bo from handle %x!\n", __func__, global_handle);
         return NULL;
    }

    qemu_log("vGT: %s %u: Created bo, with size %ld, handle %d\n", __func__, i,
             fb->bo->size ,fb->bo->handle);

    fb_count++;
    return fb;
}

static xengt_fb_t *xengt_lookup_fb(struct drm_i915_gem_vgtbuffer *gem_vgtbuffer)
{
    int i;

    for (i = 0; i < XENGT_NR_FB; i++) {
        xengt_fb_t *fb = &xengt_fb[i];

        if (memcmp(&fb->gem_vgtbuffer,
                   gem_vgtbuffer,
                   offsetof(struct drm_i915_gem_vgtbuffer, handle)) == 0) {
            fb->used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            return fb;
        }
    }

    return NULL;
}

static void xengt_disable(void)
{
    int i;

    for (i = 0; i < XENGT_NR_FB; i++)
        xengt_release_fb(i, "disable");

    xengt_enabled = false;
    qemu_log("vGT: disabled\n");
}

static xengt_fb_t *xengt_get_fb(void)
{
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    xengt_fb_t *fb = NULL;
    int rc;

    memset(&gem_vgtbuffer, 0, sizeof (gem_vgtbuffer));
    gem_vgtbuffer.plane_id = I915_VGT_PLANE_PRIMARY;
    gem_vgtbuffer.vmid = xen_domid;
    gem_vgtbuffer.flags = I915_VGTBUFFER_QUERY_ONLY;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0) {
        xengt_disable();
        goto done;
    }

    if ((fb = xengt_lookup_fb(&gem_vgtbuffer)) != NULL)
        goto done;

    gem_vgtbuffer.flags = 0;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0)
        goto done;

    if (unlikely((fb = xengt_lookup_fb(&gem_vgtbuffer)) != NULL)) {
        /* We don't need the new object so close it */
        xengt_close_object(gem_vgtbuffer.handle);
        goto done;
    }

    if ((fb = xengt_new_fb(&gem_vgtbuffer)) == NULL) {
        /* We can't use the new object so close it */
        xengt_close_object(gem_vgtbuffer.handle);
    }

done:
    return fb;
}

static int qemu_set_pixelformat(uint32_t drm_format, PixelFormat *pf)
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t alpha;
    int rc = 0;

    switch (drm_format) {
    case DRM_FORMAT_XRGB8888:
        red = 0xFF0000;
        green = 0xFF00;
        blue = 0xFF;
        alpha = 0xFF000000;
        break;

    case DRM_FORMAT_XBGR8888:
        red = 0xFF;
        green = 0xFF00;
        blue = 0xFF0000;
        alpha = 0xFF000000;
        break;

    case DRM_FORMAT_XBGR2101010:
        red = 0x3FF;
        green = 0xFFC00;
        blue = 0x3FF00000;
        alpha = 0xC0000000;
        break;

    case DRM_FORMAT_XRGB2101010:
        red = 0x3FF00000;
        green = 0xFFC00;
        blue = 0x3FF;
        alpha = 0xC0000000;
        break;

    default:
        rc = -1;
        break;
    }

    if (rc < 0)
        return rc;

    memset(pf, 0x00, sizeof(PixelFormat));

    pf->rmask = red;
    pf->gmask = green;
    pf->bmask = blue;

    pf->rbits = ctpop32(red);
    pf->gbits = ctpop32(green);
    pf->bbits = ctpop32(blue);
    pf->abits = ctpop32(alpha);

    pf->depth = pf->rbits + pf->gbits + pf->bbits;
    pf->bits_per_pixel = pf->depth + pf->abits;
    pf->bytes_per_pixel = pf->bits_per_pixel / 8;

    pf->rmax = (1 << pf->rbits) -1;
    pf->gmax = (1 << pf->gbits) -1;
    pf->bmax = (1 << pf->bbits) -1;
    pf->amax = (1 << pf->abits) -1;

    pf->rshift = ffs(red) - 1;
    pf->gshift = ffs(green) - 1;
    pf->bshift = ffs(blue) -1;
    pf->ashift = ffs(alpha) -1;

    return 0;
}

void xengt_draw_primary(QemuConsole *con, int full_update)
{
    xengt_surface_t *surface = &xengt_surface;
    xengt_fb_t *fb;
    struct drm_i915_gem_vgtbuffer *gem_vgtbuffer;
    PixelFormat pf;
    DisplaySurface *ds;
    int rc;

    if (fb_count == 0)
        full_update = 1;

    if ((fb = xengt_get_fb()) == NULL || (fb->bo == NULL)) {
        if (xengt_enabled)
            qemu_log("vGT: %s: no frame buffer", __func__);
        return;
    }

    gem_vgtbuffer = &fb->gem_vgtbuffer;

    rc = qemu_set_pixelformat(gem_vgtbuffer->drm_format, &pf);
    if (rc < 0) {
        qemu_log("vGT: %s: unknown format (%08x)", __func__, gem_vgtbuffer->drm_format);
        return;
    }

    ds = qemu_console_surface(con);

    if (full_update ||
        surface->con != con ||
        surface_width(ds) != gem_vgtbuffer->width ||
        surface_height(ds) != gem_vgtbuffer->height ||
        memcmp(&surface->pf, &pf, sizeof pf)) {

        xengt_destroy_display_surface();
        xengt_create_display_surface(con, gem_vgtbuffer, &pf);

        ds = qemu_console_surface(con);
    }

    if (ds != NULL) {
        drm_intel_bo_unmap(surface->bo);

        if (fb->bo)
            intel_blt_copy(gem_vgt_batchbuffer,
                           fb->bo, 0, 0, gem_vgtbuffer->stride,
                           surface->bo, 0, 0, surface->linesize,
                           gem_vgtbuffer->width,
                           gem_vgtbuffer->height,
                           gem_vgtbuffer->bpp);

        drm_intel_bo_map(surface->bo, 1);
    }

    dpy_gfx_update(con, 0, 0, gem_vgtbuffer->width, gem_vgtbuffer->height);
    return;
}
