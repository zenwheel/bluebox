/*
 * The Carbon Printing Manager (PMCore.h, PMApplication.h), on the
 * Printing Manager here (print.c): its print records, its dialogs, its
 * printing port and the host's printing at the end of the document.
 *
 * Page formats and print settings are blocks holding a print record
 * each (a page format's page setup, a print settings' job), so making an
 * old record or taking one apart is copying; a document starts with the
 * two put together in one record, at the resolution the page format
 * asks scaled by its scale (a page shown at 50% is drawn at twice the
 * resolution).  A session is a block too; the CarbonLib 1.0 calls without
 * one (PMBegin ... PMEnd) use one of ours, which is also their print
 * context.  A PMDialog is the classic TPrDlg the Printing Manager builds,
 * which PMGetDialogPtr hands back as the dialog.
 *
 * Rectangles and resolutions are doubles, in points (72 per inch) unless
 * said otherwise: an adjusted page rect is in the document's own
 * coordinates, the resolution and scale applied.
 */
#include <string.h>

#include "carbon_svc.h"
#include "qd_internal.h"

enum {
    kPMNoError = 0,
    kPMGeneralError = -30870,
    kPMOutOfScope = -30871,
    kPMNotImplemented = -30873,
    kPMNoSuchEntry = -30874,
    kPMInvalidPrintSettings = -30875,
    kPMInvalidPageFormat = -30876,
    kPMValueOutOfRange = -30877,
    kPMInvalidPrintSession = -30879,
    kPMInvalidPrinter = -30880,
};
enum { kPMCancel = 0x0080, iPrAbort = 128 };
enum { kPMPortrait = 1, kPMLandscape = 2, kPMReversePortrait = 3, kPMReverseLandscape = 4 };
enum { kPMBlackAndWhite = 1, kPMGray = 2, kPMColor = 3 };
enum { kPMDestinationPrinter = 1 };
enum { kPMPrintAllPages = -1, ALL_PAGES = 9999 };
#define kPMCurrentValue 0x63757272u        /* 'curr' */
#define kPMDefaultValue 0x64666c74u        /* 'dflt' */
#define kPMMinimumValue 0x6d696e76u        /* 'minv' */
#define kPMMaximumValue 0x6d617876u        /* 'maxv' */
#define kPMMinRange 0x6d6e7267u            /* 'mnrg' */
#define kPMMaxRange 0x6d787267u            /* 'mxrg' */
#define kPMMinSquareResolution 0x6d696e73u /* 'mins' */
#define kPMMaxSquareResolution 0x6d617873u /* 'maxs' */
#define kPMDefaultResolution 0x64667472u   /* 'dftr' */
enum { uppPMItemProcInfo = 0x000002C0, uppPMDialogInitProcInfo = 0x000003C0, uppPMIdleProcInfo = 0 };

/* TPrint */
enum { TPRINT_SIZE = 120, PR_VRES = 4, PR_HRES = 6, PR_PAGE = 8, PR_PAPER = 16, PR_JOB = 62, PR_JOB_LEN = 20,
       PR_FST = 62, PR_LST = 64, PR_COPIES = 66 };
/* TPrDlg, after its DialogRecord */
enum { DLG_FILTER = 170, DLG_ITEM = 174, DLG_PRINT = 178, DLG_DOIT = 182, DLG_DONE = 183 };

/* The objects: a tag and a reference count, then their own fields. */
#define PAGE_FORMAT 0x504d7066u    /* 'PMpf' */
#define PRINT_SETTINGS 0x504d7073u /* 'PMps' */
#define SESSION 0x504d7373u        /* 'PMss' */
#define PRINTER 0x504d7072u        /* 'PMpr' */
#define FLAT 0x504d666cu           /* 'PMfl', a flattened one */
enum { O_TAG = 0, O_COUNT = 4 };
/* page format: its print record, scale (a double, percent), extended data */
enum { PF_PRINT = 8, PF_SCALE = 12, PF_EXT = 20, PF_SIZE = 24 };
/* print settings: its print record, job name, colour mode, page range, extended data */
enum { PS_PRINT = 8, PS_NAME = 12, PS_COLOR = 268, PS_MIN = 272, PS_MAX = 276, PS_EXT = 280, PS_SIZE = 284 };
/* session: error, the document's port and record, idle procedure, in a page */
enum { SS_ERROR = 8, SS_PORT = 12, SS_PRINT = 16, SS_IDLE = 20, SS_PAGE = 24, SS_SIZE = 28 };

static gaddr global_session, printer, init_stub, pending_dialog;
static int global_count;

/* Data a program keeps in a session, by key. */
enum { MAX_DATA = 32 };
static struct {
    gaddr session, value;
    uint16_t key[64];
    uint32_t keylen;
} session_data[MAX_DATA];

void carbon_print_reset(void)
{
    global_session = printer = init_stub = pending_dialog = 0;
    global_count = 0;
    memset(session_data, 0, sizeof session_data);
}

static bool is(gaddr o, uint32_t tag) { return o && o < 0xF0000000u && rd32(o + O_TAG) == tag; }
static gaddr print_of(gaddr o) { return rd32(o + 8); } /* PF_PRINT, PS_PRINT */

static gaddr new_object(uint32_t tag, int32_t size)
{
    gaddr o = tb_NewPtr(size, false, true);
    if (o) {
        wr32(o + O_TAG, tag);
        wr32(o + O_COUNT, 1);
    }
    return o;
}

static gaddr new_record(void)
{
    gaddr h = tb_NewHandle(TPRINT_SIZE, false, true);
    if (h)
        tb_PrintDefault(h);
    return h;
}

static gaddr new_page_format(void)
{
    gaddr h = new_record(), o = h ? new_object(PAGE_FORMAT, PF_SIZE) : 0;
    if (!o)
        return 0;
    wr32(o + PF_PRINT, h);
    svc_wrd(o + PF_SCALE, 100.0);
    return o;
}

static gaddr new_print_settings(void)
{
    gaddr h = new_record(), o = h ? new_object(PRINT_SETTINGS, PS_SIZE) : 0;
    if (!o)
        return 0;
    wr32(o + PS_PRINT, h);
    wr32(o + PS_COLOR, kPMColor);
    wr32(o + PS_MIN, 1);
    wr32(o + PS_MAX, (uint32_t)kPMPrintAllPages);
    return o;
}

static gaddr new_session(void)
{
    gaddr s = new_object(SESSION, SS_SIZE);
    tb_PrOpen();
    return s;
}

static void end_document(gaddr s);

static void dispose_object(gaddr o)
{
    uint32_t tag = rd32(o + O_TAG);
    if (tag == PAGE_FORMAT || tag == PRINT_SETTINGS) {
        tb_DisposeHandle(print_of(o));
        gaddr ext = rd32(o + (tag == PAGE_FORMAT ? PF_EXT : PS_EXT));
        if (ext)
            tb_DisposeHandle(ext);
    } else if (tag == SESSION) {
        if (rd32(o + SS_PORT))
            end_document(o);
        for (int i = 0; i < MAX_DATA; i++)
            if (session_data[i].session == o) {
                svc_cfrelease(session_data[i].value);
                session_data[i].session = 0;
            }
    } else if (tag == PRINTER) {
        return; /* the one printer stays */
    }
    wr32(o + O_TAG, 0);
    tb_DisposePtr(o);
}

/* PMRetain(object), PMRelease(object) */
static bool known(gaddr o)
{
    return is(o, PAGE_FORMAT) || is(o, PRINT_SETTINGS) || is(o, SESSION) || is(o, PRINTER);
}

static void pm_PMRetain(ppc_cpu *c)
{
    if (!known(A(0)))
        RET(paramErr);
    wr32(A(0) + O_COUNT, rd32(A(0) + O_COUNT) + 1);
    RET(noErr);
}

