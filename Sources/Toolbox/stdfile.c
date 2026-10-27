/*
 * Standard File Package.
 *
 * The host shows its own open and save panels.  An application's own
 * dialog items are shown in the panel and clicks on them go to its dialog
 * hook (System 7 and the original calling conventions); its file filter
 * decides which files the panel offers.  Replies are filled as the classic
 * package would.  Without a host every request is cancelled.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "forks.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

int16_t fm_wd_for_path(const char *path, gaddr name_out);
uint32_t fm_type_of_path(const char *path);

/* The app's own dialog, hidden, while a custom Standard File call runs:
 * its extra items are shown by the host, and clicks go to its hook. */
static struct {
    gaddr dialog, hook, data;
    int first_extra;
    bool old_hook; /* SFPGetFile/SFPPutFile: DlgHook(item, dialog) */
    gaddr filter_proc; /* the dialog's event filter: ModalFilterYDProc */
} session;

/* The application's file filter for an open panel: FileFilterProc(pb), or
 * with yourDataPtr for CustomGetFile.  True hides the file. */
static struct {
    gaddr proc, data;
    bool yd;
} filter;

/* Or another package's filter (Navigation Services'): true hides. */
static bool (*filter_fn)(const char *path);

static machine_file_fn file_fn;
static void *file_ctx;
static char last_dir[PATH_MAX]; /* where the last panel was left */

void machine_set_file_handler(machine_file_fn fn, void *ctx)
{
    file_fn = fn;
    file_ctx = ctx;
}

uint32_t machine_file_type(const char *path) { return fm_type_of_path(path); }

static void utf8_from_pstr(gaddr p, char *out, size_t outsize)
{
    out[0] = 0;
    if (!p)
        return;
    uint8_t s[256];
    uint32_t n = rd8(p);
    gget(s, p + 1, n);
    macroman_to_utf8(s, n, out, outsize);
}

enum { sfHookFirstCall = -1, sfHookLastCall = -2 };
enum { userItem = 0, ctrlItem = 4, statText = 8, editText = 16, iconItem = 32, itemDisable = 128 };
#define SF_REFCON 0x73746466 /* 'stdf': the main dialog */

static int16_t call_hook(int16_t item)
{
    if (!session.hook)
        return item;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push16(c, (uint16_t)item);
    cpu_push32(c, session.dialog);
    if (!session.old_hook)
        cpu_push32(c, session.data);
    gaddr saved = qd_port();
    tb_SetPort(session.dialog);
    guest_call(session.hook);
    tb_SetPort(saved);
    return (int16_t)cpu_pop16(c);
}

/* The dialog as the program draws it, its user items included: drawn
 * once into an offscreen pixel map (the dialog itself stays hidden), RGBA,
 * with a copy cut out for each user item the host shows. */
static uint8_t *drawn;      /* RGBA, the dialog's port rectangle */
static int drawn_w, drawn_h;
static uint8_t *cutouts[16];

