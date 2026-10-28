/*
 * ShowMovieInformation: QuickTime's movie information dialog, built at run
 * time and shown modally (the program's filter procedure given to
 * ModalDialog): the movie's name and copyright, its duration, natural and
 * current size, data size and rate, and each track's kind and format.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "qd_internal.h"
#include "qt_comp.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"
#include "trap.h"

/* A user data text item ('©nam', '©cpy' ...) as C text. */
static void udata_text(gaddr ud, uint32_t type, char *out, size_t n)
{
    out[0] = 0;
    if (!ud)
        return;
    gaddr h = tb_NewHandle(0, false, false);
    if (tb_GetUserDataText((int32_t)ud, (int32_t)h, (int32_t)type, 1, 0) == noErr) {
        int32_t len = tb_GetHandleSize(h);
        size_t k = len > 0 ? ((size_t)len < n - 1 ? (size_t)len : n - 1) : 0;
        gget(out, rd32(h), (uint32_t)k);
        out[k] = 0;
    }
    tb_DisposeHandle(h);
}

static const char *media_kind(uint32_t t)
{
    switch (t) {
    case 0x76696465: return "Video";
    case 0x736F756E: return "Sound";
    case 0x74657874: return "Text";
    case 0x73707274: return "Sprite";
    case 0x6D757369: return "Music";
    case 0x746D6364: return "Timecode";
    case 0x71747672: return "QuickTime VR";
    case 0x7061736E: return "Panorama";
    case 0x6F626A65: return "Object";
    case 0x6D706567: return "MPEG";
    case 0x7374726D: return "Streaming";
    case 0x6D6F6F76: return "Movie";
    case 0x7477656E: return "Tween";
    }
    return "Other";
}

/* The dialog's items: OK, then a label and a value per line. */
static gaddr items_for(char lines[][2][128], int n, int *height)
{
    *height = 20 + n * 18 + 45;
    uint8_t buf[8192];
    size_t at = 2;
    int count = 0;
    #define ITEM(t, l, b, r, kind, text)                                                                               \
        do {                                                                                                           \
            size_t len = strlen(text) > 120 ? 120 : strlen(text);                                                      \
            memset(buf + at, 0, 4);                                                                                    \
            int16_t rc[4] = { (int16_t)(t), (int16_t)(l), (int16_t)(b), (int16_t)(r) };                                \
            for (int q = 0; q < 4; q++) {                                                                              \
                buf[at + 4 + q * 2] = (uint8_t)((uint16_t)rc[q] >> 8);                                                 \
                buf[at + 5 + q * 2] = (uint8_t)rc[q];                                                                  \
            }                                                                                                          \
            buf[at + 12] = (uint8_t)(kind);                                                                            \
            buf[at + 13] = (uint8_t)len;                                                                               \
            memcpy(buf + at + 14, text, len);                                                                          \
            at += 14 + len + (len & 1);                                                                                \
            count++;                                                                                                   \
        } while (0)
    ITEM(*height - 30, 300, *height - 10, 370, 4, "OK");
    for (int i = 0; i < n && at < sizeof buf - 300; i++) {
        ITEM(15 + i * 18, 10, 31 + i * 18, 120, 8 | 0x80, lines[i][0]); /* disabled static text */
        ITEM(15 + i * 18, 125, 31 + i * 18, 380, 8 | 0x80, lines[i][1]);
    }
    #undef ITEM
    buf[0] = (uint8_t)((count - 1) >> 8);
    buf[1] = (uint8_t)(count - 1);
    gaddr h = tb_NewHandle((int32_t)at, false, false);
    if (h)
        gput(rd32(h), buf, (uint32_t)at);
    return h;
}

static void clock_text(int64_t value, int32_t scale, char *out, size_t n)
{
    if (scale <= 0)
        scale = 600;
    int64_t hundredths = value * 100 / scale;
    snprintf(out, n, "%d:%02d:%02d.%02d", (int)(hundredths / 360000), (int)(hundredths / 6000 % 60),
             (int)(hundredths / 100 % 60), (int)(hundredths % 100));
}

