/*
 * BLUEBOX_CODECHECK=N: the program's code compared with a copy after
 * every Nth Toolbox call (1, or nothing after the =, for every one; 0 or
 * the variable unset for none), so a call that
 * writes into it is named rather than the illegal instruction met later
 * when the program runs into the damage.  The code is the PowerPC code
 * sections of the fragments the Code Fragment Manager loads, each copied
 * when it is loaded (a program that changes its own is caught too, at the
 * next call).  Not 68k CODE segments: their programs keep variables in
 * them (BBEdit 2.1.3 stores into CODE 1 before its first window).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb_internal.h"

#define MAX_CODE_REGIONS 256

typedef struct {
    gaddr base;
    uint32_t size;
    uint8_t *copy;
    char what[48];
} code_region;

static code_region regions[MAX_CODE_REGIONS];
static int nregions;
static int every = -1;

static bool enabled(void)
{
    if (every < 0) {
        const char *e = getenv("BLUEBOX_CODECHECK");
        /* "0" is none (the walk script sets it so); unset is none, and a
         * variable set to anything else that isn't a number is every call. */
        every = !e ? 0 : atoi(e) > 0 ? atoi(e) : *e == '0' ? 0 : 1;
    }
    return every > 0;
}

void machine_set_code_check(int n) { every = n > 0 ? n : 0; }

void code_check_add(gaddr base, uint32_t size, const char *what)
{
    if (!enabled() || !base || !size)
        return;
    code_check_remove(base);
    if (nregions >= MAX_CODE_REGIONS || !(regions[nregions].copy = malloc(size)))
        return;
    code_region *r = &regions[nregions++];
    r->base = base;
    r->size = size;
    memcpy(r->copy, g_mem + base, size);
    snprintf(r->what, sizeof r->what, "%s", what);
}

void code_check_remove(gaddr base)
{
    for (int i = 0; i < nregions; i++)
        if (regions[i].base == base) {
            free(regions[i].copy);
            regions[i] = regions[--nregions];
            return;
        }
}

void code_check_reset(void)
{
    while (nregions)
        free(regions[--nregions].copy);
}

static const char *checked_after; /* the call the last check followed */

static bool unchanged(const char *name, const char *when);

bool code_check_after_call(const char *name)
{
    static unsigned calls;
    if (!enabled() || !nregions || ++calls % (unsigned)every)
        return true;
    checked_after = name;
    return unchanged(name, NULL);
}

/* With every call checked, the code is checked on the way in too, so a
 * change the program's own code makes between two calls is told from one
 * the call makes. */
bool code_check_before_call(const char *name)
{
    if (!enabled() || every != 1 || !nregions || !checked_after)
        return true;
    return unchanged(name, checked_after);
}

static bool unchanged(const char *name, const char *since)
{
    for (int i = 0; i < nregions; i++) {
        code_region *r = &regions[i];
        if (!memcmp(r->copy, g_mem + r->base, r->size))
            continue;
        uint32_t lo = 0, hi = r->size;
        while (lo < r->size && r->copy[lo] == g_mem[r->base + lo])
            lo++;
        while (hi > lo && r->copy[hi - 1] == g_mem[r->base + hi - 1])
            hi--;
        uint32_t w = lo & ~3u;
        uint32_t was = 0, now = 0;
        for (uint32_t k = 0; k < 4 && w + k < r->size; k++) {
            was = was << 8 | r->copy[w + k];
            now = now << 8 | g_mem[r->base + w + k];
        }
        if (since)
            log_printf("code: %s changed at %08X..%08X (%u bytes; the word at %08X was %08X, now %08X) before %s,"
                       " since the check after %s: by code run between the two\n",
                       r->what, r->base + lo, r->base + hi - 1, hi - lo, r->base + w, was, now, name, since);
        else
            log_printf("code: %s changed at %08X..%08X (%u bytes; the word at %08X was %08X, now %08X) by %s"
                       " (or within the %d calls before it)\n",
                       r->what, r->base + lo, r->base + hi - 1, hi - lo, r->base + w, was, now, name, every - 1);
        /* Report each change once: the run may be let go on. */
        memcpy(r->copy, g_mem + r->base, r->size);
        return false;
    }
    return true;
}
