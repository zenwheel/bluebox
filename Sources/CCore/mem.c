#include "mem.h"

#include <stdio.h>
#include <sys/mman.h>

uint8_t *g_mem;
gaddr g_watch_lo, g_watch_len;
void (*g_watch_fn)(gaddr a);

uint8_t g_hw_map[4096];
bool g_hw_jit_off;
uint32_t (*g_hw_read)(gaddr a, int size);
void (*g_hw_write)(gaddr a, int size, uint32_t v);

uint8_t *g_code_map;
void (*g_code_written[2])(gaddr lo, gaddr hi);

#define CODE_REGION ((gaddr)1 << CODE_MAP_SHIFT)
#define CODE_MAP_SIZE ((size_t)1 << (32 - CODE_MAP_SHIFT))

bool mem_code_mark(gaddr lo, gaddr hi, int bit)
{
    if (!g_code_map) {
        /* Reserved whole, committed by the host as regions are marked. */
        void *p = mmap(NULL, CODE_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return false;
        g_code_map = p;
    }
    uint32_t first = (lo >= 8 ? lo - 8 : 0) >> CODE_MAP_SHIFT, last = (hi - 1) >> CODE_MAP_SHIFT;
    for (uint32_t r = first; r <= last; r++)
        g_code_map[r] |= (uint8_t)bit;
    return true;
}

void mem_code_unmark(gaddr lo, gaddr hi, int bit)
{
    if (!g_code_map)
        return;
    for (uint32_t r = lo >> CODE_MAP_SHIFT; r <= (hi - 1) >> CODE_MAP_SHIFT; r++)
        g_code_map[r] &= (uint8_t)~bit;
}

/* A store touched a marked region: each owner drops what it had
 * translated from the bytes written (and unmarks regions left empty). */
void mem_code_written(gaddr a, uint32_t len)
{
    uint32_t first = a >> CODE_MAP_SHIFT, last = (gaddr)(a + len - 1) >> CODE_MAP_SHIFT;
    uint8_t m = 0;
    for (uint32_t r = first;; r++) {
        m |= g_code_map[r];
        if (r == last)
            break;
    }
    for (int t = 0; t < 2; t++)
        if ((m & (1 << t)) && g_code_written[t])
            g_code_written[t](a, a + len);
}

void mem_code_range(gaddr a, uint32_t n)
{
    uint32_t first = a >> CODE_MAP_SHIFT, last = (gaddr)(a + n - 1) >> CODE_MAP_SHIFT;
    for (uint32_t r = first;; r++) {
        if (g_code_map[r]) {
            gaddr lo = r << CODE_MAP_SHIFT, hi = lo + CODE_REGION;
            if (lo < a)
                lo = a;
            if (hi - 1 > a + n - 1)
                hi = a + n;
            mem_code_written(lo, hi - lo);
        }
        if (r == last)
            break;
    }
}

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
    if (g_code_map && len)
        mem_code_range(addr, len);
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
    gput(dst + 1, s, (uint32_t)n);
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