static void pm_PMRelease(ppc_cpu *c)
{
    gaddr o = A(0);
    if (!known(o))
        RET(paramErr);
    uint32_t n = rd32(o + O_COUNT);
    if (n > 1)
        wr32(o + O_COUNT, n - 1);
    else
        dispose_object(o);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Sessions */

static void pm_PMCreateSession(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    gaddr s = new_session();
    wr32(A(0), s);
    RET(s ? noErr : memFullErr);
}

/* A session's error: its own, else the Printing Manager's. */
static int32_t session_error(gaddr s)
{
    int32_t e = (int32_t)rd32(s + SS_ERROR);
    if (!e)
        e = tb_PrError();
    return e == iPrAbort ? kPMCancel : e;
}

static void pm_PMSessionError(ppc_cpu *c) { RET(is(A(0), SESSION) ? session_error(A(0)) : kPMInvalidPrintSession); }

static void pm_PMSessionSetError(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    wr32(A(0) + SS_ERROR, A(1));
    tb_PrSetError((int16_t)A(1));
    RET(noErr);
}

/* The CarbonLib 1.0 calls' session. */
static gaddr global(void)
{
    if (!global_session)
        global_session = new_session();
    return global_session;
}

static void pm_PMBegin(ppc_cpu *c)
{
    if (!global())
        RET(memFullErr);
    global_count++;
    RET(noErr);
}

static void pm_PMEnd(ppc_cpu *c)
{
    if (global_count > 0 && --global_count == 0 && global_session) {
        dispose_object(global_session);
        global_session = 0;
    }
    tb_PrClose();
    RET(noErr);
}

static void pm_PMError(ppc_cpu *c) { RET(global() ? session_error(global()) : tb_PrError()); }

static void pm_PMSetError(ppc_cpu *c)
{
    if (global())
        wr32(global() + SS_ERROR, A(0));
    tb_PrSetError((int16_t)A(0));
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Page formats */

static void pm_PMCreatePageFormat(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    gaddr o = new_page_format();
    wr32(A(0), o);
    RET(o ? noErr : memFullErr);
}

static void pm_PMDisposeObject(ppc_cpu *c)
{
    if (known(A(0)))
        dispose_object(A(0));
    RET(noErr);
}

static int32_t default_page_format(gaddr pf)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    tb_PrintDefault(print_of(pf));
    svc_wrd(pf + PF_SCALE, 100.0);
    return noErr;
}

/* PMSessionDefaultPageFormat(session, pageFormat), PMDefaultPageFormat(pageFormat) */
static void pm_PMSessionDefaultPageFormat(ppc_cpu *c) { RET(default_page_format(A(1))); }
static void pm_PMDefaultPageFormat(ppc_cpu *c) { RET(default_page_format(A(0))); }

/* Validating: *result says whether anything changed. */
static int32_t validate(gaddr o, uint32_t tag, gaddr result)
{
    if (!is(o, tag))
        return tag == PAGE_FORMAT ? kPMInvalidPageFormat : kPMInvalidPrintSettings;
    int8_t changed = tb_PrValidate(print_of(o));
    if (result)
        wr8(result, changed ? 1 : 0);
    return noErr;
}

static void pm_PMSessionValidatePageFormat(ppc_cpu *c) { RET(validate(A(1), PAGE_FORMAT, A(2))); }
static void pm_PMValidatePageFormat(ppc_cpu *c) { RET(validate(A(0), PAGE_FORMAT, A(1))); }
static void pm_PMSessionValidatePrintSettings(ppc_cpu *c) { RET(validate(A(1), PRINT_SETTINGS, A(2))); }
static void pm_PMValidatePrintSettings(ppc_cpu *c) { RET(validate(A(0), PRINT_SETTINGS, A(1))); }

/* PMCopyPageFormat(source, destination) */
static void pm_PMCopyPageFormat(ppc_cpu *c)
{
    gaddr src = A(0), dst = A(1);
    if (!is(src, PAGE_FORMAT) || !is(dst, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    gmemcpy(rd32(print_of(dst)), rd32(print_of(src)), TPRINT_SIZE);
    svc_wrd(dst + PF_SCALE, svc_rdd(src + PF_SCALE));
    RET(noErr);
}

static double scale_of(gaddr pf) { return svc_rdd(pf + PF_SCALE); }

/* The resolution a document is drawn at: the page format's, scaled. */
static void document_res(gaddr pf, int *vres, int *hres)
{
    gaddr p = rd32(print_of(pf));
    double s = scale_of(pf) > 0 ? scale_of(pf) / 100 : 1;
    int v = (int)((int16_t)rd16(p + PR_VRES) / s + 0.5), h = (int)((int16_t)rd16(p + PR_HRES) / s + 0.5);
    *vres = v < 72 ? 72 : v > 600 ? 600 : v;
    *hres = h < 72 ? 72 : h > 600 ? 600 : h;
}

static void put_rect(gaddr r, double top, double left, double bottom, double right)
{
    svc_wrd(r, top);
    svc_wrd(r + 8, left);
    svc_wrd(r + 16, bottom);
    svc_wrd(r + 24, right);
}

/* PMGetAdjustedPageRect, PMGetAdjustedPaperRect: the document's
 * coordinates (resolution and scale applied), the page's top left 0,0. */
static int32_t adjusted(gaddr pf, gaddr out, bool paper)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (!out)
        return paramErr;
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    int vres, hres;
    document_res(pf, &vres, &hres);
    double sv = vres / 72.0, sh = hres / 72.0;
    if (paper)
        put_rect(out, -ps.top * sv, -ps.left * sh, (ps.paper_height - ps.top) * sv, (ps.paper_width - ps.left) * sh);
    else
        put_rect(out, 0, 0, (ps.bottom - ps.top) * sv, (ps.right - ps.left) * sh);
    return noErr;
}

static void pm_PMGetAdjustedPageRect(ppc_cpu *c) { RET(adjusted(A(0), A(1), false)); }
static void pm_PMGetAdjustedPaperRect(ppc_cpu *c) { RET(adjusted(A(0), A(1), true)); }

/* The unadjusted ones: points, unscaled. */
static int32_t unadjusted(gaddr pf, gaddr out, bool paper)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (!out)
        return paramErr;
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    if (paper)
        put_rect(out, -ps.top, -ps.left, ps.paper_height - ps.top, ps.paper_width - ps.left);
    else
        put_rect(out, 0, 0, ps.bottom - ps.top, ps.right - ps.left);
    return noErr;
}

static void pm_PMGetUnadjustedPageRect(ppc_cpu *c) { RET(unadjusted(A(0), A(1), false)); }
static void pm_PMGetUnadjustedPaperRect(ppc_cpu *c) { RET(unadjusted(A(0), A(1), true)); }

/* PMGetPhysicalPaperSize, PMGetPhysicalPageSize: the sizes, at 0,0. */
static int32_t physical(gaddr pf, gaddr out, bool paper)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (!out)
        return paramErr;
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    if (paper)
        put_rect(out, 0, 0, ps.paper_height, ps.paper_width);
    else
        put_rect(out, 0, 0, ps.bottom - ps.top, ps.right - ps.left);
    return noErr;
}

static void pm_PMGetPhysicalPaperSize(ppc_cpu *c) { RET(physical(A(0), A(1), true)); }
static void pm_PMGetPhysicalPageSize(ppc_cpu *c) { RET(physical(A(0), A(1), false)); }

/* Page setup changed in points, kept at the record's resolution. */
static void put_setup(gaddr pf, const machine_page_setup *ps)
{
    gaddr p = rd32(print_of(pf));
    print_put_setup(p, ps, (int16_t)rd16(p + PR_VRES), (int16_t)rd16(p + PR_HRES));
}

static bool sane(const machine_page_setup *ps)
{
    return ps->paper_width >= 1 && ps->paper_height >= 1 && ps->paper_width < 32000 / 8.34 &&
           ps->paper_height < 32000 / 8.34 && ps->left >= 0 && ps->top >= 0 && ps->right > ps->left &&
           ps->bottom > ps->top && ps->right <= ps->paper_width && ps->bottom <= ps->paper_height;
}

/* PMSetPhysicalPaperSize(pf, paperSize): the margins kept. */
static void pm_PMSetPhysicalPaperSize(ppc_cpu *c)
{
    gaddr pf = A(0), r = A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!r)
        RET(paramErr);
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    double rm = ps.paper_width - ps.right, bm = ps.paper_height - ps.bottom;
    ps.paper_height = svc_rdd(r + 16) - svc_rdd(r);
    ps.paper_width = svc_rdd(r + 24) - svc_rdd(r + 8);
    ps.right = ps.paper_width - rm;
    ps.bottom = ps.paper_height - bm;
    if (!sane(&ps))
        RET(kPMValueOutOfRange);
    put_setup(pf, &ps);
    RET(noErr);
}

/* PMSetUnadjustedPaperRect(pf, paperRect): the paper around the page,
 * which starts at 0,0. */
static void pm_PMSetUnadjustedPaperRect(ppc_cpu *c)
{
    gaddr pf = A(0), r = A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!r)
        RET(paramErr);
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    double w = ps.right - ps.left, h = ps.bottom - ps.top;
    ps.top = -svc_rdd(r);
    ps.left = -svc_rdd(r + 8);
    ps.paper_height = svc_rdd(r + 16) - svc_rdd(r);
    ps.paper_width = svc_rdd(r + 24) - svc_rdd(r + 8);
    ps.right = ps.left + w;
    ps.bottom = ps.top + h;
    if (!sane(&ps))
        RET(kPMValueOutOfRange);
    put_setup(pf, &ps);
    RET(noErr);
}

/* PMSetAdjustedPageRect(pf, pageRect): the page's size, in the
 * document's coordinates. */
static void pm_PMSetAdjustedPageRect(ppc_cpu *c)
{
    gaddr pf = A(0), r = A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!r)
        RET(paramErr);
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    int vres, hres;
    document_res(pf, &vres, &hres);
    ps.right = ps.left + (svc_rdd(r + 24) - svc_rdd(r + 8)) * 72 / hres;
    ps.bottom = ps.top + (svc_rdd(r + 16) - svc_rdd(r)) * 72 / vres;
    if (!sane(&ps))
        RET(kPMValueOutOfRange);
    put_setup(pf, &ps);
    RET(noErr);
}

