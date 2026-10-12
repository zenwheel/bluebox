#include "log.h"

#include <stdarg.h>

unsigned g_log_mask = LOG_UNIMPL;
FILE *g_log_file;

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_file ? g_log_file : stderr, fmt, ap);
    va_end(ap);
}
