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
} st;

enum {
    S_PNSIZE = 1, S_PNMODE = 2, S_PNPAT = 4, S_BKPAT = 8, S_FILLPAT = 16, S_FONT = 32, S_FACE = 64,
    S_TXMODE = 128, S_SIZE = 256, S_SPEXTRA = 512, S_FG = 1024, S_BK = 2048, S_OV = 4096,
};

static void forget(gaddr pic)
{
    free(st.clip);
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
        qd_port_pattern(port, 1, p);
        write_pat(h, 0x0002, S_BKPAT, st.bk_pat, p);
        break;
    case 4:
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
