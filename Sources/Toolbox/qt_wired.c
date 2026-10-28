/*
 * Wired sprites (QuickTime 3 and 4): actions a sprite track's samples
 * attach to events, run as the movie plays.
 *
 * A sprite atom (or the sample's root) holds 'evnt' atoms whose IDs are
 * the events ('clik' mouse down on the sprite, 'cend' its mouse up,
 * 'entr', 'exit' and 'move' as the mouse comes and goes, 'idle' at the
 * track's idle frequency, 'fram' when the sample is shown).  Each holds
 * 'actn' atoms: 'whic' the action, 'parm' 1..n its parameters (data, or an
 * 'expr' to evaluate), 'targ' what it acts on (a sprite, a track, the
 * movie) and 'flag' whether a value is a change from the current one.
 * Expressions are 'oprn' operands (constants, variables, the movie's,
 * tracks' and sprites' properties, the mouse, maths) and 'oper' operators
 * over operands; kActionCase and kActionWhile run 'list's of actions on
 * 'test's.  Values are floats, as QuickTime's were.
 *
 * Not done here: actions on text, Flash, QTVR, music, list and movie
 * tracks, custom handlers, scripts and URLs (logged and passed by).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "qt_native.h"
#include "qt_sprite.h"
#include "qt_wired.h"
#include "tb_internal.h"

static uint32_t four(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint8_t)s[3]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

typedef struct {
    qtm_movie *mv;
    gaddr movie;
    qt_native *native;
    int track;            /* the sprite track's place in the movie */
    qt_sprite *s;
    uint32_t sprite;      /* the sprite whose handler runs (0: the track's) */
    const uint8_t *c;     /* the sample's atoms */
    size_t n;
    double mx, my;        /* the mouse, in the track */
    int depth;
    bool changed;         /* sprites changed: the track is drawn again */
} wctx;

static void run_list(wctx *x, uint32_t parent);
static double eval_expr(wctx *x, uint32_t expr);

/* ---- Atoms ---- */

static const uint8_t *leaf(const wctx *x, uint32_t atom, uint32_t *len)
{
    *len = 0;
    return atom ? qta_data(x->c, x->n, atom, len) : NULL;
}

static uint32_t child(const wctx *x, uint32_t parent, uint32_t type, uint32_t id)
{
    if (!parent)
        return 0;
    return id ? qta_find(x->c, x->n, parent, type, id) : qta_index(x->c, x->n, parent, type, 1, NULL);
}

/* A number from leaf data of a kind: 'h' short, 'l' long, 'f' Fixed, 'b' Boolean, 'F' float. */
static double num(const uint8_t *d, uint32_t len, char kind)
{
    if (!d)
        return 0;
    switch (kind) {
    case 'h': return len >= 2 ? (int16_t)be16(d) : len ? (int8_t)d[0] : 0;
    case 'l': return len >= 4 ? (int32_t)be32(d) : len >= 2 ? (int16_t)be16(d) : 0;
    case 'f': return len >= 4 ? (int32_t)be32(d) / 65536.0 : 0;
    case 'b': return len >= 2 && d[0] == 0 ? d[1] != 0 : len ? d[0] != 0 : 0;
    case 'F':
        if (len >= 8) {
            uint64_t w = (uint64_t)be32(d) << 32 | be32(d + 4);
            double v;
            memcpy(&v, &w, 8);
            return v;
        }
        if (len >= 4) {
            uint32_t w = be32(d);
            float v;
            memcpy(&v, &w, 4);
            return v;
        }
        return 0;
    }
    return 0;
}

/* Parameter index of an action (or an operand): an expression's value, or its data. */
static double param(wctx *x, uint32_t a, uint32_t index, char kind)
{
    uint32_t p = child(x, a, four("parm"), index);
    if (!p)
        return 0;
    uint32_t e = child(x, p, four("expr"), 0);
    if (e)
        return eval_expr(x, e);
    uint32_t len;
    const uint8_t *d = leaf(x, p, &len);
    return num(d, len, kind);
}

static const uint8_t *param_data(wctx *x, uint32_t a, uint32_t index, uint32_t *len)
{
    return leaf(x, child(x, a, four("parm"), index), len);
}

/* ---- Targets ---- */

