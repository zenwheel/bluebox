/*
 * The small additions CarbonLib made to managers that are not otherwise
 * Carbon's: the Collection Manager's retain counts, the Resource Manager's
 * chain and fork-named files, the File Manager's POSIX paths, the Alias
 * Manager's NoUI calls, the Script Manager's ordering of text, Type Select,
 * the Text Services Manager's properties and documents, the Debugging
 * calls, the Process Manager's bundle location and names, Apple Help, the
 * Interface Builder runtime and Device Control.
 */
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

void intl_convert_text(gaddr text, int32_t len, bool upper, bool strip); /* intl.c */
/* Resources.h (rsrcmgr.c, hfsplus.c) */
int16_t tb_GetTopResourceFile(gaddr refNum);
int16_t tb_GetNextResourceFile(int16_t curRefNum, gaddr nextRefNum);
int16_t tb_DetachResourceFile(int16_t refNum);
int16_t tb_InsertResourceFile(int16_t refNum, int16_t where);
int16_t tb_FSpOpenOrphanResFile(gaddr spec, int8_t permission, gaddr refNum);
int16_t tb_FSOpenResourceFile(gaddr ref, uint32_t forkNameLength, gaddr forkName, int8_t permissions, gaddr refNum);
int16_t tb_FSCreateResourceFile(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t whichInfo, gaddr catalogInfo,
                                uint32_t forkNameLength, gaddr forkName, gaddr newRef, gaddr newSpec);
/* Collections.h (gx.c) */
int32_t tb_GetCollectionRetainCount(gaddr c);
gaddr tb_RetainCollection(gaddr c);
int32_t tb_ReleaseCollection(gaddr c);
/* TextServices.h (tsm.c) */
gaddr tb_TSMGetActiveDocument(void);
int16_t tb_TSMSetInlineInputRegion(gaddr doc, gaddr window, gaddr region);

#define RETERR(e) RET((uint32_t)(int32_t)(e))

/* ---------------------------------------------------------------------- */
/* Resources.h */

static void r_GetTopResourceFile(ppc_cpu *c) { RETERR(tb_GetTopResourceFile(A(0))); }
static void r_GetNextResourceFile(ppc_cpu *c) { RETERR(tb_GetNextResourceFile(S16(0), A(1))); }
static void r_DetachResourceFile(ppc_cpu *c) { RETERR(tb_DetachResourceFile(S16(0))); }
static void r_InsertResourceFile(ppc_cpu *c) { RETERR(tb_InsertResourceFile(S16(0), S16(1))); }
static void r_FSpOpenOrphanResFile(ppc_cpu *c) { RETERR(tb_FSpOpenOrphanResFile(A(0), S8(1), A(2))); }
static void r_FSOpenResourceFile(ppc_cpu *c) { RETERR(tb_FSOpenResourceFile(A(0), A(1), A(2), S8(3), A(4))); }
static void r_FSCreateResourceFile(ppc_cpu *c)
{
    RETERR(tb_FSCreateResourceFile(A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8)));
}

/* ---------------------------------------------------------------------- */
/* Collections.h */

static void co_GetCollectionRetainCount(ppc_cpu *c) { RET(tb_GetCollectionRetainCount(A(0))); }
static void co_RetainCollection(ppc_cpu *c) { RET(tb_RetainCollection(A(0))); }
static void co_ReleaseCollection(ppc_cpu *c) { RETERR(tb_ReleaseCollection(A(0))); }

/* ---------------------------------------------------------------------- */
/* MacMemory.h */

/* CheckAllHeaps(): the zones checked for damage (the damage is logged). */
static void mem_CheckAllHeaps(ppc_cpu *c)
{
    mm_check_zone(rd32(LM_SysZone));
    mm_check_zone(rd32(LM_ApplZone));
    DONE();
}

/* GetGrowZone(): the current zone's grow zone function. */
static void mem_GetGrowZone(ppc_cpu *c) { RET(Zone_gzProc(rd32(LM_TheZone))); }

/* ---------------------------------------------------------------------- */
/* Files.h: POSIX paths: "/Volume/Folder/File", the volume by its name, in
 * UTF-8, as Core Foundation's URLs spell them (carbon_cfurl.c). */

enum { pathTooLongErr = -2110, kCFURLPOSIXPathStyle = 0, kCFStringEncodingUTF8 = 0x08000100 };

