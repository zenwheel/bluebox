/*
 * InterfaceLib's C forms of Toolbox calls (findwindow, ptinrect, newdialog,
 * sfgetfile, iudatestring ...): MPW's C glue, which took Points by address
 * and strings as C strings.  Each converts its arguments and calls the
 * Pascal form's export.
 *
 * An argument's conversion is a character of a spec string:
 *     .  as it is
 *     p  a Point's address: the Point itself
 *     s  a C string in: a Pascal copy
 *     o  a string out: a buffer the Pascal string is made C in after
 *     r  an SFReply: its file name (at 10) made C after
 */
#include <string.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])

static void c_to_p(gaddr src, gaddr dst)
{
    uint32_t n = 0;
    while (n < 255 && rd8(src + n))
        n++;
    gmemcpy(dst + 1, src, n);
    wr8(dst, (uint8_t)n);
}

static void p_to_c(gaddr src, gaddr dst)
{
    uint8_t n = rd8(src);
    gmemcpy(dst, src + 1, n);
    wr8(dst + n, 0);
}

static void c_glue(ppc_cpu *c, const char *name, const char *spec)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn) {
        ppc_halt(c, "unimplemented: %s (for the C form)", name);
        return;
    }
    gaddr temp[8] = { 0 }, out[8] = { 0 }, reply = 0;
    for (int i = 0; spec[i] && i < 8; i++) {
        gaddr a = R(i);
        switch (spec[i]) {
        case 'p':
            c->gpr[3 + i] = a ? rd32(a) : 0;
            break;
        case 's':
            if (a && (temp[i] = tb_NewPtr(256, false, true))) {
                c_to_p(a, temp[i]);
                c->gpr[3 + i] = temp[i];
            }
            break;
        case 'o':
            if (a && (temp[i] = tb_NewPtr(256, false, true))) {
                out[i] = a;
                c->gpr[3 + i] = temp[i];
            }
            break;
        case 'r':
            reply = a;
            break;
        }
    }
    uint32_t lr = c->lr;
    fn(c);
    for (int i = 0; i < 8; i++) {
        if (out[i])
            p_to_c(temp[i], out[i]);
        if (temp[i])
            tb_DisposePtr(temp[i]);
    }
    if (reply && rd8(reply)) /* good */
        p_to_c(reply + 10, reply + 10);
    c->pc = lr;
}

#define GLUE(cname, pname, spec) \
    static void cg_##cname(ppc_cpu *c) { c_glue(c, #pname, spec); }
/* QuickDraw */
GLUE(addpt, AddPt, "p.")
GLUE(subpt, SubPt, "p.")
GLUE(equalpt, EqualPt, "pp")
GLUE(deltapoint, DeltaPoint, "pp")
GLUE(pt2rect, Pt2Rect, "pp.")
GLUE(ptinrect, PtInRect, "p.")
GLUE(ptinrgn, PtInRgn, "p.")
GLUE(pttoangle, PtToAngle, ".p.")
GLUE(shieldcursor, ShieldCursor, ".p")
GLUE(stdline, StdLine, "p")
GLUE(stdtext, StdText, "..pp")
GLUE(stuffhex, StuffHex, ".s")
/* Controls and dialogs */
GLUE(dragcontrol, DragControl, ".p")
GLUE(findcontrol, FindControl, "p")
GLUE(testcontrol, TestControl, ".p")
GLUE(trackcontrol, TrackControl, ".p")
GLUE(finddialogitem, FindDialogItem, ".p")
GLUE(newdialog, NewDialog, "..s")
GLUE(newcolordialog, NewColorDialog, "..s")
/* Windows and menus */
GLUE(draggrayrgn, DragGrayRgn, ".p")
GLUE(dragwindow, DragWindow, ".p")
GLUE(findwindow, FindWindow, "p")
GLUE(growwindow, GrowWindow, ".p")
GLUE(pinrect, PinRect, ".p")
GLUE(trackbox, TrackBox, ".p")
GLUE(trackgoaway, TrackGoAway, ".p")
GLUE(menuselect, MenuSelect, "p")
/* Lists */
GLUE(laddtocell, LAddToCell, "..p")
GLUE(lcellsize, LCellSize, "p")
GLUE(lclick, LClick, "p")
GLUE(lclrcell, LClrCell, "p")
GLUE(ldraw, LDraw, "p")
GLUE(lgetcell, LGetCell, "..p")
GLUE(lgetcelldatalocation, LGetCellDataLocation, "..p")
GLUE(lnew, LNew, "..p")
GLUE(lrect, LRect, ".p")
GLUE(lsetcell, LSetCell, "..p")
GLUE(lsetselect, LSetSelect, ".p")
/* TextEdit */
GLUE(teclick, TEClick, "p")
/* Resources */
GLUE(addresource, AddResource, "...s")
GLUE(get1namedresource, Get1NamedResource, ".s")
GLUE(getresinfo, GetResInfo, "...o")
GLUE(setresinfo, SetResInfo, "..s")
GLUE(openrfperm, OpenRFPerm, "s")
/* Strings */
GLUE(newstring, NewString, "s")
GLUE(setstring, SetString, ".s")
/* Standard File */
GLUE(sfgetfile, SFGetFile, "ps....r")
GLUE(sfpgetfile, SFPGetFile, "ps....r")
GLUE(sfputfile, SFPutFile, "pss.r")
GLUE(sfpputfile, SFPPutFile, "pss.r")
/* International Utilities */
GLUE(iucompstring, IUCompString, "ss")
GLUE(iuequalstring, IUEqualString, "ss")
GLUE(iucomppstring, IUCompPString, "ss")
GLUE(iuequalpstring, IUEqualPString, "ss")
GLUE(iustringorder, IUStringOrder, "ss")
GLUE(iudatestring, IUDateString, "..o")
GLUE(iudatepstring, IUDatePString, "..o")
GLUE(iutimestring, IUTimeString, "..o")
GLUE(iutimepstring, IUTimePString, "..o")
GLUE(iuldatestring, IULDateString, "..o")
GLUE(iultimestring, IULTimeString, "..o")
/* Volumes and files */
GLUE(eject, Eject, "s")
GLUE(flushvol, FlushVol, "s")
GLUE(fsrename, Rename, "s.s")
GLUE(getvinfo, GetVInfo, ".o")
GLUE(getvol, GetVol, "o")
GLUE(rstflock, RstFLock, "s")
GLUE(setflock, SetFLock, "s")
GLUE(setvol, SetVol, "s")
GLUE(unmountvol, UnmountVol, "s")
/* Disk Initialization */
GLUE(dizero, DIZero, ".s")
GLUE(dibadmount, DIBadMount, "p")
#undef GLUE

