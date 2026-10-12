/*
 * Standard File Package.
 *
 * The host shows its own open and save panels.  Dialog hooks, filters and
 * custom items are not run; replies are filled as the classic package would.
 * Without a host every request is cancelled.
 */
#include <limits.h>
#include <stdio.h>
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
} session;

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
enum { ctrlItem = 4, statText = 8, editText = 16, iconItem = 32, itemDisable = 128 };
#define SF_REFCON 0x73746466 /* 'stdf': the main dialog */

static int16_t call_hook(int16_t item)
{
    if (!session.hook)
        return item;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push16(c, (uint16_t)item);
    cpu_push32(c, session.dialog);
    cpu_push32(c, session.data);
    gaddr saved = qd_port();
    tb_SetPort(session.dialog);
    guest_call(session.hook);
    tb_SetPort(saved);
    return (int16_t)cpu_pop16(c);
}

static int session_items(machine_file_item *out, int max)
{
    if (!session.dialog)
        return 0;
    gaddr d = session.dialog;
    gaddr type = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    gaddr text = tb_NewPtr(256, false, true);
    int n = 0, count = (int16_t)rd16(rd32(rd32(d + DialogRecord_items_off))) + 1;
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
        } else if (base == iconItem && hh && rd32(hh)) {
            it->kind = MACHINE_ITEM_ICON;
            gget(it->icon, rd32(hh), 128);
            memset(it->icon + 128, 0xFF, 128); /* plain ICONs have no mask */
            it->has_icon = true;
            wr8(text, 0);
        } else {
            continue;
        }
        if (it->kind != MACHINE_ITEM_ICON)
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

/* Build the app's dialog, hidden, for a custom call with a hook. */
static void begin_session(int16_t dlgid, gaddr hook, gaddr data, bool save)
{
    memset(&session, 0, sizeof session);
    if (!dlgid || !hook)
        return;
    gaddr d = dialog_from_resource(dlgid, 0, 0, 0, SF_REFCON, true);
    if (!d)
        return;
    session.dialog = d;
    session.hook = hook;
    session.data = data;
    session.first_extra = save ? 13 : 10; /* the standard items come first */
    call_hook(sfHookFirstCall);
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
    snprintf(rq.directory, sizeof rq.directory, "%s", last_dir);
    if (!file_fn) {
        LOGF(LOG_UNIMPL, "Standard File: no host, %s cancelled\n", save ? "save" : "open");
        return false;
    }
    if (!file_fn(&rq, path, pathsize, file_ctx) || !path[0])
        return false;
    snprintf(last_dir, sizeof last_dir, "%s", path);
    char *slash = strrchr(last_dir, '/');
    if (slash)
        *slash = 0;
    return true;
}

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
    fill_reply(replyp, ask(false, 0, 0, tl, numt, path, sizeof path), path, false);
}

void tb_StandardPutFile(gaddr prompt, gaddr defaultname, gaddr replyp)
{
    char path[PATH_MAX];
    fill_reply(replyp, ask(true, prompt, defaultname, 0, 0, path, sizeof path), path, true);
}

void tb_CustomGetFile(gaddr filefilter, int16_t numtypes, gaddr typelist, gaddr replyp, int16_t dlgid, uint32_t where,
                      gaddr dlghook, gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    begin_session(dlgid, dlghook, yourdatap, false);
    tb_StandardGetFile(filefilter, numtypes, typelist, replyp);
    end_session();
}

void tb_CustomPutFile(gaddr prompt, gaddr defaultName, gaddr replyp, int16_t dlgid, uint32_t where, gaddr dlghook,
                      gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    begin_session(dlgid, dlghook, yourdatap, true);
    tb_StandardPutFile(prompt, defaultName, replyp);
    end_session();
}

void tb_SFGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    fill_old_reply(rep, ask(false, prompt, 0, tl, numt, path, sizeof path), path, false);
}

void tb_SFPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    fill_old_reply(rep, ask(true, prompt, name, 0, 0, path, sizeof path), path, true);
}

void tb_SFPGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep, int16_t dig,
                   gaddr fp)
{
    tb_SFGetFile(p, prompt, filef, numt, tl, dh, rep);
}

void tb_SFPPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep, int16_t dig, gaddr fp)
{
    tb_SFPutFile(p, prompt, name, dh, rep);
}