/* PMGetResolution(pf, &res), PMSetResolution(pf, &res): a LaserWriter's
 * choices, through PrGeneral. */
static void pm_PMGetResolution(ppc_cpu *c)
{
    gaddr pf = A(0), r = A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!r)
        RET(paramErr);
    gaddr p = rd32(print_of(pf));
    svc_wrd(r, (int16_t)rd16(p + PR_HRES));
    svc_wrd(r + 8, (int16_t)rd16(p + PR_VRES));
    RET(noErr);
}

static void pm_PMSetResolution(ppc_cpu *c)
{
    gaddr pf = A(0), r = A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!r)
        RET(paramErr);
    double h = svc_rdd(r), v = svc_rdd(r + 8);
    if (!(h >= 72 && h <= 600 && v >= 72 && v <= 600))
        RET(kPMValueOutOfRange);
    gaddr blk = tb_NewPtr(16, false, true);
    wr16(blk, 5); /* setRslOp */
    wr32(blk + 8, print_of(pf));
    wr16(blk + 12, (uint16_t)(int)(h + 0.5));
    wr16(blk + 14, (uint16_t)(int)(v + 0.5));
    tb_PrGeneral(blk);
    int16_t e = (int16_t)rd16(blk + 2);
    tb_DisposePtr(blk);
    RET(e ? kPMValueOutOfRange : noErr);
}

/* PMGetScale(pf, &scale), PMSetScale(pf, scale): percent. */
static void pm_PMGetScale(ppc_cpu *c)
{
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!A(1))
        RET(paramErr);
    svc_wrd(A(1), scale_of(A(0)));
    RET(noErr);
}

static void pm_PMSetScale(ppc_cpu *c)
{
    double s = ppc_fpr(c, 1);
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!(s >= 1 && s <= 10000))
        RET(kPMValueOutOfRange);
    svc_wrd(A(0) + PF_SCALE, s);
    RET(noErr);
}

/* PMGetOrientation(pf, &orientation), PMSetOrientation(pf, orientation, lock) */
static void pm_PMGetOrientation(ppc_cpu *c)
{
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (!A(1))
        RET(paramErr);
    machine_page_setup ps;
    print_get_setup(rd32(print_of(A(0))), &ps);
    wr16(A(1), ps.landscape ? kPMLandscape : kPMPortrait);
    RET(noErr);
}

static void pm_PMSetOrientation(ppc_cpu *c)
{
    gaddr pf = A(0);
    int o = (uint16_t)A(1);
    if (!is(pf, PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    if (o < kPMPortrait || o > kPMReverseLandscape)
        RET(kPMValueOutOfRange);
    bool landscape = o == kPMLandscape || o == kPMReverseLandscape;
    machine_page_setup ps;
    print_get_setup(rd32(print_of(pf)), &ps);
    if (landscape != ps.landscape && ps.paper_width != ps.paper_height) { /* the paper turned */
        machine_page_setup t = ps;
        t.paper_width = ps.paper_height;
        t.paper_height = ps.paper_width;
        t.left = ps.top;
        t.top = ps.paper_width - ps.right;
        t.right = ps.bottom;
        t.bottom = ps.paper_width - ps.left;
        t.landscape = landscape;
        put_setup(pf, &t);
    }
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Print settings */

static void pm_PMCreatePrintSettings(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    gaddr o = new_print_settings();
    wr32(A(0), o);
    RET(o ? noErr : memFullErr);
}

static int32_t default_print_settings(gaddr ps)
{
    if (!is(ps, PRINT_SETTINGS))
        return kPMInvalidPrintSettings;
    tb_PrintDefault(print_of(ps));
    wr32(ps + PS_COLOR, kPMColor);
    wr32(ps + PS_MIN, 1);
    wr32(ps + PS_MAX, (uint32_t)kPMPrintAllPages);
    return noErr;
}

static void pm_PMSessionDefaultPrintSettings(ppc_cpu *c) { RET(default_print_settings(A(1))); }
static void pm_PMDefaultPrintSettings(ppc_cpu *c) { RET(default_print_settings(A(0))); }

static void pm_PMCopyPrintSettings(ppc_cpu *c)
{
    gaddr src = A(0), dst = A(1);
    if (!is(src, PRINT_SETTINGS) || !is(dst, PRINT_SETTINGS))
        RET(kPMInvalidPrintSettings);
    gmemcpy(rd32(print_of(dst)), rd32(print_of(src)), TPRINT_SIZE);
    gmemcpy(dst + PS_NAME, src + PS_NAME, PS_EXT - PS_NAME);
    RET(noErr);
}

#define SETTINGS_ARG(o)                                                                                                \
    gaddr o = A(0);                                                                                                    \
    if (!is(o, PRINT_SETTINGS))                                                                                        \
        RET(kPMInvalidPrintSettings);

static uint32_t job16(gaddr ps, uint32_t off) { return (uint16_t)rd16(rd32(print_of(ps)) + off); }
static void set_job16(gaddr ps, uint32_t off, uint32_t v) { wr16(rd32(print_of(ps)) + off, (uint16_t)v); }

/* Pages: the record's first and last, 9999 being all of them. */
static void pm_PMGetFirstPage(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), job16(ps, PR_FST));
    RET(noErr);
}

static void pm_PMGetLastPage(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), job16(ps, PR_LST));
    RET(noErr);
}

static uint32_t page_number(uint32_t n) { return n == (uint32_t)kPMPrintAllPages || n > ALL_PAGES ? ALL_PAGES : n; }

static void pm_PMSetFirstPage(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (A(1) == 0)
        RET(kPMValueOutOfRange);
    set_job16(ps, PR_FST, page_number(A(1)));
    RET(noErr);
}

static void pm_PMSetLastPage(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (A(1) == 0)
        RET(kPMValueOutOfRange);
    set_job16(ps, PR_LST, page_number(A(1)));
    RET(noErr);
}

/* PMGetPageRange(ps, &min, &max), PMSetPageRange(ps, min, max): the pages
 * there are, which the first and last pages take. */
static void pm_PMGetPageRange(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1) || !A(2))
        RET(paramErr);
    wr32(A(1), rd32(ps + PS_MIN));
    wr32(A(2), rd32(ps + PS_MAX));
    RET(noErr);
}

static void pm_PMSetPageRange(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    uint32_t lo = A(1), hi = A(2);
    if (lo == 0 || (hi != (uint32_t)kPMPrintAllPages && hi < lo))
        RET(kPMValueOutOfRange);
    wr32(ps + PS_MIN, lo);
    wr32(ps + PS_MAX, hi);
    set_job16(ps, PR_FST, page_number(lo));
    set_job16(ps, PR_LST, page_number(hi));
    RET(noErr);
}

static void pm_PMGetCopies(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), job16(ps, PR_COPIES));
    RET(noErr);
}

static void pm_PMSetCopies(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (A(1) < 1 || A(1) > 9999)
        RET(kPMValueOutOfRange);
    set_job16(ps, PR_COPIES, A(1));
    RET(noErr);
}

static void pm_PMGetColorMode(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    wr16(A(1), (uint16_t)rd32(ps + PS_COLOR));
    RET(noErr);
}

static void pm_PMSetColorMode(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    uint32_t m = (uint16_t)A(1);
    if (m < kPMBlackAndWhite || m > 5)
        RET(kPMValueOutOfRange);
    wr32(ps + PS_COLOR, m);
    RET(noErr);
}

/* PMGetJobName(ps, name), PMSetJobName(ps, name): Pascal strings. */
static void pm_PMGetJobName(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    gmemcpy(A(1), ps + PS_NAME, (uint32_t)rd8(ps + PS_NAME) + 1);
    RET(noErr);
}

static void pm_PMSetJobName(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    gmemcpy(ps + PS_NAME, A(1), (uint32_t)rd8(A(1)) + 1);
    RET(noErr);
}

/* PMGetJobNameCFString(ps, &name): a new string the caller releases. */
static void pm_PMGetJobNameCFString(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    gaddr s = svc_cfstring_pascal(ps + PS_NAME);
    wr32(A(1), s);
    RET(s ? noErr : kPMGeneralError);
}

static void pm_PMSetJobNameCFString(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (!A(1))
        RET(paramErr);
    uint8_t p[256];
    svc_cfstring_to_pascal(A(1), p);
    gput(ps + PS_NAME, p, (uint32_t)p[0] + 1);
    RET(noErr);
}

/* PMGetDestination(ps, &type, &url): the printer (the host's print panel
 * may save a PDF, which the program isn't told). */
static void pm_PMGetDestination(ppc_cpu *c)
{
    SETTINGS_ARG(ps);
    if (A(1))
        wr16(A(1), kPMDestinationPrinter);
    if (A(2))
        wr32(A(2), 0);
    RET(noErr);
}

