/*
 * System 6's startup pieces: a program's own INITs, run at startup when
 * asked, and a cdev shown in a Control Panel window.
 *
 * An INIT file's 'INIT' resources are run in order of ID as the Start
 * Manager ran them: loaded into the system heap, locked, and called with no
 * arguments, their file's resources open while they run.  What an INIT keeps
 * resident (it detaches its own handle) stays; the rest of the resource
 * stays too, since an INIT that patched a trap with its own address and did
 * not detach would otherwise crash.
 *
 * A cdev file runs on its own, as the Control Panel hosted it: the cdev's
 * DITL becomes a dialog, and the cdev code, the first 'cdev' resource, is
 * called with the messages of the cdev protocol: macDev to ask whether it
 * runs on this Mac, initDev, activDev, hitDev for each item hit, updateDev,
 * nulDev while idle, keyEvtDev for keys, deactivDev and closeDev.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "legacy.h"
#include "m68k.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

/* ------------------------------------------------------------------ */
/* INITs */

static char *init_paths[16];
static int ninits;

bool machine_add_init(const char *path)
{
    if (ninits == 16)
        return false;
    init_paths[ninits++] = strdup(path);
    return true;
}

/* How many INIT resources ran in the last call (for tests). */
static int inits_run;
int startup_inits_run(void) { return inits_run; }

static int cmp_id(const void *a, const void *b) { return *(const int16_t *)a - *(const int16_t *)b; }

/* Run one file's INITs.  The program's own resource file stays current. */
int startup_run_init_file(const char *path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return -1;
    if (!mf.rsrc_len) {
        snprintf(err, errlen, "%s has no resource fork", path);
        mac_file_free(&mf);
        return -1;
    }
    int16_t saved_res = tb_CurResFile(); /* opening a file makes it current */
    int16_t rn = rm_open_host_file(path, mf.rsrc, (uint32_t)mf.rsrc_len, true);
    mac_file_free(&mf);
    if (rn < 0) {
        snprintf(err, errlen, "cannot open %s's resource fork", path);
        return -1;
    }
    gaddr saved_zone = rd32(LM_TheZone);
    tb_UseResFile(rn);
    int32_t type = (int32_t)fourcc("INIT");
    int n = tb_Count1Resources(type);
    int16_t ids[256];
    int count = 0;
    gaddr tmp = tb_NewPtr(8, false, true);
    for (int i = 1; i <= n && count < 256; i++) {
        gaddr h = tb_Get1IndResource(type, (int16_t)i);
        if (!h)
            continue;
        tb_GetResInfo(h, tmp, tmp + 2, 0);
        ids[count++] = (int16_t)rd16(tmp);
    }
    tb_DisposePtr(tmp);
    qsort(ids, (size_t)count, sizeof ids[0], cmp_id);
    int ran = 0;
    for (int i = 0; i < count && !g_cpu->halted; i++) {
        wr32(LM_TheZone, rd32(LM_SysZone));
        gaddr h = tb_Get1Resource(type, ids[i]);
        if (!h || !rd32(h)) {
            wr32(LM_TheZone, saved_zone);
            continue;
        }
        tb_MoveHHi(h);
        tb_HLock(h);
        LOGF(LOG_TRAPS, "INIT %d from %s\n", ids[i], path);
        uint32_t d[8], a[8];
        memcpy(d, g_cpu->d, sizeof d);
        memcpy(a, g_cpu->a, sizeof a);
        guest_call_regs(rd32(h));
        memcpy(g_cpu->d, d, sizeof d);
        memcpy(g_cpu->a, a, sizeof a);
        wr32(LM_TheZone, saved_zone);
        ran++;
    }
    tb_UseResFile(saved_res);
    inits_run += ran;
    return ran;
}

/* After the program is loaded and before it runs: the INITs asked for. */
void startup_run_inits(void)
{
    char err[256];
    for (int i = 0; i < ninits; i++)
        if (startup_run_init_file(init_paths[i], err, sizeof err) < 0)
            fprintf(stderr, "bluebox: INIT %s: %s\n", init_paths[i], err);
}

void startup_reset(void)
{
    for (int i = 0; i < ninits; i++)
        free(init_paths[i]);
    ninits = 0;
}

/* ------------------------------------------------------------------ */
/* cdevs */

enum { nullEvent = 0, mouseDown = 1, keyDown = 3, autoKey = 5, updateEvt = 6, activateEvt = 8 };

