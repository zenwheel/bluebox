/*
 * Printing Manager.
 *
 * The printer is the host's.  Page Setup is the host's page layout panel;
 * the job dialog returns at once, and the printing port records each page's
 * drawing (qd_record.c).  At PrCloseDoc the host renders the pages and
 * shows its print panel, where copies and page ranges are chosen.
 *
 * Resolution is 72 dpi in both directions, so the app's layout is in points.
 *
 * TPrint (120 bytes): iPrVersion 0, prInfo 2 (iDev, iVRes, iHRes, rPage),
 * rPaper 16, prStl 24 (wDev, iPageV, iPageH, bPort, feed), prInfoPT 32,
 * prXInfo 46, prJob 62 (iFstPage, iLstPage, iCopies, bJDocLoop, fFromUsr,
 * pIdleProc, pFileName, iFileVol, bFileVers, bJobX), printX 82.
 */
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { iPrAbort = 128, iMemFullErr = -108 };
enum { TPrint_SIZE = 120, TPrPort_SIZE = 178 };
int qd_rec_pages(const machine_print_page **out);

/* The Printing Manager's result: PrintErr in low memory, which programs
 * also read and set directly. */
static void set_print_error(int16_t e) { wr16(LM_PrintErr, (uint16_t)e); }
#define print_error ((int16_t)rd16(LM_PrintErr))
static machine_print_fn print_fn;
static machine_page_setup_fn page_setup_fn;
static void *print_ctx;
static machine_page_setup defaults = { 612, 792, 18, 18, 594, 774, false }; /* US Letter, 1/4" margins */

/* The document being printed. */
static gaddr job_port, job_print, job_bits;
static bool job_owns_port;

void machine_set_print_handlers(machine_print_fn print, machine_page_setup_fn page_setup, void *ctx)
{
    print_fn = print;
    page_setup_fn = page_setup;
    print_ctx = ctx;
}

void machine_set_page_defaults(const machine_page_setup *ps) { defaults = *ps; }

int16_t tb_PrError(void) { return print_error; }
void tb_PrSetError(int16_t iErr) { set_print_error(iErr); }
void tb_PrOpen(void) { set_print_error(noErr); }
void tb_PrClose(void) {}
void tb_PrDrvrOpen(void) {}
void tb_PrDrvrClose(void) {}
void tb_PrCtlCall(int16_t iWhichCtl, int32_t lParam1, int32_t lParam2, int32_t lParam3) {}
gaddr tb_PrDrvrDCE(void) { return 0; }
int16_t tb_PrDrvrVers(void) { return 1; }

/* Fill a print record from a page setup. */
static void put_setup(gaddr p, const machine_page_setup *ps)
{
    int w = (int)(ps->right - ps->left + 0.5), h = (int)(ps->bottom - ps->top + 0.5);
    int left = (int)(ps->left + 0.5), top = (int)(ps->top + 0.5);
    qrect page = { 0, 0, (int16_t)h, (int16_t)w };
    qrect paper = { (int16_t)-top, (int16_t)-left, (int16_t)(ps->paper_height - top + 0.5),
                    (int16_t)(ps->paper_width - left + 0.5) };
    wr16(p + 2, 3);                    /* iDev */
    wr16(p + 4, 72);                   /* iVRes */
    wr16(p + 6, 72);                   /* iHRes */
    qrect_put(p + 8, page);
    qrect_put(p + 16, paper);
    wr16(p + 24, 0x0300);              /* wDev: LaserWriter-class */
    wr16(p + 26, (uint16_t)(ps->paper_height * 10 / 7.2)); /* iPageV, 1/120 inch */
    wr16(p + 28, (uint16_t)(ps->paper_width * 10 / 7.2));
    wr8(p + 30, 0);                    /* bPort */
    wr8(p + 31, 0);                    /* feed: cut sheet */
    gmemcpy(p + 32, p + 2, 14);        /* prInfoPT */
}

static void get_setup(gaddr p, machine_page_setup *ps)
{
    qrect page = qrect_get(p + 8), paper = qrect_get(p + 16);
    ps->paper_width = paper.right - paper.left;
    ps->paper_height = paper.bottom - paper.top;
    ps->left = -paper.left;
    ps->top = -paper.top;
    ps->right = ps->left + (page.right - page.left);
    ps->bottom = ps->top + (page.bottom - page.top);
    ps->landscape = ps->paper_width > ps->paper_height;
}

void tb_PrintDefault(gaddr h)
{
    gaddr p = rd32(h);
    gmemset(p, 0, TPrint_SIZE);
    wr16(p, 3);                        /* iPrVersion */
    put_setup(p, &defaults);
    wr16(p + 62, 1);                   /* iFstPage */
    wr16(p + 64, 9999);                /* iLstPage */
    wr16(p + 66, 1);                   /* iCopies */
    wr8(p + 68, 0);                    /* bJDocLoop: draft, no spooling */
    set_print_error(noErr);
}