/* PMSetProfile(ps, tag, profile): ColorSync profiles don't reach the
 * host's printing. */
static void pm_PMSetProfile(ppc_cpu *c) { RET(is(A(0), PRINT_SETTINGS) ? kPMNotImplemented : kPMInvalidPrintSettings); }

/* ---------------------------------------------------------------------- */
/* Extended data: (id, size, data padded to 4) entries in a handle. */

static int32_t ext_get(gaddr h, uint32_t id, gaddr size, gaddr data)
{
    if (!size)
        return paramErr;
    int32_t total = h ? tb_GetHandleSize(h) : 0, pos = 0;
    while (pos + 8 <= total) {
        gaddr q = rd32(h) + (uint32_t)pos;
        uint32_t n = rd32(q + 4);
        if (rd32(q) == id) {
            if (data) {
                uint32_t want = rd32(size);
                gmemcpy(data, q + 8, want < n ? want : n);
            }
            wr32(size, n);
            return noErr;
        }
        pos += 8 + (int32_t)((n + 3) & ~3u);
    }
    return kPMNoSuchEntry;
}

static int32_t ext_set(gaddr o, uint32_t off, uint32_t id, uint32_t size, gaddr data)
{
    if (!data && size)
        return paramErr;
    gaddr h = rd32(o + off);
    if (!h) {
        h = tb_NewHandle(0, false, true);
        if (!h)
            return memFullErr;
        wr32(o + off, h);
    }
    /* Out with the old entry, then the new one at the end. */
    int32_t total = tb_GetHandleSize(h), pos = 0;
    while (pos + 8 <= total) {
        gaddr q = rd32(h) + (uint32_t)pos;
        int32_t len = 8 + (int32_t)((rd32(q + 4) + 3) & ~3u);
        if (rd32(q) == id) {
            gmemcpy(q, q + (uint32_t)len, (uint32_t)(total - pos - len));
            total -= len;
            tb_SetHandleSize(h, total);
            break;
        }
        pos += len;
    }
    int32_t len = 8 + (int32_t)((size + 3) & ~3u);
    tb_SetHandleSize(h, total + len);
    if (tb_GetHandleSize(h) != total + len)
        return memFullErr;
    gaddr q = rd32(h) + (uint32_t)total;
    wr32(q, id);
    wr32(q + 4, size);
    gmemcpy(q + 8, data, size);
    return noErr;
}

/* PMGet/SetPageFormatExtendedData(pf, id, size, data); the getter with no
 * buffer gives the size. */
static void pm_PMGetPageFormatExtendedData(ppc_cpu *c)
{
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    RET(ext_get(rd32(A(0) + PF_EXT), A(1), A(2), A(3)));
}

static void pm_PMSetPageFormatExtendedData(ppc_cpu *c)
{
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    RET(ext_set(A(0), PF_EXT, A(1), A(2), A(3)));
}

static void pm_PMGetPrintSettingsExtendedData(ppc_cpu *c)
{
    if (!is(A(0), PRINT_SETTINGS))
        RET(kPMInvalidPrintSettings);
    RET(ext_get(rd32(A(0) + PS_EXT), A(1), A(2), A(3)));
}

static void pm_PMSetPrintSettingsExtendedData(ppc_cpu *c)
{
    if (!is(A(0), PRINT_SETTINGS))
        RET(kPMInvalidPrintSettings);
    RET(ext_set(A(0), PS_EXT, A(1), A(2), A(3)));
}

/* ---------------------------------------------------------------------- */
/* Flattening: 'PMfl', the object's tag, its print record, then its own
 * fields (page format: the scale; print settings: name, colour, range),
 * then its extended data. */

static int32_t flatten(gaddr o, uint32_t tag, gaddr out)
{
    if (!is(o, tag))
        return tag == PAGE_FORMAT ? kPMInvalidPageFormat : kPMInvalidPrintSettings;
    if (!out)
        return paramErr;
    uint32_t own = tag == PAGE_FORMAT ? PF_EXT - PF_SCALE : PS_EXT - PS_NAME;
    uint32_t own_off = tag == PAGE_FORMAT ? PF_SCALE : PS_NAME;
    gaddr ext = rd32(o + (tag == PAGE_FORMAT ? PF_EXT : PS_EXT));
    int32_t next = ext ? tb_GetHandleSize(ext) : 0;
    gaddr h = tb_NewHandle((int32_t)(8 + TPRINT_SIZE + own) + next, false, true);
    if (!h)
        return memFullErr;
    gaddr p = rd32(h);
    wr32(p, FLAT);
    wr32(p + 4, tag);
    gmemcpy(p + 8, rd32(print_of(o)), TPRINT_SIZE);
    gmemcpy(p + 8 + TPRINT_SIZE, o + own_off, own);
    if (next)
        gmemcpy(p + 8 + TPRINT_SIZE + own, rd32(ext), (uint32_t)next);
    wr32(out, h);
    return noErr;
}

static int32_t unflatten(gaddr h, uint32_t tag, gaddr out)
{
    if (!out)
        return paramErr;
    wr32(out, 0);
    uint32_t own = tag == PAGE_FORMAT ? PF_EXT - PF_SCALE : PS_EXT - PS_NAME;
    uint32_t own_off = tag == PAGE_FORMAT ? PF_SCALE : PS_NAME;
    int32_t size = h && rd32(h) ? tb_GetHandleSize(h) : 0;
    if (size < (int32_t)(8 + TPRINT_SIZE + own) || rd32(rd32(h)) != FLAT || rd32(rd32(h) + 4) != tag)
        return tag == PAGE_FORMAT ? kPMInvalidPageFormat : kPMInvalidPrintSettings;
    gaddr o = tag == PAGE_FORMAT ? new_page_format() : new_print_settings();
    if (!o)
        return memFullErr;
    gaddr p = rd32(h);
    gmemcpy(rd32(print_of(o)), p + 8, TPRINT_SIZE);
    gmemcpy(o + own_off, p + 8 + TPRINT_SIZE, own);
    int32_t next = size - (int32_t)(8 + TPRINT_SIZE + own);
    if (next > 0) {
        gaddr ext = tb_NewHandle(next, false, true);
        if (ext) {
            gmemcpy(rd32(ext), rd32(h) + 8 + TPRINT_SIZE + own, (uint32_t)next);
            wr32(o + (tag == PAGE_FORMAT ? PF_EXT : PS_EXT), ext);
        }
    }
    wr32(out, o);
    return noErr;
}

static void pm_PMFlattenPageFormat(ppc_cpu *c) { RET(flatten(A(0), PAGE_FORMAT, A(1))); }
static void pm_PMUnflattenPageFormat(ppc_cpu *c) { RET(unflatten(A(0), PAGE_FORMAT, A(1))); }
static void pm_PMFlattenPrintSettings(ppc_cpu *c) { RET(flatten(A(0), PRINT_SETTINGS, A(1))); }
static void pm_PMUnflattenPrintSettings(ppc_cpu *c) { RET(unflatten(A(0), PRINT_SETTINGS, A(1))); }

/* ---------------------------------------------------------------------- */
/* Old print records */

/* A record with the page format's page setup and the settings' job. */
static void combine(gaddr dst, gaddr ps, gaddr pf)
{
    if (pf)
        gmemcpy(rd32(dst), rd32(print_of(pf)), TPRINT_SIZE);
    if (ps)
        gmemcpy(rd32(dst) + PR_JOB, rd32(print_of(ps)) + PR_JOB, PR_JOB_LEN);
}

/* And back: the job to the settings, the page setup to the format. */
static void split(gaddr src, gaddr ps, gaddr pf)
{
    if (ps)
        gmemcpy(rd32(print_of(ps)) + PR_JOB, rd32(src) + PR_JOB, PR_JOB_LEN);
    if (pf) {
        gaddr save = tb_NewPtr(PR_JOB_LEN, false, true);
        gmemcpy(save, rd32(print_of(pf)) + PR_JOB, PR_JOB_LEN);
        gmemcpy(rd32(print_of(pf)), rd32(src), TPRINT_SIZE);
        gmemcpy(rd32(print_of(pf)) + PR_JOB, save, PR_JOB_LEN);
        tb_DisposePtr(save);
    }
}

/* PMMakeOldPrintRecord(ps, pf, &handle) */
static int32_t make_old(gaddr ps, gaddr pf, gaddr out)
{
    if (!is(ps, PRINT_SETTINGS))
        return kPMInvalidPrintSettings;
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (!out)
        return paramErr;
    gaddr h = tb_NewHandle(TPRINT_SIZE, false, true);
    if (!h)
        return memFullErr;
    combine(h, ps, pf);
    wr32(out, h);
    return noErr;
}

static void pm_PMMakeOldPrintRecord(ppc_cpu *c) { RET(make_old(A(0), A(1), A(2))); }
static void pm_PMSessionMakeOldPrintRecord(ppc_cpu *c) { RET(make_old(A(1), A(2), A(3))); }

