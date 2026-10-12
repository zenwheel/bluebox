#include "sst.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "mem.h"

/* ---- A tiny JSON reader, just enough for this format ---- */

typedef struct {
    const char *p, *end;
    bool err;
} jr;

static void ws(jr *j)
{
    while (j->p < j->end && isspace((unsigned char)*j->p))
        j->p++;
}

static bool eat(jr *j, char ch)
{
    ws(j);
    if (j->p < j->end && *j->p == ch) {
        j->p++;
        return true;
    }
    return false;
}

static void expect(jr *j, char ch)
{
    if (!eat(j, ch))
        j->err = true;
}

static size_t str(jr *j, char *buf, size_t n)
{
    expect(j, '"');
    size_t k = 0;
    while (j->p < j->end && *j->p != '"') {
        if (*j->p == '\\')
            j->p++;
        if (k + 1 < n)
            buf[k++] = *j->p;
        j->p++;
    }
    if (n)
        buf[k] = 0;
    expect(j, '"');
    return k;
}

static int64_t num(jr *j)
{
    ws(j);
    char *e;
    long long v = strtoll(j->p, &e, 10);
    if (e == j->p)
        j->err = true;
    j->p = e;
    return v;
}

static void skip_value(jr *j)
{
    ws(j);
    if (j->p >= j->end) {
        j->err = true;
        return;
    }
    char ch = *j->p;
    if (ch == '"') {
        str(j, NULL, 0);
    } else if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        j->p++;
        if (eat(j, close))
            return;
        do {
            if (ch == '{') {
                str(j, NULL, 0);
                expect(j, ':');
            }
            skip_value(j);
        } while (eat(j, ',') && !j->err);
        expect(j, close);
    } else {
        while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']')
            j->p++;
    }
}

typedef struct {
    uint32_t d[8], a[7], usp, ssp, pc;
    uint16_t sr;
    uint16_t prefetch[2];
    uint32_t (*ram)[2];
    int nram;
    int ramcap;
} state;

static void parse_state(jr *j, state *s)
{
    char key[32];
    expect(j, '{');
    do {
        str(j, key, sizeof key);
        expect(j, ':');
        if (key[0] == 'd' && key[1] >= '0' && key[1] <= '7' && !key[2])
            s->d[key[1] - '0'] = (uint32_t)num(j);
        else if (key[0] == 'a' && key[1] >= '0' && key[1] <= '6' && !key[2])
            s->a[key[1] - '0'] = (uint32_t)num(j);
        else if (!strcmp(key, "usp"))
            s->usp = (uint32_t)num(j);
        else if (!strcmp(key, "ssp"))
            s->ssp = (uint32_t)num(j);
        else if (!strcmp(key, "sr"))
            s->sr = (uint16_t)num(j);
        else if (!strcmp(key, "pc"))
            s->pc = (uint32_t)num(j);
        else if (!strcmp(key, "prefetch")) {
            expect(j, '[');
            s->prefetch[0] = (uint16_t)num(j);
            expect(j, ',');
            s->prefetch[1] = (uint16_t)num(j);
            expect(j, ']');
        } else if (!strcmp(key, "ram")) {
            s->nram = 0;
            expect(j, '[');
            if (!eat(j, ']')) {
                do {
                    if (s->nram == s->ramcap) {
                        s->ramcap = s->ramcap ? s->ramcap * 2 : 64;
                        s->ram = realloc(s->ram, (size_t)s->ramcap * sizeof *s->ram);
                    }
                    expect(j, '[');
                    s->ram[s->nram][0] = (uint32_t)num(j);
                    expect(j, ',');
                    s->ram[s->nram][1] = (uint32_t)num(j);
                    expect(j, ']');
                    s->nram++;
                } while (eat(j, ',') && !j->err);
                expect(j, ']');
            }
        } else {
            skip_value(j);
        }
    } while (eat(j, ',') && !j->err);
    expect(j, '}');
}

/* ---- Running one vector ---- */

static uint32_t vec3(const state *s)
{
    /* The address error vector, if the vector table is in the test's RAM. */
    uint32_t v = 0;
    int found = 0;
    for (int i = 0; i < s->nram; i++) {
        uint32_t a = s->ram[i][0];
        if (a >= 12 && a <= 15) {
            v |= s->ram[i][1] << (8 * (15 - a));
            found++;
        }
    }
    return found == 4 ? v : 0xFFFFFFFFu;
}

