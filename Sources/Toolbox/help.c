/* Help Manager: balloon help is off; the Help menu is there for programs
 * to add to. */
#include "tb_internal.h"

enum { hmHelpManagerNotInited = -855, hmBalloonAborted = -853 };

int8_t tb_HMGetBalloons(void) { return 0; }
int16_t tb_HMSetBalloons(int8_t flag) { return noErr; }
int8_t tb_HMIsBalloon(void) { return 0; }
int16_t tb_HMShowBalloon(gaddr msgp, uint32_t tip, gaddr alternaterectp, gaddr tipprocptr, int16_t proc, int16_t variant,
                         int16_t method)
{
    return hmBalloonAborted;
}
int16_t tb_HMShowMenuBalloon(int16_t item, int16_t menuid, int32_t flags, int32_t itemreserved, uint32_t tip,
                             gaddr alternaterectp, gaddr tipproc, int16_t proc, int16_t variant)
{
    return hmBalloonAborted;
}
int16_t tb_HMRemoveBalloon(void) { return noErr; }
/* The system's Help menu (kHMHelpMenuID), to which programs append their
 * own help items after the Help Manager's two. It lives in the system
 * heap and is made once per run. */
enum { kHMHelpMenuID = -16490 };
static gaddr help_menu;

void help_reset(void) { help_menu = 0; }

int16_t tb_HMGetHelpMenuHandle(gaddr mhp)
{
    if (!help_menu) {
        gaddr saved = tb_GetZone();
        tb_SetZone(rd32(LM_SysZone));
        gaddr title = tb_NewPtr(2, false, false);
        wr8(title, 1);
        wr8(title + 1, '?');
        help_menu = tb_NewMenu(kHMHelpMenuID, title);
        tb_DisposePtr(title);
        static const char items[] = "About Balloon Help\xC9;(Show Balloons";
        gaddr s = tb_NewPtr(sizeof items, false, false);
        pstr_from_c(s, items);
        tb_AppendMenu(help_menu, s);
        tb_DisposePtr(s);
        tb_SetZone(saved);
    }
    wr32(mhp, help_menu);
    return help_menu ? noErr : memFullErr;
}
int16_t tb_HMGetFont(gaddr fontp)
{
    wr16(fontp, 3);
    return noErr;
}
int16_t tb_HMGetFontSize(gaddr sizep)
{
    wr16(sizep, 9);
    return noErr;
}
int16_t tb_HMSetFont(int16_t font) { return noErr; }
int16_t tb_HMSetFontSize(int16_t size) { return noErr; }
int16_t tb_HMScanTemplateItems(int16_t whichid, int16_t whicresfile, int32_t whictype) { return noErr; }
int16_t tb_HMGetBalloonWindow(gaddr windowpp)
{
    wr32(windowpp, 0);
    return noErr;
}
int16_t tb_HMExtractHelpMsg(int32_t type, int16_t resid, int16_t msg, int16_t state, gaddr helpmsgp) { return resNotFound; }

int16_t tb_HMSetMenuResID(int16_t menuid, int16_t resid) { return noErr; }
