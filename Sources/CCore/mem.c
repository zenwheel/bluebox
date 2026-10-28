#include "mem.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *g_mem;
bool g_mem_24bit;
gaddr g_rom_base = GA_ROM_32, g_rom_size = GA_ROM_SIZE_32, g_screen_base = GA_SCREEN_32;
gaddr g_screen_size = 0u - GA_SCREEN_32, g_low_top = 0x01000000u;
gaddr g_watch_lo, g_watch_len;
void (*g_watch_fn)(gaddr a);

uint8_t *g_code_map;
void (*g_code_written[2])(gaddr lo, gaddr hi);

/* 4 GB plus slack so a multi-byte access at the very top cannot run off. */
#define ARENA_SIZE ((size_t)1 << 32)
#define ARENA_SLACK ((size_t)1 << 16)

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

/* 24-bit mode's one 16 MB space and its mirrors. */
#define SPACE_24 ((size_t)1 << 24)
static int space_fd = -1;

/* A fresh (zeroed) space mapped at every 16 MB of the arena, and over the
 * slack too, so an access running off the top wraps to address 0 as it
 * did on a 24-bit bus.  A new object each time: the old pages go back to
 * the host, and macOS sizes a shared memory object only once. */
static bool map_space(void)
{
    char name[48];
    snprintf(name, sizeof name, "/bluebox24.%ld", (long)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("mem_set_24bit: shm_open");
        return false;
    }
    shm_unlink(name);
    if (ftruncate(fd, (off_t)SPACE_24) != 0) {
        perror("mem_set_24bit: ftruncate");
        close(fd);
        return false;
    }
    for (size_t off = 0; off < ARENA_SIZE + ARENA_SLACK; off += SPACE_24) {
        size_t n = ARENA_SIZE + ARENA_SLACK - off < SPACE_24 ? ARENA_SIZE + ARENA_SLACK - off : SPACE_24;
        if (mmap(g_mem + off, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            perror("mem_set_24bit: mmap");
            close(fd);
            return false;
        }
    }
    if (space_fd >= 0)
        close(space_fd);
    space_fd = fd;
    return true;
}

static bool map_flat(void)
{
    void *p = mmap(g_mem, ARENA_SIZE + ARENA_SLACK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mem_set_24bit: mmap");
        return false;
    }
    if (space_fd >= 0)
        close(space_fd);
    space_fd = -1;
    return true;
}

bool mem_set_24bit(bool on)
{
    if (!mem_init())
        return false;
    if (on ? !map_space() : g_mem_24bit && !map_flat())
        return false;
    g_mem_24bit = on;
    g_rom_base = on ? GA24_ROM : GA_ROM_32;
    g_rom_size = on ? GA24_ROM_SIZE : GA_ROM_SIZE_32;
    g_screen_base = on ? GA24_SCREEN : GA_SCREEN_32;
    g_screen_size = on ? GA24_SCREEN_SIZE : 0u - GA_SCREEN_32;
    g_low_top = on ? GA24_TOP : 0x01000000u;
    if (g_code_map) /* whatever was translated is gone with the memory */
        mem_code_range(0, 0xFFFFFFFFu);
    return true;
}

void mem_discard(gaddr addr, uint32_t len)
{
    if (g_code_map && len)
        mem_code_range(addr, len);
    if (g_mem_24bit) { /* within the one space, which wraps */
        if (len >= SPACE_24) {
            map_space();
            return;
        }
        gaddr a = addr & (SPACE_24 - 1);
        uint32_t first = (uint32_t)(SPACE_24 - a) < len ? (uint32_t)(SPACE_24 - a) : len;
        memset(g_mem + a, 0, first);
        memset(g_mem, 0, len - first);
        return;
    }
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