/* The sprite an action or operand is for: its target's, else the handler's. */
static uint32_t target_sprite(wctx *x, uint32_t a)
{
    uint32_t t = child(x, a, four("targ"), 0);
    if (!t)
        return x->sprite;
    uint32_t len, k;
    const uint8_t *d;
    if ((k = child(x, t, four("spid"), 0))) {
        uint32_t e = child(x, k, four("expr"), 0);
        if (e)
            return (uint32_t)eval_expr(x, e);
        d = leaf(x, k, &len);
        return (uint32_t)num(d, len, 'l');
    }
    if ((k = child(x, t, four("spin"), 0))) {
        uint32_t e = child(x, k, four("expr"), 0);
        d = leaf(x, k, &len);
        int index = e ? (int)eval_expr(x, e) : (int)num(d, len, 'h');
        return qt_sprite_id(x->s, index);
    }
    if ((k = child(x, t, four("spna"), 0))) { /* a sprite named so in the key frame */
        d = leaf(x, k, &len);
        uint32_t klen;
        const uint8_t *key = qt_sprite_key_data(x->s, &klen);
        if (!d || !key)
            return 0;
        size_t nl = d[0] && d[0] < len ? d[0] : strnlen((const char *)d, len);
        const uint8_t *name = d[0] && d[0] < len ? d + 1 : d;
        for (int i = 1; i <= qta_count(key, klen, 0, four("sprt")); i++) {
            uint32_t id, sp = qta_index(key, klen, 0, four("sprt"), i, &id);
            uint32_t na = qta_find(key, klen, sp, four("name"), 1), nn;
            const uint8_t *nd = na ? qta_data(key, klen, na, &nn) : NULL;
            if (nd && nn >= nl && !memcmp(nd, name, nl))
                return id;
        }
        return 0;
    }
    return x->sprite;
}

/* The track an action or operand is for: its target's, else the sprite track. */
static qtm_track *target_track(wctx *x, uint32_t a)
{
    uint32_t t = child(x, a, four("targ"), 0), k, len;
    qtm_movie *mv = x->mv;
    if (t) {
        if ((k = child(x, t, four("trid"), 0)))
            return qtm_track_by_id(mv, (uint32_t)num(leaf(x, k, &len), len, 'l'));
        if ((k = child(x, t, four("trin"), 0))) {
            int i = (int)num(leaf(x, k, &len), len, 'l');
            return i >= 1 && i <= mv->ntracks ? mv->tracks[i - 1] : NULL;
        }
        if ((k = child(x, t, four("trty"), 0))) { /* a type, then an index among that type */
            const uint8_t *d = leaf(x, k, &len);
            uint32_t type = d && len >= 4 ? be32(d) : 0;
            int want = d && len >= 8 ? (int)be32(d + 4) : 1, n = 0;
            for (int i = 0; i < mv->ntracks; i++)
                if (mv->tracks[i]->media && mv->tracks[i]->media->type == type && ++n == want)
                    return mv->tracks[i];
            return NULL;
        }
    }
    return x->track >= 0 && x->track < mv->ntracks ? mv->tracks[x->track] : NULL;
}

/* ---- Sprite properties ---- */

static bool sprite_matrix(wctx *x, uint32_t id, double m[6])
{
    uint8_t b[36];
    uint32_t n;
    if (!qt_sprite_get(x->s, id, 1, b, &n))
        return false;
    m[0] = (int32_t)be32(b) / 65536.0, m[1] = (int32_t)be32(b + 4) / 65536.0;
    m[2] = (int32_t)be32(b + 12) / 65536.0, m[3] = (int32_t)be32(b + 16) / 65536.0;
    m[4] = (int32_t)be32(b + 24) / 65536.0, m[5] = (int32_t)be32(b + 28) / 65536.0;
    return true;
}

static void set_matrix(wctx *x, uint32_t id, const double m[6])
{
    uint8_t b[36] = { 0 };
    put32(b, (uint32_t)(int32_t)lround(m[0] * 65536)), put32(b + 4, (uint32_t)(int32_t)lround(m[1] * 65536));
    put32(b + 12, (uint32_t)(int32_t)lround(m[2] * 65536)), put32(b + 16, (uint32_t)(int32_t)lround(m[3] * 65536));
    put32(b + 24, (uint32_t)(int32_t)lround(m[4] * 65536)), put32(b + 28, (uint32_t)(int32_t)lround(m[5] * 65536));
    put32(b + 32, 0x40000000);
    qt_sprite_set(x->s, id, 1, b, 36);
    x->changed = true;
}

static double sprite_short(wctx *x, uint32_t id, uint32_t prop)
{
    uint8_t b[36];
    uint32_t n;
    return qt_sprite_get(x->s, id, prop, b, &n) && n >= 2 ? (int16_t)be16(b) : 0;
}

static void set_short(wctx *x, uint32_t id, uint32_t prop, double v)
{
    uint8_t b[2];
    put16(b, (uint16_t)(int16_t)lround(v));
    qt_sprite_set(x->s, id, prop, b, 2);
    x->changed = true;
}

