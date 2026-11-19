/*
 * Which rows of the screen were written: the presenters (present.h) draw,
 * copy or convert only those, and a frame that wrote nothing costs them
 * nothing.  There is no comparison of the screen with a copy of it.
 *
 * Every store into guest memory goes through mem.h's helpers or checks
 * the code map inline (the processors' interpreters and both JITs), and
 * the Toolbox's own bulk copies and fills check it too.  The screen's
 * 256-byte regions are marked in that map (CODE_SCREEN), so the first
 * store into one calls written() here, which notes the region and takes
 * its mark off: the stores after it cost nothing more.  The machine's
 * thread then tells the presenters at its own safe points (the end of
 * each slice of instructions, every wait in the Toolbox, before it
 * sleeps): the rows the noted regions cover get the publication's
 * number, and the regions are marked again.  Doing that on the machine's
 * thread keeps it exact: no store is ever half done while a region is
 * marked again, so none is missed between a presenter's look and the
 * next mark.  A presenter remembers the last number it saw and takes the
 * rows numbered after it; any number of them can (the GPU presenter, each
 * window's CPU conversion, the verifier below).
 *
 * The host writing the screen through a pointer of its own says so with
 * mem_wrote.  A JIT checks only the region a store starts in: a store
 * crossing into the next region from one already noted reaches at most 3
 * bytes past it, so a noted region's rows run that far on.  A store that
 * starts just before the screen is seen through the region before it,
 * which stays marked.
 *
 * Everything counts as changed when the page shown moves or changes form
 * (depth, size, a compact Mac's alternate page), when a snapshot is put
 * back, and when screen_dirty_all says so; the presenters also notice a
 * change of colours, gamma or form themselves.  As a safety net they look
 * at everything now and then (present_metal.m, qd_screen.c): a writer
 * missed costs a moment's latency, never a stale screen.
 *
 * BLUEBOX_DIRTY=0 turns it off (the presenters compare the screen with a
 * copy, as they did before).  BLUEBOX_DIRTY_VERIFY=1 keeps a copy on the
 * machine's thread and, at each publication, logs every row that changed
 * without being marked, with the Toolbox calls made since the last.
 *
 * All of it is the host's: nothing here is saved, and a snapshot put back
 * starts it again with everything changed.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "ppc.h"
#include "present.h"
#include "tb_internal.h"

enum { REGION = 1 << CODE_MAP_SHIFT, SLOP = 3, NONE = -1 };

BB_HOST static int mode = -1; /* 0 off, 1 on, 2 on and verified */
BB_HOST static pthread_t machine_thread;
BB_HOST static bool have_thread;

/* What is watched (the machine's thread's): the page shown, its map
 * regions [r0, r1], and the region before it when a store starting there
 * can reach it. */
BB_HOST static bool tracking, retrack_wanted, all_wanted;
BB_HOST static gaddr trk_base;
BB_HOST static uint32_t trk_rb, trk_h, trk_len, r0, r1;
BB_HOST static int64_t pre = NONE;
/* The regions written since the last publication, a bit each. */
BB_HOST static _Atomic uint64_t *noted;
BB_HOST static uint32_t nwords;
BB_HOST static _Atomic bool any_noted;

/* What the presenters read, under the lock: each row's last publication,
 * the last that changed everything, and the latest. */
BB_HOST static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
BB_HOST static _Atomic uint64_t pub_seq;
BB_HOST static _Atomic bool watching;
BB_HOST static uint64_t all_seq;
BB_HOST static uint64_t *stamps;
BB_HOST static int nstamps;

static int dirty_mode(void)
{
    if (mode < 0) {
        const char *e = getenv("BLUEBOX_DIRTY"), *v = getenv("BLUEBOX_DIRTY_VERIFY");
        mode = e && e[0] == '0' ? 0 : v && *v && *v != '0' ? 2 : 1;
        if (mode == 2)
            atexit(screen_dirty_report);
    }
    return mode;
}