int8_t tb_PrValidate(gaddr h)
{
    if (!h || !rd32(h) || rd16(rd32(h)) != 3) {
        tb_PrintDefault(h);
        return 1;
    }
    return 0;
}

int8_t tb_PrStlDialog(gaddr h)
{
    tb_PrValidate(h);
    machine_page_setup ps;
    get_setup(rd32(h), &ps);
    if (!page_setup_fn) {
        set_print_error(noErr);
        return 1; /* no host panel: keep the defaults */
    }
    if (!page_setup_fn(&ps, print_ctx)) {
        set_print_error(noErr);
        return 0;
    }
    put_setup(rd32(h), &ps);
    return 1;
}

/* The host's print panel comes after the pages are drawn, so the job dialog
 * just says yes: all pages, one copy, drawn directly. */
int8_t tb_PrJobDialog(gaddr h)
{
    tb_PrValidate(h);
    gaddr p = rd32(h);
    wr16(p + 62, 1);
    wr16(p + 64, 9999);
    wr16(p + 66, 1);
    wr8(p + 68, 0);
    set_print_error(noErr);
    return 1;
}

gaddr tb_PrJobInit(gaddr h) { return 0; }
gaddr tb_PrStlInit(gaddr h) { return 0; }
int8_t tb_PrDlgMain(gaddr h, gaddr initfptr) { return h && (tb_PrJobDialog(h) != 0); }
void tb_PrGeneral(gaddr pData) { wr16(pData + 2, (uint16_t)-1); /* iError: opcode not supported */ }

gaddr tb_PrOpenDoc(gaddr h, gaddr port, gaddr pIOBuf)
{
    tb_PrValidate(h);
    qrect page = qrect_get(rd32(h) + 8);
    job_owns_port = !port;
    if (!port)
        port = tb_NewPtr(TPrPort_SIZE, false, true);
    if (!port) {
        set_print_error(iMemFullErr);
        return 0;
    }
    /* A real bitmap for the page, so anything drawing pixels has somewhere
     * to draw; the recording is what gets printed. */
    int w = page.right - page.left, hgt = page.bottom - page.top;
    uint32_t row = (uint32_t)((w + 15) / 16 * 2);
    job_bits = tb_NewPtr((int32_t)(row * (uint32_t)hgt), false, true);
    tb_OpenPort(port);
    wr32(port + GrafPort_portBits_off, job_bits);
    wr16(port + GrafPort_portBits_off + 4, (uint16_t)row);
    qrect_put(port + GrafPort_portBits_off + 6, page);
    qrect_put(port + GrafPort_portRect_off, page);
    tb_RectRgn(GrafPort_visRgn(port), port + GrafPort_portRect_off);
    job_port = port;
    job_print = h;
    qd_rec_begin(port);
    set_print_error(noErr);
    return port;
}

void tb_PrOpenPage(gaddr port, gaddr pPageFrame)
{
    if (port != job_port)
        return;
    gaddr bits = rd32(port + GrafPort_portBits_off);
    qrect b = qrect_get(port + GrafPort_portBits_off + 6);
    gmemset(bits, 0, (uint32_t)rd16(port + GrafPort_portBits_off + 4) * (uint32_t)(b.bottom - b.top));
    qd_rec_open_page();
}

void tb_PrClosePage(gaddr pPrPort) { qd_rec_close_page(); }

void tb_PrCloseDoc(gaddr port)
{
    if (port != job_port || !job_port)
        return;
    const machine_print_page *pages;
    int n = qd_rec_pages(&pages);
    if (n > 0 && print_error == noErr) {
        machine_print_job job;
        memset(&job, 0, sizeof job);
        job.npages = n;
        job.pages = pages;
        gaddr p = rd32(job_print);
        qrect page = qrect_get(p + 8), paper = qrect_get(p + 16);
        job.page = (mprect){ page.top, page.left, page.bottom, page.right };
        job.paper = (mprect){ paper.top, paper.left, paper.bottom, paper.right };
        job.copies = rd16(p + 66);
        if (print_fn)
            print_fn(&job, print_ctx);
        else
            LOGF(LOG_UNIMPL, "Printing Manager: no host, %d page(s) not printed\n", n);
    }
    qd_rec_end();
    tb_ClosePort(port);
    if (job_owns_port)
        tb_DisposePtr(port);
    tb_DisposePtr(job_bits);
    job_port = job_print = job_bits = 0;
}

void tb_PrPicFile(gaddr h, gaddr pPrPort, gaddr pIOBuf, gaddr pDevBuf, gaddr prStatus) {}