/* An action's value with its flags: a change from the current one, a
 * toggle, wrapping between 'minv' and 'maxv'. */
static double flagged(wctx *x, uint32_t a, double v, double current)
{
    uint32_t len;
    const uint8_t *f = leaf(x, child(x, a, four("flag"), 0), &len);
    uint32_t flags = f && len >= 4 ? be32(f) : 0;
    if (flags & (1 << 3))
        return current ? 0 : 1;
    if (!(flags & (1 << 1)))
        return v;
    v += current;
    uint32_t lo = child(x, a, four("minv"), 0), hi = child(x, a, four("maxv"), 0);
    if (lo || hi) {
        double mn = lo ? num(leaf(x, lo, &len), len, len >= 4 ? 'l' : 'h') : -1e30;
        double mx = hi ? num(leaf(x, hi, &len), len, len >= 4 ? 'l' : 'h') : 1e30;
        if (flags & (1 << 2)) {
            if (v > mx)
                v = mn + (v - mx - 1);
            if (v < mn)
                v = mx - (mn - v - 1);
        } else {
            v = v < mn ? mn : v > mx ? mx : v;
        }
    }
    return v;
}

/* ---- Expressions ---- */

static double movie_time(const wctx *x) { return tb_GetMovieTime((int32_t)x->movie, 0); }