enum {
    initDev = 0, hitDev, closeDev, nulDev, updateDev, activDev, deactivDev, keyEvtDev, macDev,
    undoDev, cutDev, copyDev, pasteDev, clearDev, cursorDev
};
/* cdevValue is cdevUnset until initDev returns the cdev's storage (or an
 * error), and the cdev takes the value it gets as "nothing yet". */
enum { cdevMemErr = 0, cdevResErr = 1, cdevUnset = 3 };

/* The call: pascal long main(message, item, numItems, CPanelID, theEvent,
 * cdevValue, cpDialog). */
static int32_t cdev_call(gaddr code, int rn, int16_t msg, int16_t item, int16_t nitems, gaddr evt, int32_t value,
                         gaddr dlg)
{
    cpu68k *c = g_cpu;
    int16_t saved = tb_CurResFile();
    if (rn >= 0)
        tb_UseResFile((int16_t)rn);
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    cpu_push32(c, 0); /* the result */
    cpu_push16(c, (uint16_t)msg);
    cpu_push16(c, (uint16_t)item);
    cpu_push16(c, (uint16_t)nitems);
    cpu_push16(c, 1); /* CPanelID */
    cpu_push32(c, evt);
    cpu_push32(c, (uint32_t)value);
    cpu_push32(c, dlg);
    guest_call_regs(code);
    int32_t result = (int32_t)cpu_pop32(c);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    tb_UseResFile(saved);
    return result;
}

/* The rectangle the cdev's items cover (the 'nrct' resource's first, else
 * their union), in the coordinates the items are laid out in. */
static bool items_bounds(gaddr ditl, int16_t *t, int16_t *l, int16_t *b, int16_t *r, int *count)
{
    gaddr p = rd32(ditl);
    int n = (int16_t)rd16(p) + 1;
    p += 2;
    *t = *l = 32767;
    *b = *r = -32768;
    for (int i = 0; i < n; i++) {
        int16_t it = (int16_t)rd16(p + 4), il = (int16_t)rd16(p + 6), ib = (int16_t)rd16(p + 8), ir = (int16_t)rd16(p + 10);
        if (it < *t) *t = it;
        if (il < *l) *l = il;
        if (ib > *b) *b = ib;
        if (ir > *r) *r = ir;
        int len = rd8(p + 13);
        p += 14 + ((len + 1) & ~1);
    }
    *count = n;
    return n > 0;
}

static bool cdev_native_running;