void tb_ShowMovieInformation(int32_t theMovie, int32_t filterProc, int32_t refCon)
{
    (void)refCon;
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    if (!mv) {
        qt_set_error(invalidMovie);
        return;
    }
    char lines[24][2][128];
    int n = 0;
    #define LINE(label, ...)                                                                                           \
        do {                                                                                                           \
            if (n < 24) {                                                                                              \
                snprintf(lines[n][0], 128, "%s", label);                                                               \
                snprintf(lines[n][1], 128, __VA_ARGS__);                                                               \
                n++;                                                                                                   \
            }                                                                                                          \
        } while (0)
    char text[128];
    udata_text(mv->udata, 0xA96E616D /* '©nam' */, text, sizeof text);
    if (!text[0] && qt_movie_path(mv)) {
        const char *p = strrchr(qt_movie_path(mv), '/');
        snprintf(text, sizeof text, "%s", p ? p + 1 : qt_movie_path(mv));
    }
    LINE("Name:", "%s", text[0] ? text : "Untitled");
    udata_text(mv->udata, 0xA9637079 /* '©cpy' */, text, sizeof text);
    if (text[0])
        LINE("Copyright:", "%s", text);
    char dur[32];
    clock_text(mv->duration, mv->scale, dur, sizeof dur);
    LINE("Duration:", "%s", dur);
    int16_t top = 0, left = 0, bottom = 0, right = 0;
    bool any = false;
    int64_t bytes = 0;
    for (int i = 0; i < mv->ntracks; i++) {
        qtm_track *t = mv->tracks[i];
        if (t->media)
            for (int r = 0; r < t->media->nruns; r++)
                bytes += (int64_t)t->media->runs[r].bytes;
        if (!t->width || !t->height || !(t->flags & 1))
            continue;
        int16_t a, b, c, d;
        qtm_track_bounds(t, &a, &b, &c, &d);
        if (!any || a < top) top = a;
        if (!any || b < left) left = b;
        if (!any || c > bottom) bottom = c;
        if (!any || d > right) right = d;
        any = true;
    }
    LINE("Normal Size:", "%d x %d pixels", right - left, bottom - top);
    gaddr box = tb_NewPtr(8, false, true);
    tb_GetMovieBox((gaddr)theMovie, box);
    LINE("Current Size:", "%d x %d pixels", (int16_t)rd16(box + 6) - (int16_t)rd16(box + 2),
         (int16_t)rd16(box + 4) - (int16_t)rd16(box));
    tb_DisposePtr(box);
    if (bytes >= 1024 * 1024)
        LINE("Data Size:", "%.2f megabytes", (double)bytes / (1024.0 * 1024.0));
    else
        LINE("Data Size:", "%.2f kilobytes", (double)bytes / 1024.0);
    double seconds = mv->scale > 0 ? (double)mv->duration / mv->scale : 0;
    if (seconds > 0)
        LINE("Data Rate:", "%.2f kilobytes per second", (double)bytes / 1024.0 / seconds);
    for (int i = 0; i < mv->ntracks && n < 22; i++) {
        qtm_track *t = mv->tracks[i];
        if (!t->media)
            continue;
        char fmt[64] = "";
        if (t->media->ndescs && t->media->desc_len[0] >= 8) {
            const uint8_t *d = t->media->descs[0];
            if (t->media->type == 0x76696465 && t->media->desc_len[0] >= 86 && d[50] && d[50] < 32) {
                memcpy(fmt, d + 51, d[50]);
                fmt[d[50]] = 0;
            } else {
                snprintf(fmt, sizeof fmt, "'%c%c%c%c'", d[4], d[5], d[6], d[7]);
            }
        }
        char label[24];
        snprintf(label, sizeof label, "Track %d:", i + 1);
        LINE(label, "%s%s%s%s", media_kind(t->media->type), fmt[0] ? " (" : "", fmt, fmt[0] ? ")" : "");
    }
    #undef LINE
    int height;
    gaddr items = items_for(lines, n, &height);
    gaddr r = tb_NewPtr(8, false, true), title = tb_NewPtr(32, false, true);
    qrect_put(r, (qrect){ 60, 60, (int16_t)(60 + height), 60 + 390 });
    gput(title, "\x10" "Movie Info" "\0\0\0\0\0\0", 17);
    wr8(title, 10);
    gaddr dialog = items ? tb_NewDialog(0, r, title, 1, 5 /* movableDBoxProc */, (gaddr)-1, 0, 0, items) : 0;
    tb_DisposePtr(r);
    tb_DisposePtr(title);
    if (!dialog) {
        qt_set_error(memFullErr);
        return;
    }
    tb_SetDialogDefaultItem(dialog, 1);
    gaddr item = tb_NewPtr(2, false, true);
    do
        tb_ModalDialog((gaddr)filterProc, item);
    while ((int16_t)rd16(item) != 1);
    tb_DisposePtr(item);
    tb_DisposeDialog(dialog);
    qt_set_error(noErr);
}