static void draw_offscreen(gaddr d)
{
    free(drawn);
    drawn = NULL;
    qrect pr = qrect_get(d + GrafPort_portRect_off);
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
        return;
    /* A colour port draws into a 32-bit world through its pixmap handle;
     * an old GrafPort (Word 5's Open dialog) has its BitMap in that place,
     * and draws into a 1-bit world's pixels through it. */
    bool color = (rd16(d + 6) & 0xC000) == 0xC000;
    gaddr r = tb_NewPtr(8, false, true), gwp = tb_NewPtr(4, false, true);
    qrect_put(r, pr);
    if (tb_NewGWorld(gwp, color ? 32 : 1, r, 0, 0, 0) || !rd32(gwp)) {
        tb_DisposePtr(r);
        tb_DisposePtr(gwp);
        return;
    }
    gaddr gw = rd32(gwp);
    gaddr pmh = tb_GetGWorldPixMap(gw);
    tb_LockPixels(pmh);
    gaddr pm = rd32(pmh);
    gaddr base = qd_pixmap_base(pm, true);
    uint32_t row = rd16(pm + PixMap_rowBytes_off) & 0x3FFF;
    /* The dialog's port draws into the GWorld's pixels for a moment. */
    uint8_t saved_bits[14];
    gget(saved_bits, d + 2, sizeof saved_bits);
    gaddr saved_vis = rd32(d + 24), saved_clip = rd32(d + 28);
    gaddr vis = tb_NewRgn(), clip = tb_NewRgn();
    tb_RectRgn(vis, r);
    tb_RectRgn(clip, r);
    if (color) {
        wr32(d + 2, pmh);
    } else {
        wr32(d + 2, base);
        wr16(d + 6, (uint16_t)row);
        qrect_put(d + 8, pr);
    }
    wr32(d + 24, vis);
    wr32(d + 28, clip);
    gaddr saved_port = qd_port();
    tb_SetPort(d);
    tb_EraseRect(r);
    tb_DrawDialog(d);
    tb_SetPort(saved_port);
    gput(d + 2, saved_bits, sizeof saved_bits);
    wr32(d + 24, saved_vis);
    wr32(d + 28, saved_clip);
    tb_DisposeRgn(vis);
    tb_DisposeRgn(clip);
    /* To RGBA, white clear: xRGB big-endian, or bits set for black. */
    drawn = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t px;
            if (color)
                px = rd32(base + (uint32_t)y * row + (uint32_t)x * 4);
            else
                px = (rd8(base + (uint32_t)y * row + (uint32_t)x / 8) >> (7 - x % 8)) & 1 ? 0 : 0xFFFFFF;
            uint8_t *o = drawn + ((size_t)y * (size_t)w + (size_t)x) * 4;
            o[0] = (uint8_t)(px >> 16);
            o[1] = (uint8_t)(px >> 8);
            o[2] = (uint8_t)px;
            o[3] = (px & 0xFFFFFF) == 0xFFFFFF ? 0 : 255;
        }
    drawn_w = w;
    drawn_h = h;
    tb_DisposeGWorld(gw);
    tb_DisposePtr(r);
    tb_DisposePtr(gwp);
}

/* A user item's part of the drawing; NULL if it drew nothing there. */
static uint8_t *cutout(int slot, qrect q, qrect port)
{
    free(cutouts[slot]);
    cutouts[slot] = NULL;
    int x0 = q.left - port.left, y0 = q.top - port.top, w = q.right - q.left, h = q.bottom - q.top;
    if (!drawn || w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x0 + w > drawn_w || y0 + h > drawn_h)
        return NULL;
    uint8_t *c = malloc((size_t)w * (size_t)h * 4);
    bool any = false;
    for (int y = 0; y < h; y++) {
        memcpy(c + (size_t)y * (size_t)w * 4, drawn + ((size_t)(y0 + y) * (size_t)drawn_w + (size_t)x0) * 4,
               (size_t)w * 4);
        for (int x = 0; x < w && !any; x++)
            any = c[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] != 0;
    }
    if (!any) {
        free(c);
        return NULL;
    }
    cutouts[slot] = c;
    return c;
}

