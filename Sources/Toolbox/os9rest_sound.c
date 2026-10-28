/*
 * SoundLib's calls beyond the Sound Manager's channels that Blue Box can
 * back: MACE 3:1 and 6:1 compression (Comp3to1 and Comp6to1, the opposite
 * of Exp1to3 and Exp1to6), and the routine descriptors SoundLib exports
 * for the MACE calls.
 *
 * The compressor takes 8-bit offset binary samples, cnt packets of six
 * for each channel (MACE 3:1 packs six samples into two bytes, 6:1 into
 * one), and the channels' packets alternate in the output as the
 * expander reads them.  Each sample is coded by trying every code the
 * decoder has for it and keeping the one that decodes nearest, so what
 * comes out decodes (Exp1to3, a sound resource) to what went in as nearly
 * as the coding allows.  A state block (64 bytes, the first ten the
 * predictor) carries the coder from one call to the next.
 */
#include <stdlib.h>
#include <string.h>

#include "mace_decoder.h"
#include "os9rest.h"
#include "tb_internal.h"

enum { STATE_BYTES = 10 };

static void state_load(mace_state *st, gaddr block)
{
    memset(st, 0, sizeof *st);
    if (!block)
        return;
    st->index = (int16_t)rd16(block);
    st->factor = (int16_t)rd16(block + 2);
    st->prev2 = (int16_t)rd16(block + 4);
    st->previous = (int16_t)rd16(block + 6);
    st->level = (int16_t)rd16(block + 8);
}

static void state_store(const mace_state *st, gaddr block)
{
    if (!block)
        return;
    wr16(block, (uint16_t)st->index);
    wr16(block + 2, (uint16_t)st->factor);
    wr16(block + 4, (uint16_t)st->prev2);
    wr16(block + 6, (uint16_t)st->previous);
    wr16(block + 8, (uint16_t)st->level);
}

/* The 16-bit sample an 8-bit offset binary one is when the decoder says it:
 * its high byte twice. */
static int16_t target_of(uint8_t b)
{
    int8_t h = (int8_t)(b - 128);
    return (int16_t)(h * 257);
}

/* The codes of each of a byte's three positions: 3 bits, 2 bits, 3 bits. */
static const int code_count[3] = { 8, 4, 8 };

/* One position's code: the one that decodes nearest the targets. */
static uint8_t best_code(mace_state *st, bool mace3, int pos, const int16_t *target)
{
    int best = 0;
    long best_err = -1;
    for (int v = 0; v < code_count[pos]; v++) {
        mace_state trial = *st;
        int16_t out[2];
        int n = mace_step(&trial, mace3, pos, (uint8_t)v, out);
        long err = 0;
        for (int k = 0; k < n; k++) {
            long d = (long)out[k] - target[k];
            err += d * d;
        }
        if (best_err < 0 || err < best_err) {
            best_err = err;
            best = v;
        }
    }
    int16_t out[2];
    mace_step(st, mace3, pos, (uint8_t)best, out);
    return (uint8_t)best;
}

static void mace_compress(gaddr inp, gaddr outp, int32_t cnt, gaddr instate, gaddr outstate, int32_t nch,
                          int32_t which, bool mace3)
{
    if (cnt <= 0 || nch < 1 || which < 1 || which > nch || !inp || !outp)
        return;
    mace_state st;
    state_load(&st, instate);
    uint32_t per = mace3 ? 2 : 1, ch = (uint32_t)(which - 1);
    for (int32_t k = 0; k < cnt; k++) {
        uint8_t s[6];
        for (int i = 0; i < 6; i++)
            s[i] = rd8(inp + ((uint32_t)k * 6 + (uint32_t)i) * (uint32_t)nch + ch);
        gaddr to = outp + ((uint32_t)k * (uint32_t)nch + ch) * per;
        if (mace3) {
            for (int b = 0; b < 2; b++) {
                int16_t t[3] = { target_of(s[b * 3]), target_of(s[b * 3 + 1]), target_of(s[b * 3 + 2]) };
                uint8_t c0 = best_code(&st, true, 0, &t[0]), c1 = best_code(&st, true, 1, &t[1]),
                        c2 = best_code(&st, true, 2, &t[2]);
                wr8(to + (uint32_t)b, (uint8_t)(c2 << 5 | c1 << 3 | c0));
            }
        } else {
            int16_t t[6];
            for (int i = 0; i < 6; i++)
                t[i] = target_of(s[i]);
            uint8_t c0 = best_code(&st, false, 0, &t[0]), c1 = best_code(&st, false, 1, &t[2]),
                    c2 = best_code(&st, false, 2, &t[4]);
            wr8(to, (uint8_t)(c0 << 5 | c1 << 3 | c2));
        }
    }
    state_store(&st, outstate);
}

void tb_Comp3to1(gaddr inp, gaddr outp, int32_t cnt, gaddr instatep, gaddr outstatep, int32_t numchannels,
                 int32_t whichchannel)
{
    mace_compress(inp, outp, cnt, instatep, outstatep, numchannels, whichchannel, true);
}

void tb_Comp6to1(gaddr inp, gaddr outp, int32_t cnt, gaddr instatep, gaddr outstatep, int32_t numchannels,
                 int32_t whichchannel)
{
    mace_compress(inp, outp, cnt, instatep, outstatep, numchannels, whichchannel, false);
}

/* ---- The routine descriptors SoundLib exports as data ---- */

/* A one-routine PowerPC descriptor for an export (C calling convention,
 * nargs arguments of 4 bytes, none or a 4-byte result). */
static gaddr descriptor_for(const char *export_name, int nargs, bool result)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(export_name);
    gaddr tv = fn ? cfm_native_tvector(fn, export_name) : 0;
    uint32_t info = 1 /* kCStackBased */ | (result ? 3u << 4 : 0);
    for (int i = 0; i < nargs; i++)
        info |= 3u << (6 + 2 * i);
    return tv ? mm_new_descriptor(tv, info, 1 /* kPowerPCISA */) : 0;
}

static const struct { const char *data, *routine; int nargs; bool result; } descriptors[] = {
    { "g__Comp3to1RD", "Comp3to1", 7, false }, { "g__Comp6to1RD", "Comp6to1", 7, false },
    { "g__Exp1to3RD", "Exp1to3", 7, false }, { "g__Exp1to6RD", "Exp1to6", 7, false },
    { "g__MACEVersionLongRD", "MACEVersion", 0, true },
};
static gaddr descriptor_cells[sizeof descriptors / sizeof descriptors[0]];

void os9rest_sound_reset(void) { memset(descriptor_cells, 0, sizeof descriptor_cells); }

bool os9rest_sound_data_known(const char *name)
{
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; i++)
        if (!strcmp(name, descriptors[i].data))
            return true;
    return false;
}

/* The descriptor itself is the data: its address is the symbol's. */
gaddr os9rest_sound_data(const char *name)
{
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; i++)
        if (!strcmp(name, descriptors[i].data)) {
            if (!descriptor_cells[i])
                descriptor_cells[i] = descriptor_for(descriptors[i].routine, descriptors[i].nargs, descriptors[i].result);
            return descriptor_cells[i];
        }
    return 0;
}
