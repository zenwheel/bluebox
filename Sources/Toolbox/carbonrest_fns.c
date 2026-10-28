/*
 * FontSync (FontSync.h): a font reference describes a font closely enough
 * to tell whether another machine's font is the same one: its names, its
 * technology, its glyphs, encodings, metrics, kerning and layout tables.
 * A reference here is made from a font the Font Manager has (its 'sfnt'
 * tables, or for a bitmap font its family name and style alone); each
 * section is kept as a fingerprint, flagged missing where the font has no
 * such data, and matching compares the sections asked for.  A profile is a
 * file ('fnsp') of references.
 *
 * References flatten to 'FNSr', their version and the sections; a profile
 * file is 'fnsp', its version and its flattened references.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

enum {
    kFNSInvalidReferenceErr = -29580,
    kFNSBadReferenceVersionErr = -29581,
    kFNSInvalidProfileErr = -29582,
    kFNSBadProfileVersionErr = -29583,
    kFNSDuplicateReferenceErr = -29584,
    kFNSMismatchErr = -29585,
    kFNSInsufficientDataErr = -29586,
    kFNSBadFlattenedSizeErr = -29587,
    kFNSNameNotFoundErr = -29589,
};
enum {
    kFNSMatchNames = 1, kFNSMatchTechnology = 2, kFNSMatchGlyphs = 4, kFNSMatchEncodings = 8, kFNSMatchQDMetrics = 0x10,
    kFNSMatchATSUMetrics = 0x20, kFNSMatchKerning = 0x40, kFNSMatchWSLayout = 0x80, kFNSMatchAATLayout = 0x100,
    kFNSMatchPrintEncoding = 0x200, kFNSMissingDataNoMatch = (int)0x80000000,
};
enum { SECTIONS = 10, MAX_NAMES = 64, REF_TAG = 0x464E0000, PROFILE_TAG = 0x46500000, MAX_REFS = 256, MAX_PROFILES = 8 };
#define T(s) fourcc(s)

typedef struct {
    uint16_t code, platform, script, language;
    uint8_t *text;
    uint32_t len;
} fname;

typedef struct {
    bool used;
    uint32_t version;
    uint8_t family[256]; /* Pascal */
    int16_t script;
    uint16_t style;
    uint32_t technology;
    uint32_t hash[SECTIONS]; /* in the order of the match options' bits */
    uint16_t have;           /* a bit per section that has data */
    fname *names;
    int nnames;
} fnsref;

typedef struct {
    bool used, write, dirty;
    char path[1024];
    uint32_t version;
    fnsref *refs;
    int nrefs;
} fnsprofile;

static fnsref refs[MAX_REFS];
static fnsprofile profiles[MAX_PROFILES];

static void ref_clear(fnsref *r)
{
    for (int i = 0; i < r->nnames; i++)
        free(r->names[i].text);
    free(r->names);
    memset(r, 0, sizeof *r);
}

static void ref_copy(fnsref *to, const fnsref *from)
{
    *to = *from;
    to->names = calloc(from->nnames ? (size_t)from->nnames : 1, sizeof *to->names);
    for (int i = 0; i < from->nnames; i++) {
        to->names[i] = from->names[i];
        to->names[i].text = malloc(from->names[i].len ? from->names[i].len : 1);
        memcpy(to->names[i].text, from->names[i].text, from->names[i].len);
    }
}

void rest_fns_reset(void)
{
    for (int i = 0; i < MAX_REFS; i++)
        ref_clear(&refs[i]);
    for (int i = 0; i < MAX_PROFILES; i++) {
        for (int k = 0; k < profiles[i].nrefs; k++)
            ref_clear(&profiles[i].refs[k]);
        free(profiles[i].refs);
        memset(&profiles[i], 0, sizeof profiles[i]);
    }
}

static fnsref *ref_of(uint32_t id)
{
    uint32_t i = id - REF_TAG;
    return i >= 1 && i <= MAX_REFS && refs[i - 1].used ? &refs[i - 1] : NULL;
}

/* A new reference slot holding a copy of r. */
static uint32_t store_ref(const fnsref *r)
{
    for (int i = 0; i < MAX_REFS; i++)
        if (!refs[i].used) {
            ref_copy(&refs[i], r);
            refs[i].used = true;
            return REF_TAG + (uint32_t)i + 1;
        }
    return 0;
}