static double operand(wctx *x, uint32_t oprn)
{
    uint32_t o = qta_index(x->c, x->n, oprn, 0, 1, NULL);
    if (!o)
        return 0;
    uint32_t type = qta_type(x->c, o), len;
    const uint8_t *d;
    qtm_track *t;
    uint32_t sp;
    int r[4];
    switch (type) {
    case 1: { uint32_t e = child(x, o, four("expr"), 0); return e ? eval_expr(x, e) : 0; } /* kOperandExpression */
    case 2: d = leaf(x, o, &len); return num(d, len, 'F');                                  /* kOperandConstant */
    case 6: return 10000;                         /* kOperandConnectionSpeed: a fast network */
    case 7: case 8: case 9: case 10: case 11: case 12:
    case 13: case 14: case 15: case 16: case 17: case 18: {
        time_t now = time(NULL);
        struct tm tm;
        if (type <= 12)
            gmtime_r(&now, &tm);
        else
            localtime_r(&now, &tm);
        switch ((type - 7) % 6) {
        case 0: return tm.tm_mday;
        case 1: return tm.tm_mon + 1;
        case 2: return tm.tm_year + 1900;
        case 3: return tm.tm_hour;
        case 4: return tm.tm_min;
        default: return tm.tm_sec;
        }
    }
    case 19: case 29: return 0;                   /* not registered for QuickTime Pro */
    case 20: return 1;                            /* kOperandPlatformRunningOn: the Mac OS */
    case 21: return (double)0x04128000u;          /* kOperandQuickTimeVersion */
    case 24: return (double)tb_TickCount();
    case 25: return tb_GetMovieDuration((int32_t)x->movie);
    case 27: return 64.0 * 1024 * 1024;           /* kOperandFreeMemory */
    case 28: return 1;                            /* kOperandNetworkStatus: connected */
    case 30: return 0x0910;                       /* kOperandSystemVersion */
    case 1024: return tb_GetMovieVolume(x->movie);
    case 1025: return (int32_t)tb_GetMovieRate((int32_t)x->movie) / 65536.0;
    case 1026: return x->mv->play_hints & 2 ? 1 : 0;
    case 1027: return x->mv->play_hints & 4 ? 1 : 0;
    case 1028: return movie_time(x);
    case 1029: return tb_GetMovieDuration((int32_t)x->movie);
    case 1030: return x->mv->scale;
    case 1031: case 1032: {
        gaddr box = tb_NewPtr(8, false, true);
        tb_GetMovieBox(x->movie, box);
        double v = type == 1031 ? (int16_t)rd16(box + 6) - (int16_t)rd16(box + 2) : (int16_t)rd16(box + 4) - (int16_t)rd16(box);
        tb_DisposePtr(box);
        return v;
    }
    case 1033: return 100000;                     /* kOperandMovieLoadState: complete */
    case 1034: return x->mv->ntracks;
    case 1035: return 1;
    case 2048: t = target_track(x, o); return t ? t->volume : 0;
    case 2050: t = target_track(x, o); return t ? (t->flags & 1) != 0 : 0;
    case 2051: t = target_track(x, o); return t ? t->layer : 0;
    case 2052: t = target_track(x, o); return t ? t->width / 65536.0 : 0;
    case 2053: t = target_track(x, o); return t ? t->height / 65536.0 : 0;
    case 2054: t = target_track(x, o); return t ? (double)qtm_track_duration(t) : 0;
    case 2056: t = target_track(x, o); return t ? t->id : 0;
    case 2057: { uint32_t *m; return *qt_sprite_idle(x->s, &m); }
    case 3072: case 3073: case 3074: case 3075:   /* bounds */
        sp = target_sprite(x, o);
        if (!qt_sprite_bounds(x->s, sp, r))
            return 0;
        return type == 3072 ? r[1] : type == 3073 ? r[0] : type == 3074 ? r[3] : r[2];
    case 3076: return sprite_short(x, target_sprite(x, o), 100);
    case 3077: return sprite_short(x, target_sprite(x, o), 4);
    case 3078: return sprite_short(x, target_sprite(x, o), 5);
    case 3079: return qt_sprite_var(x->s, (uint32_t)param(x, o, 1, 'l'));
    case 3080: return qt_sprite_count(x->s);
    case 3081: return qt_sprite_image_count(x->s);
    case 3082: return target_sprite(x, o);
    case 3083: return qt_sprite_index(x->s, target_sprite(x, o));
    case 3084: case 3085: case 3086: case 3087: case 3088: case 3089: case 3090: case 3091: {
        sp = target_sprite(x, o);
        double m[6];
        int idx = qt_sprite_index(x->s, sp), w = 0, h = 0;
        double rx = 0, ry = 0;
        uint8_t ib[2];
        uint32_t n2;
        if (!sprite_matrix(x, sp, m) || !idx || !qt_sprite_get(x->s, sp, 100, ib, &n2))
            return 0;
        qt_sprite_image_info(x->s, be16(ib), &w, &h, &rx, &ry);
        int corner = (int)(type - 3084) / 2;
        double px = (corner == 1 || corner == 2 ? w : 0) - rx, py = (corner >= 2 ? h : 0) - ry;
        return (type - 3084) % 2 ? m[1] * px + m[3] * py + m[5] : m[0] * px + m[2] * py + m[4];
    }
    case 3094: return qt_sprite_hit(x->s, param(x, o, 1, 'h'), param(x, o, 2, 'h'), true, false);
    case 5120: return x->mx;
    case 5121: return x->my;
    case 5122: { /* kOperandKeyIsDown: the key code in its parameter */
        gaddr keys = tb_NewPtr(16, false, true);
        tb_GetKeys(keys);
        int code = (int)param(x, o, 2, 'h');
        double down = code >= 0 && code < 128 ? (rd8(keys + (uint32_t)code / 8) >> (code % 8)) & 1 : 0;
        tb_DisposePtr(keys);
        return down;
    }
    case 5123: { /* kOperandRandom: between its two parameters */
        double lo = param(x, o, 1, 'h'), hi = param(x, o, 2, 'h');
        return lo + (double)(rand() % (int)(hi - lo + 1 > 0 ? hi - lo + 1 : 1));
    }
    case 5124: case 5125: return 0;
    case 8192: return sin(param(x, o, 1, 'F'));
    case 8193: return cos(param(x, o, 1, 'F'));
    case 8194: return tan(param(x, o, 1, 'F'));
    case 8195: return atan(param(x, o, 1, 'F'));
    case 8197: return param(x, o, 1, 'F') * M_PI / 180;
    case 8198: return param(x, o, 1, 'F') * 180 / M_PI;
    case 8199: return sqrt(param(x, o, 1, 'F'));
    case 8200: return exp(param(x, o, 1, 'F'));
    case 8201: return log(param(x, o, 1, 'F'));
    }
    LOGF(LOG_UNIMPL, "wired sprites: operand %u not evaluated\n", type);
    return 0;
}