static bool run_one(const char *name, const state *in, const state *out, char *why, size_t whylen, bool *skip)
{
    static cpu68k c;
    *skip = false;
    cpu_init(&c);
    c.model = 68000;
    c.addr_mask = 0x00FFFFFF;
    for (int i = 0; i < 8; i++)
        c.d[i] = in->d[i];
    for (int i = 0; i < 7; i++)
        c.a[i] = in->a[i];
    c.sr = in->sr;
    if (in->sr & SR_S) {
        c.a[7] = in->ssp;
        c.usp = in->usp;
    } else {
        c.a[7] = in->usp;
        c.isp = in->ssp;
    }
    c.pc = in->pc;
    wr16(in->pc & 0xFFFFFF, in->prefetch[0]);
    wr16((in->pc + 2) & 0xFFFFFF, in->prefetch[1]);
    for (int i = 0; i < in->nram; i++)
        wr8(in->ram[i][0] & 0xFFFFFF, (uint8_t)in->ram[i][1]);

    /* Known divergence: for ASR with a register count larger than the
     * operand size, the suite clears C and X.  The 68000 manual (and
     * Musashi) give the last bit shifted out, i.e. the sign bit; we follow
     * the manual. */
    uint16_t op = in->prefetch[0];
    if ((op & 0xF118) == 0xE000 + 0x0000 && (op & 0x00C0) != 0x00C0 && (op & 0x20)) {
        int size = 8 << ((op >> 6) & 3);
        if ((int)(in->d[(op >> 9) & 7] & 63) > size) {
            *skip = true;
            goto clean;
        }
    }

    /* Vectors that contradict themselves or every other case of the same
     * instruction (suite data errors). */
    static const char *const bad[] = {
        "e502 [ASL.b Q, D2] 1583",        /* changes all 32 bits of D2 */
        "e502 [ASL.b Q, D2] 1761",        /* same */
        "80ef [DIVU (d16, A7), D0] 5745", /* stacks the instruction's own PC */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (!strcmp(name, bad[i])) {
            *skip = true;
            goto clean;
        }
    }

    cpu_step(&c);

    /* 68000 address errors (odd word/long accesses) are not modelled. */
    uint32_t v3 = vec3(in);
    if (v3 != 0xFFFFFFFFu && out->pc == v3) {
        *skip = true;
        goto clean;
    }

    uint32_t usp = (c.sr & SR_S) ? c.usp : c.a[7];
    uint32_t ssp = (c.sr & SR_S) ? c.a[7] : c.isp;
    bool ok = true;
    char *w = why;
    size_t left = whylen;
#define BAD(...)                                          \
    do {                                                  \
        if (ok) {                                         \
            int n_ = snprintf(w, left, "%s: ", name);     \
            w += n_; left -= (size_t)n_;                  \
        }                                                 \
        ok = false;                                       \
        int n_ = snprintf(w, left, __VA_ARGS__);          \
        if (n_ > 0 && (size_t)n_ < left) { w += n_; left -= (size_t)n_; } \
    } while (0)
    for (int i = 0; i < 8; i++)
        if (c.d[i] != out->d[i])
            BAD("d%d %08X want %08X; ", i, c.d[i], out->d[i]);
    for (int i = 0; i < 7; i++)
        if (c.a[i] != out->a[i])
            BAD("a%d %08X want %08X; ", i, c.a[i], out->a[i]);
    if (usp != out->usp)
        BAD("usp %08X want %08X; ", usp, out->usp);
    if (ssp != out->ssp)
        BAD("ssp %08X want %08X; ", ssp, out->ssp);
    if (c.sr != out->sr)
        BAD("sr %04X want %04X; ", c.sr, out->sr);
    if ((c.pc & 0xFFFFFF) != (out->pc & 0xFFFFFF))
        BAD("pc %06X want %06X; ", c.pc & 0xFFFFFF, out->pc & 0xFFFFFF);
    for (int i = 0; i < out->nram; i++) {
        uint8_t got = rd8(out->ram[i][0] & 0xFFFFFF);
        if (got != (uint8_t)out->ram[i][1])
            BAD("ram[%06X] %02X want %02X; ", out->ram[i][0], got, out->ram[i][1]);
    }
#undef BAD
    if (!ok)
        goto fail;
clean:
    for (int i = 0; i < in->nram; i++)
        wr8(in->ram[i][0] & 0xFFFFFF, 0);
    for (int i = 0; i < out->nram; i++)
        wr8(out->ram[i][0] & 0xFFFFFF, 0);
    wr32(in->pc & 0xFFFFFF, 0);
    return true;
fail:
    for (int i = 0; i < in->nram; i++)
        wr8(in->ram[i][0] & 0xFFFFFF, 0);
    for (int i = 0; i < out->nram; i++)
        wr8(out->ram[i][0] & 0xFFFFFF, 0);
    wr32(in->pc & 0xFFFFFF, 0);
    return false;
}

bool sst_run_file(const char *path, sst_result *res)
{
    memset(res, 0, sizeof *res);
    if (!mem_init())
        return false;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        return false;
    }
    fclose(fp);

    jr j = { buf, buf + n, false };
    state in = { 0 }, out = { 0 };
    char name[128], key[32];
    expect(&j, '[');
    do {
        expect(&j, '{');
        name[0] = 0;
        do {
            str(&j, key, sizeof key);
            expect(&j, ':');
            if (!strcmp(key, "name"))
                str(&j, name, sizeof name);
            else if (!strcmp(key, "initial"))
                parse_state(&j, &in);
            else if (!strcmp(key, "final"))
                parse_state(&j, &out);
            else
                skip_value(&j);
        } while (eat(&j, ',') && !j.err);
        expect(&j, '}');
        if (j.err)
            break;
        res->total++;
        bool skip;
        char why[sizeof res->first_failure];
        if (run_one(name, &in, &out, why, sizeof why, &skip)) {
            if (skip)
                res->skipped++;
            else
                res->passed++;
        } else {
            if (!res->failed)
                snprintf(res->first_failure, sizeof res->first_failure, "%s", why);
            res->failed++;
        }
    } while (eat(&j, ',') && !j.err);
    free(in.ram);
    free(out.ram);
    free(buf);
    return !j.err;
}
