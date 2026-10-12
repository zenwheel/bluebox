/* Help Manager: balloon help is off and there is no Help menu (yet). */
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
int16_t tb_HMGetHelpMenuHandle(gaddr mhp)
{
    wr32(mhp, 0);
    return noErr;
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
