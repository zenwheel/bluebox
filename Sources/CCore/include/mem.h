/*
 * Guest memory: one flat 4 GB arena holding the whole 32-bit Mac address
 * space.  A guest address is an offset into it.  The arena is reserved up
 * front and committed lazily by the host OS, so untouched space costs nothing.
 *
 * All guest-visible data is big-endian and is accessed only through these
 * helpers (or the generated struct accessors built on them).
 */
#ifndef CCORE_MEM_H
#define CCORE_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "mem.h assumes a little-endian host"
#endif

typedef uint32_t gaddr;

/* Guest address space layout (32-bit mode). */
enum {
    GA_SYSHEAP       = 0x00004000u, /* system heap start */
    GA_SYSHEAP_END   = 0x00400000u,
    GA_APPHEAP       = 0x00400000u, /* application heap start (grows up) */
    /* The heap, stack and A5 world stay below 2 GB: applications do signed
     * arithmetic on addresses (free memory as ApplLimit - HeapEnd …). */
    GA_APPLLIMIT     = 0x7F000000u, /* application heap may grow to here */
    /* Not at 2 GB itself: far-model jump tables end at A5 + their size,
     * which signed comparisons must still see as positive. */
    GA_A5WORLD_TOP   = 0x7FF00000u, /* A5 world ends here; stack below it */
    GA_ROM           = 0xF0000000u, /* fake ROM: header + native stubs */
    GA_ROM_SIZE      = 0x00100000u,
    GA_SCREEN        = 0xF8000000u, /* framebuffer */
};

extern uint8_t *g_mem;

/* Reserve the arena. Returns false on failure. Safe to call more than once. */
bool mem_init(void);
/* Zero the whole arena and give the pages back to the host. */
void mem_reset(void);
/* Tell the host a guest range is no longer needed (contents become zero). */
void mem_discard(gaddr addr, uint32_t len);

static inline uint8_t *gptr(gaddr a) { return g_mem + a; }

/* A range the CPUs' own stores are watched in (the screen, for writes a
 * program makes to it directly rather than through QuickDraw): a store
 * there calls g_watch_fn.  The Toolbox's own writes aren't watched. */
extern gaddr g_watch_lo, g_watch_len;
extern void (*g_watch_fn)(gaddr a);
static inline void watch_store(gaddr a)
{
    if (a - g_watch_lo < g_watch_len)
        g_watch_fn(a);
}

/* Guest memory a JIT has translated code from, in 256-byte regions: one
 * byte each, a bit per translator (CODE_PPC, CODE_68K).  NULL until a JIT
 * starts.  Every store into guest memory (the CPUs', the Toolbox's, the
 * bulk copies below) checks it, so a translation never outlives the code
 * it came from: a store into a marked region calls mem_code_written, which
 * tells each translator owning it to drop its blocks there. */
enum { CODE_MAP_SHIFT = 8, CODE_PPC = 1, CODE_68K = 2 };
extern uint8_t *g_code_map;
/* A translator's handler for stores into its regions [lo, hi). */
extern void (*g_code_written[2])(gaddr lo, gaddr hi);
void mem_code_written(gaddr a, uint32_t len);
/* Mark [lo, hi) as holding a translator's code (also the region just
 * before, so a misaligned store starting there is seen).  Returns false if
 * the map can't be made. */
bool mem_code_mark(gaddr lo, gaddr hi, int bit);
/* Clear a translator's bit over [lo, hi), whole regions. */
void mem_code_unmark(gaddr lo, gaddr hi, int bit);

static inline void code_store(gaddr a, uint32_t len)
{
    if (__builtin_expect(g_code_map != NULL, 0) &&
        (g_code_map[a >> CODE_MAP_SHIFT] | g_code_map[(gaddr)(a + len - 1) >> CODE_MAP_SHIFT]))
        mem_code_written(a, len);
}

static inline uint8_t rd8(gaddr a) { return g_mem[a]; }
static inline uint16_t rd16(gaddr a)
{
    uint16_t v;
    memcpy(&v, g_mem + a, 2);
    return __builtin_bswap16(v);
}
static inline uint32_t rd32(gaddr a)
{
    uint32_t v;
    memcpy(&v, g_mem + a, 4);
    return __builtin_bswap32(v);
}
static inline void wr8(gaddr a, uint8_t v)
{
    code_store(a, 1);
    g_mem[a] = v;
}
static inline void wr16(gaddr a, uint16_t v)
{
    code_store(a, 2);
    v = __builtin_bswap16(v);
    memcpy(g_mem + a, &v, 2);
}
static inline void wr32(gaddr a, uint32_t v)
{
    code_store(a, 4);
    v = __builtin_bswap32(v);
    memcpy(g_mem + a, &v, 4);
}

/* Bulk stores (a whole range checked against the code map). */
void mem_code_range(gaddr a, uint32_t n);
static inline void gmemcpy(gaddr dst, gaddr src, uint32_t n)
{
    if (g_code_map && n)
        mem_code_range(dst, n);
    memmove(g_mem + dst, g_mem + src, n);
}
static inline void gmemset(gaddr dst, int c, uint32_t n)
{
    if (g_code_map && n)
        mem_code_range(dst, n);
    memset(g_mem + dst, c, n);
}
static inline void gput(gaddr dst, const void *src, uint32_t n)
{
    if (g_code_map && n)
        mem_code_range(dst, n);
    memcpy(g_mem + dst, src, n);
}
static inline void gget(void *dst, gaddr src, uint32_t n) { memcpy(dst, g_mem + src, n); }

/* Pascal strings in guest memory. */
static inline uint8_t pstr_len(gaddr s) { return rd8(s); }
/* Copy a guest Pascal string into a NUL-terminated host buffer (MacRoman bytes). */
void pstr_to_c(gaddr s, char *buf, size_t bufsize);
/* Store a C string as a Pascal string (truncated to 255). */
void pstr_from_c(gaddr dst, const char *s);

/* Four-character codes. */
static inline uint32_t fourcc(const char *s)
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}
/* Format a four-character code into buf (at least 5 bytes, printable). */
const char *fourcc_str(uint32_t code, char buf[5]);

#endif