/* ---- Making a reference from a font ---- */

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The bytes of a font's table (malloc'd; NULL if it has none). */
static uint8_t *font_table(uint32_t font, uint32_t tag, uint32_t *len)
{
    gaddr scratch = tb_NewPtr(8, false, true);
    if (!scratch)
        return NULL;
    uint32_t a[6] = { font, tag, 0, 0, 0, scratch };
    uint8_t *out = NULL;
    if (svc_call_export("FMGetFontTable", a, 6) == 0) {
        uint32_t n = rd32(scratch);
        gaddr buf = tb_NewPtr(n ? (int32_t)n : 1, false, true);
        if (buf) {
            uint32_t b[6] = { font, tag, 0, n, buf, scratch };
            if (svc_call_export("FMGetFontTable", b, 6) == 0) {
                out = malloc(n ? n : 1);
                gget(out, buf, n);
                *len = n;
            }
            tb_DisposePtr(buf);
        }
    }
    tb_DisposePtr(scratch);
    return out;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Hash tables into a section; false if none was there. */
static bool section(uint32_t font, const char *const *tags, int n, uint32_t *hash)
{
    bool any = false;
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        uint32_t len;
        uint8_t *t = font_table(font, fourcc(tags[i]), &len);
        if (t) {
            h = fnv(t, len, h ^ fourcc(tags[i]));
            free(t);
            any = true;
        }
    }
    *hash = h;
    return any;
}

static void read_names(uint32_t font, fnsref *r)
{
    uint32_t len;
    uint8_t *t = font_table(font, T("name"), &len);
    if (!t)
        return;
    if (len >= 6) {
        uint32_t count = be16(t + 2), base = be16(t + 4);
        for (uint32_t i = 0; i < count && i < MAX_NAMES && 6 + 12 * (i + 1) <= len; i++) {
            const uint8_t *e = t + 6 + 12 * i;
            uint32_t n = be16(e + 8), off = base + be16(e + 10);
            if (off + n > len)
                continue;
            r->names = realloc(r->names, (size_t)(r->nnames + 1) * sizeof *r->names);
            fname *f = &r->names[r->nnames++];
            f->platform = be16(e);
            f->script = be16(e + 2);
            f->language = be16(e + 4);
            f->code = be16(e + 6);
            f->len = n;
            f->text = malloc(n ? n : 1);
            memcpy(f->text, t + off, n);
        }
    }
    free(t);
}

static int16_t make_reference(uint32_t font, uint32_t version, fnsref *r)
{
    if (version != 0 && version != 1)
        return kFNSBadReferenceVersionErr;
    gaddr scratch = tb_NewPtr(272, false, true);
    if (!scratch)
        return memFullErr;
    memset(r, 0, sizeof *r);
    r->version = 1;
    uint32_t a[3] = { font, scratch, scratch + 2 };
    if (svc_call_export("FMGetFontFamilyInstanceFromFont", a, 3) != 0) {
        tb_DisposePtr(scratch);
        return kFNSInvalidReferenceErr;
    }
    uint16_t family = rd16(scratch);
    r->style = rd16(scratch + 2);
    uint32_t n[2] = { family, scratch + 16 };
    svc_call_export("FMGetFontFamilyName", n, 2);
    gget(r->family, scratch + 16, (uint32_t)rd8(scratch + 16) + 1);
    r->script = 0;
    uint32_t f[2] = { font, scratch + 8 };
    r->technology = svc_call_export("FMGetFontFormat", f, 2) == 0 ? rd32(scratch + 8) : T("bitm");
    tb_DisposePtr(scratch);
    read_names(font, r);
    /* Names: the names table and the family; the sections of tables */
    uint32_t h = fnv(r->family, (size_t)r->family[0] + 1, 2166136261u);
    for (int i = 0; i < r->nnames; i++)
        h = fnv(r->names[i].text, r->names[i].len, h ^ r->names[i].code);
    r->hash[0] = fnv((const uint8_t *)&r->style, 2, h);
    r->have |= 1;
    r->hash[1] = r->technology;
    r->have |= 2;
    static const char *const glyphs[] = { "maxp", "post" }, *const enc[] = { "cmap" },
                             *const qd[] = { "head", "hhea", "hmtx", "maxp" }, *const atsu[] = { "head", "hhea", "hmtx", "OS/2" },
                             *const kern[] = { "kern" }, *const aat[] = { "mort", "morx", "feat", "prop", "trak", "bsln", "just", "lcar", "opbd" },
                             *const print[] = { "post" };
    if (section(font, glyphs, 1, &r->hash[2]))
        r->have |= 4;
    if (section(font, enc, 1, &r->hash[3]))
        r->have |= 8;
    if (section(font, qd, 4, &r->hash[4]))
        r->have |= 0x10;
    if (section(font, atsu, 4, &r->hash[5]))
        r->have |= 0x20;
    if (section(font, kern, 1, &r->hash[6]))
        r->have |= 0x40;
    /* WorldScript layout lives in a FOND, not an sfnt: none here */
    if (section(font, aat, 9, &r->hash[8]))
        r->have |= 0x100;
    if (section(font, print, 1, &r->hash[9]))
        r->have |= 0x200;
    (void)glyphs;
    return noErr;
}