static int session_items(machine_file_item *out, int max)
{
    if (!session.dialog)
        return 0;
    gaddr d = session.dialog;
    gaddr type = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    gaddr text = tb_NewPtr(256, false, true);
    int n = 0, count = (int16_t)rd16(rd32(rd32(d + DialogRecord_items_off))) + 1;
    /* User items with something to draw (SimpleText's document and
     * stationery icons beside its radio buttons): the dialog is drawn. */
    bool user_items = false;
    for (int i = session.first_extra; i <= count && !user_items; i++) {
        tb_GetDialogItem(d, (int16_t)i, type, h, r);
        user_items = (rd16(type) & 0x7F) == userItem && rd32(h) && qrect_get(r).left < 8192;
    }
    if (user_items)
        draw_offscreen(d);
    qrect port = qrect_get(d + GrafPort_portRect_off);
    for (int i = session.first_extra; i <= count && n < max; i++) {
        tb_GetDialogItem(d, (int16_t)i, type, h, r);
        int t = rd16(type), base = t & 0x7F;
        machine_file_item *it = &out[n];
        memset(it, 0, sizeof *it);
        it->item = (int16_t)i;
        qrect q = qrect_get(r);
        it->visible = q.left < 8192;
        it->rect = (mprect){ q.top, q.left, q.bottom, q.right };
        it->enabled = !(t & itemDisable);
        gaddr hh = rd32(h);
        if (base & ctrlItem) {
            int kind = base & 3;
            if (kind == 3)
                continue; /* resource controls (pop-ups): not shown */
            it->kind = kind == 0 ? MACHINE_ITEM_BUTTON : kind == 1 ? MACHINE_ITEM_CHECKBOX : MACHINE_ITEM_RADIO;
            it->value = tb_GetControlValue(hh);
            tb_GetControlTitle(hh, text);
            gaddr cp = rd32(hh);
            if (rd8(cp + ControlRecord_contrlHilite_off) == 255)
                it->enabled = false;
            if (!rd8(cp + ControlRecord_contrlVis_off))
                it->visible = false;
        } else if (base == statText || base == editText) {
            it->kind = base == statText ? MACHINE_ITEM_TEXT : MACHINE_ITEM_EDIT;
            tb_GetDialogItemText(hh, text);
        } else if (base == userItem && hh && user_items && n < 16) {
            uint8_t *img = cutout(n, q, port);
            if (!img)
                continue;
            it->kind = MACHINE_ITEM_IMAGE;
            it->image = img;
            it->image_width = q.right - q.left;
            it->image_height = q.bottom - q.top;
            wr8(text, 0);
        } else if (base == iconItem && hh && rd32(hh)) {
            it->kind = MACHINE_ITEM_ICON;
            gget(it->icon, rd32(hh), 128);
            memset(it->icon + 128, 0xFF, 128); /* plain ICONs have no mask */
            it->has_icon = true;
            wr8(text, 0);
        } else {
            continue;
        }
        if (it->kind != MACHINE_ITEM_ICON && it->kind != MACHINE_ITEM_IMAGE)
            utf8_from_pstr(text, it->title, sizeof it->title);
        n++;
    }
    tb_DisposePtr(text);
    tb_DisposePtr(r);
    tb_DisposePtr(h);
    tb_DisposePtr(type);
    return n;
}

int machine_file_items(machine_file_item *out, int max) { return session_items(out, max); }

/* While the panel is up: the dialog hook's null event (sfHookNullEvent),
 * for programs that do something periodically meanwhile. */
enum { sfHookNullEvent = 100 };

void machine_file_idle(void)
{
    if (!session.dialog)
        return;
    /* The dialog's filter sees a null event first, as from ModalDialog; an
     * item it answers goes to the hook as a click would. */
    if (session.filter_proc) {
        cpu68k *c = g_cpu;
        gaddr ev = tb_NewPtr(EventRecord_SIZEOF, false, true), item = tb_NewPtr(2, false, true);
        wr32(ev + EventRecord_when_off, rd32(LM_Ticks));
        wr32(ev + EventRecord_where_off, rd32(LM_MouseLocation));
        cpu_push16(c, 0);
        cpu_push32(c, session.dialog);
        cpu_push32(c, ev);
        cpu_push32(c, item);
        cpu_push32(c, session.data);
        gaddr saved = qd_port();
        tb_SetPort(session.dialog);
        guest_call(session.filter_proc);
        tb_SetPort(saved);
        bool handled = (cpu_pop16(c) & 0xFF00) != 0;
        int16_t hit = (int16_t)rd16(item);
        tb_DisposePtr(item);
        tb_DisposePtr(ev);
        if (handled && hit >= session.first_extra) {
            call_hook(hit);
            return;
        }
    }
    call_hook(sfHookNullEvent);
}