/* PMConvertOldPrintRecord(handle, &ps, &pf) */
static int32_t convert_old(gaddr h, gaddr ps_out, gaddr pf_out)
{
    if (!h || !rd32(h) || tb_GetHandleSize(h) < TPRINT_SIZE || !ps_out || !pf_out)
        return paramErr;
    gaddr ps = new_print_settings(), pf = new_page_format();
    if (!ps || !pf)
        return memFullErr;
    gmemcpy(rd32(print_of(ps)), rd32(h), TPRINT_SIZE);
    gmemcpy(rd32(print_of(pf)), rd32(h), TPRINT_SIZE);
    tb_PrValidate(print_of(ps));
    tb_PrValidate(print_of(pf));
    wr32(ps_out, ps);
    wr32(pf_out, pf);
    return noErr;
}

static void pm_PMConvertOldPrintRecord(ppc_cpu *c) { RET(convert_old(A(0), A(1), A(2))); }
static void pm_PMSessionConvertOldPrintRecord(ppc_cpu *c) { RET(convert_old(A(1), A(2), A(3))); }

/* PMSessionGeneral(session, pData), PMGeneral(pData) */
static void pm_PMSessionGeneral(ppc_cpu *c)
{
    if (!A(1))
        RET(paramErr);
    tb_PrGeneral(A(1));
    RET(tb_PrError());
}

static void pm_PMGeneral(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    tb_PrGeneral(A(0));
    RET(tb_PrError());
}

/* ---------------------------------------------------------------------- */
/* Dialogs */

/* PMSessionPageSetupDialog(session, pf, &accepted): the host's page
 * layout panel. */
static int32_t page_setup_dialog(gaddr pf, gaddr accepted)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    int8_t ok = tb_PrStlDialog(print_of(pf));
    if (accepted)
        wr8(accepted, ok ? 1 : 0);
    return noErr;
}

static void pm_PMSessionPageSetupDialog(ppc_cpu *c) { RET(page_setup_dialog(A(1), A(2))); }
static void pm_PMPageSetupDialog(ppc_cpu *c) { RET(page_setup_dialog(A(0), A(1))); }

/* PMSessionPrintDialog(session, ps, pf, &accepted): as PrJobDialog, which
 * says yes (the host's print panel comes when the pages are drawn). */
static int32_t print_dialog(gaddr ps, gaddr pf, gaddr accepted)
{
    if (!is(ps, PRINT_SETTINGS))
        return kPMInvalidPrintSettings;
    if (pf && !is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    gaddr h = tb_NewHandle(TPRINT_SIZE, false, true);
    if (!h)
        return memFullErr;
    combine(h, ps, pf ? pf : 0);
    if (!pf)
        tb_PrValidate(h);
    int8_t ok = tb_PrJobDialog(h);
    split(h, ps, 0);
    tb_DisposeHandle(h);
    if (accepted)
        wr8(accepted, ok ? 1 : 0);
    return noErr;
}

static void pm_PMSessionPrintDialog(ppc_cpu *c) { RET(print_dialog(A(1), A(2), A(3))); }
static void pm_PMPrintDialog(ppc_cpu *c) { RET(print_dialog(A(0), A(1), A(2))); }

/* The Init calls: the classic TPrDlg, as the PMDialog.  A print dialog's
 * record is its own copy, put back into the settings when the dialog's
 * Main finishes. */
static int32_t print_dialog_init(gaddr ps, gaddr pf, gaddr out)
{
    if (!is(ps, PRINT_SETTINGS))
        return kPMInvalidPrintSettings;
    if (!out)
        return paramErr;
    gaddr h = tb_NewHandle(TPRINT_SIZE, false, true);
    if (!h)
        return memFullErr;
    combine(h, ps, is(pf, PAGE_FORMAT) ? pf : 0);
    tb_PrValidate(h);
    gaddr d = tb_PrJobInit(h);
    wr32(out, d);
    return d ? noErr : memFullErr;
}

static int32_t page_setup_init(gaddr pf, gaddr out)
{
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (!out)
        return paramErr;
    gaddr d = tb_PrStlInit(print_of(pf));
    wr32(out, d);
    return d ? noErr : memFullErr;
}

static void pm_PMSessionPrintDialogInit(ppc_cpu *c) { RET(print_dialog_init(A(1), A(2), A(3))); }
static void pm_PMPrintDialogInit(ppc_cpu *c) { RET(print_dialog_init(A(0), 0, A(1))); }
static void pm_PMPrintDialogInitWithPageFormat(ppc_cpu *c) { RET(print_dialog_init(A(0), A(1), A(2))); }
static void pm_PMSessionPageSetupDialogInit(ppc_cpu *c) { RET(page_setup_init(A(1), A(2))); }
static void pm_PMPageSetupDialogInit(ppc_cpu *c) { RET(page_setup_init(A(0), A(1))); }

/* PrDlgMain's init function: the dialog already built. */
static void init_trampoline(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* the print record */
    wr32(c->a[7], pending_dialog);
    cpu_push32(c, ret);
}

/* Run a dialog the program's init procedure built (calling our Init and
 * adding to it), or ours when it built none. */
static int32_t dialog_main(gaddr obj, gaddr ps, gaddr pf, gaddr accepted, gaddr init, bool job)
{
    gaddr var = tb_NewPtr(4, false, true);
    if (!var)
        return memFullErr;
    if (init) {
        uint32_t args[2] = { obj, var };
        svc_call_upp(init, uppPMDialogInitProcInfo, args, 2);
    }
    gaddr d = rd32(var);
    tb_DisposePtr(var);
    int32_t e = noErr;
    if (!d) {
        gaddr tmp = tb_NewPtr(4, false, true);
        e = job ? print_dialog_init(ps, pf, tmp) : page_setup_init(pf, tmp);
        d = rd32(tmp);
        tb_DisposePtr(tmp);
        if (e != noErr)
            return e;
    }
    gaddr h = rd32(d + DLG_PRINT);
    if (!init_stub)
        init_stub = native_register(init_trampoline, "PMDialogInit", 0);
    pending_dialog = d;
    int8_t ok = tb_PrDlgMain(h, init_stub);
    pending_dialog = 0;
    if (job) {
        split(h, ps, 0);
        tb_DisposeHandle(h);
    }
    if (accepted)
        wr8(accepted, ok ? 1 : 0);
    return noErr;
}

/* PMSessionPrintDialogMain(session, ps, pf, &accepted, initProc) */
static void pm_PMSessionPrintDialogMain(ppc_cpu *c)
{
    if (!is(A(1), PRINT_SETTINGS))
        RET(kPMInvalidPrintSettings);
    RET(dialog_main(A(1), A(1), A(2), A(3), A(4), true));
}

static void pm_PMPrintDialogMain(ppc_cpu *c)
{
    if (!is(A(0), PRINT_SETTINGS))
        RET(kPMInvalidPrintSettings);
    RET(dialog_main(A(0), A(0), A(1), A(2), A(3), true));
}

/* PMSessionPageSetupDialogMain(session, pf, &accepted, initProc) */
static void pm_PMSessionPageSetupDialogMain(ppc_cpu *c)
{
    if (!is(A(1), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    RET(dialog_main(A(1), 0, A(1), A(2), A(3), false));
}

static void pm_PMPageSetupDialogMain(ppc_cpu *c)
{
    if (!is(A(0), PAGE_FORMAT))
        RET(kPMInvalidPageFormat);
    RET(dialog_main(A(0), 0, A(0), A(1), A(2), false));
}

/* PMSessionUseSheets(session, window, sheetDone): sheets are Mac OS X's;
 * the dialogs stay modal. */
static void pm_PMSessionUseSheets(ppc_cpu *c) { RET(is(A(0), SESSION) ? kPMNotImplemented : kPMInvalidPrintSession); }

/* The PMDialog's fields: the TPrDlg's. */
static void pm_PMGetDialogPtr(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(paramErr);
    wr32(A(1), A(0));
    RET(noErr);
}

static void pm_PMGetModalFilterProc(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(paramErr);
    wr32(A(1), rd32(A(0) + DLG_FILTER));
    RET(noErr);
}

static void pm_PMSetModalFilterProc(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr32(A(0) + DLG_FILTER, A(1));
    RET(noErr);
}

/* The item procedure: ours is 68k code, handed out as a UPP the program
 * can invoke from its own. */
static void pm_PMGetItemProc(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(paramErr);
    wr32(A(1), svc_upp_for_68k(rd32(A(0) + DLG_ITEM), uppPMItemProcInfo));
    RET(noErr);
}

static void pm_PMSetItemProc(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr32(A(0) + DLG_ITEM, A(1));
    RET(noErr);
}

static void pm_PMGetDialogAccepted(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(paramErr);
    wr8(A(1), rd8(A(0) + DLG_DOIT) ? 1 : 0);
    RET(noErr);
}

static void pm_PMSetDialogAccepted(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr8(A(0) + DLG_DOIT, (A(1) & 0xFF) ? 1 : 0);
    RET(noErr);
}

static void pm_PMGetDialogDone(ppc_cpu *c)
{
    if (!A(0) || !A(1))
        RET(paramErr);
    wr8(A(1), rd8(A(0) + DLG_DONE) ? 1 : 0);
    RET(noErr);
}

static void pm_PMSetDialogDone(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr8(A(0) + DLG_DONE, (A(1) & 0xFF) ? 1 : 0);
    RET(noErr);
}

/* ---------------------------------------------------------------------- */
/* Documents and pages */

/* PMSessionBeginDocument(session, ps, pf): the printing port opened on a
 * record of the two, at the page format's scaled resolution. */
static int32_t begin_document(gaddr s, gaddr ps, gaddr pf)
{
    if (!is(s, SESSION))
        return kPMInvalidPrintSession;
    if (!is(ps, PRINT_SETTINGS))
        return kPMInvalidPrintSettings;
    if (!is(pf, PAGE_FORMAT))
        return kPMInvalidPageFormat;
    if (rd32(s + SS_PORT))
        return kPMOutOfScope;
    gaddr h = tb_NewHandle(TPRINT_SIZE, false, true);
    if (!h)
        return memFullErr;
    combine(h, ps, pf);
    tb_PrValidate(h);
    int vres, hres;
    document_res(pf, &vres, &hres);
    machine_page_setup setup;
    print_get_setup(rd32(h), &setup);
    print_put_setup(rd32(h), &setup, vres, hres);
    wr32(s + SS_ERROR, 0);
    tb_PrSetError(noErr);
    gaddr port = tb_PrOpenDoc(h, 0, 0);
    if (!port) {
        tb_DisposeHandle(h);
        return tb_PrError() ? tb_PrError() : kPMGeneralError;
    }
    wr32(s + SS_PORT, port);
    wr32(s + SS_PRINT, h);
    wr8(s + SS_PAGE, 0);
    return noErr;
}

static void end_document(gaddr s)
{
    gaddr port = rd32(s + SS_PORT);
    if (!port)
        return;
    if (rd8(s + SS_PAGE))
        tb_PrClosePage(port);
    if (rd32(s + SS_IDLE)) /* printing is the program's to wait for */
        svc_call_upp(rd32(s + SS_IDLE), uppPMIdleProcInfo, NULL, 0);
    tb_PrCloseDoc(port);
    tb_DisposeHandle(rd32(s + SS_PRINT));
    wr32(s + SS_PORT, 0);
    wr32(s + SS_PRINT, 0);
    wr8(s + SS_PAGE, 0);
}

static int32_t end_document_call(gaddr s)
{
    if (!is(s, SESSION))
        return kPMInvalidPrintSession;
    if (!rd32(s + SS_PORT))
        return kPMOutOfScope;
    end_document(s);
    return session_error(s);
}

/* PMSessionBeginPage(session, pf, pageFrame): the port current. */
static int32_t begin_page(gaddr s)
{
    if (!is(s, SESSION))
        return kPMInvalidPrintSession;
    gaddr port = rd32(s + SS_PORT);
    if (!port || rd8(s + SS_PAGE))
        return kPMOutOfScope;
    tb_PrOpenPage(port, 0);
    wr8(s + SS_PAGE, 1);
    tb_SetPort(port);
    return session_error(s);
}

static int32_t end_page(gaddr s)
{
    if (!is(s, SESSION))
        return kPMInvalidPrintSession;
    if (!rd32(s + SS_PORT) || !rd8(s + SS_PAGE))
        return kPMOutOfScope;
    tb_PrClosePage(rd32(s + SS_PORT));
    wr8(s + SS_PAGE, 0);
    return session_error(s);
}

static void pm_PMSessionBeginDocument(ppc_cpu *c) { RET(begin_document(A(0), A(1), A(2))); }
static void pm_PMSessionEndDocument(ppc_cpu *c) { RET(end_document_call(A(0))); }
static void pm_PMSessionBeginPage(ppc_cpu *c) { RET(begin_page(A(0))); }
static void pm_PMSessionEndPage(ppc_cpu *c) { RET(end_page(A(0))); }

/* PMBeginDocument(ps, pf, &context): the context is our session. */
static void pm_PMBeginDocument(ppc_cpu *c)
{
    if (!A(2))
        RET(paramErr);
    gaddr s = global();
    int32_t e = begin_document(s, A(0), A(1));
    wr32(A(2), e == noErr ? s : 0);
    RET(e);
}

static void pm_PMEndDocument(ppc_cpu *c) { RET(end_document_call(A(0))); }
static void pm_PMBeginPage(ppc_cpu *c) { RET(begin_page(A(0))); }
static void pm_PMEndPage(ppc_cpu *c) { RET(end_page(A(0))); }

/* PMGetGrafPtr(context, &port) */
static void pm_PMGetGrafPtr(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), rd32(A(0) + SS_PORT));
    RET(rd32(A(0) + SS_PORT) ? noErr : kPMOutOfScope);
}

/* PMSessionGetGraphicsContext(session, type, &context): a QuickDraw port
 * (Core Graphics contexts are Mac OS X's). */
static bool quickdraw_type(gaddr type)
{
    return !type || svc_cfstring_equals(type, "com.apple.graphicscontext.quickdraw") ||
           svc_cfstring_equals(type, "com.apple.graphicscontext.default");
}

static void pm_PMSessionGetGraphicsContext(ppc_cpu *c)
{
    gaddr s = A(0), out = A(2);
    if (!is(s, SESSION))
        RET(kPMInvalidPrintSession);
    if (!out)
        RET(paramErr);
    wr32(out, 0);
    if (!quickdraw_type(A(1)))
        RET(kPMNotImplemented);
    if (!rd32(s + SS_PORT))
        RET(kPMOutOfScope);
    wr32(out, rd32(s + SS_PORT));
    RET(noErr);
}

/* Document formats: PICT drawn with QuickDraw (the default) is what the
 * Printing Manager takes. */
static bool pict_format(gaddr f)
{
    return !f || svc_cfstring_equals(f, "com.apple.documentformat.default") ||
           svc_cfstring_equals(f, "application/vnd.apple.printing-pict");
}

static void pm_PMSessionIsDocumentFormatSupported(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!A(2))
        RET(paramErr);
    wr8(A(2), pict_format(A(1)) ? 1 : 0);
    RET(noErr);
}

