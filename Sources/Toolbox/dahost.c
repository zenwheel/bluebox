/*
 * The desk accessory host: a desk accessory file (a Font/DA Mover
 * suitcase, type 'dfil', or any file of DRVRs with no program) run on its
 * own, as System 7's DA Handler ran desk accessories: a small program of
 * ours with an Apple menu of the file's desk accessories, File (Close,
 * Quit) and Edit menus, that opens the first desk accessory and passes
 * its windows' clicks, keys and menu commands on with SystemClick,
 * SystemEvent, SystemMenu and SystemEdit.  It quits when the last desk
 * accessory closes.
 *
 * A file holding the Talking Moose (a DRVR named "\0Talking Moose") gets
 * the Moose's host instead (moose.c): the Moose is started hidden, as its
 * MooseUnit's StartMoose(TRUE) did, and the host stays, with no window of
 * its own, so the Moose pops up when the Mac has been left alone; its
 * Moose menu tells a joke now, keeps it quiet, or opens its Talk Settings.
 */
#include <stdio.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { nullEvent = 0, mouseDown = 1, keyDown = 3, autoKey = 5, kHighLevelEvent = 23 };
enum { inDesk = 0, inMenuBar = 1, inSysWindow = 2 };
enum { cmdKey = 0x0100 };
enum { appleID = 128, fileID = 129, editID = 130 };
enum { fileClose = 1, fileQuit = 3 };

int16_t moose_menu_id(void);
bool moose_host_start(void);
bool moose_host_menu(int16_t item);
void moose_host_quit(void);
void moose_host_idle(void);

static gaddr new_menu(int16_t id, const char *title, const char *items)
{
    gaddr s = tb_NewPtr(256, false, true);
    pstr_from_c(s, title);
    gaddr m = tb_NewMenu(id, s);
    if (items && *items) {
        pstr_from_c(s, items);
        tb_AppendMenu(m, s);
    }
    tb_DisposePtr(s);
    tb_InsertMenu(m, 0);
    return m;
}

/* The desk accessories open now (refnums of the 68k drivers in the unit
 * table that are open and are desk accessories, by their NUL names). */
static int open_desk_accessories(void)
{
    int n = 0, count = rd16(LM_UnitNtryCnt);
    gaddr table = rd32(LM_UTableBase);
    for (int u = 12; table && u < count; u++) {
        gaddr h = rd32(table + 4 * (uint32_t)u);
        gaddr e = h ? rd32(h) : 0;
        if (!e || !(rd16(e + DCtlEntry_dCtlFlags_off) & 0x0020) || !drv68k_refnum((int16_t)(-1 - u)))
            continue;
        if (rd32(e + DCtlEntry_dCtlWindow_off))
            n++;
    }
    return n;
}

static gaddr front_da_window(void)
{
    gaddr w = tb_FrontWindow();
    return w && WindowRecord_windowKind(w) < 0 ? w : 0;
}

static bool quitting;
static bool moose;

/* A menu command: ours, or a desk accessory's. */
static void do_menu(int32_t choice)
{
    int16_t id = (int16_t)(choice >> 16), item = (int16_t)choice;
    if (!id)
        return;
    if (id == appleID) {
        gaddr s = tb_NewPtr(256, false, true);
        tb_GetMenuItemText(tb_GetMenuHandle(appleID), item, s);
        tb_OpenDeskAcc(s);
        tb_DisposePtr(s);
    } else if (id == fileID) {
        if (item == fileClose) {
            gaddr w = front_da_window();
            if (w)
                tb_CloseDeskAcc(WindowRecord_windowKind(w));
        } else if (item == fileQuit) {
            quitting = true;
        }
    } else if (id == editID) {
        if (item != 2)
            tb_SystemEdit((int16_t)(item - 1));
    } else if (moose && id == moose_menu_id()) {
        moose_host_menu(item);
    } else {
        tb_SystemMenu(choice);
    }
    tb_HiliteMenu(0);
}

static void da_host_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
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
    gaddr apple = new_menu(appleID, "\x14", NULL);
    tb_AppendResMenu(apple, (int32_t)fourcc("DRVR"));
    new_menu(fileID, "File", "Close/W;(-;Quit/Q");
    new_menu(editID, "Edit", "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");

    gaddr name = tb_NewPtr(256, false, true);
    moose = moose_host_start();
    if (!moose) {
        /* The file's first desk accessory, by the name it has in the menu. */
        gaddr h = tb_Get1IndResource((int32_t)fourcc("DRVR"), 1);
        gaddr tmp = tb_NewPtr(8, false, true);
        if (h)
            tb_GetResInfo(h, tmp, tmp + 2, name);
        tb_DisposePtr(tmp);
        if (!h || !rd8(name) || !tb_OpenDeskAcc(name)) {
            tb_DrawMenuBar();
            cpu_halt(c, "ExitToShell (the desk accessory did not open)");
            return;
        }
    }
    tb_DrawMenuBar();

    gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
    gaddr wp = tb_NewPtr(4, false, true);
    while (!c->halted && !quitting) {
        if (!moose && !open_desk_accessories())
            break; /* the last one closed */
        tb_SystemTask();
        if (moose)
            moose_host_idle();
        if (!tb_WaitNextEvent((int16_t)(0xFFFF & ~0x0400), evt, 30, 0))
            continue;
        int16_t what = (int16_t)rd16(evt);
        if (what == mouseDown) {
            uint32_t where = rd32(evt + EventRecord_where_off);
            int16_t part = tb_FindWindow(where, wp);
            if (part == inMenuBar)
                do_menu(tb_MenuSelect(where));
            else if (part == inSysWindow)
                tb_SystemClick(evt, rd32(wp));
        } else if ((what == keyDown || what == autoKey) && (rd16(evt + EventRecord_modifiers_off) & cmdKey)) {
            do_menu(tb_MenuKey((int16_t)(rd32(evt + EventRecord_message_off) & 0xFF)));
        }
    }
    if (c->halted)
        return;
    if (moose)
        moose_host_quit();
    /* The desk accessories close as the program does. */
    for (gaddr w; (w = front_da_window());)
        tb_CloseDeskAcc(WindowRecord_windowKind(w));
    cpu_halt(c, "ExitToShell");
}

/* A file of desk accessories and no program: the host stands in. */
bool startup_is_da_file(void)
{
    if (tb_Get1Resource((int32_t)fourcc("CODE"), 0))
        return false;
    return tb_Count1Resources((int32_t)fourcc("DRVR")) > 0;
}

uint32_t startup_da_entry(void) { return native_register(da_host_native, "DeskAccessoryHost", 0); }