/* ---- CreateMovieControl (QuickTime 6, CarbonLib) ----
 *
 * A user pane control holding a movie controller: the pane's procedures,
 * ours, draw the controller, pass clicks, keys and idle time to it, and
 * GetControlData gives the controller, the movie and whether the program
 * idles it itself. */

enum {
    kMovieControlOptionHideController = 1 << 0, kMovieControlOptionLocateTopLeft = 1 << 1,
    kMovieControlOptionEnableEditing = 1 << 2, kMovieControlOptionHandleEditingHI = 1 << 3,
    kMovieControlOptionSetKeysEnabled = 1 << 4, kMovieControlOptionManuallyIdled = 1 << 5,
    mcTopLeftMovie = 1 << 0, mcNotVisible = 1 << 3,
};

#define MAX_MOVIE_CONTROLS 16
static struct {
    gaddr control, mc, movie;
    bool manual;
} movie_controls[MAX_MOVIE_CONTROLS];

static int movie_control_index(gaddr ch)
{
    for (int i = 0; ch && i < MAX_MOVIE_CONTROLS; i++)
        if (movie_controls[i].control == ch)
            return i;
    return -1;
}

static int32_t mc(gaddr ch, const char *call, const uint32_t *args)
{
    int i = movie_control_index(ch);
    return i >= 0 ? qtc_call_named(movie_controls[i].mc, call, args) : 0;
}

static gaddr owner_of(gaddr ch) { return ControlRecord_contrlOwner(rd32(ch)); }

/* ControlUserPaneDrawProc(control, part) */
static void mcp_draw(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    uint32_t a[] = { owner_of(ch) };
    mc(ch, "MCDraw", a);
    cpu_push32(c, ret);
}

/* ControlUserPaneHitTestProc(control, where): the pane's part */
static void mcp_hit(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    uint32_t pt = cpu_pop32(c);
    gaddr ch = cpu_pop32(c);
    qrect r = qrect_get(rd32(ch) + ControlRecord_contrlRect_off);
    int16_t v = (int16_t)(pt >> 16), h = (int16_t)pt;
    wr16(c->a[7], v >= r.top && v < r.bottom && h >= r.left && h < r.right ? 1 : 0);
    cpu_push32(c, ret);
}

/* ControlUserPaneTrackingProc(control, startPt, actionProc): the click to the controller */
static void mcp_track(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c);
    uint32_t pt = cpu_pop32(c);
    gaddr ch = cpu_pop32(c);
    /* the point is local; MCClick takes it global as events have it */
    gaddr p = tb_NewPtr(4, false, true);
    wr32(p, pt);
    tb_LocalToGlobal(p);
    uint32_t a[] = { owner_of(ch), rd32(p), tb_TickCount(), 0 };
    tb_DisposePtr(p);
    mc(ch, "MCClick", a);
    wr16(c->a[7], 1);
    cpu_push32(c, ret);
}

/* ControlUserPaneIdleProc(control) */
static void mcp_idle(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    gaddr ch = cpu_pop32(c);
    int i = movie_control_index(ch);
    if (i >= 0 && !movie_controls[i].manual)
        mc(ch, "MCIdle", NULL);
    cpu_push32(c, ret);
}

/* ControlUserPaneKeyDownProc(control, keyCode, charCode, modifiers) */
static void mcp_key(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    int16_t mods = (int16_t)cpu_pop16(c);
    int16_t ch_code = (int16_t)cpu_pop16(c);
    cpu_pop16(c);
    gaddr ch = cpu_pop32(c);
    uint32_t a[] = { (uint32_t)(uint8_t)ch_code, (uint32_t)(uint16_t)mods };
    mc(ch, "MCKey", a);
    wr16(c->a[7], 1);
    cpu_push32(c, ret);
}