bool screen_dirty_verifying(void) { return dirty_mode() == 2; }

static void restored(void)
{
    retrack_wanted = true; /* the page's marks belonged to the memory before */
}

/* A store of [lo, hi) touched a marked region (mem.c). */
static void written(gaddr lo, gaddr hi)
{
    uint64_t end = hi > lo ? (uint64_t)hi : (uint64_t)hi + 0x100000000ull; /* a range up to the top wraps to 0 */
    uint32_t first = lo >> CODE_MAP_SHIFT, last = (uint32_t)((end - 1) >> CODE_MAP_SHIFT);
    for (uint32_t r = first;; r++) {
        uint8_t *m = &g_code_map[r];
        if (*m & CODE_SCREEN) {
            if (tracking && r >= r0 && r <= r1) {
                __atomic_fetch_and(m, (uint8_t)~CODE_SCREEN, __ATOMIC_RELAXED);
                uint32_t i = r - r0;
                atomic_fetch_or_explicit(&noted[i / 64], 1ull << (i % 64), memory_order_relaxed);
                atomic_store_explicit(&any_noted, true, memory_order_relaxed);
            } else if ((int64_t)r != pre || !tracking) {
                __atomic_fetch_and(m, (uint8_t)~CODE_SCREEN, __ATOMIC_RELAXED); /* a page no longer shown */
            }
        }
        if (r == last)
            break;
    }
}

static void mark(uint32_t r) { __atomic_fetch_or(&g_code_map[r], (uint8_t)CODE_SCREEN, __ATOMIC_RELAXED); }
static void unmark(uint32_t r) { __atomic_fetch_and(&g_code_map[r], (uint8_t)~CODE_SCREEN, __ATOMIC_RELAXED); }

/* The rows a noted region's stores can have reached. */
static void region_rows(uint32_t r, int *y0, int *y1)
{
    uint64_t lo = (uint64_t)r << CODE_MAP_SHIFT, hi = lo + REGION + SLOP;
    if (lo < trk_base)
        lo = trk_base;
    if (hi > (uint64_t)trk_base + trk_len)
        hi = (uint64_t)trk_base + trk_len;
    *y0 = (int)((lo - trk_base) / trk_rb);
    *y1 = hi > lo ? (int)((hi - 1 - trk_base) / trk_rb) + 1 : *y0;
}

/* The page shown watched afresh: everything has changed. */
static void retrack(void)
{
    if (tracking) {
        for (uint32_t r = r0; r <= r1; r++)
            unmark(r);
        if (pre != NONE)
            unmark((uint32_t)pre);
    }
    tracking = false;
    retrack_wanted = false;
    int w, h;
    machine_screen_size(&w, &h);
    trk_base = qd_screen_display_base();
    trk_rb = qd_screen_row_bytes();
    trk_h = h > 0 ? (uint32_t)h : 0;
    trk_len = trk_rb * trk_h;
    if (trk_base && trk_len && (uint64_t)trk_base + trk_len <= 0x100000000ull && mem_code_mark(trk_base, trk_base + trk_len, CODE_SCREEN)) {
        g_code_written[3] = written;
        g_code_restored[3] = restored;
        r0 = trk_base >> CODE_MAP_SHIFT;
        r1 = (trk_base + trk_len - 1) >> CODE_MAP_SHIFT;
        uint32_t before = (trk_base >= 8 ? trk_base - 8 : 0) >> CODE_MAP_SHIFT; /* mem_code_mark's */
        pre = before < r0 ? (int64_t)before : NONE;
        uint32_t words = (r1 - r0 + 1 + 63) / 64;
        if (words > nwords) {
            bb_host_free((void *)noted);
            noted = bb_host_malloc(words * sizeof *noted);
            nwords = noted ? words : 0;
        }
        tracking = noted != NULL;
        if (noted)
            memset((void *)noted, 0, nwords * sizeof *noted);
    }
    atomic_store(&any_noted, false);
    pthread_mutex_lock(&lock);
    if (nstamps != (int)trk_h) {
        bb_host_free(stamps);
        stamps = trk_h ? bb_host_malloc(trk_h * sizeof *stamps) : NULL;
        nstamps = stamps ? (int)trk_h : 0;
    }
    if (stamps)
        memset(stamps, 0, (size_t)nstamps * sizeof *stamps);
    all_seq = atomic_load(&pub_seq) + 1;
    atomic_store_explicit(&pub_seq, all_seq, memory_order_release);
    atomic_store(&watching, tracking); /* else the presenters compare */
    pthread_mutex_unlock(&lock);
}