static double eval_expr(wctx *x, uint32_t expr)
{
    if (++x->depth > 64) {
        x->depth--;
        return 0;
    }
    double v = 0;
    uint32_t o = qta_index(x->c, x->n, expr, 0, 1, NULL);
    if (o && qta_type(x->c, o) == four("oprn")) {
        v = operand(x, o);
    } else if (o && qta_type(x->c, o) == four("oper")) {
        uint32_t op = qta_id(x->c, o);
        int count = qta_count(x->c, x->n, o, four("oprn"));
        double a[8] = { 0 };
        for (int i = 0; i < count && i < 8; i++)
            a[i] = operand(x, qta_find(x->c, x->n, o, four("oprn"), (uint32_t)i + 1));
        if (op == four("add ")) {
            for (int i = 0; i < count && i < 8; i++)
                v += a[i];
        } else if (op == four("mult")) {
            v = 1;
            for (int i = 0; i < count && i < 8; i++)
                v *= a[i];
        } else if (op == four("sub ")) v = a[0] - a[1];
        else if (op == four("div ")) v = a[1] ? a[0] / a[1] : 0;
        else if (op == four("idiv")) v = a[1] ? trunc(a[0] / a[1]) : 0;
        else if (op == four("mod ")) v = (int64_t)a[1] ? (double)((int64_t)a[0] % (int64_t)a[1]) : 0;
        else if (op == four("or  ")) v = a[0] || a[1];
        else if (op == four("and ")) v = a[0] && a[1];
        else if (op == four("not ")) v = !a[0];
        else if (op == four("<   ")) v = a[0] < a[1];
        else if (op == four("<=  ")) v = a[0] <= a[1];
        else if (op == four("=   ")) v = a[0] == a[1];
        else if (op == four("!=  ")) v = a[0] != a[1];
        else if (op == four(">   ")) v = a[0] > a[1];
        else if (op == four(">=  ")) v = a[0] >= a[1];
        else if (op == four("abs ")) v = fabs(a[0]);
        else if (op == four("neg ")) v = -a[0];
    }
    x->depth--;
    return v;
}

/* ---- Actions ---- */

static void movie_action(wctx *x, uint32_t which, uint32_t a)
{
    int32_t m = (int32_t)x->movie;
    switch (which) {
    case 1024: tb_SetMovieVolume(m, (int16_t)flagged(x, a, param(x, a, 1, 'h'), tb_GetMovieVolume(x->movie))); break;
    case 1025: tb_SetMovieRate(m, (int32_t)lround(flagged(x, a, param(x, a, 1, 'f'), tb_GetMovieRate(m) / 65536.0) * 65536)); break;
    case 1026: { /* looping flags: loopTimeBase 1, palindromeLoopTimeBase 2 */
        int32_t f = (int32_t)param(x, a, 1, 'l');
        tb_SetMoviePlayHints(m, (f & 1 ? 2 : 0) | (f & 2 ? 4 : 0), 6);
        break;
    }
    case 1027: tb_SetMovieTimeValue(m, (int32_t)flagged(x, a, param(x, a, 1, 'l'), movie_time(x))); break;
    case 1028: { /* to a chapter by name */
        uint32_t len;
        const uint8_t *d = param_data(x, a, 1, &len);
        if (d && len) {
            gaddr name = tb_NewPtr(256, false, true), tr = tb_NewPtr(16, false, true);
            gput(name, d, len > 256 ? 256 : len);
            if (qt_chapter_time(x->mv, name, tr))
                tb_SetMovieTimeValue(m, (int32_t)rd32(tr + 4));
            tb_DisposePtr(name);
            tb_DisposePtr(tr);
        }
        break;
    }
    case 1029: tb_GoToBeginningOfMovie(x->movie); break;
    case 1030: tb_GoToEndOfMovie(m); break;
    case 1031: case 1032: { /* a frame: the movie's time scale over 15 */
        int32_t step = x->mv->scale / 15 > 0 ? x->mv->scale / 15 : 1;
        tb_SetMovieTimeValue(m, (int32_t)movie_time(x) + (which == 1031 ? step : -step));
        break;
    }
    case 1033: {
        int32_t s0 = (int32_t)param(x, a, 1, 'l'), s1 = (int32_t)param(x, a, 2, 'l');
        tb_SetMovieSelection(m, s0, s1 > s0 ? s1 - s0 : 0);
        break;
    }
    case 1035: break; /* play the selection only: our movies play whole */
    case 1036: tb_SetMovieLanguage(m, (int32_t)param(x, a, 1, 'l')); break;
    case 1037: qt_movie_touch(x->mv); break;
    case 1038:
        tb_SetMovieTimeValue(m, (int32_t)param(x, a, 1, 'l'));
        tb_SetMovieRate(m, x->mv->pref_rate ? x->mv->pref_rate : 0x10000);
        break;
    default:
        LOGF(LOG_UNIMPL, "wired sprites: movie action %u passed by\n", which);
    }
}

