#include "mem.h"

#include <stdio.h>
#include <sys/mman.h>

uint8_t *g_mem;
gaddr g_watch_lo, g_watch_len;
void (*g_watch_fn)(gaddr a);

/* 4 GB plus slack so a multi-byte access at the very top cannot run off. */
#define ARENA_SIZE ((size_t)1 << 32)
#define ARENA_SLACK ((size_t)1 << 16)

bool mem_init(void)
{
    if (g_mem)
        return true;
    void *p = mmap(NULL, ARENA_SIZE + ARENA_SLACK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mem_init: mmap");
        return false;
    }
    g_mem = p;
    return true;
}

void mem_discard(gaddr addr, uint32_t len)
{
    /* Only whole pages can go back to the host; zero the ragged edges. */
    const uintptr_t page = 16384;
    uintptr_t start = (uintptr_t)(g_mem + addr);
    uintptr_t end = start + len;
    uintptr_t pstart = (start + page - 1) & ~(page - 1);
    uintptr_t pend = end & ~(page - 1);
    if (pend <= pstart) {
        memset((void *)start, 0, len);
        return;
    }
    memset((void *)start, 0, pstart - start);
    memset((void *)pend, 0, end - pend);
    /* MAP_FIXED over the range: fresh zero pages, old ones released. */
    void *p = mmap((void *)pstart, pend - pstart, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        memset((void *)pstart, 0, pend - pstart);
}

void mem_reset(void)
{
    if (g_mem)
        mem_discard(0, 0xFFFFFFFFu);
}

void pstr_to_c(gaddr s, char *buf, size_t bufsize)
{
    size_t n = rd8(s);
    if (bufsize == 0)
        return;
    if (n > bufsize - 1)
        n = bufsize - 1;
    memcpy(buf, g_mem + s + 1, n);
    buf[n] = 0;
}

void pstr_from_c(gaddr dst, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    wr8(dst, (uint8_t)n);
    memcpy(g_mem + dst + 1, s, n);
}

const char *fourcc_str(uint32_t code, char buf[5])
{
    for (int i = 0; i < 4; i++) {
        uint8_t ch = (uint8_t)(code >> (24 - 8 * i));
        buf[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    buf[4] = 0;
    return buf;
}
