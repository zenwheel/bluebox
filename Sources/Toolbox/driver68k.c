/*
 * The Device Manager for 68k drivers a program supplies: desk accessories
 * and drivers in DRVR resources (opened by name with OpenDriver and
 * OpenDeskAcc), drivers installed with DrvrInstall, and device control
 * entries a program writes into the unit table itself.
 *
 * A driver is a header (flags, delay, event mask, menu, the offsets of its
 * Open, Prime, Control, Status and Close routines, its name) and code.
 * The routines are entered with A0 the parameter block and A1 the device
 * control entry.  Open and Close finish with RTS, D0 the result; Prime,
 * Control and Status finish by jumping through jIODone (D0 the result),
 * which ends the request at the head of the entry's queue and starts the
 * next one.  A request that is not finished when its routine returns
 * stays in progress (ioResult 1) until the driver calls IODone later, at
 * interrupt time or from accRun.
 *
 * The system reaches desk accessories and drivers with windows through
 * SystemEvent, SystemClick, SystemMenu, SystemEdit and SystemTask, which
 * Control them with accEvent, accMenu, accUndo ... accClear and accRun.
 */
#include <ctype.h>
#include <string.h>

#include "tb_internal.h"

enum {
    dReadEnable = 0x0100, dWritEnable = 0x0200, dCtlEnable = 0x0400, dStatEnable = 0x0800,
    dNeedGoodBye = 0x1000, dNeedTime = 0x2000,
    dOpened = 0x0020, dRAMBased = 0x0040, drvrActive = 0x0080,
};
enum {
    drvrDelay = 2, drvrEMask = 4, drvrMenu = 6, drvrOpen = 8, drvrPrime = 10, drvrCtl = 12, drvrStatus = 14,
    drvrClose = 16, drvrName = 18,
};
enum {
    goodbye = -1, killIO = 1, accEvent = 64, accRun = 65, accMenu = 67, accUndo = 68,
};
enum { controlErr = -17, statusErr = -18, readErr = -19, writErr = -20, abortErr = -27, notOpenErr = -28 };
enum { badUnitErr = -21, rfNumErr = -51 };
enum { firstDAUnit = 12, MAX_UNITS = 256 };
enum { mouseDown = 1, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt };
enum { wInContent = 1, wInDrag, wInGrow, wInGoAway };
enum { cmdKey = 0x0100, activeFlag = 1 };

bool driver_is_native(int16_t refnum); /* drivers.c: one of ours, called in C */
int32_t wm_window_hit(gaddr w, uint32_t pt);
void machine_pass_tick(void);
int16_t rm_open_path(const char *path, int8_t perm); /* rsrcmgr.c */

static uint32_t iodone_stub;

/* The DRVR resource each entry was loaded from (its name), so that a
 * second OpenDeskAcc of the same name finds the entry it made; and the
 * menu a desk accessory has in the menu bar. */
static uint8_t res_name[MAX_UNITS][33];
static int16_t menu_in_bar[MAX_UNITS];

/* ---- The unit table and the entries in it ---- */

static gaddr dce_of_unit(int unit)
{
    gaddr table = rd32(LM_UTableBase);
    if (unit < 0 || !table || unit >= (int)rd16(LM_UnitNtryCnt))
        return 0;
    gaddr h = rd32(table + 4 * (uint32_t)unit);
    return h ? rd32(h) : 0;
}

static gaddr dce_of_refnum(int16_t refnum) { return refnum < 0 ? dce_of_unit(-1 - refnum) : 0; }

/* The driver header of an entry: dCtlDriver is a pointer, or a handle
 * when the entry says the driver is RAM based. */
static gaddr header_of(gaddr e)
{
    gaddr d = rd32(e + DCtlEntry_dCtlDriver_off);
    if (d && (rd16(e + DCtlEntry_dCtlFlags_off) & dRAMBased))
        d = rd32(d);
    return d;
}