int machine_file_item_hit(int16_t item, const char *text)
{
    if (!session.dialog)
        return 0;
    if (text) {
        gaddr h = tb_NewPtr(4, false, true), type = tb_NewPtr(2, false, true), r = tb_NewPtr(8, false, true);
        tb_GetDialogItem(session.dialog, item, type, h, r);
        gaddr s = tb_NewPtr(256, false, true);
        uint8_t mac[256];
        size_t n = utf8_to_macroman(text, mac, 255);
        wr8(s, (uint8_t)n);
        gput(s + 1, mac, (uint32_t)n);
        gaddr saved = qd_port();
        tb_SetPort(session.dialog);
        tb_SetDialogItemText(rd32(h), s);
        tb_SetPort(saved);
        tb_DisposePtr(s);
        tb_DisposePtr(r);
        tb_DisposePtr(type);
        tb_DisposePtr(h);
    }
    int16_t r = call_hook(item);
    return r == 1 || r == 2 ? r : 0;
}

/* The package's own user items (the file list, the volume name, the
 * dividing line) drew through procedures it installed; programs that
 * wrap one call the package's after their own (PrettyC's file list).
 * Here they draw nothing, the host showing those parts. */
static void user_item_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    c->a[7] += 6; /* (theDialog, itemNo) */
    cpu_push32(c, ret);
}

static void standard_user_items(gaddr d, int first_extra)
{
    static gaddr stub;
    if (!stub || native_fn_at(native_index_for(stub)) != user_item_native)
        stub = native_register(user_item_native, "SFUserItem", 0);
    gaddr type = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    int count = (int16_t)rd16(rd32(rd32(d + DialogRecord_items_off))) + 1;
    for (int i = 1; i < first_extra && i <= count; i++) {
        tb_GetDialogItem(d, (int16_t)i, type, h, r);
        if ((rd16(type) & 0x7F) == userItem)
            tb_SetDialogItem(d, (int16_t)i, (int16_t)rd16(type), stub, r);
    }
    tb_DisposePtr(r);
    tb_DisposePtr(h);
    tb_DisposePtr(type);
}

/* Build the app's dialog, hidden, for a custom call with a hook.  Its own
 * items follow the standard dialog's: 9 or 12 in System 7's, 10 in the
 * original ones. */
static void begin_session(int16_t dlgid, gaddr hook, gaddr data, bool save, bool old, gaddr filter_proc)
{
    memset(&session, 0, sizeof session);
    if (!hook && !filter_proc)
        return;
    /* System 7's calls without a dialog of the program's own get the
     * standard one, for the hook and filter to be given. */
    if (!dlgid && old)
        return;
    if (!dlgid)
        dlgid = save ? -6043 : -6042;
    gaddr d = dialog_from_resource(dlgid, 0, 0, 0, SF_REFCON, true);
    if (!d)
        return;
    session.dialog = d;
    session.hook = hook;
    session.data = data;
    session.old_hook = old;
    session.filter_proc = filter_proc;
    session.first_extra = old ? 11 : save ? 13 : 10;
    standard_user_items(d, session.first_extra);
    call_hook(sfHookFirstCall);
}

static void set_filter(gaddr proc, gaddr data, bool yd)
{
    filter.proc = proc;
    filter.data = data;
    filter.yd = yd;
}

/* Asked by the host (with the emulator waiting in the panel call) for each
 * file the panel would show: whether the application's filter hides it.
 * The filter gets the file's catalog information, as from PBGetCatInfo. */
bool machine_file_filtered(const char *path)
{
    if (filter_fn)
        return filter_fn(path);
    if (!filter.proc)
        return false;
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true), spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    gaddr name = tb_NewPtr(256, false, true);
    fm_spec_for_path(path, spec);
    gmemcpy(name, spec + FSSpec_name_off, 64);
    wr32(pb + HFileInfo_ioNamePtr_off, name);
    wr16(pb + HFileInfo_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
    wr32(pb + HFileInfo_ioDirID_off, rd32(spec + FSSpec_parID_off));
    bool hide = false;
    if (tb_PBGetCatInfo(pb, false) == 0) {
        cpu68k *c = g_cpu;
        cpu_push16(c, 0);
        cpu_push32(c, pb);
        if (filter.yd)
            cpu_push32(c, filter.data);
        guest_call(filter.proc);
        hide = (cpu_pop16(c) & 0xFF00) != 0;
    }
    tb_DisposePtr(name);
    tb_DisposePtr(spec);
    tb_DisposePtr(pb);
    return hide;
}