void screen_dirty_all(void) { all_wanted = true; }

static void verify(void);

void machine_screen_publish(void)
{
    if (!dirty_mode() || !g_mem)
        return;
    int w, h;
    machine_screen_size(&w, &h);
    if (retrack_wanted || trk_base != qd_screen_display_base() || trk_rb != qd_screen_row_bytes() ||
        trk_h != (uint32_t)h)
        retrack();
    if (all_wanted && tracking) {
        all_wanted = false;
        pthread_mutex_lock(&lock);
        all_seq = atomic_load(&pub_seq) + 1;
        atomic_store_explicit(&pub_seq, all_seq, memory_order_release);
        pthread_mutex_unlock(&lock);
    }
    if (tracking && atomic_exchange_explicit(&any_noted, false, memory_order_relaxed)) {
        pthread_mutex_lock(&lock);
        uint64_t seq = atomic_load(&pub_seq) + 1;
        for (uint32_t k = 0; k < nwords; k++) {
            uint64_t bits = atomic_exchange_explicit(&noted[k], 0, memory_order_relaxed);
            while (bits) {
                uint32_t i = k * 64 + (uint32_t)__builtin_ctzll(bits);
                bits &= bits - 1;
                int y0, y1;
                region_rows(r0 + i, &y0, &y1);
                for (int y = y0; y < y1 && y < nstamps; y++)
                    stamps[y] = seq;
                mark(r0 + i);
            }
        }
        atomic_store_explicit(&pub_seq, seq, memory_order_release);
        pthread_mutex_unlock(&lock);
    }
    if (dirty_mode() == 2)
        verify();
}

void screen_dirty_machine_thread(void)
{
    machine_thread = pthread_self();
    have_thread = true;
}

void screen_dirty_publish(void)
{
    if (!have_thread || pthread_equal(pthread_self(), machine_thread))
        machine_screen_publish();
}

/* The rows [y0, y1) published since *seen as bands, *seen brought up to
 * date: their number, PRESENT_DIRTY_ALL for everything, PRESENT_DIRTY_UNKNOWN
 * when nothing is tracked (compare the screen instead). */
int present_dirty_rows(uint64_t *seen, int y0, int y1, int32_t *bands, int max)
{
    if (!dirty_mode())
        return PRESENT_DIRTY_UNKNOWN;
    uint64_t p = atomic_load_explicit(&pub_seq, memory_order_acquire);
    if (!atomic_load_explicit(&watching, memory_order_relaxed))
        return PRESENT_DIRTY_UNKNOWN; /* not started yet, or the page can't be watched */
    if (p == *seen)
        return 0;
    pthread_mutex_lock(&lock);
    p = atomic_load(&pub_seq);
    int n = 0;
    if (!*seen || all_seq > *seen) {
        n = PRESENT_DIRTY_ALL;
    } else {
        if (y0 < 0)
            y0 = 0;
        if (y1 > nstamps)
            y1 = nstamps;
        int run = -1;
        for (int y = y0; y < y1; y++) {
            bool d = stamps[y] > *seen;
            if (d && run < 0)
                run = y;
            else if (!d && run >= 0) {
                present_add_band(bands, &n, max, run, y);
                run = -1;
            }
        }
        if (run >= 0)
            present_add_band(bands, &n, max, run, y1);
    }
    *seen = p;
    pthread_mutex_unlock(&lock);
    return n;
}