static gaddr routine_of(gaddr e, uint32_t which)
{
    gaddr hdr = header_of(e);
    uint16_t off = hdr ? rd16(hdr + which) : 0;
    return off ? hdr + off : 0;
}

static uint16_t flags_of(gaddr e) { return rd16(e + DCtlEntry_dCtlFlags_off); }
static int16_t refnum_of(gaddr e) { return (int16_t)rd16(e + DCtlEntry_dCtlRefNum_off); }

bool drv68k_refnum(int16_t refnum)
{
    if (refnum >= 0 || driver_is_native(refnum))
        return false;
    gaddr e = dce_of_refnum(refnum);
    return e && rd32(e + DCtlEntry_dCtlDriver_off);
}

/* ---- Names ---- */

/* A driver's name as a program compares it: no leading period (and a
 * desk accessory's, which starts with a NUL, loses that), any case. */
static int bare_name(gaddr p, uint8_t *out)
{
    int len = rd8(p);
    if (len && (rd8(p + 1) == '.' || rd8(p + 1) == 0)) {
        len--;
        p++;
    }
    for (int i = 0; i < len; i++)
        out[i] = (uint8_t)tolower(rd8(p + 1 + (uint32_t)i));
    return len;
}

static bool same_name(gaddr a, gaddr b)
{
    uint8_t x[256], y[256];
    int n = bare_name(a, x), m = bare_name(b, y);
    return n > 0 && n == m && !memcmp(x, y, (size_t)n);
}

static bool same_as_saved(gaddr a, int unit)
{
    uint8_t x[256], y[40];
    int n = bare_name(a, x);
    const uint8_t *s = res_name[unit];
    int m = s[0];
    if (m && (s[1] == '.' || s[1] == 0)) {
        s++;
        m--;
    }
    for (int i = 0; i < m && i < 32; i++)
        y[i] = (uint8_t)tolower(s[1 + i]);
    return n > 0 && n == m && !memcmp(x, y, (size_t)n);
}

/* The entry in the unit table whose driver has this name. */
static gaddr find_by_name(gaddr namep)
{
    int count = rd16(LM_UnitNtryCnt);
    for (int u = 0; u < count && u < MAX_UNITS; u++) {
        gaddr e = dce_of_unit(u);
        if (!e || driver_is_native(refnum_of(e)) || !rd32(e + DCtlEntry_dCtlDriver_off))
            continue;
        gaddr hdr = header_of(e);
        if ((hdr && same_name(namep, hdr + drvrName)) || same_as_saved(namep, u)) {
            if (!refnum_of(e))
                wr16(e + DCtlEntry_dCtlRefNum_off, (uint16_t)(-1 - u));
            return e;
        }
    }
    return 0;
}

/* ---- Installing ---- */

static gaddr new_entry(int unit, gaddr drvr, bool ram)
{
    gaddr table = rd32(LM_UTableBase);
    gaddr h = tb_NewHandle(DCtlEntry_SIZEOF, true, true);
    if (!h)
        return 0;
    gaddr e = rd32(h);
    gaddr hdr = ram && drvr ? rd32(drvr) : drvr;
    wr32(e + DCtlEntry_dCtlDriver_off, drvr);
    wr16(e + DCtlEntry_dCtlFlags_off, (uint16_t)((hdr ? rd16(hdr) : 0) | (ram ? dRAMBased : 0)));
    wr16(e + DCtlEntry_dCtlRefNum_off, (uint16_t)(-1 - unit));
    if (hdr) {
        wr16(e + DCtlEntry_dCtlDelay_off, rd16(hdr + drvrDelay));
        wr16(e + DCtlEntry_dCtlEMask_off, rd16(hdr + drvrEMask));
        wr16(e + DCtlEntry_dCtlMenu_off, rd16(hdr + drvrMenu));
    }
    wr32(table + 4 * (uint32_t)unit, h);
    LOGF(LOG_TRAPS, "  driver installed: unit %d header %08X flags %04X emask %04X open %04X ctl %04X\n", unit, hdr,
         flags_of(e), rd16(e + DCtlEntry_dCtlEMask_off), hdr ? rd16(hdr + drvrOpen) : 0, hdr ? rd16(hdr + drvrCtl) : 0);
    if (unit < MAX_UNITS) {
        res_name[unit][0] = 0;
        menu_in_bar[unit] = 0;
    }
    return e;
}