/* ---- Matching ---- */

static uint32_t failed_options(const fnsref *a, const fnsref *b, uint32_t options)
{
    uint32_t failed = 0;
    bool missing_fails = (options & (uint32_t)kFNSMissingDataNoMatch) != 0;
    for (int i = 0; i < SECTIONS; i++) {
        if (!(options & (1u << i)))
            continue;
        bool ha = a->have >> i & 1, hb = b->have >> i & 1;
        if (ha && hb) {
            if (a->hash[i] != b->hash[i])
                failed |= 1u << i;
        } else if (missing_fails || ha != hb) {
            if (ha != hb || missing_fails)
                failed |= 1u << i;
        }
    }
    return failed;
}

static void fns_FNSMatchDefaultsGet(ppc_cpu *c) { RET(0x3FF); }

/* FNSSysInfoGet(ioInfo): version in; the features and versions out */
static void fns_FNSSysInfoGet(ppc_cpu *c)
{
    gaddr p = A(0);
    if (!p)
        DONE();
    wr32(p + 4, 0);      /* oFeatures */
    wr32(p + 8, 1);      /* oCurRefVersion */
    wr32(p + 12, 1);     /* oMinRefVersion */
    wr32(p + 16, 1);     /* oCurProfileVersion */
    wr32(p + 20, 1);     /* oMinProfileVersion */
    wr16(p + 24, 0x0100); /* oFontSyncVersion */
    DONE();
}

static void fns_FNSEnabled(ppc_cpu *c) { RET(1); }

/* FNSReferenceCreate(font, desiredVersion, &ref) */
static void fns_FNSReferenceCreate(ppc_cpu *c)
{
    if (!A(2))
        RET(kFNSInvalidReferenceErr);
    wr32(A(2), 0);
    fnsref r;
    int16_t e = make_reference(A(0), A(1), &r);
    if (e)
        RET((int32_t)e);
    uint32_t id = store_ref(&r);
    ref_clear(&r);
    if (!id)
        RET(rest_memFullErr);
    wr32(A(2), id);
    RET(noErr);
}

/* FNSReferenceCreateFromFamily(family, style, desiredVersion, &ref, &actualStyle) */
static void fns_FNSReferenceCreateFromFamily(ppc_cpu *c)
{
    if (!A(3))
        RET(kFNSInvalidReferenceErr);
    wr32(A(3), 0);
    gaddr scratch = tb_NewPtr(8, false, true);
    uint32_t a[4] = { A(0), A(1), scratch, scratch + 4 };
    int32_t e = scratch ? (int32_t)svc_call_export("FMGetFontFromFontFamilyInstance", a, 4) : memFullErr;
    if (e) {
        if (scratch)
            tb_DisposePtr(scratch);
        RET(kFNSInvalidReferenceErr);
    }
    uint32_t font = rd32(scratch);
    uint16_t actual = rd16(scratch + 4);
    tb_DisposePtr(scratch);
    fnsref r;
    int16_t err = make_reference(font, A(2), &r);
    if (err)
        RET((int32_t)err);
    uint32_t id = store_ref(&r);
    ref_clear(&r);
    if (!id)
        RET(rest_memFullErr);
    wr32(A(3), id);
    if (A(4))
        wr16(A(4), actual);
    RET(noErr);
}

static void fns_FNSReferenceGetVersion(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(1))
        RET(kFNSInvalidReferenceErr);
    wr32(A(1), r->version);
    RET(noErr);
}

static void fns_FNSReferenceDispose(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r)
        RET(kFNSInvalidReferenceErr);
    ref_clear(r);
    RET(noErr);
}