int16_t tb_CreateMovieControl(int32_t theWindow, gaddr localRect, int32_t theMovie, int32_t options,
                              gaddr returnedControl)
{
    if (returnedControl)
        wr32(returnedControl, 0);
    if (!theWindow || !theMovie || !qt_movie_model((gaddr)theMovie))
        return qt_set_error(paramErr);
    int slot = -1;
    for (int i = 0; i < MAX_MOVIE_CONTROLS; i++)
        if (!movie_controls[i].control) {
            slot = i;
            break;
        }
    if (slot < 0)
        return qt_set_error(memFullErr);
    /* the rectangle: the program's, or the movie's natural size at its corner */
    gaddr r = tb_NewPtr(8, false, true);
    qrect box = localRect ? qrect_get(localRect) : (qrect){ 0, 0, 0, 0 };
    if (!localRect || (options & kMovieControlOptionLocateTopLeft)) {
        gaddr nb = tb_NewPtr(8, false, true);
        tb_GetMovieNaturalBoundsRect((int32_t)theMovie, nb);
        qrect n = qrect_get(nb);
        tb_DisposePtr(nb);
        box.bottom = (int16_t)(box.top + (n.bottom - n.top) + (options & kMovieControlOptionHideController ? 0 : 16));
        box.right = (int16_t)(box.left + (n.right - n.left));
    }
    qrect_put(r, box);
    tb_SetMovieGWorld((gaddr)theMovie, (gaddr)theWindow, 0); /* a window is its port */
    gaddr out = tb_NewPtr(4, false, true);
    int32_t err = tb_CreateUserPaneControl((gaddr)theWindow, r, 0, out);
    gaddr ch = rd32(out);
    gaddr ctl = err ? 0 : tb_NewMovieController((gaddr)theMovie, r,
                                               mcTopLeftMovie | (options & kMovieControlOptionHideController ? mcNotVisible : 0));
    tb_DisposePtr(out);
    tb_DisposePtr(r);
    if (err || !ctl) {
        if (ch && !err)
            tb_DisposeControl(ch);
        return qt_set_error(err ? (int16_t)err : cantOpenHandler);
    }
    movie_controls[slot].control = ch;
    movie_controls[slot].mc = ctl;
    movie_controls[slot].movie = (gaddr)theMovie;
    movie_controls[slot].manual = (options & kMovieControlOptionManuallyIdled) != 0;
    static gaddr procs[5];
    if (!procs[0]) {
        procs[0] = native_register(mcp_draw, "movie control draw", 0);
        procs[1] = native_register(mcp_hit, "movie control hit test", 0);
        procs[2] = native_register(mcp_track, "movie control tracking", 0);
        procs[3] = native_register(mcp_idle, "movie control idle", 0);
        procs[4] = native_register(mcp_key, "movie control key", 0);
    }
    static const uint32_t tags[5] = { 0x64726177, 0x68697474, 0x7472616B, 0x69646C65, 0x6B657964 };
    gaddr v = tb_NewPtr(4, false, true);
    for (int i = 0; i < 5; i++) {
        wr32(v, procs[i]);
        tb_SetControlData(ch, 0, (int32_t)tags[i], 4, v);
    }
    tb_DisposePtr(v);
    if (options & kMovieControlOptionEnableEditing) {
        uint32_t a[] = { 1 };
        qtc_call_named(ctl, "MCEnableEditing", a);
    }
    if (options & kMovieControlOptionSetKeysEnabled) {
        uint32_t a[] = { 76 /* mcActionSetKeysEnabled: the Boolean as the parameter */, 1 };
        qtc_call_named(ctl, "MCDoAction", a);
    }
    wr32(returnedControl, ch);
    return qt_set_error(noErr);
}

/* GetControlData on a movie control: 'mc  ' the controller, 'moov' the
 * movie, 'manu' whether it's idled by the program.  False for others. */
bool qt_movie_control_data(gaddr ch, uint32_t tag, int32_t size, gaddr buffer, gaddr actual, int16_t *err)
{
    int i = movie_control_index(ch);
    if (i < 0)
        return false;
    uint32_t v;
    int32_t n = 4;
    switch (tag) {
    case 0x6D632020: v = movie_controls[i].mc; break;
    case 0x6D6F6F76: v = movie_controls[i].movie; break;
    case 0x6D616E75: v = movie_controls[i].manual; n = 1; break;
    default: return false;
    }
    if (actual)
        wr32(actual, (uint32_t)n);
    *err = noErr;
    if (buffer && size >= n) {
        if (n == 1)
            wr8(buffer, (uint8_t)v);
        else
            wr32(buffer, v);
    } else if (buffer) {
        *err = -30584; /* errDataSizeMismatch */
    }
    return true;
}

void qt_movie_controls_reset(void) { memset(movie_controls, 0, sizeof movie_controls); }