#define E(name) { #name, cg_##name }
const ppc_export cglue_exports[] = {
    E(addpt), E(subpt), E(equalpt), E(deltapoint), E(pt2rect), E(ptinrect), E(ptinrgn), E(pttoangle),
    E(shieldcursor), E(stdline), E(stdtext), E(stuffhex), E(dragcontrol), E(findcontrol), E(testcontrol),
    E(trackcontrol), E(finddialogitem), E(newdialog), E(newcolordialog), E(draggrayrgn), E(dragwindow),
    E(findwindow), E(growwindow), E(pinrect), E(trackbox), E(trackgoaway), E(menuselect), E(laddtocell),
    E(lcellsize), E(lclick), E(lclrcell), E(ldraw), E(lgetcell), E(lgetcelldatalocation), E(lnew), E(lrect),
    E(lsetcell), E(lsetselect), E(teclick), E(addresource), E(get1namedresource), E(getresinfo), E(setresinfo),
    E(openrfperm), E(newstring), E(setstring), E(sfgetfile), E(sfpgetfile), E(sfputfile), E(sfpputfile),
    E(iucompstring), E(iuequalstring), E(iucomppstring), E(iuequalpstring), E(iustringorder), E(iudatestring),
    E(iudatepstring), E(iutimestring), E(iutimepstring), E(iuldatestring), E(iultimestring), E(eject),
    E(flushvol), E(fsrename), E(getvinfo), E(getvol), E(rstflock), E(setflock), E(setvol), E(unmountvol),
    E(dizero), E(dibadmount),
};
#undef E
const unsigned cglue_export_count = sizeof cglue_exports / sizeof cglue_exports[0];

/* Every Pascal form the C forms call (for the tests). */
bool cglue_check(char *missing, size_t size)
{
    missing[0] = 0;
    static const char *const names[] = {
        "AddPt", "SubPt", "EqualPt", "DeltaPoint", "Pt2Rect", "PtInRect", "PtInRgn", "PtToAngle", "ShieldCursor",
        "StdLine", "StdText", "StuffHex", "DragControl", "FindControl", "TestControl", "TrackControl",
        "FindDialogItem", "NewDialog", "NewColorDialog", "DragGrayRgn", "DragWindow", "FindWindow", "GrowWindow",
        "PinRect", "TrackBox", "TrackGoAway", "MenuSelect", "LAddToCell", "LCellSize", "LClick", "LClrCell",
        "LDraw", "LGetCell", "LGetCellDataLocation", "LNew", "LRect", "LSetCell", "LSetSelect", "TEClick",
        "AddResource", "Get1NamedResource", "GetResInfo", "SetResInfo", "OpenRFPerm", "NewString", "SetString",
        "SFGetFile", "SFPGetFile", "SFPutFile", "SFPPutFile", "IUCompString", "IUEqualString", "IUCompPString",
        "IUEqualPString", "IUStringOrder", "IUDateString", "IUDatePString", "IUTimeString", "IUTimePString",
        "IULDateString", "IULTimeString", "Eject", "FlushVol", "Rename", "GetVInfo", "GetVol", "RstFLock",
        "SetFLock", "SetVol", "UnmountVol", "DIZero", "DIBadMount",
    };
    bool ok = true;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!cfm_export_fn(names[i])) {
            ok = false;
            strlcat(missing, names[i], size);
            strlcat(missing, " ", size);
        }
    return ok;
}
