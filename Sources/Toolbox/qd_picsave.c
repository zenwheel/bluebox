/*
 * Recording pictures.
 *
 * Between OpenPicture and ClosePicture the port's picSave holds the
 * picture, and what is drawn in that port is written to it as version 2
 * PICT opcodes, each preceded by the state it depends on (clip, pen,
 * patterns, text settings, colours) when that changed since it was last
 * written.  OpenPicture hides the pen, so nothing appears on the screen
 * meanwhile; drawing calls record first and then check the pen.
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

/* What the picture being written already says. */
static struct {
    gaddr pic;
    bool open_clip; /* clip covers the frame (nothing written, or the frame) */
    uint8_t *clip;
    uint32_t clip_len;
    uint32_t pn_size, ov_size;
    int pn_mode, font, face, tx_mode, size;
    int32_t sp_extra;
    uint8_t pn_pat[8], bk_pat[8], fill_pat[8];
    uint16_t fg[3], bk[3];
    uint32_t fg_old, bk_old;
    unsigned have; /* S_ bits: written at least once */
    uint8_t *pixpat[3]; /* the colour patterns last written (pen, back, fill) */
    uint32_t pixpat_len[3];
} st;

enum {
    S_PNSIZE = 1, S_PNMODE = 2, S_PNPAT = 4, S_BKPAT = 8, S_FILLPAT = 16, S_FONT = 32, S_FACE = 64,
    S_TXMODE = 128, S_SIZE = 256, S_SPEXTRA = 512, S_FG = 1024, S_BK = 2048, S_OV = 4096,
};

static void forget(gaddr pic)
{
    free(st.clip);
    for (int i = 0; i < 3; i++)
        free(st.pixpat[i]);
    memset(&st, 0, sizeof st);
    st.pic = pic;
    st.open_clip = true;
}

gaddr qd_pic_of(gaddr port)
{
    gaddr h = port ? GrafPort_picSave(port) : 0;
    if (!h || !rd32(h))
        return 0;
    if (h != st.pic)
        forget(h);
    return h;
}

static void put(gaddr h, const void *data, uint32_t n)
{
    uint32_t size = (uint32_t)tb_GetHandleSize(h);
    tb_SetHandleSize(h, (int32_t)(size + n));
    if ((uint32_t)tb_GetHandleSize(h) != size + n)
        return;
    gput(rd32(h) + size, data, n);
}

static void put8(gaddr h, int v)
{
    uint8_t b = (uint8_t)v;
    put(h, &b, 1);
}

static void put16(gaddr h, int v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(h, b, 2);
}

static void put32(gaddr h, uint32_t v)
{
    put16(h, (int)(v >> 16));
    put16(h, (int)(v & 0xFFFF));
}

static void put_rect(gaddr h, qrect r)
{
    put16(h, r.top);
    put16(h, r.left);
    put16(h, r.bottom);
    put16(h, r.right);
}

/* Opcodes start on even offsets in version 2 pictures. */
static void op(gaddr h, int code)
{
    if (tb_GetHandleSize(h) & 1)
        put8(h, 0);
    put16(h, code);
}

static qrect pic_frame(gaddr h) { return qrect_get(rd32(h) + 2); }

static void put_handle_data(gaddr h, gaddr data, uint32_t len)
{
    uint8_t *buf = malloc(len ? len : 1);
    gget(buf, rd32(data), len);
    put(h, buf, len);
    free(buf);
}

static void write_clip(gaddr h, gaddr port)
{
    gaddr rh = rd32(port + GrafPort_clipRgn_off);
    if (!rh || !rd32(rh))
        return;
    uint32_t len = rd16(rd32(rh));
    if (len < 10)
        return;
    qrect b = qrect_get(rd32(rh) + 2), f = pic_frame(h);
    bool open = len == 10 && b.top <= f.top && b.left <= f.left && b.bottom >= f.bottom && b.right >= f.right;
    if (open) {
        if (st.open_clip)
            return;
        st.open_clip = true;
        free(st.clip);
        st.clip = NULL;
        op(h, 0x0001);
        put16(h, 10);
        put_rect(h, f);
        return;
    }
    uint8_t *bytes = malloc(len);
    gget(bytes, rd32(rh), len);
    if (!st.open_clip && st.clip_len == len && !memcmp(st.clip, bytes, len)) {
        free(bytes);
        return;
    }
    free(st.clip);
    st.clip = bytes;
    st.clip_len = len;
    st.open_clip = false;
    op(h, 0x0001);
    put(h, bytes, len);
}