static void track_action(wctx *x, uint32_t which, uint32_t a)
{
    qtm_track *t = target_track(x, a);
    if (!t)
        return;
    int32_t tt = (int32_t)t->token;
    uint32_t len;
    const uint8_t *d;
    switch (which) {
    case 2048: tb_SetTrackVolume(tt, (int16_t)flagged(x, a, param(x, a, 1, 'h'), t->volume)); break;
    case 2050: tb_SetTrackEnabled(tt, (int8_t)flagged(x, a, param(x, a, 1, 'b'), (t->flags & 1) != 0)); break;
    case 2051:
        if ((d = param_data(x, a, 1, &len)) && len >= 36) {
            gaddr mm = tb_NewPtr(36, false, true);
            gput(mm, d, 36);
            tb_SetTrackMatrix(tt, mm);
            tb_DisposePtr(mm);
        }
        break;
    case 2052: tb_SetTrackLayer(tt, (int16_t)flagged(x, a, param(x, a, 1, 'h'), t->layer)); break;
    case 2056: {
        uint32_t *mark;
        if (x->track >= 0 && t == x->mv->tracks[x->track])
            *qt_sprite_idle(x->s, &mark) = (int32_t)param(x, a, 1, 'l');
        break;
    }
    default:
        LOGF(LOG_UNIMPL, "wired sprites: track action %u passed by\n", which);
    }
    qt_movie_touch(x->mv);
}

static void sprite_action(wctx *x, uint32_t which, uint32_t a)
{
    uint32_t id = target_sprite(x, a), len;
    if (!id || !qt_sprite_index(x->s, id))
        return;
    double m[6];
    const uint8_t *d;
    switch (which) {
    case 3072: /* SetMatrix */
        if ((d = param_data(x, a, 1, &len)) && len >= 36) {
            qt_sprite_set(x->s, id, 1, d, 36);
            x->changed = true;
        }
        break;
    case 3073: set_short(x, id, 100, flagged(x, a, param(x, a, 1, 'h'), sprite_short(x, id, 100))); break;
    case 3074: set_short(x, id, 4, flagged(x, a, param(x, a, 1, 'h'), sprite_short(x, id, 4))); break;
    case 3075: set_short(x, id, 5, flagged(x, a, param(x, a, 1, 'h'), sprite_short(x, id, 5))); break;
    case 3076:
        if ((d = param_data(x, a, 1, &len)) && len >= 10) {
            qt_sprite_set(x->s, id, 6, d, 10);
            x->changed = true;
        }
        break;
    case 3080: /* Translate (x, y, relative) */
        if (sprite_matrix(x, id, m)) {
            double tx = param(x, a, 1, 'f'), ty = param(x, a, 2, 'f');
            if (param(x, a, 3, 'b'))
                m[4] += tx, m[5] += ty;
            else
                m[4] = tx, m[5] = ty;
            set_matrix(x, id, m);
        }
        break;
    case 3081: /* Scale (x, y) */
        if (sprite_matrix(x, id, m)) {
            double sx = param(x, a, 1, 'f'), sy = param(x, a, 2, 'f');
            m[0] *= sx, m[2] *= sx, m[1] *= sy, m[3] *= sy;
            set_matrix(x, id, m);
        }
        break;
    case 3082: /* Rotate (degrees), about the sprite's place */
        if (sprite_matrix(x, id, m)) {
            double r = param(x, a, 1, 'f') * M_PI / 180, c = cos(r), s = sin(r);
            double a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3];
            m[0] = c * a0 - s * b0, m[1] = s * a0 + c * b0;
            m[2] = c * c0 - s * d0, m[3] = s * c0 + c * d0;
            set_matrix(x, id, m);
        }
        break;
    case 3083: { /* Stretch to four corners: the first three's affine map */
        int w = 0, h = 0;
        double rx = 0, ry = 0;
        uint8_t ib[2];
        uint32_t n2;
        if (!qt_sprite_get(x->s, id, 100, ib, &n2) || !qt_sprite_image_info(x->s, be16(ib), &w, &h, &rx, &ry) || !w || !h)
            break;
        double x1 = param(x, a, 1, 'f'), y1 = param(x, a, 2, 'f'), x2 = param(x, a, 3, 'f'), y2 = param(x, a, 4, 'f');
        double x4 = param(x, a, 7, 'f'), y4 = param(x, a, 8, 'f');
        m[0] = (x2 - x1) / w, m[1] = (y2 - y1) / w;
        m[2] = (x4 - x1) / h, m[3] = (y4 - y1) / h;
        m[4] = x1 + m[0] * rx + m[2] * ry, m[5] = y1 + m[1] * rx + m[3] * ry;
        set_matrix(x, id, m);
        break;
    }
    default:
        LOGF(LOG_UNIMPL, "wired sprites: sprite action %u passed by\n", which);
    }
}

/* Case: the first 'test' (in ID order) whose expression holds runs its list. */
static void run_case(wctx *x, uint32_t a)
{
    uint32_t p = child(x, a, four("parm"), 1);
    int n = qta_count(x->c, x->n, p, four("test"));
    for (int i = 1; i <= n; i++) {
        uint32_t t = qta_index(x->c, x->n, p, four("test"), i, NULL);
        uint32_t e = child(x, t, four("expr"), 0);
        if (!e || eval_expr(x, e)) {
            run_list(x, child(x, t, four("list"), 0));
            return;
        }
    }
}

