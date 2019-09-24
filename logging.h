#ifndef LOGGING_H_
# define LOGGING_H_

# include <stdarg.h>
# include <stdio.h>
# include <stdlib.h>
# include <assert.h>

#ifdef printf
# undef printf
#endif
#ifdef vfprintf
# undef vfprintf
#endif
#ifdef fprintf
# undef fprintf
#endif
#ifdef assert
# undef assert
#endif

/* Rolling my own assert() to send its error to syslog... */
#undef assert
#ifdef NDEBUG
# define assert(expr)		(__ASSERT_VOID_CAST (0))
#else
# define assert(expr)		do { \
    if (!(expr)) { \
        if (logging_redirect_output) { \
            qemu_log_printf("%s:%s:%d Assertion `%s' failed.", __FILE__, __FUNCTION__, __LINE__, #expr); \
            abort(); \
        } else { \
            __assert_fail(#expr, __FILE__, __LINE__, __ASSERT_FUNCTION); \
        } \
    } \
} while(0)
#endif

# define printf(...) qemu_log_printf(__VA_ARGS__)
# define vfprintf(...) qemu_log_vfprintf(__VA_ARGS__)
# define fprintf(...) qemu_log_fprintf(__VA_ARGS__)

void logging_set_redirect(int redirect);
void logging_set_prefix(const char *ident);
int qemu_log_vfprintf(FILE *stream, const char *format, va_list ap);
int qemu_log_printf(const char *format, ...)
  __attribute__ ((format (printf, 1, 2)));
int qemu_log_fprintf(FILE *stream, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));
extern int logging_redirect_output;

#endif /* !LOGGING_H_ */
