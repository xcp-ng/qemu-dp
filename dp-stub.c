#include "qemu/osdep.h"
#include "monitor/monitor.h"
#include "net/net.h"
#include "hw/xen/xen_backend.h"

/* Provide empty stubs for functions not needed by qemu-dp. */

struct XenDevOps xen_console_ops;
struct XenDevOps xen_kbdmouse_ops;
struct XenDevOps xen_9pfs_ops;
struct XenDevOps xen_usb_ops;

void monitor_printf(Monitor *mon, const char *fmt, ...)
{
}

struct NetClientState;
int net_hub_id_for_client(NetClientState *nc, int *id)
{

    return 0;
}