static void run_while(wctx *x, uint32_t a)
{
    uint32_t p = child(x, a, four("parm"), 1);
    uint32_t t = child(x, p, four("test"), 0);
    uint32_t e = child(x, t, four("expr"), 0);
    for (int guard = 0; e && guard < 100000 && eval_expr(x, e); guard++)
        run_list(x, child(x, t, four("list"), 0));
}

static void run_action(wctx *x, uint32_t a)
{
    uint32_t len;
    const uint8_t *w = leaf(x, child(x, a, four("whic"), 0), &len);
    if (!w || len < 4)
        return;
    uint32_t which = be32(w);
    if (which >= 1024 && which < 2048)
        movie_action(x, which, a);
    else if (which >= 2048 && which < 3072)
        track_action(x, which, a);
    else if (which >= 3072 && which < 4096)
        sprite_action(x, which, a);
    else if (which == 6144)
        run_case(x, a);
    else if (which == 6145)
        run_while(x, a);
    else if (which == 7168) /* SetVariable (ID, value) */
        qt_sprite_set_var(x->s, (uint32_t)param(x, a, 1, 'l'),
                          flagged(x, a, param(x, a, 2, 'F'), qt_sprite_var(x->s, (uint32_t)param(x, a, 1, 'l'))));
    else if (which == 6147) { /* SendQTEventToSprite: the target sprite's handlers for an event */
        uint32_t ev = (uint32_t)param(x, a, 2, 'l');
        const uint8_t *d = param_data(x, a, 2, &len);
        if (d && len >= 4)
            ev = be32(d);
        uint32_t target = target_sprite(x, a);
        qt_wired_sprite_event(x->mv, x->movie, x->native, x->track, x->s, target, ev ? ev : four("clik"));
    } else if (which == 6148 || which == 6153 || which == 6146) {
        const uint8_t *d = param_data(x, a, 1, &len);
        LOGF(LOG_UNIMPL, "wired sprites: %s %.*s\n", which == 6146 ? "URL" : "message", d ? (int)len : 0,
             d ? (const char *)d : "");
    } else {
        LOGF(LOG_UNIMPL, "wired sprites: action %u passed by\n", which);
    }
}

static void run_list(wctx *x, uint32_t parent)
{
    if (!parent || ++x->depth > 32) {
        if (parent)
            x->depth--;
        return;
    }
    int n = qta_count(x->c, x->n, parent, four("actn"));
    for (int i = 1; i <= n; i++)
        run_action(x, qta_index(x->c, x->n, parent, four("actn"), i, NULL));
    x->depth--;
}

/* The handlers for an event: the sprite's (in the container), else the root's. */
static bool run_event(wctx *x, uint32_t event)
{
    uint32_t root = qta_root(x->c, x->n), at = 0;
    if (!root)
        return false;
    if (x->sprite) {
        uint32_t sp = qta_find(x->c, x->n, 0, four("sprt"), x->sprite);
        at = sp ? qta_find(x->c, x->n, sp, four("evnt"), event) : 0;
    }
    if (!at && !x->sprite)
        at = qta_find(x->c, x->n, 0, four("evnt"), event);
    if (!at)
        return false;
    run_list(x, at);
    return true;
}

static void finish(wctx *x)
{
    if (x->changed && x->native) {
        qt_native_redraw(x->native, x->track);
        qt_movie_redraw(x->mv);
    }
}

bool qt_wired_sprite_event(qtm_movie *mv, gaddr movie, qt_native *native, int track, qt_sprite *s, uint32_t sprite,
                           uint32_t event)
{
    uint32_t len;
    const uint8_t *key = qt_sprite_key_data(s, &len);
    if (!key)
        return false;
    wctx x = { mv, movie, native, track, s, sprite, key, len, 0, 0, 0, false };
    bool ran = run_event(&x, event);
    finish(&x);
    return ran;
}

/* ---- What the movie gives: clicks, the mouse, idle time ---- */