static void cdev_host_native(cpu68k *c, uint16_t tw)
{
    char title[64];
    /* An A5 world of its own for QuickDraw's globals, which sit below A5. */
    gaddr world = tb_NewPtr(1024, false, true);
    gaddr a5 = world + 768;
    wr32(LM_CurrentA5, a5);
    c->a[5] = a5;
    tb_InitGraf(a5 - 4);
    tb_InitFonts();
    tb_FlushEvents(-1, 0);
    tb_InitWindows();
    tb_InitMenus();
    tb_TEInit();
    tb_InitDialogs(0);
    tb_InitCursor();

    int16_t rn = (int16_t)rd16(LM_CurApRefNum);
    tb_UseResFile(rn);
    int32_t ctype = (int32_t)fourcc("cdev");
    gaddr code = tb_Get1IndResource(ctype, 1);
    if (!code || !rd32(code)) {
        cpu_halt(c, "ExitToShell (no cdev code)");
        return;
    }
    gaddr tmp = tb_NewPtr(16, false, true);
    tb_GetResInfo(code, tmp, tmp + 2, tmp + 6);
    int16_t id = (int16_t)rd16(tmp);
    tb_MoveHHi(code);
    tb_HLock(code);
    gaddr entry = rd32(code);

    /* May it run here? */
    int32_t ok = cdev_call(entry, rn, macDev, 0, 0, 0, cdevUnset, 0);
    if (ok == 0) {
        cpu_halt(c, "ExitToShell (the cdev does not run on this Macintosh)");
        return;
    }

    gaddr ditl = tb_Get1Resource((int32_t)fourcc("DITL"), id);
    if (!ditl || !rd32(ditl)) {
        cpu_halt(c, "ExitToShell (no DITL for the cdev)");
        return;
    }
    int16_t it, il, ib, ir;
    int nitems = 0;
    items_bounds(ditl, &it, &il, &ib, &ir, &nitems);
    /* A copy for the dialog, which owns and disposes its items. */
    int32_t sz = tb_GetHandleSize(ditl);
    gaddr items = tb_NewHandle(sz, false, true);
    gmemcpy(rd32(items), rd32(ditl), (uint32_t)sz);

    /* The window shows the area the items lie in, with a margin; the local
     * origin is set so the items keep their own coordinates. */
    int16_t margin = 8;
    gaddr bounds = tb_NewPtr(8, false, true);
    int16_t w = (int16_t)(ir - il + 2 * margin), h = (int16_t)(ib - it + 2 * margin);
    wr16(bounds, 60);
    wr16(bounds + 2, 40);
    wr16(bounds + 4, (uint16_t)(60 + h));
    wr16(bounds + 6, (uint16_t)(40 + w));
    const char *name = (const char *)gptr(LM_CurApName + 1);
    snprintf(title, sizeof title, "%.*s", rd8(LM_CurApName), name);
    gaddr tptr = tb_NewPtr(64, false, true);
    pstr_from_c(tptr, title);
    gaddr dlg = tb_NewDialog(0, bounds, tptr, 1, 4 /* noGrowDocProc */, 0xFFFFFFFFu, 1, 0, items);
    if (!dlg) {
        cpu_halt(c, "ExitToShell (no dialog)");
        return;
    }
    tb_SetPort(dlg);
    tb_SetOrigin((int16_t)(il - margin), (int16_t)(it - margin));

    int32_t value = cdev_call(entry, rn, initDev, 1, (int16_t)nitems, 0, cdevUnset, dlg);
    if (value == cdevMemErr || value == cdevResErr) {
        cpu_halt(c, "ExitToShell (the cdev could not start: %d)", (int)value);
        return;
    }
    gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
    gaddr dp = tb_NewPtr(4, false, true), itemp = tb_NewPtr(2, false, true);
    value = cdev_call(entry, rn, activDev, 0, (int16_t)nitems, evt, value, dlg);
    tb_DrawDialog(dlg);
    cdev_native_running = true;
    while (!c->halted) {
        bool got = tb_WaitNextEvent(0xFFFF & ~0x0400, evt, 6, 0) != 0;
        int what = got ? (int16_t)rd16(evt) : nullEvent;
        if (what == nullEvent) {
            value = cdev_call(entry, rn, nulDev, 0, (int16_t)nitems, evt, value, dlg);
            continue;
        }
        if (what == updateEvt && rd32(evt + EventRecord_message_off) == dlg) {
            tb_BeginUpdate(dlg);
            tb_DrawDialog(dlg);
            tb_EndUpdate(dlg);
            value = cdev_call(entry, rn, updateDev, 0, (int16_t)nitems, evt, value, dlg);
            continue;
        }
        if (what == activateEvt && rd32(evt + EventRecord_message_off) == dlg) {
            bool on = rd16(evt + EventRecord_modifiers_off) & 1;
            value = cdev_call(entry, rn, on ? activDev : deactivDev, 0, (int16_t)nitems, evt, value, dlg);
            continue;
        }
        if (what == mouseDown) {
            gaddr wp = tb_NewPtr(4, false, true);
            int16_t part = tb_FindWindow(rd32(evt + EventRecord_where_off), wp);
            gaddr win = rd32(wp);
            tb_DisposePtr(wp);
            if (win == dlg && part == 3 /* inGoAway */) {
                if (tb_TrackGoAway(dlg, rd32(evt + EventRecord_where_off)))
                    break;
                continue;
            }
            if (win == dlg && part == 4 /* inDrag */) {
                continue;
            }
        }
        if (what == keyDown || what == autoKey)
            value = cdev_call(entry, rn, keyEvtDev, 0, (int16_t)nitems, evt, value, dlg);
        if (tb_IsDialogEvent(evt) && tb_DialogSelect(evt, dp, itemp) && rd32(dp) == dlg) {
            int16_t item = (int16_t)rd16(itemp);
            value = cdev_call(entry, rn, hitDev, item, (int16_t)nitems, evt, value, dlg);
        }
    }
    if (!c->halted) {
        value = cdev_call(entry, rn, deactivDev, 0, (int16_t)nitems, evt, value, dlg);
        value = cdev_call(entry, rn, closeDev, 0, (int16_t)nitems, evt, value, dlg);
        tb_DisposeDialog(dlg);
        cpu_halt(c, "ExitToShell");
    }
    cdev_native_running = false;
}

/* A file with a cdev and no application code: the Control Panel's host
 * stands in for the program.  The stub is where the CPU starts. */
bool startup_is_cdev_file(void)
{
    gaddr c = tb_Get1Resource((int32_t)fourcc("CODE"), 0);
    if (c)
        return false;
    return tb_Count1Resources((int32_t)fourcc("cdev")) > 0;
}

uint32_t startup_cdev_entry(void)
{
    return native_register(cdev_host_native, "ControlPanel", 0);
}
