/*
 * Printing Manager (placeholder until M3).
 *
 * Printing is reported as available with LaserWriter-like US Letter
 * defaults, but the style and job dialogs return "cancelled" so nothing is
 * printed yet.  M3 replaces this with native dialogs and PDF output.
 */
#include "qd_internal.h"
#include "tb_internal.h"

enum { iPrAbort = 128 };
static int16_t print_error;

int16_t tb_PrError(void) { return print_error; }
void tb_PrSetError(int16_t iErr) { print_error = iErr; }
void tb_PrOpen(void) { print_error = noErr; }
void tb_PrClose(void) {}
void tb_PrDrvrOpen(void) {}
void tb_PrDrvrClose(void) {}
void tb_PrCtlCall(int16_t iWhichCtl, int32_t lParam1, int32_t lParam2, int32_t lParam3) {}
gaddr tb_PrDrvrDCE(void) { return 0; }
int16_t tb_PrDrvrVers(void) { return 1; }

/* TPrint: iPrVersion, prInfo (iDev, iVRes, iHRes, rPage), rPaper, prStl,
 * prInfoPT, prXInfo, prJob, printX. */
void tb_PrintDefault(gaddr h)
{
    gaddr p = rd32(h);
    gmemset(p, 0, 120);
    wr16(p, 3);
    wr16(p + 4, 72);
    wr16(p + 6, 72);
    qrect_put(p + 8, (qrect){ 0, 0, 733, 576 });
    qrect_put(p + 16, (qrect){ -30, -18, 762, 594 });
    wr16(p + 24, 0x0300); /* wDev: LaserWriter */
    wr16(p + 26, 1100);
    wr16(p + 28, 850);
    gmemcpy(p + 34, p + 2, 14); /* prInfoPT */
    wr16(p + 62, 1);             /* iFstPage */
    wr16(p + 64, 9999);          /* iLstPage */
    wr16(p + 66, 1);             /* iCopies */
    print_error = noErr;
}

int8_t tb_PrValidate(gaddr h)
{
    if (rd16(rd32(h)) != 3) {
        tb_PrintDefault(h);
        return 1;
    }
    return 0;
}

int8_t tb_PrStlDialog(gaddr h) { return 0; }
int8_t tb_PrJobDialog(gaddr h) { return 0; }
gaddr tb_PrJobInit(gaddr h) { return 0; }
gaddr tb_PrStlInit(gaddr h) { return 0; }
int8_t tb_PrDlgMain(gaddr h, gaddr initfptr) { return 0; }
void tb_PrGeneral(gaddr pData) { wr16(pData + 2, (uint16_t)-1); /* iError: not supported */ }

gaddr tb_PrOpenDoc(gaddr h, gaddr port, gaddr pIOBuf)
{
    print_error = iPrAbort;
    return 0;
}
void tb_PrOpenPage(gaddr port, gaddr pPageFrame) {}
void tb_PrClosePage(gaddr pPrPort) {}
void tb_PrCloseDoc(gaddr port) {}
void tb_PrPicFile(gaddr h, gaddr pPrPort, gaddr pIOBuf, gaddr pDevBuf, gaddr prStatus) {}