/* FNSReferenceMatch(ref1, ref2, options, &failedOptions) */
static void fns_FNSReferenceMatch(ppc_cpu *c)
{
    fnsref *a = ref_of(A(0)), *b = ref_of(A(1));
    if (!a || !b)
        RET(kFNSInvalidReferenceErr);
    uint32_t failed = failed_options(a, b, A(2));
    if (A(3))
        wr32(A(3), failed);
    RET(failed ? kFNSMismatchErr : noErr);
}

/* ---- Flattening ---- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static uint8_t *flatten(const fnsref *r, uint32_t *len)
{
    size_t n = 4 + 4 + 256 + 2 + 2 + 4 + 2 + SECTIONS * 4 + 4;
    for (int i = 0; i < r->nnames; i++)
        n += 8 + 2 + r->names[i].len;
    uint8_t *b = calloc(1, n);
    size_t at = 0;
    put_be32(b + at, T("FNSr")), at += 4;
    put_be32(b + at, r->version), at += 4;
    memcpy(b + at, r->family, 256), at += 256;
    b[at++] = (uint8_t)(r->script >> 8), b[at++] = (uint8_t)r->script;
    b[at++] = (uint8_t)(r->style >> 8), b[at++] = (uint8_t)r->style;
    put_be32(b + at, r->technology), at += 4;
    b[at++] = (uint8_t)(r->have >> 8), b[at++] = (uint8_t)r->have;
    for (int i = 0; i < SECTIONS; i++, at += 4)
        put_be32(b + at, r->hash[i]);
    put_be32(b + at, (uint32_t)r->nnames), at += 4;
    for (int i = 0; i < r->nnames; i++) {
        b[at++] = (uint8_t)(r->names[i].code >> 8), b[at++] = (uint8_t)r->names[i].code;
        b[at++] = (uint8_t)(r->names[i].platform >> 8), b[at++] = (uint8_t)r->names[i].platform;
        b[at++] = (uint8_t)(r->names[i].script >> 8), b[at++] = (uint8_t)r->names[i].script;
        b[at++] = (uint8_t)(r->names[i].language >> 8), b[at++] = (uint8_t)r->names[i].language;
        b[at++] = (uint8_t)(r->names[i].len >> 8), b[at++] = (uint8_t)r->names[i].len;
        memcpy(b + at, r->names[i].text, r->names[i].len), at += r->names[i].len;
    }
    *len = (uint32_t)at;
    return b;
}

/* A reference from its flattened form; the error, or noErr. */
static int16_t unflatten(const uint8_t *b, size_t n, fnsref *r)
{
    memset(r, 0, sizeof *r);
    size_t fixed = 4 + 4 + 256 + 2 + 2 + 4 + 2 + SECTIONS * 4 + 4;
    if (n < 8 || be32(b) != T("FNSr"))
        return n < 8 ? kFNSBadFlattenedSizeErr : kFNSInvalidReferenceErr;
    if (be32(b + 4) != 1)
        return kFNSBadReferenceVersionErr;
    if (n < fixed)
        return kFNSBadFlattenedSizeErr;
    size_t at = 8;
    r->version = 1;
    memcpy(r->family, b + at, 256), at += 256;
    r->script = (int16_t)be16(b + at), at += 2;
    r->style = be16(b + at), at += 2;
    r->technology = be32(b + at), at += 4;
    r->have = be16(b + at), at += 2;
    for (int i = 0; i < SECTIONS; i++, at += 4)
        r->hash[i] = be32(b + at);
    uint32_t nn = be32(b + at);
    at += 4;
    if (nn > MAX_NAMES)
        return kFNSInvalidReferenceErr;
    r->names = calloc(nn ? nn : 1, sizeof *r->names);
    for (uint32_t i = 0; i < nn; i++) {
        if (at + 10 > n) {
            ref_clear(r);
            return kFNSBadFlattenedSizeErr;
        }
        fname *f = &r->names[r->nnames++];
        f->code = be16(b + at), f->platform = be16(b + at + 2), f->script = be16(b + at + 4), f->language = be16(b + at + 6);
        f->len = be16(b + at + 8);
        at += 10;
        if (at + f->len > n) {
            ref_clear(r);
            return kFNSBadFlattenedSizeErr;
        }
        f->text = malloc(f->len ? f->len : 1);
        memcpy(f->text, b + at, f->len);
        at += f->len;
    }
    return noErr;
}

static void fns_FNSReferenceFlattenedSize(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(1))
        RET(kFNSInvalidReferenceErr);
    uint32_t n;
    free(flatten(r, &n));
    wr32(A(1), n);
    RET(noErr);
}

