#include "m68k.h"

#include <stdio.h>

/* Minimal for now: raw words.  Enough for trace logs to be diffable. */
int cpu_disasm(uint32_t addr, char *buf, int bufsize)
{
    snprintf(buf, (size_t)bufsize, "dc.w $%04X", rd16(addr));
    return 2;
}
