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
    /* The heap, stack and A5 world stay below 2 GB: applications do signed
     * arithmetic on addresses (free memory as ApplLimit - HeapEnd …). */
    GA_APPLLIMIT     = 0x7F000000u, /* application heap may grow to here */
    /* Not at 2 GB itself: far-model jump tables end at A5 + their size,
     * which signed comparisons must still see as positive. */
    GA_A5WORLD_TOP   = 0x7FF00000u, /* A5 world ends here; stack below it */
    GA_ROM_32        = 0xF0000000u, /* fake ROM: header + native stubs */
    GA_ROM_SIZE_32   = 0x00100000u,
    GA_SCREEN_32     = 0xF8000000u, /* framebuffer */
    /* In 24-bit mode (mem_set_layout) everything is inside the 16 MB a
     * 24-bit Mac addressed.  Blue Box's own layout (no machine named) is
     * a Mac II's with more RAM: RAM (the partition ending at GA24_TOP),
     * the screen where a video card's slot space was, the I/O space at
     * $F00000 (for the hardware a program touches directly), and the ROM
     * above it.  A Mac II and the compact Macs have their own (mem.c).
     * The screen has the megabyte a card had in 24-bit mode, so it is at
     * most 8 bits deep at 1024 by 768, as a 24-bit Mac's was; the slot
     * space after it is empty, and a program drawing straight to the
     * screen past its bottom (Life does) writes there, not round the top
     * of the 16 MB into low memory. */
    GA24_TOP         = 0x00B00000u,
    GA24_SCREEN      = 0x00B00000u,
    GA24_SCREEN_SIZE = 0x00100000u,
    GA24_IO          = 0x00F00000u,
    GA24_ROM         = 0x00F80000u,
    GA24_ROM_SIZE    = 0x00080000u,
    /* The compact Macs' (Plus, SE, Classic) with 4 MB: the screen $5900
     * below the top of RAM and its alternate page $8000 under it, the
     * sound buffer $300 below the top, the ROM at $400000 and the chips
     * between $800000 and $EFFFFF. */
    GAC_MEMTOP       = 0x00400000u,
    GAC_SCREEN       = 0x003FA700u,
    GAC_SOUND        = 0x003FFD00u,
    GAC_ROM          = 0x00400000u,
};

/* The layouts: the flat 32-bit space; and in 24-bit mode Blue Box's own,
 * a Mac II's (8 MB of RAM, the ROM at $800000, the screen on a video card
 * in slot 9, I/O at $F00000) or a compact Mac's (above). */
enum { MEM_LAYOUT_32, MEM_LAYOUT_24, MEM_LAYOUT_MACII, MEM_LAYOUT_COMPACT };

/* Where things are in the layout chosen: the ROM, the screen, the top of
 * RAM (MemTop), the top of a partition kept below 16 MB (a program not
 * 32-bit clean in 32-bit mode, or any in 24-bit mode), the system heap's
 * end and the application heap's start, and the stack a partition below
 * 16 MB keeps under its A5 world. */
extern gaddr g_rom_base, g_rom_size, g_screen_base, g_screen_size, g_low_top;
extern gaddr g_mem_top, g_sysheap_end, g_appheap, g_stack_24;
extern int g_mem_layout;
#define GA_ROM g_rom_base
#define GA_ROM_SIZE g_rom_size
#define GA_SCREEN g_screen_base
#define GA_A5WORLD_TOP_24 g_low_top
#define GA_SYSHEAP_END g_sysheap_end
#define GA_APPHEAP g_appheap
#define GA_STACK_24 g_stack_24

extern uint8_t *g_mem;

/* Reserve the arena. Returns false on failure. Safe to call more than once. */
bool mem_init(void);
/* Choose the layout.  The 24-bit ones make the arena 256 mirrors of one
 * 16 MB space, so an address's top byte is ignored by every access (the
 * CPUs', the Toolbox's own and the JIT's), as the 24-bit address bus of a
 * Mac without 32-bit mode ignored it; MEM_LAYOUT_32 is the flat 4 GB
 * arena.  Either way the arena is left zeroed. */
bool mem_set_layout(int layout);
extern bool g_mem_24bit;
static inline gaddr mem_strip(gaddr a) { return g_mem_24bit ? a & 0x00FFFFFFu : a; }
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

/* Memory-mapped devices (the VIA, SCC, IWM ... of a classic Mac, hw68k.c):
 * one byte per megabyte of the address space says whether a device answers
 * in it.  The 68k interpreter's data accesses there go to g_hw_read and
 * g_hw_write (which fall back to plain memory outside the devices' own
 * windows); a JIT's don't, so a machine with devices runs interpreted. */
extern uint8_t g_hw_map[4096];
extern uint32_t (*g_hw_read)(gaddr a, int size);
extern void (*g_hw_write)(gaddr a, int size, uint32_t v);
extern bool g_hw_jit_off;
static inline bool hw_mapped(gaddr a) { return __builtin_expect(g_hw_map[a >> 20] != 0, 0); }

/* Guest memory a JIT has translated code from, in 256-byte regions: one
 * byte each, a bit per translator (CODE_PPC, CODE_68K).  NULL until a JIT
 * starts.  Every store into guest memory (the CPUs', the Toolbox's, the
 * bulk copies below) checks it, so a translation never outlives the code
 * it came from: a store into a marked region calls mem_code_written, which
 * tells each translator owning it to drop the blocks it overlaps. */
enum { CODE_MAP_SHIFT = 8, CODE_PPC = 1, CODE_68K = 2, CODE_ICACHE = 4 };
extern uint8_t *g_code_map;
/* A translator's handler for a store of [lo, hi) touching its regions
 * (the third is the 68020's and 68030's instruction cache, which the
 * Toolbox's own stores flush as the routines that moved code did). */
extern void (*g_code_written[3])(gaddr lo, gaddr hi);
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