static void end_session(void)
{
    if (!session.dialog)
        return;
    call_hook(sfHookLastCall);
    tb_DisposeDialog(session.dialog);
    memset(&session, 0, sizeof session);
}

/* Ask the host; returns true with a path if the user did not cancel. */
static bool ask(bool save, gaddr prompt, gaddr default_name, gaddr types, int16_t ntypes, char *path, size_t pathsize)
{
    machine_file_request rq;
    memset(&rq, 0, sizeof rq);
    rq.nitems = session_items(rq.items, 16);
    rq.save = save;
    utf8_from_pstr(prompt, rq.prompt, sizeof rq.prompt);
    utf8_from_pstr(default_name, rq.default_name, sizeof rq.default_name);
    for (char *c = rq.default_name; *c; c++)
        if (*c == '/')
            *c = ':'; /* a Mac name's '/' is ':' on the host */
    if (types && ntypes > 0) {
        rq.ntypes = ntypes > 16 ? 16 : ntypes;
        for (int i = 0; i < rq.ntypes; i++)
            rq.types[i] = rd32(types + (uint32_t)i * 4);
    }
    return stdfile_request(&rq, path, pathsize);
}

/* A panel request (Standard File's, or Navigation Services'): it starts
 * where the last one was left unless it says where. */
bool stdfile_request(machine_file_request *rq, char *path, size_t pathsize)
{
    if (!rq->directory[0])
        snprintf(rq->directory, sizeof rq->directory, "%s", last_dir);
    if (!file_fn) {
        LOGF(LOG_UNIMPL, "Standard File: no host, %s cancelled\n", rq->save ? "save" : "open");
        return false;
    }
    if (!file_fn(rq, path, pathsize, file_ctx) || !path[0])
        return false;
    snprintf(last_dir, sizeof last_dir, "%s", path);
    char *slash = strrchr(last_dir, '/');
    if (slash && !rq->folders)
        *slash = 0;
    return true;
}

void stdfile_set_filter(bool (*fn)(const char *path)) { filter_fn = fn; }

/* System 7 reply.  The chosen folder also becomes the default for the next
 * call, as with the classic package (SFSaveDisk, CurDirStore). */
static void fill_reply(gaddr reply, bool good, const char *path, bool save)
{
    gmemset(reply, 0, StandardFileReply_SIZEOF);
    if (!good)
        return;
    gaddr spec = reply + StandardFileReply_sfFile_off;
    struct stat st;
    bool exists = stat(path, &st) == 0;
    fm_grant(path, exists && S_ISDIR(st.st_mode)); /* the user chose it */
    fm_spec_for_path(path, spec);
    wr8(reply + StandardFileReply_sfGood_off, 1);
    wr8(reply + StandardFileReply_sfReplacing_off, save && exists ? 1 : 0);
    wr32(reply + StandardFileReply_sfType_off, exists ? fm_type_of_path(path) : 0);
    wr8(reply + StandardFileReply_sfIsFolder_off, exists && S_ISDIR(st.st_mode) ? 1 : 0);
    wr16(LM_SFSaveDisk, (uint16_t)(-(int16_t)rd16(spec + FSSpec_vRefNum_off)));
    wr32(LM_CurDirStore, rd32(spec + FSSpec_parID_off));
}

/* Old-style reply: a working directory and a name. */
static void fill_old_reply(gaddr reply, bool good, const char *path, bool save)
{
    gmemset(reply, 0, SFReply_SIZEOF);
    if (!good)
        return;
    gaddr name = tb_NewPtr(256, false, true);
    fm_grant(path, false);
    int16_t wd = fm_wd_for_path(path, name);
    uint32_t n = rd8(name) > 63 ? 63 : rd8(name);
    wr8(reply + SFReply_fName_off, (uint8_t)n);
    gmemcpy(reply + SFReply_fName_off + 1, name + 1, n);
    tb_DisposePtr(name);
    struct stat st;
    bool exists = stat(path, &st) == 0;
    wr8(reply + SFReply_good_off, 1);
    wr16(reply + SFReply_vRefNum_off, (uint16_t)wd);
    wr32(reply + SFReply_fType_off, exists ? fm_type_of_path(path) : 0);
    (void)save;
}