/* DrvrInstall: the driver at drvr (a pointer to its header) gets an entry
 * at the unit refnum names. */
int16_t drv68k_install(gaddr drvr, int16_t refnum)
{
    int unit = -1 - refnum;
    gaddr table = rd32(LM_UTableBase);
    if (refnum >= 0 || !table || unit >= (int)rd16(LM_UnitNtryCnt))
        return badUnitErr;
    return new_entry(unit, drvr, false) ? noErr : memFullErr;
}

/* A DRVR resource named like namep (a desk accessory's menu item, a
 * driver's name): as given, then with a leading period or NUL taken off
 * or put on. */
static gaddr find_drvr_resource(gaddr namep)
{
    int n = rd8(namep);
    uint8_t body[256];
    int skip = n && (rd8(namep + 1) == '.' || rd8(namep + 1) == 0);
    int blen = n - skip;
    for (int i = 0; i < blen; i++)
        body[i] = rd8(namep + 1 + (uint32_t)(skip + i));
    gaddr tmp = tb_NewPtr(264, false, true);
    if (!tmp)
        return 0;
    gaddr res = 0;
    for (int variant = 0; variant < 4 && !res; variant++) {
        int len = 0;
        if (variant == 0) {
            if (!n)
                continue;
            wr8(tmp, (uint8_t)n);
            for (int i = 0; i < n; i++)
                wr8(tmp + 1 + (uint32_t)i, rd8(namep + 1 + (uint32_t)i));
            len = n;
        } else {
            if (variant == 1 && !skip)
                continue; /* the same as given */
            int lead = variant == 2 ? 0x00 : variant == 3 ? '.' : -1;
            if (!blen)
                continue;
            if (lead >= 0)
                wr8(tmp + 1, (uint8_t)lead);
            len = (lead >= 0 ? 1 : 0) + blen;
            if (len <= 0 || len > 255)
                continue;
            wr8(tmp, (uint8_t)len);
            for (int i = 0; i < blen; i++)
                wr8(tmp + 1 + (uint32_t)((lead >= 0 ? 1 : 0) + i), body[i]);
        }
        res = tb_GetNamedResource((int32_t)fourcc("DRVR"), tmp);
        if (res && !rd32(res))
            res = 0;
    }
    tb_DisposePtr(tmp);
    return res;
}

/* The entry for a DRVR resource: it keeps the resource (detached, locked
 * and not purgeable, as the Device Manager did) at the unit its ID names,
 * or the first free unit from the desk accessories' range up. */
static gaddr install_resource(gaddr res)
{
    int count = rd16(LM_UnitNtryCnt);
    gaddr info = tb_NewPtr(300, false, true);
    if (!info || !rd32(res))
        return 0;
    tb_GetResInfo(res, info, info + 4, info + 8);
    int id = (int16_t)rd16(info);
    uint8_t name[33];
    memset(name, 0, sizeof name);
    int nl = rd8(info + 8);
    if (nl > 32)
        nl = 32;
    name[0] = (uint8_t)nl;
    for (int i = 0; i < nl; i++)
        name[1 + i] = rd8(info + 9 + (uint32_t)i);
    tb_DisposePtr(info);
    int unit = id;
    if (unit < 0 || unit >= count || unit >= MAX_UNITS || dce_of_unit(unit)) {
        unit = -1;
        for (int u = firstDAUnit; u < count && u < MAX_UNITS; u++)
            if (!dce_of_unit(u)) {
                unit = u;
                break;
            }
        if (unit < 0)
            return 0;
    }
    tb_DetachResource(res);
    tb_HNoPurge(res);
    tb_HLock(res);
    gaddr e = new_entry(unit, res, true);
    if (e)
        memcpy(res_name[unit], name, sizeof name);
    return e;
}