/* A CFArray of the format we take. */
static int32_t format_array(gaddr out)
{
    static const char fmt[] = "application/vnd.apple.printing-pict";
    uint16_t u[sizeof fmt];
    for (size_t i = 0; i + 1 < sizeof fmt; i++)
        u[i] = (uint8_t)fmt[i];
    gaddr s = svc_cfstring(u, sizeof fmt - 1), vals = tb_NewPtr(4, false, true);
    if (!s || !vals)
        return kPMGeneralError;
    wr32(vals, s);
    uint32_t args[4] = { 0, vals, 1, 0 };
    gaddr a = cfm_export_fn("CFArrayCreate") ? svc_call_export("CFArrayCreate", args, 4) : 0;
    tb_DisposePtr(vals);
    if (!a) {
        svc_cfrelease(s);
        return kPMNotImplemented; /* no CFArray to answer with */
    }
    wr32(out, a);
    return noErr;
}

static void pm_PMSessionGetDocumentFormatGeneration(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!A(1))
        RET(paramErr);
    RET(format_array(A(1)));
}

static void pm_PMSessionGetDocumentFormatSupported(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!A(1))
        RET(paramErr);
    RET(format_array(A(1)));
}

/* PMSessionSetDocumentFormatGeneration(session, format, contexts, options) */
static void pm_PMSessionSetDocumentFormatGeneration(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!pict_format(A(1)))
        RET(kPMNotImplemented);
    gaddr contexts = A(2);
    if (contexts && cfm_export_fn("CFArrayGetCount")) {
        uint32_t a[1] = { contexts };
        uint32_t n = svc_call_export("CFArrayGetCount", a, 1);
        for (uint32_t i = 0; i < n && i < 64; i++) {
            uint32_t b[2] = { contexts, i };
            if (!quickdraw_type(svc_call_export("CFArrayGetValueAtIndex", b, 2)))
                RET(kPMNotImplemented);
        }
    }
    RET(noErr);
}

/* PMSessionSetIdleProc(session, idle), PMSetIdleProc(idle) */
static void pm_PMSessionSetIdleProc(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    wr32(A(0) + SS_IDLE, A(1));
    RET(noErr);
}

static void pm_PMSetIdleProc(ppc_cpu *c)
{
    if (!global())
        RET(memFullErr);
    wr32(global() + SS_IDLE, A(0));
    RET(noErr);
}