static void write_colors(gaddr h, gaddr port)
{
    if (qd_port_is_color(port)) {
        for (int which = 0; which < 2; which++) {
            uint16_t c[3];
            gaddr a = port + (which ? CGrafPort_rgbBkColor_off : CGrafPort_rgbFgColor_off);
            for (int i = 0; i < 3; i++)
                c[i] = rd16(a + (uint32_t)i * 2);
            uint16_t *last = which ? st.bk : st.fg;
            unsigned bit = which ? S_BK : S_FG;
            if ((st.have & bit) && !memcmp(last, c, sizeof c))
                continue;
            memcpy(last, c, sizeof c);
            st.have |= bit;
            op(h, which ? 0x001B : 0x001A);
            for (int i = 0; i < 3; i++)
                put16(h, c[i]);
        }
        return;
    }
    for (int which = 0; which < 2; which++) {
        uint32_t v = rd32(port + (which ? GrafPort_bkColor_off : GrafPort_fgColor_off));
        uint32_t *last = which ? &st.bk_old : &st.fg_old;
        unsigned bit = which ? S_BK : S_FG;
        if ((st.have & bit) && *last == v)
            continue;
        *last = v;
        st.have |= bit;
        op(h, which ? 0x000F : 0x000E);
        put32(h, v);
    }
}

static void write_pat(gaddr h, int code, unsigned bit, uint8_t last[8], const uint8_t pat[8])
{
    if ((st.have & bit) && !memcmp(last, pat, 8))
        return;
    memcpy(last, pat, 8);
    st.have |= bit;
    op(h, code);
    put(h, pat, 8);
}

static void write_word(gaddr h, int code, unsigned bit, int *last, int v)
{
    if ((st.have & bit) && *last == v)
        return;
    *last = v;
    st.have |= bit;
    op(h, code);
    put16(h, v);
}

/* A colour pattern as a picture's PixPat: type 2 with its colour, or
 * type 1 with an 8-bit pixel map of its colours (rows as literal PackBits
 * runs).  Returns the bytes (malloc'd), or NULL to use its black-and-white
 * version (a type 0 pattern, or one too big or with too many colours). */
static uint8_t *pixpat_bytes(gaddr pp, uint32_t *len)
{
    if (!pp || !rd32(pp))
        return NULL;
    int type = (int16_t)rd16(rd32(pp) + PixPat_patType_off);
    int w, hh;
    uint16_t (*rgb)[3];
    if (type == 0 || !qd_pixpat_image(pp, &w, &hh, &rgb))
        return NULL;
    uint8_t pat1[8];
    gget(pat1, rd32(pp) + PixPat_pat1Data_off, 8);
    if (type == 2) {
        uint8_t *b = malloc(16);
        b[0] = 0;
        b[1] = 2;
        memcpy(b + 2, pat1, 8);
        for (int i = 0; i < 3; i++) {
            b[10 + i * 2] = (uint8_t)(rgb[0][i] >> 8);
            b[11 + i * 2] = (uint8_t)rgb[0][i];
        }
        free(rgb);
        *len = 16;
        return b;
    }
    int rb = (w + 1) & ~1;
    if (rb > 128) {
        free(rgb);
        return NULL;
    }
    /* The distinct colours, as the colour table. */
    uint16_t table[256][3];
    int n = 0;
    uint8_t *idx = malloc((size_t)w * (size_t)hh);
    for (int i = 0; i < w * hh; i++) {
        int k = 0;
        while (k < n && memcmp(table[k], rgb[i], 6))
            k++;
        if (k == n) {
            if (n == 256) {
                free(idx);
                free(rgb);
                return NULL;
            }
            memcpy(table[n++], rgb[i], 6);
        }
        idx[i] = (uint8_t)k;
    }
    free(rgb);
    size_t cap = 10 + 4 + 2 + 8 + 2 + 36 + 8 + (size_t)n * 8 + (size_t)hh * ((size_t)rb + 2);
    uint8_t *b = calloc(cap, 1), *o = b;
#define P16(v) (o[0] = (uint8_t)((v) >> 8), o[1] = (uint8_t)(v), o += 2)
#define P32(v) (P16((uint32_t)(v) >> 16), P16((uint32_t)(v) & 0xFFFF))
    P16(1);
    memcpy(o, pat1, 8);
    o += 8;
    P32(0);                              /* baseAddr */
    P16(0x8000 | rb);
    P16(0); P16(0); P16(hh); P16(w);     /* bounds */
    P16(0);                              /* pmVersion */
    P16(0); P32(0);                      /* packType, packSize */
    P32(72u << 16); P32(72u << 16);      /* hRes, vRes */
    P16(0); P16(8); P16(1); P16(8);      /* pixelType, pixelSize, cmpCount, cmpSize */
    P32(0); P32(0); P32(0);              /* planeBytes, pmTable, pmReserved */
    P32(0); P16(0); P16(n - 1);          /* ctSeed, ctFlags, ctSize */
    for (int k = 0; k < n; k++) {
        P16(k);
        P16(table[k][0]);
        P16(table[k][1]);
        P16(table[k][2]);
    }
    for (int y = 0; y < hh; y++) {
        uint8_t *row = o;
        if (rb >= 8) {
            *o++ = (uint8_t)(rb + 1); /* the row's byte count, then one literal run */
            *o++ = (uint8_t)(rb - 1);
        }
        for (int x = 0; x < rb; x++)
            *o++ = x < w ? idx[y * w + x] : 0;
        (void)row;
    }
#undef P16
#undef P32
    free(idx);
    *len = (uint32_t)(o - b);
    return b;
}