/* ---- Calling a driver's routines ---- */

/* An immediate call (not queued): the entry's frame is how IODone tells
 * the routine's own finish from the head of the queue's. */
typedef struct {
    gaddr e;
    bool done;
    bool periodic; /* accRun: its IODone may end the request the driver has waiting */
    int16_t result;
} frame;
static frame frames[16];
static int nframes;
static gaddr submitting; /* the call being queued: its caller completes it */
static gaddr last_pb;    /* what the last queued routine returned in D0 */
static int16_t last_d0;

static int16_t call_routine(gaddr fn, gaddr pb, gaddr e)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    uint16_t sr = c->sr;
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->d[0] = 0;
    c->a[0] = pb;
    c->a[1] = e;
    guest_call_regs(fn);
    int16_t r = (int16_t)c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    return r;
}

static void run_head(gaddr e);

/* IODone: the request at the head of the queue is over. */
static void finish(gaddr e, int16_t r)
{
    if (nframes && frames[nframes - 1].e == e) { /* inside an immediate call */
        frames[nframes - 1].done = true;
        frames[nframes - 1].result = r;
        if (!frames[nframes - 1].periodic || !rd32(e + DCtlEntry_dCtlQHdr_off + 2))
            return;
    }
    gaddr q = e + DCtlEntry_dCtlQHdr_off;
    gaddr pb = rd32(q + 2);
    if (!pb)
        return;
    tb_Dequeue(pb, q);
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    if ((rd16(pb + IOParam_ioTrap_off) & 0x0400) && rd32(pb + IOParam_ioCompletion_off) && pb != submitting)
        io_complete_now(pb, r);
    if (rd32(q + 2)) {
        gaddr saved = submitting;
        submitting = 0;
        run_head(e);
        submitting = saved;
    }
}

static void iodone_native(cpu68k *c, uint16_t tw) { finish(c->a[1], (int16_t)c->d[0]); }

static void run_head(gaddr e)
{
    gaddr pb = rd32(e + DCtlEntry_dCtlQHdr_off + 2);
    if (!pb)
        return;
    uint16_t t = rd16(pb + IOParam_ioTrap_off) & 0xFF;
    gaddr fn = routine_of(e, t == 2 || t == 3 ? drvrPrime : t == 4 ? drvrCtl : drvrStatus);
    if (!fn) {
        finish(e, t == 2 ? readErr : t == 3 ? writErr : t == 4 ? controlErr : statusErr);
        return;
    }
    int16_t d0 = call_routine(fn, pb, e);
    last_pb = pb;
    last_d0 = d0;
}

/* A call outside the queue, through the routine at offset which. */
static int16_t immediate(gaddr e, uint32_t which, gaddr pb, uint16_t trap, int16_t none, bool periodic)
{
    gaddr fn = routine_of(e, which);
    if (!fn)
        return none;
    if (nframes >= 16)
        return abortErr;
    wr16(pb + IOParam_ioTrap_off, trap);
    wr16(pb + IOParam_ioResult_off, ioInProgress);
    frame *f = &frames[nframes++];
    *f = (frame){ e, false, periodic, 0 };
    int16_t r = call_routine(fn, pb, e);
    --nframes;
    if (f->done)
        r = f->result;
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    return r;
}

/* Control with a code and a long parameter. */
static int16_t control_now(gaddr e, int16_t code, uint32_t param)
{
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, false, true);
    if (!pb)
        return memFullErr;
    wr16(pb + CntrlParam_ioCRefNum_off, (uint16_t)refnum_of(e));
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    wr32(pb + CntrlParam_csParam_off, param);
    int16_t r = immediate(e, drvrCtl, pb, 0xA004, controlErr, code == accRun);
    tb_DisposePtr(pb);
    return r;
}

