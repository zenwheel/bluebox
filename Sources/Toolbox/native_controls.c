/*
 * Native controls: with the native appearance, the host renders its own
 * push buttons, checkboxes, radio buttons and scroll bars, and the
 * standard control definitions show those pixels in the control's place.
 * Hit testing and values stay the classic ones.
 */
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

static machine_control_fn control_fn;
static void *control_ctx;

void machine_set_control_renderer(machine_control_fn fn, void *ctx)
{
    control_fn = fn;
    control_ctx = ctx;
}

static bool native_on(void) { return g_appearance == MACHINE_APPEARANCE_NATIVE && control_fn; }

/* Render a request and show it at r in the current port, through its clip. */
static bool show(machine_control_request *rq, qrect r)
{
    rq->width = r.right - r.left;
    rq->height = r.bottom - r.top;
    if (rq->width <= 0 || rq->height <= 0 || rq->width > 4096 || rq->height > 4096)
        return false;
    uint8_t *rgba = calloc((size_t)rq->width * (size_t)rq->height, 4);
    if (!control_fn(rq, rgba, control_ctx)) {
        free(rgba);
        return false;
    }
    gaddr port = qd_port();
    pixdst d = qd_port_pixels(port);
    hrgn clip;
    hrgn_init(&clip);
    qd_port_clip(port, &clip);
    uint32_t white;
    {
        static const uint16_t w[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
        white = qd_rgb_px(d.depth, d.ctab, w);
    }
    for (int y = 0; y < rq->height; y++) {
        int ty = r.top + y, n;
        const int16_t *xs = hrgn_row(&clip, ty, &n);
        if (!xs)
            continue;
        for (int x = 0; x < rq->width; x++) {
            int tx = r.left + x;
            bool in = false;
            for (int k = 0; k < n; k += 2)
                if (tx >= xs[k] && tx < xs[k + 1]) {
                    in = true;
                    break;
                }
            if (!in || tx < d.bounds.left || tx >= d.bounds.right || ty < d.bounds.top || ty >= d.bounds.bottom)
                continue;
            const uint8_t *p = rgba + ((size_t)y * (size_t)rq->width + (size_t)x) * 4;
            /* Composite over white (premultiplied alpha). */
            int a = p[3];
            uint16_t c[3];
            for (int i = 0; i < 3; i++)
                c[i] = (uint16_t)((p[i] + (255 - a)) * 0x101);
            uint32_t v = a ? qd_rgb_px(d.depth, d.ctab, c) : white;
            put_px(&d, tx - d.bounds.left, ty - d.bounds.top, v);
        }
    }
    hrgn_free(&clip);
    free(rgba);
    return true;
}

bool native_draw_button(qrect r, const uint8_t *title, int n, bool pressed, bool disabled)
{
    if (!native_on())
        return false;
    machine_control_request rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = MACHINE_CONTROL_BUTTON;
    rq.pressed = pressed;
    rq.disabled = disabled;
    macroman_to_utf8(title, (size_t)n, rq.title, sizeof rq.title);
    return show(&rq, r);
}

bool native_draw_check(qrect box, bool radio, bool on, bool pressed, bool disabled)
{
    if (!native_on())
        return false;
    machine_control_request rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = radio ? MACHINE_CONTROL_RADIO : MACHINE_CONTROL_CHECKBOX;
    rq.on = on;
    rq.pressed = pressed;
    rq.disabled = disabled;
    qrect r = { (int16_t)(box.top - 1), (int16_t)(box.left - 1), (int16_t)(box.bottom + 2), (int16_t)(box.right + 2) };
    return show(&rq, r);
}

bool native_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, int value, int min, int max)
{
    if (!native_on())
        return false;
    machine_control_request rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = MACHINE_CONTROL_SCROLLBAR;
    rq.vertical = vertical;
    rq.disabled = !enabled;
    rq.pressed_part = hilite;
    int len = vertical ? r.bottom - r.top : r.right - r.left;
    if (active && max > min && len > 0) {
        rq.position = (double)(value - min) / (double)(max - min);
        /* The classic thumb is fixed; show a modest knob. */
        rq.proportion = 48.0 / (double)len;
        if (rq.proportion > 0.5)
            rq.proportion = 0.5;
    }
    return show(&rq, r);
}