/* PMSessionSetDataInSession(session, key, data), PMSessionGetDataFromSession(session, key, &data) */
static int data_slot(gaddr s, gaddr key, bool make)
{
    uint16_t k[64];
    uint32_t n = svc_cfstring_get(key, k, 64);
    if (n > 64)
        n = 64;
    int free_slot = -1;
    for (int i = 0; i < MAX_DATA; i++) {
        if (session_data[i].session == s && session_data[i].keylen == n &&
            !memcmp(session_data[i].key, k, n * sizeof *k))
            return i;
        if (!session_data[i].session && free_slot < 0)
            free_slot = i;
    }
    if (!make || free_slot < 0)
        return -1;
    session_data[free_slot].session = s;
    session_data[free_slot].keylen = n;
    memcpy(session_data[free_slot].key, k, n * sizeof *k);
    session_data[free_slot].value = 0;
    return free_slot;
}

static void pm_PMSessionSetDataInSession(ppc_cpu *c)
{
    gaddr s = A(0), key = A(1), data = A(2);
    if (!is(s, SESSION))
        RET(kPMInvalidPrintSession);
    if (!key || !data)
        RET(paramErr);
    int i = data_slot(s, key, true);
    if (i < 0)
        RET(memFullErr);
    svc_cfretain(data);
    svc_cfrelease(session_data[i].value);
    session_data[i].value = data;
    RET(noErr);
}

static void pm_PMSessionGetDataFromSession(ppc_cpu *c)
{
    gaddr s = A(0), key = A(1), out = A(2);
    if (!is(s, SESSION))
        RET(kPMInvalidPrintSession);
    if (!key || !out)
        RET(paramErr);
    int i = data_slot(s, key, false);
    wr32(out, i >= 0 ? session_data[i].value : 0);
    RET(i >= 0 ? noErr : kPMNoSuchEntry);
}

/* ---------------------------------------------------------------------- */
/* The printer: the host's, through PDF.  Not a PostScript driver, so
 * PostScript passed through and injected has nowhere to go. */

static gaddr the_printer(void)
{
    if (!printer)
        printer = new_object(PRINTER, 8);
    return printer;
}

static void pm_PMSessionGetCurrentPrinter(ppc_cpu *c)
{
    if (!is(A(0), SESSION))
        RET(kPMInvalidPrintSession);
    if (!A(1))
        RET(paramErr);
    wr32(A(1), the_printer());
    RET(noErr);
}

static void pm_PMPrinterGetDescriptionURL(ppc_cpu *c)
{
    if (A(2))
        wr32(A(2), 0);
    RET(is(A(0), PRINTER) ? kPMNotImplemented : kPMInvalidPrinter); /* no PPD */
}

static void pm_PMIsPostScriptDriver(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr8(A(0), 0);
    RET(noErr);
}

static void pm_not_postscript(ppc_cpu *c) { RET(kPMNotImplemented); }

/* PMLanguageInfo: level, version, release (Str32s): PDF's. */
static int32_t language_info(gaddr info)
{
    if (!info)
        return paramErr;
    gmemset(info, 0, 99);
    static const char *const fields[] = { "PDF", "1.3", "1" };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(fields[i]);
        wr8(info + 33 * (uint32_t)i, (uint8_t)n);
        gput(info + 33 * (uint32_t)i + 1, fields[i], (uint32_t)n);
    }
    return noErr;
}

static void pm_PMGetLanguageInfo(ppc_cpu *c) { RET(language_info(A(0))); }
static void pm_PMPrinterGetLanguageInfo(ppc_cpu *c) { RET(is(A(0), PRINTER) ? language_info(A(1)) : kPMInvalidPrinter); }

/* The driver: ours, 'bbox' 1.0. */
static int32_t driver_creator(gaddr out)
{
    if (!out)
        return paramErr;
    wr32(out, 0x62626f78u);
    return noErr;
}

static int32_t driver_release(gaddr vers)
{
    if (!vers)
        return paramErr;
    gmemset(vers, 0, 6);
    wr8(vers, 1);
    wr8(vers + 2, 0x80); /* final */
    static const char v[] = "1.0";
    wr8(vers + 6, 3);
    gput(vers + 7, v, 3);
    wr8(vers + 10, 3);
    gput(vers + 11, v, 3);
    return noErr;
}

static void pm_PMGetDriverCreator(ppc_cpu *c) { RET(driver_creator(A(0))); }
static void pm_PMPrinterGetDriverCreator(ppc_cpu *c) { RET(is(A(0), PRINTER) ? driver_creator(A(1)) : kPMInvalidPrinter); }
static void pm_PMGetDriverReleaseInfo(ppc_cpu *c) { RET(driver_release(A(0))); }
static void pm_PMPrinterGetDriverReleaseInfo(ppc_cpu *c)
{
    RET(is(A(0), PRINTER) ? driver_release(A(1)) : kPMInvalidPrinter);
}

/* Resolutions: PrGeneral's (72, 144, 300 and 600 dpi). */
static const double resolutions[] = { 72, 144, 300, 600 };
enum { NRES = 4 };

static int32_t res_count(gaddr out)
{
    if (!out)
        return paramErr;
    wr32(out, NRES);
    return noErr;
}

static int32_t res_by_tag(uint32_t tag, gaddr res)
{
    if (!res)
        return paramErr;
    double v;
    switch (tag) {
    case kPMMinRange:
    case kPMMinSquareResolution:
    case kPMMinimumValue:
        v = resolutions[0];
        break;
    case kPMMaxRange:
    case kPMMaxSquareResolution:
    case kPMMaximumValue:
        v = resolutions[NRES - 1];
        break;
    case kPMDefaultResolution:
    case kPMDefaultValue:
    case kPMCurrentValue:
        v = 72;
        break;
    default:
        return kPMValueOutOfRange;
    }
    svc_wrd(res, v);
    svc_wrd(res + 8, v);
    return noErr;
}

static int32_t res_by_index(uint32_t i, gaddr res)
{
    if (!res)
        return paramErr;
    if (i < 1 || i > NRES)
        return kPMValueOutOfRange;
    svc_wrd(res, resolutions[i - 1]);
    svc_wrd(res + 8, resolutions[i - 1]);
    return noErr;
}

static void pm_PMGetPrinterResolutionCount(ppc_cpu *c) { RET(res_count(A(0))); }
static void pm_PMPrinterGetPrinterResolutionCount(ppc_cpu *c) { RET(is(A(0), PRINTER) ? res_count(A(1)) : kPMInvalidPrinter); }
static void pm_PMGetPrinterResolution(ppc_cpu *c) { RET(res_by_tag(A(0), A(1))); }
static void pm_PMPrinterGetPrinterResolution(ppc_cpu *c)
{
    RET(is(A(0), PRINTER) ? res_by_tag(A(1), A(2)) : kPMInvalidPrinter);
}
static void pm_PMGetIndexedPrinterResolution(ppc_cpu *c) { RET(res_by_index(A(0), A(1))); }
static void pm_PMPrinterGetIndexedPrinterResolution(ppc_cpu *c)
{
    RET(is(A(0), PRINTER) ? res_by_index(A(1), A(2)) : kPMInvalidPrinter);
}

/* ColorSync: the host matches colours itself; nothing to switch. */
static void pm_colorsync(ppc_cpu *c) { RET(noErr); }

