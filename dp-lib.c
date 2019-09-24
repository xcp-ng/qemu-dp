#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qerror.h"
#include "hw/qdev.h"
#include "monitor/qdev.h"
#include "sysemu/block-backend.h"

static Object *qdev_get_peripheral(void)
{
    static Object *dev;

    if (dev == NULL) {
        dev = container_get(qdev_get_machine(), "/peripheral");
    }

    return dev;
}

static Object *qdev_get_peripheral_anon(void)
{
    static Object *dev;

    if (dev == NULL) {
        dev = container_get(qdev_get_machine(), "/peripheral-anon");
    }

    return dev;
}

static DeviceState *find_device_state(const char *id, Error **errp)
{
    Object *obj;

    if (id[0] == '/') {
        obj = object_resolve_path(id, NULL);
    } else {
        char *root_path = object_get_canonical_path(qdev_get_peripheral());
        char *path = g_strdup_printf("%s/%s", root_path, id);

        g_free(root_path);
        obj = object_resolve_path_type(path, TYPE_DEVICE, NULL);
        g_free(path);
    }

    if (!obj) {
        error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                  "Device '%s' not found", id);
        return NULL;
    }

    if (!object_dynamic_cast(obj, TYPE_DEVICE)) {
        error_setg(errp, "%s is not a hotpluggable device", id);
        return NULL;
    }

    return DEVICE(obj);
}

BlockBackend *blk_by_qdev_id(const char *id, Error **errp)
{
    DeviceState *dev;
    BlockBackend *blk;

    dev = find_device_state(id, errp);
    if (dev == NULL) {
        return NULL;
    }

    blk = blk_by_dev(dev);
    if (!blk) {
        error_setg(errp, "Device does not have a block device backend");
    }
    return blk;
}

void qdev_set_id(DeviceState *dev, const char *id)
{
    if (id) {
        dev->id = id;
    }

    if (dev->id) {
        object_property_add_child(qdev_get_peripheral(), dev->id,
                                  OBJECT(dev), NULL);
    } else {
        static int anon_count;
        gchar *name = g_strdup_printf("device[%d]", anon_count++);
        object_property_add_child(qdev_get_peripheral_anon(), name,
                                  OBJECT(dev), NULL);
        g_free(name);
    }
}

void qdev_unplug(DeviceState *dev, Error **errp)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);
    HotplugHandler *hotplug_ctrl;
    HotplugHandlerClass *hdc;

    if (dev->parent_bus && !qbus_is_hotpluggable(dev->parent_bus)) {
        error_setg(errp, QERR_BUS_NO_HOTPLUG, dev->parent_bus->name);
        return;
    }

    if (!dc->hotpluggable) {
        error_setg(errp, QERR_DEVICE_NO_HOTPLUG,
                   object_get_typename(OBJECT(dev)));
        return;
    }

    qdev_hot_removed = true;

    hotplug_ctrl = qdev_get_hotplug_handler(dev);
    /* hotpluggable device MUST have HotplugHandler, if it doesn't
     * then something is very wrong with it */
    g_assert(hotplug_ctrl);

    /* If device supports async unplug just request it to be done,
     * otherwise just remove it synchronously */
    hdc = HOTPLUG_HANDLER_GET_CLASS(hotplug_ctrl);
    if (hdc->unplug_request) {
        hotplug_handler_unplug_request(hotplug_ctrl, dev, errp);
    } else {
        hotplug_handler_unplug(hotplug_ctrl, dev, errp);
    }
}
