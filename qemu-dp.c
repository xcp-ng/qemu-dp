#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "qemu/config-file.h"
#include "crypto/init.h"
#include "chardev/char.h"
#include "sysemu/iothread.h"
#include "hw/xen/xen_backend.h"
#include "hw/xen/xen_pvdev.h"
#include "monitor/dp-monitor.h"
#include "qapi/qapi-commands-misc.h"

/* Normally provided by vl.c */
enum xen_mode xen_mode = XEN_ATTACH;
bool xen_allowed = true;
uint32_t xen_domid;
bool xen_domid_restrict;
/* Normally provided by xen_common.c */
xc_interface *xen_xc;
xenforeignmemory_handle *xen_fmem;
xendevicemodel_handle *xen_dmod;

#define QEMU_ARCH_I386 (1 << 3)
const uint32_t arch_type = QEMU_ARCH_I386; /* arch_init.c */
const char *qemu_name = "qemu-dp"; /* vl.c */

/* Normally provided by cpus.c */
static QemuMutex qemu_global_mutex;
static __thread bool iothread_locked = false;

static bool run_loop = true;

void qmp_quit(Error **errp)
{
    /* Stop the main loop when the quit QMP command is executed. */
    run_loop = false;
}

/* Normally provided by cpus.c */
bool qemu_mutex_iothread_locked(void)
{
    return iothread_locked;
}

void qemu_mutex_lock_iothread(void)
{
    g_assert(!qemu_mutex_iothread_locked());
    qemu_mutex_lock(&qemu_global_mutex);
    iothread_locked = true;
}

void qemu_mutex_unlock_iothread(void)
{
    g_assert(qemu_mutex_iothread_locked());
    iothread_locked = false;
    qemu_mutex_unlock(&qemu_global_mutex);
}

static void qemu_dp_trace_init_events(const char *fname)
{
    FILE *fp;
    char line_buf[1024];

    if (fname == NULL) {
        return;
    }

    fp = fopen(fname, "r");
    if (!fp) {
        /* just return if file is not there */
        return;
    }
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        size_t len = strlen(line_buf);
        if (len > 1) {              /* skip empty lines */
            line_buf[len - 1] = '\0';
            if ('#' == line_buf[0]) { /* skip commented lines */
                continue;
            }
            trace_enable_events(line_buf);
        }
    }
    if (fclose(fp) != 0) {
        exit(1);
    }
}

int main(int argc, char **argv)
{
    Error *err = NULL;
    char *qmparg;
    Chardev *chr;
    QemuOpts *opts;

    logging_set_prefix("qemu-dp");
    logging_set_redirect(1);

    module_call_init(MODULE_INIT_TRACE);
    qcrypto_init(&error_fatal);
    module_call_init(MODULE_INIT_QOM);

    if (argc != 2) {
        printf("Usage: %s <qmp-socket-path>\n", argv[0]);
        exit(1);
    }

    if (!trace_init_backends()) {
        exit(1);
    }

    qemu_dp_trace_init_events("/usr/lib64/xen/bin/qemu-dp-tracing");

    if (qemu_init_main_loop(&err)) {
        error_report_err(err);
        exit(EXIT_FAILURE);
    }

    qemu_mutex_init(&qemu_global_mutex);
    qemu_mutex_lock_iothread();

    qemu_add_opts(&qemu_chardev_opts);
    qmparg = g_strdup_printf("unix:%s,server,nowait", argv[1]);
    opts = qemu_chr_parse_compat("monitor0", qmparg);
    g_free(qmparg);
    chr = qemu_chr_new_from_opts(opts, &error_abort);

    dp_monitor_init(chr);

    bdrv_init();

    xen_xc = xc_interface_open(0, 0, 0);
    if (xen_xc == NULL) {
        fprintf(stderr, "can't open xen interface\n");
        exit(1);
    }
    xen_fmem = xenforeignmemory_open(0, 0);
    if (xen_fmem == NULL) {
        fprintf(stderr, "can't open xen fmem interface\n");
        exit(1);
    }

    /* Initialize backend core & drivers */
    if (xen_be_init() != 0) {
        fprintf(stderr, "%s: xen backend core setup failed\n", __FUNCTION__);
        exit(1);
    }

    while (run_loop)
        main_loop_wait(false);


// Dropped in 4486e89c219c0d1b9bd8dfa0b1dd5b0d51ff2268
//     iothread_stop_all();
    bdrv_close_all();
    dp_monitor_destroy();
    qemu_chr_cleanup();

    return 0;
}