/* FSPathMakeRef(path, ref, isDirectory) */
static void fs_FSPathMakeRef(ppc_cpu *c)
{
    char path[1024];
    if (!A(0) || !A(1))
        RETERR(paramErr);
    if (rest_cstr(A(0), path, sizeof path) >= sizeof path - 1)
        RETERR(pathTooLongErr);
    if (path[0] != '/')
        RETERR(paramErr);
    uint32_t a[3] = { 0, A(0), kCFStringEncodingUTF8 };
    gaddr str = svc_call_export("CFStringCreateWithCString", a, 3);
    if (!str)
        RETERR(memFullErr);
    uint32_t u[4] = { 0, str, kCFURLPOSIXPathStyle, 0 };
    gaddr url = svc_call_export("CFURLCreateWithFileSystemPath", u, 4);
    svc_cfrelease(str);
    if (!url)
        RETERR(memFullErr);
    uint32_t g[2] = { url, A(1) };
    bool found = (uint8_t)svc_call_export("CFURLGetFSRef", g, 2) != 0;
    svc_cfrelease(url);
    int16_t err = found ? noErr : fnfErr;
    char host[1024];
    if (!err && A(2))
        wr8(A(2), hfs_fsref_path(A(1), host, sizeof host) == noErr && fm_is_dir(host) ? 1 : 0);
    RETERR(err);
}

/* FSRefMakePath(ref, path, maxPathSize): the path with its end. */
static void fs_FSRefMakePath(ppc_cpu *c)
{
    char host[1024];
    int16_t err = hfs_fsref_path(A(0), host, sizeof host);
    if (err)
        RETERR(err);
    uint32_t a[2] = { 0, A(0) };
    gaddr url = svc_call_export("CFURLCreateFromFSRef", a, 2);
    if (!url)
        RETERR(fnfErr);
    uint32_t p[2] = { url, kCFURLPOSIXPathStyle };
    gaddr str = svc_call_export("CFURLCopyFileSystemPath", p, 2);
    svc_cfrelease(url);
    if (!str)
        RETERR(fnfErr);
    gaddr out = A(1);
    bool ok = out && A(2) > 0;
    if (ok) {
        uint32_t g[4] = { str, out, A(2), kCFStringEncodingUTF8 };
        ok = (uint8_t)svc_call_export("CFStringGetCString", g, 4) != 0;
    }
    svc_cfrelease(str);
    RETERR(ok ? noErr : pathTooLongErr);
}

/* ---------------------------------------------------------------------- */
/* Aliases.h: the calls that never put up a dialog (the others don't
 * here either: a volume that is not mounted is not asked for). */

static void al_MatchAliasNoUI(ppc_cpu *c)
{
    RETERR(tb_MatchAlias(A(0), (int32_t)A(1), A(2), A(3), A(4), A(5), A(6), A(7)));
}
static void al_ResolveAliasFileWithMountFlagsNoUI(ppc_cpu *c)
{
    RETERR(tb_ResolveAliasFileWithMountFlags(A(0), (int8_t)A(1), A(2), A(3), A(4)));
}

/* ---------------------------------------------------------------------- */
/* StringCompare.h: the Carbon names of the Pack 6 comparisons. */

/* IdenticalText(aPtr, bPtr, aLen, bLen, itl2Handle) */
static void sc_IdenticalText(ppc_cpu *c) { RET((int32_t)tb_IUMagIDPString(A(0), A(1), S16(2), S16(3), A(4))); }
/* TextOrder(aPtr, bPtr, aLen, bLen, aScript, bScript, aLang, bLang) */
static void sc_TextOrder(ppc_cpu *c)
{
    RET((int32_t)tb_IUTextOrder(A(0), A(1), S16(2), S16(3), S16(4), S16(5), S16(6), S16(7)));
}
/* StringOrder(aStr, bStr, aScript, bScript, aLang, bLang) */
static void sc_StringOrder(ppc_cpu *c)
{
    gaddr a = A(0), b = A(1);
    RET((int32_t)tb_IUTextOrder(a + 1, b + 1, rd8(a), rd8(b), S16(2), S16(3), S16(4), S16(5)));
}
/* LanguageOrder(language1, language2) */
static void sc_LanguageOrder(ppc_cpu *c) { RET((int32_t)tb_IULangOrder(S16(0), S16(1))); }