/* Every request waiting in the entry's queue given up. */
static void abort_queue(gaddr e)
{
    gaddr q = e + DCtlEntry_dCtlQHdr_off;
    gaddr pb;
    while ((pb = rd32(q + 2))) {
        tb_Dequeue(pb, q);
        wr16(pb + IOParam_ioResult_off, (uint16_t)abortErr);
        if ((rd16(pb + IOParam_ioTrap_off) & 0x0400) && rd32(pb + IOParam_ioCompletion_off) && pb != submitting)
            io_complete_now(pb, abortErr);
    }
}

/* ---- Open and Close ---- */

static int16_t open_entry(gaddr e, gaddr pb)
{
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum_of(e));
    if (flags_of(e) & dOpened)
        return noErr;
    gaddr fn = routine_of(e, drvrOpen);
    int16_t r = noErr;
    if (fn) {
        wr16(pb + IOParam_ioResult_off, 0);
        r = call_routine(fn, pb, e);
    }
    if (r < 0)
        return r;
    wr16(e + DCtlEntry_dCtlFlags_off, (uint16_t)(flags_of(e) | dOpened | drvrActive));
    wr32(e + DCtlEntry_dCtlCurTicks_off, rd32(LM_Ticks) + rd16(e + DCtlEntry_dCtlDelay_off));
    return noErr;
}

static int16_t close_entry(gaddr e, gaddr pb)
{
    if (!(flags_of(e) & dOpened))
        return noErr;
    if (flags_of(e) & dNeedGoodBye)
        control_now(e, goodbye, 0);
    abort_queue(e);
    gaddr fn = routine_of(e, drvrClose);
    int16_t r = fn ? call_routine(fn, pb, e) : noErr;
    if (r < 0)
        return r;
    wr16(e + DCtlEntry_dCtlFlags_off, (uint16_t)(flags_of(e) & ~(dOpened | drvrActive)));
    int u = -1 - refnum_of(e);
    if (u >= 0 && u < MAX_UNITS && menu_in_bar[u]) {
        tb_DeleteMenu(menu_in_bar[u]);
        tb_DrawMenuBar();
        menu_in_bar[u] = 0;
    }
    return noErr;
}

/* OpenDriver and PBOpen by name: the entry already in the unit table, or a
 * DRVR resource installed in one. */
bool drv68k_open_name(gaddr pb, int16_t *err)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (!namep || !rd8(namep))
        return false;
    gaddr e = find_by_name(namep);
    if (!e) {
        gaddr res = find_drvr_resource(namep);
        e = res ? install_resource(res) : 0;
    }
    if (!e)
        return false;
    *err = open_entry(e, pb);
    return true;
}

/* ---- PBRead, PBWrite, PBControl, PBStatus, PBKillIO, PBClose ---- */

static int16_t kill_io(gaddr e, gaddr pb)
{
    int16_t r = noErr;
    if (flags_of(e) & dCtlEnable) {
        wr16(pb + CntrlParam_csCode_off, killIO);
        r = immediate(e, drvrCtl, pb, 0xA004, noErr, false);
    }
    abort_queue(e);
    return r < 0 && r != abortErr ? r : noErr;
}

