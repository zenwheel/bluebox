/*
 * Disk Initialization (Pack2) and the Color Picker (Pack12).
 *
 * There are no disks to initialize.  GetColor asks the host for a colour
 * (a colour panel) and the colour-model conversions are done here.
 */
#include <math.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

/* ---- Pack2 ---- */

void tb_DILoad(void) {}
void tb_DIUnload(void) {}
int16_t tb_DIBadMount(uint32_t pt, int32_t evtmess) { return -59; /* nothing to format: ioErr-like refusal */ }
int16_t tb_DIFormat(int16_t dn) { return -64; /* noDriveErr */ }
int16_t tb_DIVerify(int16_t dn) { return -64; }
int16_t tb_DIZero(int16_t dn, gaddr vname) { return -64; }

/* ---- Pack12 ---- */

static machine_color_fn color_fn;
static void *color_ctx;

void machine_set_color_picker(machine_color_fn fn, void *ctx)
{
    color_fn = fn;
    color_ctx = ctx;
}

int8_t tb_GetColor(uint32_t where, gaddr prompt, gaddr in, gaddr out)
{
    uint16_t a[3] = { rd16(in), rd16(in + 2), rd16(in + 4) }, b[3] = { a[0], a[1], a[2] };
    char text[256];
    pstr_to_c(prompt, text, sizeof text);
    bool ok = color_fn && color_fn(text, a, b, color_ctx);
    if (!ok)
        b[0] = a[0], b[1] = a[1], b[2] = a[2];
    wr16(out, b[0]);
    wr16(out + 2, b[1]);
    wr16(out + 4, b[2]);
    return ok;
}

/* SmallFract: an unsigned 16-bit fraction of one. */
uint16_t tb_Fix2SmallFract(int32_t f) { return f >= 0x10000 ? 0xFFFF : f <= 0 ? 0 : (uint16_t)f; }
int32_t tb_SmallFract2Fix(uint16_t s) { return s; }

static void rgb_in(gaddr p, double c[3])
{
    for (int i = 0; i < 3; i++)
        c[i] = rd16(p + (uint32_t)i * 2) / 65535.0;
}

static void frac_out(gaddr p, const double c[3])
{
    for (int i = 0; i < 3; i++) {
        double v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        wr16(p + (uint32_t)i * 2, (uint16_t)lround(v * 65535.0));
    }
}

void tb_CMY2RGB(gaddr cmy, gaddr rgb)
{
    double c[3];
    rgb_in(cmy, c);
    for (int i = 0; i < 3; i++)
        c[i] = 1 - c[i];
    frac_out(rgb, c);
}

void tb_RGB2CMY(gaddr rgb, gaddr cmy) { tb_CMY2RGB(rgb, cmy); }

void tb_RGB2HSV(gaddr rgb, gaddr hsv)
{
    double c[3], out[3];
    rgb_in(rgb, c);
    double mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2])), d = mx - mn, h = 0;
    if (d > 0) {
        if (mx == c[0])
            h = fmod((c[1] - c[2]) / d, 6.0);
        else if (mx == c[1])
            h = (c[2] - c[0]) / d + 2;
        else
            h = (c[0] - c[1]) / d + 4;
        h /= 6;
        if (h < 0)
            h += 1;
    }
    out[0] = h;
    out[1] = mx > 0 ? d / mx : 0;
    out[2] = mx;
    frac_out(hsv, out);
}

void tb_HSV2RGB(gaddr hsv, gaddr rgb)
{
    double in[3], c[3];
    rgb_in(hsv, in);
    double h = in[0] * 6, s = in[1], v = in[2];
    int k = (int)floor(h) % 6;
    double f = h - floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (k) {
    case 0: c[0] = v; c[1] = t; c[2] = p; break;
    case 1: c[0] = q; c[1] = v; c[2] = p; break;
    case 2: c[0] = p; c[1] = v; c[2] = t; break;
    case 3: c[0] = p; c[1] = q; c[2] = v; break;
    case 4: c[0] = t; c[1] = p; c[2] = v; break;
    default: c[0] = v; c[1] = p; c[2] = q; break;
    }
    frac_out(rgb, c);
}

void tb_RGB2HSL(gaddr rgb, gaddr hsl)
{
    double c[3], out[3];
    rgb_in(rgb, c);
    double mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2])), d = mx - mn, h = 0;
    double l = (mx + mn) / 2;
    if (d > 0) {
        if (mx == c[0])
            h = fmod((c[1] - c[2]) / d, 6.0);
        else if (mx == c[1])
            h = (c[2] - c[0]) / d + 2;
        else
            h = (c[0] - c[1]) / d + 4;
        h /= 6;
        if (h < 0)
            h += 1;
    }
    out[0] = h;
    out[1] = d > 0 ? d / (1 - fabs(2 * l - 1)) : 0;
    out[2] = l;
    frac_out(hsl, out);
}

void tb_HSL2RGB(gaddr hsl, gaddr rgb)
{
    double in[3];
    rgb_in(hsl, in);
    double h = in[0], s = in[1], l = in[2];
    double cc = (1 - fabs(2 * l - 1)) * s, m = l - cc / 2;
    /* As HSV with value l + cc/2 and the matching saturation. */
    double v = m + cc, sv = v > 0 ? cc / v : 0;
    gaddr tmp = tb_NewPtr(6, false, false);
    double hsv[3] = { h, sv, v };
    frac_out(tmp, hsv);
    tb_HSV2RGB(tmp, rgb);
    tb_DisposePtr(tmp);
}