/* FNSReferenceFlatten(ref, flatRef, &flattenedSize): size in is the room */
static void fns_FNSReferenceFlatten(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(2))
        RET(kFNSInvalidReferenceErr);
    uint32_t n;
    uint8_t *b = flatten(r, &n);
    uint32_t room = rd32(A(2));
    wr32(A(2), n);
    if (!A(1) || room < n) {
        free(b);
        RET(kFNSBadFlattenedSizeErr);
    }
    gput(A(1), b, n);
    free(b);
    RET(noErr);
}

/* FNSReferenceUnflatten(flatRef, flattenedSize, &ref) */
static void fns_FNSReferenceUnflatten(ppc_cpu *c)
{
    if (!A(0) || !A(2))
        RET(kFNSInvalidReferenceErr);
    wr32(A(2), 0);
    uint8_t *b = malloc(A(1) ? A(1) : 1);
    gget(b, A(0), A(1));
    fnsref r;
    int16_t e = unflatten(b, A(1), &r);
    free(b);
    if (e)
        RET((int32_t)e);
    uint32_t id = store_ref(&r);
    ref_clear(&r);
    if (!id)
        RET(rest_memFullErr);
    wr32(A(2), id);
    RET(noErr);
}

/* ---- What a reference tells ---- */

static void fns_FNSReferenceGetFamilyInfo(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r)
        RET(kFNSInvalidReferenceErr);
    if (A(1))
        gput(A(1), r->family, (uint32_t)r->family[0] + 1);
    if (A(2))
        wr16(A(2), (uint16_t)r->script);
    if (A(3))
        wr16(A(3), r->style);
    RET(noErr);
}

static void fns_FNSReferenceCountNames(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(1))
        RET(kFNSInvalidReferenceErr);
    wr32(A(1), (uint32_t)r->nnames);
    RET(noErr);
}

/* FNSReferenceGetIndName(ref, index (from 1), maxLength, name, &actualLength, &code, &platform, &script, &language) */
static void fns_FNSReferenceGetIndName(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r)
        RET(kFNSInvalidReferenceErr);
    if (A(1) < 1 || A(1) > (uint32_t)r->nnames)
        RET(kFNSNameNotFoundErr);
    fname *f = &r->names[A(1) - 1];
    if (A(4))
        wr32(A(4), f->len);
    if (A(3) && A(2))
        gput(A(3), f->text, f->len < A(2) ? f->len : A(2));
    if (A(5))
        wr32(A(5), f->code);
    if (A(6))
        wr32(A(6), f->platform);
    if (A(7))
        wr32(A(7), f->script);
    if (A(8))
        wr32(A(8), f->language);
    RET(noErr);
}

/* FNSReferenceFindName(ref, code, platform, script, language, maxLength, name, &actual, &index) */
static void fns_FNSReferenceFindName(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r)
        RET(kFNSInvalidReferenceErr);
    for (int i = 0; i < r->nnames; i++) {
        fname *f = &r->names[i];
        if (f->code == A(1) && f->platform == A(2) && f->script == A(3) && f->language == A(4)) {
            if (A(7))
                wr32(A(7), f->len);
            if (A(6) && A(5))
                gput(A(6), f->text, f->len < A(5) ? f->len : A(5));
            if (A(8))
                wr32(A(8), (uint32_t)i + 1);
            RET(noErr);
        }
    }
    RET(kFNSNameNotFoundErr);
}

/* The installed fonts that match: FMFont values, up to the room. */
static int16_t match_fonts(fnsref *want, uint32_t options, uint32_t room, bool families, gaddr out, gaddr count)
{
    gaddr it = tb_NewPtr(64, false, true), font = tb_NewPtr(8, false, true);
    if (!it || !font)
        return memFullErr;
    uint32_t cr[4] = { 0, 0, 0, it };
    uint32_t k = 0;
    if (svc_call_export("FMCreateFontIterator", cr, 4) == 0) {
        for (;;) {
            uint32_t nx[2] = { it, font };
            if (svc_call_export("FMGetNextFont", nx, 2) != 0)
                break;
            fnsref r;
            if (make_reference(rd32(font), 1, &r) != noErr)
                continue;
            bool same = failed_options(want, &r, options) == 0;
            ref_clear(&r);
            if (!same)
                continue;
            if (out && k < room) {
                if (families) {
                    gaddr fi = tb_NewPtr(8, false, true);
                    uint32_t g[3] = { rd32(font), fi, fi + 2 };
                    svc_call_export("FMGetFontFamilyInstanceFromFont", g, 3);
                    wr16(out + 4 * k, rd16(fi));
                    wr16(out + 4 * k + 2, rd16(fi + 2));
                    tb_DisposePtr(fi);
                } else {
                    wr32(out + 4 * k, rd32(font));
                }
            }
            k++;
        }
        uint32_t d[1] = { it };
        svc_call_export("FMDisposeFontIterator", d, 1);
    }
    tb_DisposePtr(it);
    tb_DisposePtr(font);
    if (count)
        wr32(count, out && k > room ? room : k);
    return noErr;
}