/* A band of rows [y0, y1) added to a list of them, at most max: one
 * touching the last grows it, and past max the last grows over the gap
 * (the rows between done again, unchanged). */
void present_add_band(int32_t *bands, int *n, int max, int y0, int y1)
{
    if (max < 1)
        return;
    if (*n > 0 && (bands[*n * 2 - 1] == y0 || *n == max)) {
        bands[*n * 2 - 1] = y1;
        return;
    }
    bands[*n * 2] = y0;
    bands[*n * 2 + 1] = y1;
    (*n)++;
}

/* ---- BLUEBOX_DIRTY_VERIFY ---- */

BB_HOST static uint8_t *vcopy;
BB_HOST static size_t vlen;
BB_HOST static gaddr vbase;
BB_HOST static uint64_t vseen, vmisses, vrows_missed, vchecks;
enum { CALLS = 8 };
BB_HOST static const char *vcalls[CALLS];
BB_HOST static unsigned vncalls;

/* A Toolbox call returned (trap.c, cfm.c): named in a miss's report. */
void screen_dirty_note_call(const char *name)
{
    if (!name || (vncalls && vcalls[(vncalls - 1) % CALLS] == name))
        return;
    vcalls[vncalls++ % CALLS] = name;
}

static void verify(void)
{
    if (!tracking)
        return;
    size_t len = trk_len;
    int32_t bands[512];
    int n = present_dirty_rows(&vseen, 0, (int)trk_h, bands, 256);
    if (len != vlen || vbase != trk_base || !vcopy || n == PRESENT_DIRTY_ALL) {
        if (len != vlen) {
            bb_host_free(vcopy);
            vcopy = bb_host_malloc(len);
            vlen = vcopy ? len : 0;
        }
        vbase = trk_base;
        if (vcopy)
            memcpy(vcopy, gptr(trk_base), vlen);
        vncalls = 0;
        return;
    }
    vchecks++;
    int b = 0, missed0 = -1, missed1 = -1, missed = 0;
    for (uint32_t y = 0; y < trk_h; y++) {
        uint8_t *c = vcopy + (size_t)y * trk_rb;
        const uint8_t *p = gptr(trk_base + y * trk_rb);
        if (!memcmp(c, p, trk_rb))
            continue;
        while (b < n && bands[b * 2 + 1] <= (int32_t)y)
            b++;
        bool marked = b < n && bands[b * 2] <= (int32_t)y;
        if (!marked) {
            if (missed0 < 0)
                missed0 = (int)y;
            missed1 = (int)y;
            missed++;
        }
        memcpy(c, p, trk_rb);
    }
    if (missed) {
        vmisses++;
        vrows_missed += (uint64_t)missed;
        if (vmisses <= 50) {
            char calls[400] = "";
            size_t at = 0;
            unsigned k0 = vncalls > CALLS ? vncalls - CALLS : 0;
            for (unsigned k = k0; k < vncalls && at < sizeof calls - 40; k++)
                at += (size_t)snprintf(calls + at, sizeof calls - at, "%s%s", k > k0 ? " " : "", vcalls[k % CALLS]);
            log_printf("dirty: miss: %d rows changed unmarked (%d-%d) of %u at %08X, %d bits; 68k pc %08X, "
                       "PowerPC pc %08X; calls since the last check: %s\n",
                       missed, missed0, missed1, trk_h, trk_base, qd_screen_depth(), g_cpu ? g_cpu->op_pc : 0,
                       g_ppc.pc, at ? calls : "none");
        }
    }
    vncalls = 0;
}

void screen_dirty_report(void)
{
    if (dirty_mode() == 2)
        log_printf("dirty: verified %llu times, %llu misses, %llu rows missed\n", (unsigned long long)vchecks,
                   (unsigned long long)vmisses, (unsigned long long)vrows_missed);
}