void tb_StandardGetFile(gaddr filef, int16_t numt, gaddr tl, gaddr replyp)
{
    char path[PATH_MAX];
    if (!filter.proc)
        set_filter(filef, 0, false);
    fill_reply(replyp, ask(false, 0, 0, tl, numt, path, sizeof path), path, false);
    set_filter(0, 0, false);
}

void tb_StandardPutFile(gaddr prompt, gaddr defaultname, gaddr replyp)
{
    char path[PATH_MAX];
    fill_reply(replyp, ask(true, prompt, defaultname, 0, 0, path, sizeof path), path, true);
}

void tb_CustomGetFile(gaddr filefilter, int16_t numtypes, gaddr typelist, gaddr replyp, int16_t dlgid, uint32_t where,
                      gaddr dlghook, gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    begin_session(dlgid, dlghook, yourdatap, false, false, filterproc);
    set_filter(filefilter, yourdatap, true);
    tb_StandardGetFile(filefilter, numtypes, typelist, replyp);
    end_session();
}

void tb_CustomPutFile(gaddr prompt, gaddr defaultName, gaddr replyp, int16_t dlgid, uint32_t where, gaddr dlghook,
                      gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    begin_session(dlgid, dlghook, yourdatap, true, false, filterproc);
    tb_StandardPutFile(prompt, defaultName, replyp);
    end_session();
}

void tb_SFGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    set_filter(filef, 0, false);
    fill_old_reply(rep, ask(false, prompt, 0, tl, numt, path, sizeof path), path, false);
    set_filter(0, 0, false);
}

void tb_SFPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    fill_old_reply(rep, ask(true, prompt, name, 0, 0, path, sizeof path), path, true);
}

void tb_SFPGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep, int16_t dig,
                   gaddr fp)
{
    begin_session(dig, dh, 0, false, true, 0);
    tb_SFGetFile(p, prompt, filef, numt, tl, dh, rep);
    end_session();
}

/* QuickTime's open dialogs with a preview pane: the same dialogs. */
void tb_SFGetFilePreview(int32_t where, int32_t prompt, int32_t filef, int16_t numt, int32_t tl, int32_t dh, gaddr rep)
{
    tb_SFGetFile((uint32_t)where, (gaddr)prompt, (gaddr)filef, numt, (gaddr)tl, (gaddr)dh, rep);
}

void tb_SFPGetFilePreview(int32_t where, int32_t prompt, int32_t filef, int16_t numt, int32_t tl, int32_t dh, gaddr rep,
                          int16_t dig, int32_t fp)
{
    tb_SFPGetFile((uint32_t)where, (gaddr)prompt, (gaddr)filef, numt, (gaddr)tl, (gaddr)dh, rep, dig, (gaddr)fp);
}

void tb_StandardGetFilePreview(int32_t filef, int16_t numt, int32_t tl, gaddr replyp)
{
    tb_StandardGetFile((gaddr)filef, numt, (gaddr)tl, replyp);
}

void tb_CustomGetFilePreview(int32_t filef, int16_t numt, int32_t tl, gaddr replyp, int16_t dlgid, int32_t where,
                             int32_t dlghook, int32_t filterproc, int32_t activeList, int32_t activateproc,
                             gaddr yourdatap)
{
    tb_CustomGetFile((gaddr)filef, numt, (gaddr)tl, replyp, dlgid, (uint32_t)where, (gaddr)dlghook, (gaddr)filterproc,
                     (gaddr)activeList, (gaddr)activateproc, yourdatap);
}

/* A file's preview ('pnot' and its picture) only shows in a preview
 * pane, which our dialogs do not have: nothing is added. */
int16_t tb_MakeFilePreview(int16_t refnum, int32_t progress) { return noErr; }
int16_t tb_AddFilePreview(int16_t refnum, int32_t type, int32_t data) { return noErr; }

void tb_SFPPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep, int16_t dig, gaddr fp)
{
    begin_session(dig, dh, 0, true, true, 0);
    tb_SFPutFile(p, prompt, name, dh, rep);
    end_session();
}