/* ---------------------------------------------------------------------- */
/* TypeSelect.h (the routines are in Pack 6: yaml, and so tb_TypeSelect...) */

enum { tsrLastKeyTime = 0, tsrScript = 4, tsrKeyStrokes = 6, kTypeSelectMax = 63 };
enum { tsPreviousSelectMode = -1, tsNormalSelectMode = 0, tsNextSelectMode = 1 };
enum { keyDown = 3, autoKey = 5, cmdKey = 0x0100, controlKey = 0x1000 };

void tb_TypeSelectClear(gaddr tsr)
{
    if (!tsr)
        return;
    wr32(tsr + tsrLastKeyTime, 0);
    wr16(tsr + tsrScript, 0);
    wr8(tsr + tsrKeyStrokes, 0);
}

/* A key that adds to what is being typed, or that doesn't (and the
 * typing is not touched): printable characters, with no Command or
 * Control key. */
int8_t tb_TypeSelectNewKey(gaddr event, gaddr tsr)
{
    if (!event || !tsr)
        return 0;
    uint16_t what = rd16(event), mods = rd16(event + 14);
    uint8_t ch = (uint8_t)rd32(event + 2);
    if ((what != keyDown && what != autoKey) || (mods & (cmdKey | controlKey)) || ch < 0x20 || ch == 0x7F)
        return 0;
    uint32_t when = rd32(event + 6), last = rd32(tsr + tsrLastKeyTime);
    uint32_t delay = 2u * (uint16_t)rd16(LM_KeyThresh);
    if (!delay)
        delay = 60;
    uint8_t n = rd8(tsr + tsrKeyStrokes);
    if (!last || when - last > delay || n > kTypeSelectMax)
        n = 0;
    if (n < kTypeSelectMax) {
        wr8(tsr + tsrKeyStrokes + 1 + n, ch);
        n++;
    }
    wr8(tsr + tsrKeyStrokes, n);
    wr32(tsr + tsrLastKeyTime, when);
    wr16(tsr + tsrScript, 0);
    return 1;
}

/* How the typing sorts against a string: 0 when the string begins with it
 * (case and marks aside), else -1 or 1 as the typing comes before or after. */
int16_t tb_TypeSelectCompare(gaddr tsr, int16_t script, gaddr str)
{
    if (!tsr || !str)
        return 0;
    int n = rd8(tsr + tsrKeyStrokes), m = rd8(str);
    int cut = m < n ? m : n;
    /* Upper case and without marks, both: case and marks don't count. */
    gaddr copy = tb_NewPtr(2 * kTypeSelectMax + 2, false, true);
    if (!copy)
        return 0;
    gmemset(copy, 0, 2 * kTypeSelectMax + 2);
    for (int i = 0; i < n; i++)
        wr8(copy + (uint32_t)i, rd8(tsr + tsrKeyStrokes + 1 + (uint32_t)i));
    for (int i = 0; i < cut; i++)
        wr8(copy + kTypeSelectMax + 1 + (uint32_t)i, rd8(str + 1 + (uint32_t)i));
    intl_convert_text(copy, n, true, true);
    intl_convert_text(copy + kTypeSelectMax + 1, cut, true, true);
    int16_t r = tb_IUMagString(copy, copy + kTypeSelectMax + 1, (int16_t)n, (int16_t)cut);
    tb_DisposePtr(copy);
    return r > 0 ? 1 : r < 0 ? -1 : 0;
}

/* TypeSelectFindItem(tsr, listSize, selectMode, getStringProc, data): the
 * item of a sorted list that the typing leads to: the first that does not
 * sort before it (the last when all do), or the one before or after that.
 * 0 when nothing is typed, or the list is empty. */