/* A point of the movie's box in a track's own coordinates. */
static bool track_point(qtm_movie *mv, qtm_track *t, qrect box, int h, int v, double *tx, double *ty)
{
    int16_t top = 0x7FFF, left = 0x7FFF, bottom = -0x8000, right = -0x8000;
    for (int i = 0; i < mv->ntracks; i++) {
        int16_t a, b, c, d;
        if (!mv->tracks[i]->width || !mv->tracks[i]->height)
            continue;
        qtm_track_bounds(mv->tracks[i], &a, &b, &c, &d);
        top = a < top ? a : top, left = b < left ? b : left, bottom = c > bottom ? c : bottom, right = d > right ? d : right;
    }
    int16_t a, b, c, d;
    qtm_track_bounds(t, &a, &b, &c, &d);
    if (right <= left || bottom <= top || c <= a || d <= b || box.right <= box.left || box.bottom <= box.top)
        return false;
    double mx = left + (h - box.left + 0.5) * (right - left) / (box.right - box.left);
    double my = top + (v - box.top + 0.5) * (bottom - top) / (box.bottom - box.top);
    *tx = (mx - b) * (t->width / 65536.0) / (d - b);
    *ty = (my - a) * (t->height / 65536.0) / (c - a);
    return true;
}

bool qt_wired_click(qtm_movie *mv, gaddr movie, qt_native *native, qrect box, int h, int v)
{
    if (!native)
        return false;
    /* the front-most sprite track first: the lowest layer */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < mv->ntracks; i++) {
            qtm_track *t = mv->tracks[i];
            qt_sprite *s = t->media && t->media->type == four("sprt") && (t->flags & 1) ? qt_native_sprite(native, i) : NULL;
            double x, y;
            if (!s || !track_point(mv, t, box, h, v, &x, &y))
                continue;
            uint32_t id = qt_sprite_hit(s, x, y, true, false);
            if (!id)
                continue;
            uint32_t len;
            const uint8_t *key = qt_sprite_key_data(s, &len);
            wctx w = { mv, movie, native, i, s, id, key, len, x, y, 0, false };
            if (!key)
                continue;
            bool ran = run_event(&w, four("clik"));
            ran |= run_event(&w, four("cend"));
            finish(&w);
            if (ran)
                return true;
        }
    return false;
}

void qt_wired_task(qtm_movie *mv, gaddr movie, qt_native *native, gaddr port, qrect box)
{
    if (!native)
        return;
    gaddr pt = 0;
    for (int i = 0; i < mv->ntracks; i++) {
        qtm_track *t = mv->tracks[i];
        if (!t->media || t->media->type != four("sprt"))
            continue;
        qt_sprite *s = qt_native_sprite(native, i);
        if (!s)
            continue;
        /* samples shown: their frame-loaded handlers */
        uint8_t *data;
        uint32_t len;
        while (qt_sprite_take_frame(s, &data, &len)) {
            wctx w = { mv, movie, native, i, s, 0, data, len, 0, 0, 0, false };
            run_event(&w, four("fram"));
            for (int k = 1; k <= qt_sprite_count(s); k++) {
                w.sprite = qt_sprite_id(s, k);
                run_event(&w, four("fram"));
            }
            finish(&w);
            free(data);
        }
        uint32_t klen;
        const uint8_t *key = qt_sprite_key_data(s, &klen);
        if (!key || !(t->flags & 1))
            continue;
        /* the mouse: where it is over the track */
        double x = -1, y = -1;
        if (port) {
            if (!pt)
                pt = tb_NewPtr(8, false, true);
            gaddr saved = pt + 4;
            tb_GetPort(saved);
            tb_SetPort(port);
            tb_GetMouse(pt);
            tb_SetPort(rd32(saved));
            int mh = (int16_t)rd16(pt + 2), mvv = (int16_t)rd16(pt);
            if (mh < box.left || mh >= box.right || mvv < box.top || mvv >= box.bottom ||
                !track_point(mv, t, box, mh, mvv, &x, &y))
                x = y = -1;
        }
        uint32_t over = x >= 0 ? qt_sprite_hit(s, x, y, true, false) : 0;
        uint32_t *hover = qt_sprite_hover(s);
        wctx w = { mv, movie, native, i, s, 0, key, klen, x, y, 0, false };
        if (over != *hover) {
            if (*hover) {
                w.sprite = *hover;
                run_event(&w, four("exit"));
            }
            if (over) {
                w.sprite = over;
                run_event(&w, four("entr"));
            }
            *hover = over;
        } else if (over) {
            w.sprite = over;
            run_event(&w, four("move"));
        }
        /* idle events, every so many ticks */
        uint32_t *mark;
        int32_t freq = *qt_sprite_idle(s, &mark);
        uint32_t now = (uint32_t)tb_TickCount();
        if (freq >= 0 && now - *mark >= (uint32_t)freq) {
            *mark = now;
            for (int k = 1; k <= qt_sprite_count(s); k++) {
                w.sprite = qt_sprite_id(s, k);
                run_event(&w, four("idle"));
            }
        }
        finish(&w);
    }
    if (pt)
        tb_DisposePtr(pt);
}
