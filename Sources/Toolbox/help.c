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

gaddr hm_help_menu(void)
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
    return help_menu;
}

int16_t tb_HMGetHelpMenuHandle(gaddr mhp)
{
    wr32(mhp, hm_help_menu());
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



/* The resource IDs a program names for its menus' and dialogs' help ('hmnu',
 * 'hdlg'), kept for it to read back; balloons themselves are not shown. */
static int16_t dialog_res_id = -1;
static struct {
    int16_t menu, res;
} menu_res[32];
static int nmenu_res;

int16_t tb_HMSetDialogResID(int16_t resid)
{
    dialog_res_id = resid;
    return noErr;
}

int16_t tb_HMGetDialogResID(gaddr residp)
{
    if (residp)
        wr16(residp, (uint16_t)dialog_res_id);
    return noErr;
}

int16_t tb_HMSetMenuResID(int16_t menuid, int16_t resid)
{
    int i = 0;
    while (i < nmenu_res && menu_res[i].menu != menuid)
        i++;
    if (i == 32)
        return noErr;
    if (i == nmenu_res)
        nmenu_res++;
    menu_res[i].menu = menuid;
    menu_res[i].res = resid;
    return noErr;
}

int16_t tb_HMGetMenuResID(gaddr menuidp, gaddr residp)
{
    int16_t menu = menuidp ? (int16_t)rd16(menuidp) : 0;
    for (int i = 0; i < nmenu_res; i++)
        if (menu_res[i].menu == menu) {
            if (residp)
                wr16(residp, (uint16_t)menu_res[i].res);
            return noErr;
        }
    if (residp)
        wr16(residp, 0xFFFF);
    return resNotFound;
}

/* The rectangle a balloon for the message would take (Internet Explorer 5
 * asks, for its toolbar's tips): its text in the help font, wrapped at 240
 * pixels, with a margin; a fixed size for a picture or resource message. */
int16_t tb_HMBalloonRect(gaddr messp, gaddr rectp)
{
    int w = 200, h = 60;
    if (messp && (int16_t)rd16(messp) == 1) { /* khmmString: a Str255 */
        int width = tb_StringWidth(messp + 2);
        int lines = width / 224 + 1;
        w = (width < 224 ? width : 224) + 16;
        h = lines * 12 + 16;
    }
    if (rectp) {
        wr16(rectp, 0);
        wr16(rectp + 2, 0);
        wr16(rectp + 4, (uint16_t)h);
        wr16(rectp + 6, (uint16_t)w);
    }
    return noErr;
}

/* No balloon is drawn, so no picture of one: hmBalloonAborted. */
int16_t tb_HMBalloonPict(gaddr messp, gaddr pictp)
{
    if (pictp)
        wr32(pictp, 0);
    return hmBalloonAborted;
}

/* No 'hmnu' or 'hdlg' messages are read: none found. */
int16_t tb_HMGetIndHelpMsg(int32_t type, int16_t resid, int16_t msg, int16_t state, gaddr options, uint32_t tip,
                           gaddr altrectp, gaddr theprocp, gaddr variantp, gaddr helpmsgp, gaddr count)
{
    if (count)
        wr16(count, 0);
    return resNotFound;
}