/* FNSReferenceMatchFonts(ref, options, outputSize, fonts[], &numMatches) */
static void fns_FNSReferenceMatchFonts(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(4))
        RET(kFNSInvalidReferenceErr);
    RET((int32_t)match_fonts(r, A(1), A(2), false, A(3), A(4)));
}

static void fns_FNSReferenceMatchFamilies(ppc_cpu *c)
{
    fnsref *r = ref_of(A(0));
    if (!r || !A(4))
        RET(kFNSInvalidReferenceErr);
    RET((int32_t)match_fonts(r, A(1), A(2), true, A(3), A(4)));
}

/* ---- Profiles ---- */

static fnsprofile *profile_of(uint32_t id)
{
    uint32_t i = id - PROFILE_TAG;
    return i >= 1 && i <= MAX_PROFILES && profiles[i - 1].used ? &profiles[i - 1] : NULL;
}

static bool save_profile(fnsprofile *p)
{
    size_t cap = 16;
    uint8_t *out = malloc(cap);
    size_t n = 0;
    put_be32(out, T("fnsp")), put_be32(out + 4, p->version), put_be32(out + 8, (uint32_t)p->nrefs);
    n = 12;
    for (int i = 0; i < p->nrefs; i++) {
        uint32_t len;
        uint8_t *f = flatten(&p->refs[i], &len);
        out = realloc(out, n + 4 + len);
        put_be32(out + n, len);
        memcpy(out + n + 4, f, len);
        n += 4 + len;
        free(f);
    }
    FILE *fp = fopen(p->path, "wb");
    bool ok = fp && fwrite(out, 1, n, fp) == n;
    if (fp)
        ok = fclose(fp) == 0 && ok;
    free(out);
    p->dirty = !ok;
    return ok;
}

static int16_t load_profile(fnsprofile *p)
{
    FILE *fp = fopen(p->path, "rb");
    if (!fp)
        return fnfErr;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc(size > 0 ? (size_t)size : 1);
    size_t got = size > 0 ? fread(b, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (got < 12 || be32(b) != T("fnsp")) {
        free(b);
        return kFNSInvalidProfileErr;
    }
    p->version = be32(b + 4);
    if (p->version != 1) {
        free(b);
        return kFNSBadProfileVersionErr;
    }
    uint32_t n = be32(b + 8);
    size_t at = 12;
    p->refs = calloc(n ? n : 1, sizeof *p->refs);
    for (uint32_t i = 0; i < n; i++) {
        if (at + 4 > got) {
            free(b);
            return kFNSInvalidProfileErr;
        }
        uint32_t len = be32(b + at);
        at += 4;
        if (at + len > got || unflatten(b + at, len, &p->refs[p->nrefs]) != noErr) {
            free(b);
            return kFNSInvalidProfileErr;
        }
        p->refs[p->nrefs].used = true;
        p->nrefs++;
        at += len;
    }
    free(b);
    return noErr;
}

static fnsprofile *new_profile(uint32_t *id)
{
    for (int i = 0; i < MAX_PROFILES; i++)
        if (!profiles[i].used) {
            memset(&profiles[i], 0, sizeof profiles[i]);
            profiles[i].used = true;
            *id = PROFILE_TAG + (uint32_t)i + 1;
            return &profiles[i];
        }
    return NULL;
}

static void profile_free(fnsprofile *p)
{
    for (int i = 0; i < p->nrefs; i++)
        ref_clear(&p->refs[i]);
    free(p->refs);
    memset(p, 0, sizeof *p);
}

static bool spec_path(gaddr spec, char *out, size_t size, bool *exists)
{
    int16_t e = fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, out, size);
    if (e != noErr && !(e == fnfErr && out[0]))
        return false;
    *exists = e == noErr && fm_host_exists(out);
    return true;
}

