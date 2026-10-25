/*
 * CarbonLib's universal procedure pointer calls for QuickDraw, the Window,
 * Menu and List Managers, TextEdit, Appearance, the Drag Manager and help
 * tags: NewXxxUPP makes a routine descriptor for the PowerPC routine (as
 * CarbonLib did on Mac OS 8 and 9, so the managers' 68k code could call it
 * too), DisposeXxxUPP disposes of it, and InvokeXxxUPP calls one, the
 * routine's arguments first and the UPP last.
 */
#define CARBON_HI_EXPORTS
#include "carbon_hi.h"
#include "tb_internal.h"

enum { kPowerPCISA = 1 };

static void new_upp(ppc_cpu *c, uint32_t procinfo) { RETURN(mm_new_descriptor(R(0), procinfo, kPowerPCISA)); }

static void dispose_upp(ppc_cpu *c)
{
    if (mm_is_descriptor(R(0)))
        tb_DisposePtr(R(0));
    DONE();
}

static void invoke_upp(ppc_cpu *c, uint32_t procinfo, int nargs)
{
    uint32_t args[13];
    for (int i = 0; i < nargs; i++)
        args[i] = R(i);
    gaddr upp = R(nargs);
    RETURN(upp ? mm_call(upp, procinfo, args, nargs) : 0);
}

#define UPP(name, procinfo, nargs)                                                                                    \
    static void New##name##UPP(ppc_cpu *c) { new_upp(c, procinfo); }                                                  \
    static void Invoke##name##UPP(ppc_cpu *c) { invoke_upp(c, procinfo, nargs); }
#define UPPS                                                                                                           \
    UPP(ColorComplement, 0x000000D0, 1) \
    UPP(ColorSearch, 0x000003D0, 2) \
    UPP(DeviceLoopDrawing, 0x00003E80, 4) \
    UPP(DragGrayRgn, 0x00000000, 0) \
    UPP(QDArc, 0x00002B40, 4) \
    UPP(QDBits, 0x0000EFC0, 5) \
    UPP(QDComment, 0x00000E80, 3) \
    UPP(QDGetPic, 0x000002C0, 2) \
    UPP(QDJShieldCursor, 0x00002A80, 4) \
    UPP(QDLine, 0x000000C0, 1) \
    UPP(QDOpcode, 0x00002BC0, 4) \
    UPP(QDOval, 0x00000340, 2) \
    UPP(QDPoly, 0x00000340, 2) \
    UPP(QDPutPic, 0x000002C0, 2) \
    UPP(QDRRect, 0x00002B40, 4) \
    UPP(QDRect, 0x00000340, 2) \
    UPP(QDRgn, 0x00000340, 2) \
    UPP(QDStdGlyphs, 0x000003F1, 2) \
    UPP(QDText, 0x00003F80, 4) \
    UPP(QDTxMeas, 0x0000FFA0, 5) \
    UPP(RegionToRects, 0x00003FB1, 4) \
    UPP(StyleRunDirection, 0x00000390, 2) \
    UPP(WindowDef, 0x00003BB0, 4) \
    UPP(WindowPaint, 0x0003FFF0, 6) \
    UPP(MenuDef, 0x0000FF80, 5) \
    UPP(ListClickLoop, 0x00000012, 0) \
    UPP(ListDef, 0x000EBD80, 7) \
    UPP(ListSearch, 0x00002BE0, 4) \
    UPP(CaretHook, 0x0000000F, 2) \
    UPP(DrawHook, 0x0000004F, 5) \
    UPP(EOLHook, 0x0000001F, 3) \
    UPP(HighHook, 0x0000000F, 2) \
    UPP(HitTestHook, 0x0000005F, 9) \
    UPP(NWidthHook, 0x0000003F, 8) \
    UPP(TEClickLoop, 0x0000F812, 1) \
    UPP(TEDoText, 0x000000AF, 6) \
    UPP(TEFindWord, 0x0000006F, 6) \
    UPP(TERecalc, 0x0000009F, 5) \
    UPP(TextWidthHook, 0x0000002F, 5) \
    UPP(WidthHook, 0x0000002F, 5) \
    UPP(MenuItemDrawing, 0x000036C0, 4) \
    UPP(MenuTitleDrawing, 0x000036C0, 4) \
    UPP(ThemeButtonDraw, 0x0001BEC0, 6) \
    UPP(ThemeErase, 0x00001BC0, 4) \
    UPP(ThemeIterator, 0x00003ED0, 4) \
    UPP(ThemeTabTitleDraw, 0x00036AC0, 6) \
    UPP(WindowTitleDrawing, 0x000036C0, 4) \
    UPP(DragDrawing, 0x000FFFA0, 7) \
    UPP(DragInput, 0x00003FE0, 4) \
    UPP(DragReceiveHandler, 0x00000FE0, 3) \
    UPP(DragSendData, 0x00003FE0, 4) \
    UPP(DragTrackingHandler, 0x00003FA0, 4) \
    UPP(HMControlContent, 0x0000FBF0, 5) \
    UPP(HMMenuItemContent, 0x00003EF0, 4) \
    UPP(HMMenuTitleContent, 0x00003EF0, 4) \
    UPP(HMWindowContent, 0x0000FBF0, 5) \

UPPS
#undef UPP

#define UPP(name, procinfo, nargs)                                                                                    \
    { "New" #name "UPP", New##name##UPP }, { "Invoke" #name "UPP", Invoke##name##UPP },                               \
        { "Dispose" #name "UPP", dispose_upp },
const ppc_export carbonupp_exports[] = { UPPS };
const unsigned carbonupp_export_count = sizeof carbonupp_exports / sizeof carbonupp_exports[0];