static int16_t submit(gaddr e, gaddr pb, bool async, int kind)
{
    static const uint16_t trap[] = { [DRIVER_CONTROL] = 0xA004, [DRIVER_STATUS] = 0xA005, [DRIVER_READ] = 0xA002,
                                     [DRIVER_WRITE] = 0xA003 };
    gaddr q = e + DCtlEntry_dCtlQHdr_off;
    wr16(pb + IOParam_ioTrap_off, (uint16_t)(trap[kind] | (async ? 0x0400 : 0)));
    wr16(pb + IOParam_ioResult_off, ioInProgress);
    bool idle = !rd32(q + 2);
    tb_Enqueue(pb, q);
    if (idle) {
        gaddr saved = submitting;
        submitting = pb;
        run_head(e);
        submitting = saved;
    }
    int16_t r = (int16_t)rd16(pb + IOParam_ioResult_off);
    if (r != ioInProgress || async)
        return r;
    /* A synchronous call waits for the driver, which finishes it from
     * interrupt-level work.  One that never does gave RTS and the D0 it
     * left: that is its answer; otherwise the call is given up. */
    for (int i = 0; i < 120 && (int16_t)rd16(pb + IOParam_ioResult_off) == ioInProgress; i++) {
        machine_pass_tick();
        machine_advance_ticks();
        time_pump();
    }
    r = (int16_t)rd16(pb + IOParam_ioResult_off);
    if (r == ioInProgress) {
        bool queued = false;
        for (gaddr p = rd32(q + 2); p; p = rd32(p))
            queued |= p == pb;
        if (queued)
            tb_Dequeue(pb, q);
        r = last_pb == pb && last_d0 <= 0 ? last_d0 : abortErr;
        LOGF(LOG_TRAPS, "  driver %d never finished call %04X: %d\n", refnum_of(e), rd16(pb + IOParam_ioTrap_off), r);
        wr16(pb + IOParam_ioResult_off, (uint16_t)r);
        if (rd32(q + 2) && !last_pb)
            run_head(e);
    }
    return r;
}

int16_t drv68k_call(gaddr pb, bool async, int kind)
{
    gaddr e = dce_of_refnum((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!e)
        return rfNumErr;
    if (kind == DRIVER_CLOSE)
        return close_entry(e, pb);
    uint16_t f = flags_of(e);
    if (kind == DRIVER_KILLIO)
        return f & dOpened ? kill_io(e, pb) : notOpenErr;
    if (!(f & dOpened))
        return notOpenErr;
    switch (kind) {
    case DRIVER_READ: if (!(f & dReadEnable)) return readErr; break;
    case DRIVER_WRITE: if (!(f & dWritEnable)) return writErr; break;
    case DRIVER_CONTROL:
        if (!(f & dCtlEnable))
            return controlErr;
        if ((int16_t)rd16(pb + CntrlParam_csCode_off) == killIO) /* KillIO is not queued */
            return kill_io(e, pb);
        break;
    case DRIVER_STATUS: if (!(f & dStatEnable)) return statusErr; break;
    default: return rfNumErr;
    }
    LOGF(LOG_TRAPS, "  driver %d call %d%s\n", refnum_of(e), kind, async ? " (async)" : "");
    return submit(e, pb, async, kind);
}

/* ---- The system's calls: desk accessories and events ---- */

static void use_window_menu(gaddr e, bool on)
{
    int u = -1 - refnum_of(e);
    int16_t id = (int16_t)rd16(e + DCtlEntry_dCtlMenu_off);
    if (!id || u < 0 || u >= MAX_UNITS)
        return;
    if (on && !menu_in_bar[u]) {
        gaddr m = tb_GetMenu(id);
        if (m && rd32(m)) {
            int16_t mid = (int16_t)rd16(rd32(m));
            tb_InsertMenu(m, 0);
            tb_DrawMenuBar();
            menu_in_bar[u] = mid;
        }
    } else if (!on && menu_in_bar[u]) {
        tb_DeleteMenu(menu_in_bar[u]);
        tb_DrawMenuBar();
        menu_in_bar[u] = 0;
    }
}

/* OpenDeskAcc: the refnum of the desk accessory named, 0 if there is
 * none of that name among the DRVRs (the caller then looks further). */
int16_t drv68k_open_desk_acc(gaddr name, bool *found)
{
    *found = false;
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    if (!pb)
        return 0;
    wr32(pb + IOParam_ioNamePtr_off, name);
    gaddr e = find_by_name(name);
    bool was_open = e && (flags_of(e) & dOpened);
    int16_t err = noErr;
    if (!drv68k_open_name(pb, &err)) {
        tb_DisposePtr(pb);
        return 0;
    }
    *found = true;
    int16_t rn = (int16_t)rd16(pb + IOParam_ioRefNum_off);
    tb_DisposePtr(pb);
    if (err < 0)
        return 0;
    e = dce_of_refnum(rn);
    gaddr w = e ? rd32(e + DCtlEntry_dCtlWindow_off) : 0;
    if (was_open && w) /* already open: its window comes forward */
        tb_SelectWindow(w);
    return rn;
}

static int16_t file_da;

/* A desk accessory file (type 'dfil'): its DRVR resources installed, and
 * the first opened. */
bool drv68k_open_da_path(const char *path)
{
    int16_t cur = tb_CurResFile();
    int16_t rn = rm_open_path(path, 0);
    if (rn < 0)
        return false;
    tb_UseResFile(rn);
    int16_t n = tb_Count1Resources((int32_t)fourcc("DRVR"));
    gaddr first = 0;
    for (int16_t i = 1; i <= n; i++) {
        gaddr res = tb_Get1IndResource((int32_t)fourcc("DRVR"), i);
        gaddr e = res ? install_resource(res) : 0;
        if (e && !first)
            first = e;
    }
    tb_CloseResFile(rn); /* the drivers were detached */
    tb_UseResFile(cur);
    if (!first)
        return false;
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    if (!pb)
        return false;
    int16_t err = open_entry(first, pb);
    tb_DisposePtr(pb);
    if (err >= 0)
        file_da = refnum_of(first);
    return err >= 0;
}

/* The refnum of the desk accessory a file just opened (0 if none). */
int16_t drv68k_take_opened(void)
{
    int16_t r = file_da;
    file_da = 0;
    return r;
}

void drv68k_close_desk_acc(int16_t rn)
{
    gaddr e = dce_of_refnum(rn);
    if (!e || driver_is_native(rn))
        return;
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    if (!pb)
        return;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)rn);
    close_entry(e, pb);
    tb_DisposePtr(pb);
}