int16_t tb_TypeSelectFindItem(gaddr tsr, int16_t listSize, int16_t selectMode, gaddr proc, gaddr data)
{
    if (!tsr || !proc || listSize <= 0 || !rd8(tsr + tsrKeyStrokes))
        return 0;
    gaddr scratch = tb_NewPtr(8, false, true);
    if (!scratch)
        return 0;
    int found = listSize;
    for (int i = 1; i <= listSize; i++) {
        wr16(scratch, 0);
        wr32(scratch + 4, 0);
        uint32_t args[4] = { (uint32_t)i, scratch, scratch + 4, data };
        if (!(uint8_t)aem_call_proc(proc, 0x00003F90, args, 4) || !rd32(scratch + 4))
            continue;
        if (tb_TypeSelectCompare(tsr, (int16_t)rd16(scratch), rd32(scratch + 4)) <= 0) {
            found = i;
            break;
        }
    }
    tb_DisposePtr(scratch);
    if (selectMode == tsPreviousSelectMode)
        found = found > 1 ? found - 1 : 1;
    else if (selectMode == tsNextSelectMode)
        found = found < listSize ? found + 1 : listSize;
    return (int16_t)found;
}

/* ---------------------------------------------------------------------- */
/* Power.h */

/* MinimumProcessorSpeed(): the slowest the processor can be set to, in
 * megahertz; the processor here has one speed. */
int16_t tb_MinimumProcessorSpeed(void) { return tb_MaximumProcessorSpeed(); }

/* ---------------------------------------------------------------------- */
/* AEDataModel.h */

/* AEReplaceDescData(typeCode, dataPtr, dataSize, theAEDesc): the
 * descriptor's type and data replaced (a list or a record becomes data). */
static void ae_AEReplaceDescData(ppc_cpu *c)
{
    gaddr desc = A(3);
    if (!desc || (!A(1) && (int32_t)A(2) > 0) || (int32_t)A(2) < 0)
        RETERR(paramErr);
    gaddr tmp = tb_NewPtr(8, false, true);
    if (!tmp)
        RETERR(memFullErr);
    int16_t err = tb_AECreateDesc((int32_t)A(0), A(1), (int32_t)A(2), tmp);
    if (!err) {
        tb_AEDisposeDesc(desc);
        wr32(desc, rd32(tmp));
        wr32(desc + 4, rd32(tmp + 4));
    }
    tb_DisposePtr(tmp);
    RETERR(err);
}

/* ---------------------------------------------------------------------- */
/* TextServices.h, TSMTE.h */

enum { tsmInvalidDocIDErr = -2505 };
enum { kTextServiceSelProp = 0 };

/* A call of a text service's component instance: _ComponentDispatch with
 * the instance, then the parameters (sizes 4 or 2), the parameter size and
 * the selector on the stack. */
static uint32_t instance_call(ppc_cpu *c, int16_t what, const char *sizes, const uint32_t *args)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, args[0]);
    uint32_t size = 0;
    for (int i = 0; sizes[i]; i++) {
        int n = sizes[i] - '0';
        ppc_trap_push(n, args[1 + i]);
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    return r;
}

/* GetTextServiceProperty(ts, propertySelector, *result): selector 12. */
static void ts_GetTextServiceProperty(ppc_cpu *c)
{
    uint32_t a[3] = { A(0), A(1), A(2) };
    RET(instance_call(c, 12, "44", a));
}
/* SetTextServiceProperty(ts, propertySelector, value): selector 13. */
static void ts_SetTextServiceProperty(ppc_cpu *c)
{
    uint32_t a[3] = { A(0), A(1), A(2) };
    RET(instance_call(c, 13, "44", a));
}
/* TextServiceEventRef(ts, event): a Carbon event to the service, as the
 * one event TextServiceEvent takes (selector 6). */
static void ts_TextServiceEventRef(ppc_cpu *c)
{
    uint32_t a[3] = { A(0), 1, A(1) };
    RET(instance_call(c, 6, "24", a));
}
static void ts_TSMGetActiveDocument(ppc_cpu *c) { RET(tb_TSMGetActiveDocument()); }
static void ts_TSMSetInlineInputRegion(ppc_cpu *c) { RETERR(tb_TSMSetInlineInputRegion(A(0), A(1), A(2))); }

/* A TSMTE dialog: the dialog's document and its TSMTE record. */
enum { MAX_TSMTE = 16 };
static struct {
    gaddr dialog, doc, rec;
} tsmte[MAX_TSMTE];

static int tsmte_find(gaddr dialog, bool make)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_TSMTE; i++) {
        if (tsmte[i].dialog == dialog && dialog)
            return i;
        if (!tsmte[i].dialog && free_slot < 0)
            free_slot = i;
    }
    if (make && free_slot >= 0) {
        tsmte[free_slot].dialog = dialog;
        return free_slot;
    }
    return -1;
}

