#ifndef CCORE_LOG_H
#define CCORE_LOG_H

#include <stdio.h>

enum {
    LOG_TRAPS = 1 << 0,  /* every trap call */
    LOG_UNIMPL = 1 << 1, /* unimplemented traps */
    LOG_SEG = 1 << 2,    /* segment loader */
    LOG_RSRC = 1 << 3,   /* resource manager */
    LOG_MEM = 1 << 4,    /* memory manager */
    LOG_CPU = 1 << 5,    /* instruction trace */
};

extern unsigned g_log_mask;
extern FILE *g_log_file; /* defaults to stderr */

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOGF(mask, ...)                      \
    do {                                     \
        if (g_log_mask & (mask))             \
            log_printf(__VA_ARGS__);         \
    } while (0)

#endif