/* The window a system event is for, and its driver's entry (0 if not a
 * driver's). */
static gaddr entry_of_window(gaddr w)
{
    if (!w || WindowRecord_windowKind(w) >= 0)
        return 0;
    gaddr e = dce_of_refnum(WindowRecord_windowKind(w));
    return e && (flags_of(e) & dOpened) && !driver_is_native(refnum_of(e)) ? e : 0;
}

/* SystemEvent: whether a desk accessory or driver took the event.  Events
 * for its window (updates, activates, keys, and the mouse once it has been
 * pressed there) go to it through accEvent; clicks come by SystemClick. */
bool drv68k_system_event(gaddr evp, bool automatic)
{
    /* SEvtEnb stops GetNextEvent from asking: a program that turns it off
     * (THINK C's console library) calls SystemEvent itself. */
    if (automatic && !rd8(LM_SEvtEnb))
        return false;
    int16_t what = (int16_t)rd16(evp + EventRecord_what_off);
    gaddr e = 0;
    switch (what) {
    case updateEvt:
    case activateEvt:
        e = entry_of_window(rd32(evp + EventRecord_message_off));
        break;
    case keyDown:
    case autoKey:
        if (rd16(evp + EventRecord_modifiers_off) & cmdKey)
            return false; /* the application's menu keys */
        /* fall through */
    case keyUp:
    case mouseUp:
        e = entry_of_window(tb_FrontWindow());
        if (e && !(rd16(e + DCtlEntry_dCtlEMask_off) & (1u << what)))
            e = 0;
        break;
    default: return false;
    }
    if (!e)
        return false;
    if (what == activateEvt)
        use_window_menu(e, rd16(evp + EventRecord_modifiers_off) & activeFlag);
    control_now(e, accEvent, evp);
    return true;
}