static void te_IsTSMTEDialog(ppc_cpu *c) { RET(tsmte_find(A(0), false) >= 0); }
static void te_GetTSMTEDialogDocumentID(ppc_cpu *c)
{
    int i = tsmte_find(A(0), false);
    RET(i >= 0 ? tsmte[i].doc : 0);
}
static void te_GetTSMTEDialogTSMTERecHandle(ppc_cpu *c)
{
    int i = tsmte_find(A(0), false);
    RET(i >= 0 ? tsmte[i].rec : 0);
}
static void te_SetTSMTEDialogDocumentID(ppc_cpu *c)
{
    int i = tsmte_find(A(0), true);
    if (i >= 0)
        tsmte[i].doc = A(1);
    DONE();
}
static void te_SetTSMTEDialogTSMTERecHandle(ppc_cpu *c)
{
    int i = tsmte_find(A(0), true);
    if (i >= 0)
        tsmte[i].rec = A(1);
    DONE();
}

/* ---------------------------------------------------------------------- */
/* Sound.h: Sound Manager 3.6 */

enum { siUnknownInfoType = -231 };

/* SoundManagerGetInfo(selector, infoPtr), SoundManagerSetInfo(selector,
 * infoPtr): the Sound Manager keeps no settings of its own beyond the
 * channels' and devices'. */
int16_t tb_SoundManagerGetInfo(int32_t selector, gaddr infoPtr) { return infoPtr ? siUnknownInfoType : paramErr; }
int16_t tb_SoundManagerSetInfo(int32_t selector, gaddr infoPtr) { return infoPtr ? siUnknownInfoType : paramErr; }

/* ---------------------------------------------------------------------- */
/* Processes.h: CarbonLib's GetProcessBundleLocation and CopyProcessName
 * (process.c knows the processes). */

int16_t process_bundle_location(gaddr psn, gaddr ref);
int16_t process_name(gaddr psn, uint8_t *name);
/* carbon_cfstring.c */
gaddr cfstr_new(gaddr alloc, const uint16_t *u, uint32_t n, bool mutable_);
bool cf_decode(uint32_t enc, const uint8_t *b, size_t n, uint16_t **out, uint32_t *nout);

static void pm_GetProcessBundleLocation(ppc_cpu *c) { RETERR(process_bundle_location(A(0), A(1))); }

static void pm_CopyProcessName(ppc_cpu *c)
{
    uint8_t name[256];
    if (!A(1))
        RETERR(paramErr);
    wr32(A(1), 0);
    int16_t err = process_name(A(0), name);
    if (err)
        RETERR(err);
    uint16_t *u = NULL;
    uint32_t n = 0;
    if (!cf_decode(0 /* Mac Roman */, name + 1, name[0], &u, &n))
        RETERR(paramErr);
    wr32(A(1), cfstr_new(0, u, n, false));
    free(u);
    RETERR(noErr);
}

/* ---------------------------------------------------------------------- */
/* AppleHelp.h: no Help Viewer, no help books. */

enum { kAHInternalErr = -10790 };
static void ah_AHGotoMainTOC(ppc_cpu *c) { RETERR(A(0) > 1 ? paramErr : kAHInternalErr); }
static void ah_AHGotoPage(ppc_cpu *c) { RETERR(A(0) ? kAHInternalErr : paramErr); }
static void ah_AHLookupAnchor(ppc_cpu *c) { RETERR(A(0) && A(1) ? kAHInternalErr : paramErr); }
static void ah_AHRegisterHelpBook(ppc_cpu *c) { RETERR(A(0) ? kAHInternalErr : paramErr); }
static void ah_AHSearch(ppc_cpu *c) { RETERR(A(0) && A(1) ? kAHInternalErr : paramErr); }

/* ---------------------------------------------------------------------- */
/* DeviceControl.h: no FireWire AV/C devices. */

static void dc_DeviceControlDoAVCTransaction(ppc_cpu *c) { RETERR(rest_noHardwareErr); }

/* ---------------------------------------------------------------------- */
/* IBCarbonRuntime.h: nib files are a Mac OS X bundle's; there is none. */

enum { kIBCarbonRuntimeCantFindNibFile = -10960 };

