#include "qemu/osdep.h"
#include "qemu-version.h"
#include "qemu/thread.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/dispatch.h"
#include "qapi/qmp/json-parser.h"
#include "qapi/qmp/json-streamer.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qerror.h"
#include "qapi/qmp/qobject.h"
#include "qapi/qmp/qdict.h"
#include "monitor/dp-monitor.h"
#include "qapi/qmp/dispatch.h"
#include "dp-qapi/qapi-commands-misc.h"
#include "dp-qapi/qapi-commands.h"
#include "trace-root.h"
#include "trace/control.h"
#ifdef CONFIG_TRACE_SIMPLE
#include "trace/simple.h"
#endif

/* Most of this code has been taken from monitor.c */

QemuRecMutex monitor_rec_lock;

typedef struct DPMonitor {
    CharBackend chr;
    bool skip_flush;
    QemuMutex out_lock;
    QString *outbuf;
    guint out_watch;
    JSONMessageParser parser;
    /*
     * When a client connects, we're in capabilities negotiation mode.
     * When command qmp_capabilities succeeds, we go into command
     * mode.
     */
    QmpCommandList *commands;
} DPMonitor;

static QmpCommandList qmp_commands, qmp_cap_negotiation_commands;
static DPMonitor *mon;

static void dp_monitor_flush_locked(DPMonitor *mon);

static gboolean dp_monitor_unblocked(GIOChannel *chan, GIOCondition cond,
                                  void *opaque)
{
    DPMonitor *mon = opaque;

    qemu_mutex_lock(&mon->out_lock);
    mon->out_watch = 0;
    dp_monitor_flush_locked(mon);
    qemu_mutex_unlock(&mon->out_lock);
    return FALSE;
}

/* Called with m->out_lock held.  */
static void dp_monitor_flush_locked(DPMonitor *m)
{
    int rc;
    size_t len;
    const char *buf;

    if (m->skip_flush) {
        return;
    }

    buf = qstring_get_str(m->outbuf);
    len = qstring_get_length(m->outbuf);

    if (len) {
        rc = qemu_chr_fe_write(&m->chr, (const uint8_t *) buf, len);
        if ((rc < 0 && errno != EAGAIN) || (rc == len)) {
            /* all flushed or error */
            QDECREF(m->outbuf);
            m->outbuf = qstring_new();
            return;
        }
        if (rc > 0) {
            /* partial write */
            QString *tmp = qstring_from_str(buf + rc);
            QDECREF(m->outbuf);
            m->outbuf = tmp;
        }
        if (m->out_watch == 0) {
            m->out_watch =
                qemu_chr_fe_add_watch(&m->chr, G_IO_OUT | G_IO_HUP,
                                      dp_monitor_unblocked, m);
        }
    }
}

/* flush at every end of line */
static void dp_monitor_puts(DPMonitor *m, const char *str)
{
    char c;

    qemu_mutex_lock(&m->out_lock);
    for(;;) {
        c = *str++;
        if (c == '\0')
            break;
        if (c == '\n') {
            qstring_append_chr(m->outbuf, '\r');
        }
        qstring_append_chr(m->outbuf, c);
        if (c == '\n') {
            dp_monitor_flush_locked(m);
        }
    }
    qemu_mutex_unlock(&m->out_lock);
}

static void dp_monitor_json_emitter(DPMonitor *m, const QObject *data)
{
    QString *json;

    json = qobject_to_json(data);
    assert(json != NULL);

    qstring_append_chr(json, '\n');
    dp_monitor_puts(m, qstring_get_str(json));

    QDECREF(json);
}

static void handle_qmp_command(JSONMessageParser *parser, GQueue *tokens)
{
    QObject *req, *rsp = NULL, *id = NULL;
    QDict *qdict = NULL;
    Error *err = NULL;

    req = json_parser_parse_err(tokens, NULL, &err);
    if (!req && !err) {
        /* json_parser_parse_err() sucks: can fail without setting @err */
        error_setg(&err, QERR_JSON_PARSING);
    }
    if (err) {
        goto err_out;
    }

    qdict = qobject_to(QDict, req);
    if (qdict) {
        id = qdict_get(qdict, "id");
        qobject_incref(id);
        qdict_del(qdict, "id");
    } /* else will fail qmp_dispatch() */

    qemu_rec_mutex_lock(&monitor_rec_lock);

    QString *req_json = qobject_to_json(req);
//     trace_dp_handle_qmp_command_enter(qstring_get_str(req_json));

    rsp = qmp_dispatch(mon->commands, req);

//     trace_dp_handle_qmp_command_exit(qstring_get_str(req_json));
    QDECREF(req_json);

    qemu_rec_mutex_unlock(&monitor_rec_lock);

err_out:
    if (err) {
        qdict = qdict_new();
        qdict_put_obj(qdict, "error", qmp_build_error_object(err));
        error_free(err);
        rsp = QOBJECT(qdict);
    }

    if (rsp) {
        if (id) {
            qdict_put_obj(qobject_to(QDict, rsp), "id", id);
            id = NULL;
        }

        dp_monitor_json_emitter(mon, rsp);
    }

    qobject_decref(id);
    qobject_decref(rsp);
    qobject_decref(req);
}