/* FNSProfileCreate(file, creator, estNumRefs, desiredVersion, &profile) */
static void fns_FNSProfileCreate(ppc_cpu *c)
{
    if (!A(0) || !A(4))
        RET(kFNSInvalidProfileErr);
    wr32(A(4), 0);
    if (A(3) != 0 && A(3) != 1)
        RET(kFNSBadProfileVersionErr);
    char path[1024];
    bool exists;
    if (!spec_path(A(0), path, sizeof path, &exists))
        RET(rest_fnfErr);
    if (exists)
        RET(rest_dupFNErr);
    uint32_t id;
    fnsprofile *p = new_profile(&id);
    if (!p)
        RET(rest_memFullErr);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->version = 1;
    p->write = true;
    int16_t e = fm_create_file(path);
    if (!e) {
        uint32_t creator = A(1) ? A(1) : T("fnsp");
        uint8_t finfo[16];
        put_be32(finfo, T("fnsp"));
        put_be32(finfo + 4, creator);
        memset(finfo + 8, 0, 8);
        fm_set_info(path, finfo, NULL, 0, 0);
        e = save_profile(p) ? noErr : ioErr;
    }
    if (e) {
        profile_free(p);
        RET((int32_t)e);
    }
    wr32(A(4), id);
    RET(noErr);
}

/* FNSProfileOpen(file, openForWrite, &profile) */
static void fns_FNSProfileOpen(ppc_cpu *c)
{
    if (!A(0) || !A(2))
        RET(kFNSInvalidProfileErr);
    wr32(A(2), 0);
    char path[1024];
    bool exists;
    if (!spec_path(A(0), path, sizeof path, &exists) || !exists)
        RET(rest_fnfErr);
    uint32_t id;
    fnsprofile *p = new_profile(&id);
    if (!p)
        RET(rest_memFullErr);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->write = BOOL(1);
    int16_t e = load_profile(p);
    if (e) {
        profile_free(p);
        RET((int32_t)e);
    }
    wr32(A(2), id);
    RET(noErr);
}

static void fns_FNSProfileGetVersion(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p || !A(1))
        RET(kFNSInvalidProfileErr);
    wr32(A(1), p->version);
    RET(noErr);
}

static void fns_FNSProfileCompact(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p)
        RET(kFNSInvalidProfileErr);
    RET(p->write && !save_profile(p) ? ioErr : noErr);
}

static void fns_FNSProfileClose(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p)
        RET(kFNSInvalidProfileErr);
    int16_t e = noErr;
    if (p->write && p->dirty && !save_profile(p))
        e = ioErr;
    profile_free(p);
    RET((int32_t)e);
}

static int16_t changed(fnsprofile *p)
{
    if (!p->write)
        return -61; /* wrPermErr */
    p->dirty = true;
    return save_profile(p) ? noErr : ioErr;
}

static void fns_FNSProfileAddReference(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    fnsref *r = ref_of(A(1));
    if (!p)
        RET(kFNSInvalidProfileErr);
    if (!r)
        RET(kFNSInvalidReferenceErr);
    for (int i = 0; i < p->nrefs; i++)
        if (failed_options(&p->refs[i], r, 0x3FF) == 0 && p->refs[i].have == r->have)
            RET(kFNSDuplicateReferenceErr);
    p->refs = realloc(p->refs, (size_t)(p->nrefs + 1) * sizeof *p->refs);
    ref_copy(&p->refs[p->nrefs], r);
    p->nrefs++;
    RET((int32_t)changed(p));
}

static int16_t remove_at(fnsprofile *p, int i)
{
    ref_clear(&p->refs[i]);
    memmove(&p->refs[i], &p->refs[i + 1], (size_t)(p->nrefs - i - 1) * sizeof *p->refs);
    p->nrefs--;
    return changed(p);
}

static void fns_FNSProfileRemoveReference(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    fnsref *r = ref_of(A(1));
    if (!p)
        RET(kFNSInvalidProfileErr);
    if (!r)
        RET(kFNSInvalidReferenceErr);
    for (int i = 0; i < p->nrefs; i++)
        if (failed_options(&p->refs[i], r, 0x3FF) == 0 && p->refs[i].have == r->have)
            RET((int32_t)remove_at(p, i));
    RET(kFNSMismatchErr);
}

