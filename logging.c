#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include "logging.h"

#undef fprintf
#undef printf
#undef vfprintf

int logging_redirect_output = 0;

/* the use of logging_set_prefix() is optional */
void logging_set_prefix(const char *ident)
{
    closelog();
    openlog(ident, LOG_NOWAIT | LOG_PID, LOG_DAEMON);
}

void logging_set_redirect(int redirect)
{
    logging_redirect_output = redirect;
}

static inline void __syslog_vfprintf(const char *format, va_list ap)
{
    vsyslog(LOG_DAEMON | LOG_NOTICE, format, ap);
}

int qemu_log_vfprintf(FILE *stream, const char *format, va_list ap)
{
    if (logging_redirect_output && (stream == stdout || stream == stderr)) {
        __syslog_vfprintf(format, ap);
        return 0;
    } else {
        return vfprintf(stream, format, ap);
    }
}

int qemu_log_printf(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    qemu_log_vfprintf(stdout, format, ap);
    va_end(ap);

    return 0;
}

int qemu_log_fprintf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int ret = 0;

    va_start(ap, format);
    if (logging_redirect_output && (stream == stdout || stream == stderr)) {
        __syslog_vfprintf(format, ap);
    } else {
        ret = vfprintf(stream, format, ap);
    }
    va_end(ap);

    return ret;
}