static QObject *get_qmp_greeting(void)
{
    QObject *ver = NULL;

    qmp_marshal_query_version(NULL, &ver, NULL);

    return qobject_from_jsonf("{'QMP': {'version': %p, 'capabilities': []}}",
                              ver);
}

static void dp_monitor_qmp_event(void *opaque, int event)
{
    QObject *data;

    switch (event) {
    case CHR_EVENT_OPENED:
        mon->commands = &qmp_cap_negotiation_commands;
        data = get_qmp_greeting();
        dp_monitor_json_emitter(mon, data);
        qobject_decref(data);
        break;
    case CHR_EVENT_CLOSED:
        json_message_parser_destroy(&mon->parser);
        json_message_parser_init(&mon->parser, handle_qmp_command);
        break;
    }
}

static int dp_monitor_can_read(void *opaque)
{
    return 1;
}

static void dp_monitor_qmp_read(void *opaque, const uint8_t *buf, int size)
{
    json_message_parser_feed(&mon->parser, (const char *)buf, size);
}

static void query_commands_cb(QmpCommand *cmd, void *opaque)
{
    CommandInfoList *info, **list = opaque;

    if (!cmd->enabled) {
        return;
    }

    info = g_malloc0(sizeof(*info));
    info->value = g_malloc0(sizeof(*info->value));
    info->value->name = g_strdup(cmd->name);
    info->next = *list;
    *list = info;
}

CommandInfoList *qmp_query_commands(Error **errp)
{
    CommandInfoList *list = NULL;

    qmp_for_each_command(mon->commands, query_commands_cb, &list);

    return list;
}

VersionInfo *qmp_query_version(Error **errp)
{
    VersionInfo *info = g_new0(VersionInfo, 1);

    info->qemu = g_new0(VersionTriple, 1);
    info->qemu->major = QEMU_VERSION_MAJOR;
    info->qemu->minor = QEMU_VERSION_MINOR;
    info->qemu->micro = QEMU_VERSION_MICRO;
    info->package = g_strdup(QEMU_PKGVERSION);

    return info;
}

void qmp_qmp_capabilities(bool has_enable, QMPCapabilityList *enable,
                          Error **errp)
{
#if 0
    Error *local_err = NULL;
#endif

    if (mon->commands == &qmp_commands) {
        error_set(errp, ERROR_CLASS_COMMAND_NOT_FOUND,
                  "Capabilities negotiation is already complete, command "
                  "ignored");
        return;
    }

#if 0
    /* Enable QMP capabilities provided by the client if applicable. */
    if (has_enable) {
        qmp_caps_check(mon, enable, &local_err);
        if (local_err) {
            /*
             * Failed check on any of the capabilities will fail the
             * entire command (and thus not apply any of the other
             * capabilities that were also requested).
             */
            error_propagate(errp, local_err);
            return;
        }
        qmp_caps_apply(mon, enable);
    }
#endif

    mon->commands = &qmp_commands;
}

// void qmp_qmp_capabilities(Error **errp)
// {
//     if (mon->commands == &qmp_commands) {
//         error_set(errp, ERROR_CLASS_COMMAND_NOT_FOUND,
//                   "Capabilities negotiation is already complete, command "
//                   "ignored");
//         return;
//     }
// 
//     mon->commands = &qmp_commands;
// }

void dp_monitor_init(Chardev *chr)
{
    mon = g_malloc(sizeof(DPMonitor));
    memset(mon, 0, sizeof(DPMonitor));
    qemu_mutex_init(&mon->out_lock);
    qemu_rec_mutex_init(&monitor_rec_lock);
    mon->outbuf = qstring_new();
    qemu_chr_fe_init(&mon->chr, chr, &error_abort);
    QTAILQ_INIT(&qmp_cap_negotiation_commands);
    qmp_register_command(&qmp_cap_negotiation_commands, "qmp_capabilities",
                         qmp_marshal_qmp_capabilities, QCO_NO_OPTIONS);
    qmp_init_marshal(&qmp_commands);

    qemu_chr_fe_set_handlers(&mon->chr, dp_monitor_can_read,
            dp_monitor_qmp_read, dp_monitor_qmp_event, NULL, mon, NULL, true);
    json_message_parser_init(&mon->parser, handle_qmp_command);

}

void dp_monitor_destroy(void)
{
    qemu_chr_fe_deinit(&mon->chr, false);
    json_message_parser_destroy(&mon->parser);
    QDECREF(mon->outbuf);
    qemu_mutex_destroy(&mon->out_lock);
    qemu_rec_mutex_destroy(&monitor_rec_lock);
}