static void fns_FNSProfileRemoveIndReference(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p)
        RET(kFNSInvalidProfileErr);
    if (A(1) >= (uint32_t)p->nrefs)
        RET(kFNSMismatchErr);
    RET((int32_t)remove_at(p, (int)A(1)));
}

static void fns_FNSProfileClear(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p)
        RET(kFNSInvalidProfileErr);
    for (int i = 0; i < p->nrefs; i++)
        ref_clear(&p->refs[i]);
    p->nrefs = 0;
    RET((int32_t)changed(p));
}

static void fns_FNSProfileCountReferences(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p || !A(1))
        RET(kFNSInvalidProfileErr);
    wr32(A(1), (uint32_t)p->nrefs);
    RET(noErr);
}

/* FNSProfileGetIndReference(profile, whichReference (from 0), &ref): a reference of its own */
static void fns_FNSProfileGetIndReference(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    if (!p || !A(2))
        RET(kFNSInvalidProfileErr);
    wr32(A(2), 0);
    if (A(1) >= (uint32_t)p->nrefs)
        RET(kFNSMismatchErr);
    uint32_t id = store_ref(&p->refs[A(1)]);
    if (!id)
        RET(rest_memFullErr);
    wr32(A(2), id);
    RET(noErr);
}

/* FNSProfileMatchReference(profile, ref, options, outputSize, indices[], &numMatches) */
static void fns_FNSProfileMatchReference(ppc_cpu *c)
{
    fnsprofile *p = profile_of(A(0));
    fnsref *r = ref_of(A(1));
    if (!p)
        RET(kFNSInvalidProfileErr);
    if (!r || !A(5))
        RET(kFNSInvalidReferenceErr);
    uint32_t k = 0;
    for (int i = 0; i < p->nrefs; i++)
        if (failed_options(&p->refs[i], r, A(2)) == 0) {
            if (A(4) && k < A(3))
                wr32(A(4) + 4 * k, (uint32_t)i);
            k++;
        }
    wr32(A(5), A(4) && k > A(3) ? A(3) : k);
    RET(k ? noErr : kFNSMismatchErr);
}

const ppc_export rest_fns_exports[] = {
    { "FNSEnabled", fns_FNSEnabled },
    { "FNSMatchDefaultsGet", fns_FNSMatchDefaultsGet },
    { "FNSProfileAddReference", fns_FNSProfileAddReference },
    { "FNSProfileClear", fns_FNSProfileClear },
    { "FNSProfileClose", fns_FNSProfileClose },
    { "FNSProfileCompact", fns_FNSProfileCompact },
    { "FNSProfileCountReferences", fns_FNSProfileCountReferences },
    { "FNSProfileCreate", fns_FNSProfileCreate },
    { "FNSProfileGetIndReference", fns_FNSProfileGetIndReference },
    { "FNSProfileGetVersion", fns_FNSProfileGetVersion },
    { "FNSProfileMatchReference", fns_FNSProfileMatchReference },
    { "FNSProfileOpen", fns_FNSProfileOpen },
    { "FNSProfileRemoveIndReference", fns_FNSProfileRemoveIndReference },
    { "FNSProfileRemoveReference", fns_FNSProfileRemoveReference },
    { "FNSReferenceCountNames", fns_FNSReferenceCountNames },
    { "FNSReferenceCreate", fns_FNSReferenceCreate },
    { "FNSReferenceCreateFromFamily", fns_FNSReferenceCreateFromFamily },
    { "FNSReferenceDispose", fns_FNSReferenceDispose },
    { "FNSReferenceFindName", fns_FNSReferenceFindName },
    { "FNSReferenceFlatten", fns_FNSReferenceFlatten },
    { "FNSReferenceFlattenedSize", fns_FNSReferenceFlattenedSize },
    { "FNSReferenceGetFamilyInfo", fns_FNSReferenceGetFamilyInfo },
    { "FNSReferenceGetIndName", fns_FNSReferenceGetIndName },
    { "FNSReferenceGetVersion", fns_FNSReferenceGetVersion },
    { "FNSReferenceMatch", fns_FNSReferenceMatch },
    { "FNSReferenceMatchFamilies", fns_FNSReferenceMatchFamilies },
    { "FNSReferenceMatchFonts", fns_FNSReferenceMatchFonts },
    { "FNSReferenceUnflatten", fns_FNSReferenceUnflatten },
    { "FNSSysInfoGet", fns_FNSSysInfoGet },
};
const unsigned rest_fns_export_count = sizeof rest_fns_exports / sizeof rest_fns_exports[0];