/* Write a colour pattern (PnPixPat, BkPixPat, FillPixPat) when it changed;
 * false when the pattern has no colours of its own. */
static bool write_pixpat(gaddr h, int slot, gaddr pp)
{
    static const int codes[3] = { 0x0013, 0x0012, 0x0014 };
    static const unsigned old_bits[3] = { S_PNPAT, S_BKPAT, S_FILLPAT };
    uint32_t len;
    uint8_t *b = pixpat_bytes(pp, &len);
    if (!b) {
        free(st.pixpat[slot]);
        st.pixpat[slot] = NULL;
        return false;
    }
    st.have &= ~old_bits[slot]; /* an old-style pattern after this is written again */
    if (st.pixpat[slot] && st.pixpat_len[slot] == len && !memcmp(st.pixpat[slot], b, len)) {
        free(b);
        return true;
    }
    free(st.pixpat[slot]);
    st.pixpat[slot] = b;
    st.pixpat_len[slot] = len;
    op(h, codes[slot]);
    put(h, b, len);
    return true;
}

static gaddr port_pixpat_of(gaddr port, uint32_t off) { return qd_port_is_color(port) ? rd32(port + off) : 0; }

static void write_pen(gaddr h, gaddr port)
{
    uint32_t size = rd32(port + GrafPort_pnSize_off);
    if (!(st.have & S_PNSIZE) || st.pn_size != size) {
        st.pn_size = size;
        st.have |= S_PNSIZE;
        op(h, 0x0007);
        put32(h, size);
    }
    write_word(h, 0x0008, S_PNMODE, &st.pn_mode, GrafPort_pnMode(port));
    if (write_pixpat(h, 0, port_pixpat_of(port, CGrafPort_pnPixPat_off)))
        return;
    uint8_t p[8];
    qd_port_pattern(port, 0, p);
    write_pat(h, 0x0009, S_PNPAT, st.pn_pat, p);
}

static void write_common(gaddr h, gaddr port)
{
    write_clip(h, port);
    write_colors(h, port);
}

/* Verbs as QuickDraw numbers them: frame, paint, erase, invert, fill. */
void qd_pic_shape(gaddr port, int opbase, int verb, qrect r, int a, int b, gaddr data, const uint8_t fill[8])
{
    gaddr h = qd_pic_of(port);
    if (!h)
        return;
    write_common(h, port);
    uint8_t p[8];
    switch (verb) {
    case 0:
    case 1:
        write_pen(h, port);
        break;
    case 2:
        if (write_pixpat(h, 1, port_pixpat_of(port, CGrafPort_bkPixPat_off)))
            break;
        qd_port_pattern(port, 1, p);
        write_pat(h, 0x0002, S_BKPAT, st.bk_pat, p);
        break;
    case 4:
        if (write_pixpat(h, 2, qd_fill_pixpat))
            break;
        write_pat(h, 0x000A, S_FILLPAT, st.fill_pat, fill);
        break;
    default:
        break;
    }
    if (opbase == 0x40) {
        uint32_t ov = (uint32_t)(uint16_t)b << 16 | (uint16_t)a; /* height, width */
        if (!(st.have & S_OV) || st.ov_size != ov) {
            st.ov_size = ov;
            st.have |= S_OV;
            op(h, 0x000B);
            put32(h, ov);
        }
    }
    op(h, opbase + verb);
    if (opbase == 0x70 || opbase == 0x80) {
        put_handle_data(h, data, rd16(rd32(data)));
        return;
    }
    put_rect(h, r);
    if (opbase == 0x60) {
        put16(h, a);
        put16(h, b);
    }
}