void drv68k_system_click(gaddr evp, gaddr wp)
{
    gaddr e = entry_of_window(wp);
    if (!e)
        return;
    uint32_t where = rd32(evp + EventRecord_where_off);
    int32_t part = wm_window_hit(wp, where);
    if (tb_FrontWindow() != wp && part == wInContent) {
        tb_SelectWindow(wp);
        return;
    }
    switch (part) {
    case wInDrag: {
        gaddr r = tb_NewPtr(8, false, true);
        gaddr gray = rd32(LM_GrayRgn);
        if (r && gray && rd32(gray)) {
            for (uint32_t i = 0; i < 8; i += 2)
                wr16(r + i, rd16(rd32(gray) + 2 + i));
            wr16(r, (uint16_t)(rd16(r) + 4));
            wr16(r + 2, (uint16_t)(rd16(r + 2) + 4));
            wr16(r + 4, (uint16_t)(rd16(r + 4) - 4));
            wr16(r + 6, (uint16_t)(rd16(r + 6) - 4));
            tb_DragWindow(wp, where, r);
        }
        if (r)
            tb_DisposePtr(r);
        break;
    }
    case wInGoAway:
        if (tb_TrackGoAway(wp, where))
            drv68k_close_desk_acc(WindowRecord_windowKind(wp));
        break;
    default:
        control_now(e, accEvent, evp);
        break;
    }
}

/* SystemMenu: the item chosen from a desk accessory's menu. */
void drv68k_system_menu(int32_t result)
{
    int16_t id = (int16_t)(result >> 16), item = (int16_t)(result & 0xFFFF);
    int count = rd16(LM_UnitNtryCnt);
    for (int u = 0; u < count && u < MAX_UNITS; u++) {
        gaddr e = dce_of_unit(u);
        if (!e || !(flags_of(e) & dOpened) || driver_is_native(refnum_of(e)))
            continue;
        int16_t m = (int16_t)rd16(e + DCtlEntry_dCtlMenu_off);
        if (m && (m == id || menu_in_bar[u] == id || refnum_of(e) == id)) {
            control_now(e, accMenu, ((uint32_t)(uint16_t)id << 16) | (uint16_t)item);
            return;
        }
    }
}

/* SystemEdit: an Edit menu command for the front window if it is a desk
 * accessory's. */
bool drv68k_system_edit(int16_t cmd)
{
    gaddr e = entry_of_window(tb_FrontWindow());
    if (!e || cmd < 0 || cmd > 5 || cmd == 1)
        return false;
    control_now(e, (int16_t)(accUndo + cmd), 0);
    return true;
}

/* SystemTask: the drivers that need time are run when their delay is up. */
void drv68k_periodic(void)
{
    static int busy;
    if (busy)
        return;
    busy++;
    int count = rd16(LM_UnitNtryCnt);
    uint32_t now = rd32(LM_Ticks);
    for (int u = 0; u < count && u < MAX_UNITS; u++) {
        gaddr e = dce_of_unit(u);
        if (!e || (flags_of(e) & (dOpened | dNeedTime)) != (dOpened | dNeedTime) || driver_is_native(refnum_of(e)))
            continue;
        if ((int32_t)(now - rd32(e + DCtlEntry_dCtlCurTicks_off)) < 0)
            continue;
        wr32(e + DCtlEntry_dCtlCurTicks_off, now + rd16(e + DCtlEntry_dCtlDelay_off));
        control_now(e, accRun, 0);
    }
    busy--;
}

void drv68k_reset(void)
{
    nframes = 0;
    submitting = last_pb = 0;
    memset(res_name, 0, sizeof res_name);
    memset(menu_in_bar, 0, sizeof menu_in_bar);
    iodone_stub = native_register(iodone_native, "IODone", 0);
    wr32(LM_JIODone, iodone_stub);
}