const ppc_export carbon_print_exports[] = {
    { "PMBegin", pm_PMBegin },
    { "PMBeginDocument", pm_PMBeginDocument },
    { "PMBeginPage", pm_PMBeginPage },
    { "PMConvertOldPrintRecord", pm_PMConvertOldPrintRecord },
    { "PMCopyPageFormat", pm_PMCopyPageFormat },
    { "PMCopyPrintSettings", pm_PMCopyPrintSettings },
    { "PMCreatePageFormat", pm_PMCreatePageFormat },
    { "PMCreatePrintSettings", pm_PMCreatePrintSettings },
    { "PMCreateSession", pm_PMCreateSession },
    { "PMDefaultPageFormat", pm_PMDefaultPageFormat },
    { "PMDefaultPrintSettings", pm_PMDefaultPrintSettings },
    { "PMDisableColorSync", pm_colorsync },
    { "PMDisposePageFormat", pm_PMDisposeObject },
    { "PMDisposePrintSettings", pm_PMDisposeObject },
    { "PMEnableColorSync", pm_colorsync },
    { "PMEnd", pm_PMEnd },
    { "PMEndDocument", pm_PMEndDocument },
    { "PMEndPage", pm_PMEndPage },
    { "PMError", pm_PMError },
    { "PMFlattenPageFormat", pm_PMFlattenPageFormat },
    { "PMFlattenPrintSettings", pm_PMFlattenPrintSettings },
    { "PMGeneral", pm_PMGeneral },
    { "PMGetAdjustedPageRect", pm_PMGetAdjustedPageRect },
    { "PMGetAdjustedPaperRect", pm_PMGetAdjustedPaperRect },
    { "PMGetColorMode", pm_PMGetColorMode },
    { "PMGetCopies", pm_PMGetCopies },
    { "PMGetDestination", pm_PMGetDestination },
    { "PMGetDialogAccepted", pm_PMGetDialogAccepted },
    { "PMGetDialogDone", pm_PMGetDialogDone },
    { "PMGetDialogPtr", pm_PMGetDialogPtr },
    { "PMGetDriverCreator", pm_PMGetDriverCreator },
    { "PMGetDriverReleaseInfo", pm_PMGetDriverReleaseInfo },
    { "PMGetFirstPage", pm_PMGetFirstPage },
    { "PMGetGrafPtr", pm_PMGetGrafPtr },
    { "PMGetIndexedPrinterResolution", pm_PMGetIndexedPrinterResolution },
    { "PMGetItemProc", pm_PMGetItemProc },
    { "PMGetJobName", pm_PMGetJobName },
    { "PMGetJobNameCFString", pm_PMGetJobNameCFString },
    { "PMGetLanguageInfo", pm_PMGetLanguageInfo },
    { "PMGetLastPage", pm_PMGetLastPage },
    { "PMGetModalFilterProc", pm_PMGetModalFilterProc },
    { "PMGetOrientation", pm_PMGetOrientation },
    { "PMGetPageFormatExtendedData", pm_PMGetPageFormatExtendedData },
    { "PMGetPageRange", pm_PMGetPageRange },
    { "PMGetPhysicalPageSize", pm_PMGetPhysicalPageSize },
    { "PMGetPhysicalPaperSize", pm_PMGetPhysicalPaperSize },
    { "PMGetPrintSettingsExtendedData", pm_PMGetPrintSettingsExtendedData },
    { "PMGetPrinterResolution", pm_PMGetPrinterResolution },
    { "PMGetPrinterResolutionCount", pm_PMGetPrinterResolutionCount },
    { "PMGetResolution", pm_PMGetResolution },
    { "PMGetScale", pm_PMGetScale },
    { "PMGetUnadjustedPageRect", pm_PMGetUnadjustedPageRect },
    { "PMGetUnadjustedPaperRect", pm_PMGetUnadjustedPaperRect },
    { "PMIsPostScriptDriver", pm_PMIsPostScriptDriver },
    { "PMMakeOldPrintRecord", pm_PMMakeOldPrintRecord },
    { "PMNewPageFormat", pm_PMCreatePageFormat },
    { "PMNewPrintSettings", pm_PMCreatePrintSettings },
    { "PMPageSetupDialog", pm_PMPageSetupDialog },
    { "PMPageSetupDialogInit", pm_PMPageSetupDialogInit },
    { "PMPageSetupDialogMain", pm_PMPageSetupDialogMain },
    { "PMPostScriptBegin", pm_not_postscript },
    { "PMPostScriptData", pm_not_postscript },
    { "PMPostScriptEnd", pm_not_postscript },
    { "PMPostScriptFile", pm_not_postscript },
    { "PMPostScriptHandle", pm_not_postscript },
    { "PMPrintDialog", pm_PMPrintDialog },
    { "PMPrintDialogInit", pm_PMPrintDialogInit },
    { "PMPrintDialogInitWithPageFormat", pm_PMPrintDialogInitWithPageFormat },
    { "PMPrintDialogMain", pm_PMPrintDialogMain },
    { "PMPrinterGetDescriptionURL", pm_PMPrinterGetDescriptionURL },
    { "PMPrinterGetDriverCreator", pm_PMPrinterGetDriverCreator },
    { "PMPrinterGetDriverReleaseInfo", pm_PMPrinterGetDriverReleaseInfo },
    { "PMPrinterGetIndexedPrinterResolution", pm_PMPrinterGetIndexedPrinterResolution },
    { "PMPrinterGetLanguageInfo", pm_PMPrinterGetLanguageInfo },
    { "PMPrinterGetPrinterResolution", pm_PMPrinterGetPrinterResolution },
    { "PMPrinterGetPrinterResolutionCount", pm_PMPrinterGetPrinterResolutionCount },
    { "PMRelease", pm_PMRelease },
    { "PMRetain", pm_PMRetain },
    { "PMSessionBeginDocument", pm_PMSessionBeginDocument },
    { "PMSessionBeginPage", pm_PMSessionBeginPage },
    { "PMSessionConvertOldPrintRecord", pm_PMSessionConvertOldPrintRecord },
    { "PMSessionDefaultPageFormat", pm_PMSessionDefaultPageFormat },
    { "PMSessionDefaultPrintSettings", pm_PMSessionDefaultPrintSettings },
    { "PMSessionDisableColorSync", pm_colorsync },
    { "PMSessionEnableColorSync", pm_colorsync },
    { "PMSessionEndDocument", pm_PMSessionEndDocument },
    { "PMSessionEndPage", pm_PMSessionEndPage },
    { "PMSessionError", pm_PMSessionError },
    { "PMSessionGeneral", pm_PMSessionGeneral },
    { "PMSessionGetCurrentPrinter", pm_PMSessionGetCurrentPrinter },
    { "PMSessionGetDataFromSession", pm_PMSessionGetDataFromSession },
    { "PMSessionGetDocumentFormatGeneration", pm_PMSessionGetDocumentFormatGeneration },
    { "PMSessionGetDocumentFormatSupported", pm_PMSessionGetDocumentFormatSupported },
    { "PMSessionGetGraphicsContext", pm_PMSessionGetGraphicsContext },
    { "PMSessionIsDocumentFormatSupported", pm_PMSessionIsDocumentFormatSupported },
    { "PMSessionMakeOldPrintRecord", pm_PMSessionMakeOldPrintRecord },
    { "PMSessionPageSetupDialog", pm_PMSessionPageSetupDialog },
    { "PMSessionPageSetupDialogInit", pm_PMSessionPageSetupDialogInit },
    { "PMSessionPageSetupDialogMain", pm_PMSessionPageSetupDialogMain },
    { "PMSessionPostScriptBegin", pm_not_postscript },
    { "PMSessionPostScriptData", pm_not_postscript },
    { "PMSessionPostScriptEnd", pm_not_postscript },
    { "PMSessionPostScriptFile", pm_not_postscript },
    { "PMSessionPostScriptHandle", pm_not_postscript },
    { "PMSessionPrintDialog", pm_PMSessionPrintDialog },
    { "PMSessionPrintDialogInit", pm_PMSessionPrintDialogInit },
    { "PMSessionPrintDialogMain", pm_PMSessionPrintDialogMain },
    { "PMSessionSetDataInSession", pm_PMSessionSetDataInSession },
    { "PMSessionSetDocumentFormatGeneration", pm_PMSessionSetDocumentFormatGeneration },
    { "PMSessionSetError", pm_PMSessionSetError },
    { "PMSessionSetIdleProc", pm_PMSessionSetIdleProc },
    { "PMSessionSetPSInjectionData", pm_not_postscript },
    { "PMSessionUseSheets", pm_PMSessionUseSheets },
    { "PMSessionValidatePageFormat", pm_PMSessionValidatePageFormat },
    { "PMSessionValidatePrintSettings", pm_PMSessionValidatePrintSettings },
    { "PMSetAdjustedPageRect", pm_PMSetAdjustedPageRect },
    { "PMSetColorMode", pm_PMSetColorMode },
    { "PMSetCopies", pm_PMSetCopies },
    { "PMSetDialogAccepted", pm_PMSetDialogAccepted },
    { "PMSetDialogDone", pm_PMSetDialogDone },
    { "PMSetError", pm_PMSetError },
    { "PMSetFirstPage", pm_PMSetFirstPage },
    { "PMSetIdleProc", pm_PMSetIdleProc },
    { "PMSetItemProc", pm_PMSetItemProc },
    { "PMSetJobName", pm_PMSetJobName },
    { "PMSetJobNameCFString", pm_PMSetJobNameCFString },
    { "PMSetLastPage", pm_PMSetLastPage },
    { "PMSetModalFilterProc", pm_PMSetModalFilterProc },
    { "PMSetOrientation", pm_PMSetOrientation },
    { "PMSetPageFormatExtendedData", pm_PMSetPageFormatExtendedData },
    { "PMSetPageRange", pm_PMSetPageRange },
    { "PMSetPhysicalPaperSize", pm_PMSetPhysicalPaperSize },
    { "PMSetPrintSettingsExtendedData", pm_PMSetPrintSettingsExtendedData },
    { "PMSetProfile", pm_PMSetProfile },
    { "PMSetResolution", pm_PMSetResolution },
    { "PMSetScale", pm_PMSetScale },
    { "PMSetUnadjustedPaperRect", pm_PMSetUnadjustedPaperRect },
    { "PMUnflattenPageFormat", pm_PMUnflattenPageFormat },
    { "PMUnflattenPrintSettings", pm_PMUnflattenPrintSettings },
    { "PMValidatePageFormat", pm_PMValidatePageFormat },
    { "PMValidatePrintSettings", pm_PMValidatePrintSettings },
};
const unsigned carbon_print_export_count = sizeof carbon_print_exports / sizeof carbon_print_exports[0];