static void ib_CreateNibReference(ppc_cpu *c)
{
    if (A(1))
        wr32(A(1), 0);
    RETERR(A(0) && A(1) ? kIBCarbonRuntimeCantFindNibFile : paramErr);
}
static void ib_CreateNibReferenceWithCFBundle(ppc_cpu *c)
{
    if (A(2))
        wr32(A(2), 0);
    RETERR(A(0) && A(1) && A(2) ? kIBCarbonRuntimeCantFindNibFile : paramErr);
}
static void ib_DisposeNibReference(ppc_cpu *c) { DONE(); }
/* A nib reference is never made, so one given is not one. */
static void ib_FromNib1(ppc_cpu *c) { RETERR(paramErr); }
static void ib_FromNib2(ppc_cpu *c)
{
    if (A(2))
        wr32(A(2), 0);
    RETERR(paramErr);
}

void rest_misc_reset(void)
{
    memset(tsmte, 0, sizeof tsmte);
}

const ppc_export rest_misc_exports[] = {
    { "AHGotoMainTOC", ah_AHGotoMainTOC },
    { "AHGotoPage", ah_AHGotoPage },
    { "AHLookupAnchor", ah_AHLookupAnchor },
    { "AHRegisterHelpBook", ah_AHRegisterHelpBook },
    { "AHSearch", ah_AHSearch },
    { "AEReplaceDescData", ae_AEReplaceDescData },
    { "CheckAllHeaps", mem_CheckAllHeaps },
    { "CopyProcessName", pm_CopyProcessName },
    { "CreateMenuBarFromNib", ib_FromNib2 },
    { "CreateMenuFromNib", ib_FromNib2 },
    { "CreateNibReference", ib_CreateNibReference },
    { "CreateNibReferenceWithCFBundle", ib_CreateNibReferenceWithCFBundle },
    { "CreateWindowFromNib", ib_FromNib2 },
    { "DetachResourceFile", r_DetachResourceFile },
    { "DeviceControlDoAVCTransaction", dc_DeviceControlDoAVCTransaction },
    { "DisposeNibReference", ib_DisposeNibReference },
    { "FSCreateResourceFile", r_FSCreateResourceFile },
    { "FSOpenResourceFile", r_FSOpenResourceFile },
    { "FSPathMakeRef", fs_FSPathMakeRef },
    { "FSRefMakePath", fs_FSRefMakePath },
    { "FSpOpenOrphanResFile", r_FSpOpenOrphanResFile },
    { "GetCollectionRetainCount", co_GetCollectionRetainCount },
    { "GetGrowZone", mem_GetGrowZone },
    { "GetNextResourceFile", r_GetNextResourceFile },
    { "GetProcessBundleLocation", pm_GetProcessBundleLocation },
    { "GetTSMTEDialogDocumentID", te_GetTSMTEDialogDocumentID },
    { "GetTSMTEDialogTSMTERecHandle", te_GetTSMTEDialogTSMTERecHandle },
    { "GetTextServiceProperty", ts_GetTextServiceProperty },
    { "GetTopResourceFile", r_GetTopResourceFile },
    { "IdenticalText", sc_IdenticalText },
    { "InsertResourceFile", r_InsertResourceFile },
    { "IsTSMTEDialog", te_IsTSMTEDialog },
    { "LanguageOrder", sc_LanguageOrder },
    { "MatchAliasNoUI", al_MatchAliasNoUI },
    { "ReleaseCollection", co_ReleaseCollection },
    { "ResolveAliasFileWithMountFlagsNoUI", al_ResolveAliasFileWithMountFlagsNoUI },
    { "RetainCollection", co_RetainCollection },
    { "SetMenuBarFromNib", ib_FromNib1 },
    { "SetTSMTEDialogDocumentID", te_SetTSMTEDialogDocumentID },
    { "SetTSMTEDialogTSMTERecHandle", te_SetTSMTEDialogTSMTERecHandle },
    { "SetTextServiceProperty", ts_SetTextServiceProperty },
    { "StringOrder", sc_StringOrder },
    { "TSMGetActiveDocument", ts_TSMGetActiveDocument },
    { "TSMSetInlineInputRegion", ts_TSMSetInlineInputRegion },
    { "TextOrder", sc_TextOrder },
    { "TextServiceEventRef", ts_TextServiceEventRef },
};
const unsigned rest_misc_export_count = sizeof rest_misc_exports / sizeof rest_misc_exports[0];