void qd_pic_line(gaddr port, int h0, int v0, int h1, int v1)
{
    gaddr h = qd_pic_of(port);
    if (!h)
        return;
    write_common(h, port);
    write_pen(h, port);
    op(h, 0x0020);
    put16(h, v0);
    put16(h, h0);
    put16(h, v1);
    put16(h, h1);
}

void qd_pic_text(gaddr port, int x, int y, const uint8_t *text, int len)
{
    gaddr h = qd_pic_of(port);
    if (!h || len <= 0)
        return;
    write_common(h, port);
    write_word(h, 0x0003, S_FONT, &st.font, GrafPort_txFont(port));
    int face = rd8(port + GrafPort_txFace_off);
    if (!(st.have & S_FACE) || st.face != face) {
        st.face = face;
        st.have |= S_FACE;
        op(h, 0x0004);
        put8(h, face);
    }
    write_word(h, 0x0005, S_TXMODE, &st.tx_mode, GrafPort_txMode(port));
    write_word(h, 0x000D, S_SIZE, &st.size, GrafPort_txSize(port));
    int32_t sp = (int32_t)rd32(port + GrafPort_spExtra_off);
    if (!(st.have & S_SPEXTRA) || st.sp_extra != sp) {
        st.sp_extra = sp;
        st.have |= S_SPEXTRA;
        op(h, 0x0006);
        put32(h, (uint32_t)sp);
    }
    for (int done = 0; done < len;) {
        int n = len - done > 255 ? 255 : len - done;
        op(h, 0x0028);
        put16(h, y);
        put16(h, x);
        put8(h, n);
        put(h, text + done, (uint32_t)n);
        done += n;
    }
}

void qd_pic_comment(gaddr port, int kind, int size, gaddr data)
{
    gaddr h = qd_pic_of(port);
    if (!h)
        return;
    if (!data || size <= 0) {
        op(h, 0x00A0);
        put16(h, kind);
        return;
    }
    op(h, 0x00A1);
    put16(h, kind);
    put16(h, size);
    put_handle_data(h, data, (uint32_t)size);
}

/* Pixel data for CopyBits, already laid out by the caller from the
 * opcode on (see record_copy_pic in qd_draw.c). */
void qd_pic_bits(gaddr port, int code, const uint8_t *data, uint32_t len)
{
    gaddr h = qd_pic_of(port);
    if (!h)
        return;
    write_common(h, port);
    op(h, code);
    put(h, data, len);
}

gaddr tb_OpenPicture(gaddr r)
{
    gaddr port = qd_port();
    if (!port)
        return 0;
    gaddr h = tb_NewHandle(40, false, true);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    qrect q = qrect_get(r);
    /* size(2) frame(8) VersionOp 0x0011 0x02FF, HeaderOp 0x0C00 + 24 bytes
       (version -1, -1, fixed-point bounds, reserved). */
    wr16(p, 0);
    gmemcpy(p + 2, r, 8);
    wr16(p + 10, 0x0011);
    wr16(p + 12, 0x02FF);
    wr16(p + 14, 0x0C00);
    wr16(p + 16, 0xFFFF);
    wr16(p + 18, 0xFFFF);
    wr32(p + 20, (uint32_t)q.left << 16);
    wr32(p + 24, (uint32_t)q.top << 16);
    wr32(p + 28, (uint32_t)q.right << 16);
    wr32(p + 32, (uint32_t)q.bottom << 16);
    wr32(p + 36, 0);
    GrafPort_set_picSave(port, h);
    forget(h);
    tb_HidePen();
    return h;
}

void tb_ClosePicture(void)
{
    gaddr port = qd_port();
    gaddr h = port ? GrafPort_picSave(port) : 0;
    if (!h)
        return;
    if (rd32(h)) {
        op(h, 0x00FF);
        wr16(rd32(h), (uint16_t)tb_GetHandleSize(h));
    }
    GrafPort_set_picSave(port, 0);
    forget(0);
    tb_ShowPen();
}

/* OpenCPictParams: srcRect, hRes, vRes (Fixed), version (-2), reserved.
 * The extended version 2 header keeps the resolution. */
gaddr tb_OpenCPicture(gaddr params)
{
    gaddr h = tb_OpenPicture(params);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    uint32_t hres = rd32(params + 8), vres = rd32(params + 12);
    wr16(p + 16, 0xFFFE);
    wr16(p + 18, 0);
    wr32(p + 20, hres ? hres : 72u << 16);
    wr32(p + 24, vres ? vres : 72u << 16);
    gmemcpy(p + 28, params, 8);
    wr32(p + 36, 0);
    return h;
}
