/*
 * The Open Scripting Architecture (M14): the AppleScript component
 * ('osa ' 'ascr', and the generic scripting component 'osa ' 'scpt') and
 * the applet shell ('aplt' 'scpt') that AppleScript applets open, both
 * our own, on our own AppleScript runtime (applescript.c).
 *
 * An applet's CODE is a stub: it opens the applet component, calls it with
 * selector 1 and quits when that returns (without the component it beeps
 * twice).  The component is the applet: it loads the script ('scpt' 128),
 * shows the startup screen (TEXT 1128) unless told not to, takes the Open
 * Application and Open Documents events to run the script's run and open
 * handlers, and stays open when the applet says so ('pref' 128), calling
 * the idle handler and running the quit handler at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "as_internal.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

enum {
    errOSASystemError = -1750,
    errOSAInvalidID = -1751,
    errOSABadStorageType = -1752,
    errOSAScriptError = -1753,
    errOSABadSelector = -1754,
    errOSASourceNotAvailable = -1756,
    errAEEventNotHandled = -1708,
    badComponentSelector = (int32_t)0x80008002,
};

/* The OSA's selectors (OSA.h's, as the 68k glue's inline code has them). */
enum {
    kOSASelectLoad = 0x0001, kOSASelectStore = 0x0002, kOSASelectExecute = 0x0003, kOSASelectDisplay = 0x0004,
    kOSASelectScriptError = 0x0005, kOSASelectDispose = 0x0006, kOSASelectSetScriptInfo = 0x0007,
    kOSASelectGetScriptInfo = 0x0008, kOSASelectSetActiveProc = 0x0009, kOSASelectGetActiveProc = 0x000A,
    kOSASelectScriptingComponentName = 0x0102, kOSASelectCompile = 0x0103, kOSASelectCopyID = 0x0104,
    kOSASelectGetSource = 0x0201, kOSASelectCoerceFromDesc = 0x0301, kOSASelectCoerceToDesc = 0x0302,
    kOSASelectSetSendProc = 0x0401, kOSASelectGetSendProc = 0x0402, kOSASelectSetCreateProc = 0x0403,
    kOSASelectGetCreateProc = 0x0404, kOSASelectSetDefaultTarget = 0x0405, kOSASelectStartRecording = 0x0501,
    kOSASelectStopRecording = 0x0502, kOSASelectLoadExecute = 0x0601, kOSASelectCompileExecute = 0x0602,
    kOSASelectDoScript = 0x0603, kOSASelectSetResumeDispatchProc = 0x0801,
    kOSASelectGetResumeDispatchProc = 0x0802, kOSASelectExecuteEvent = 0x0803, kOSASelectDoEvent = 0x0804,
    kOSASelectMakeContext = 0x0805,
};

/* The procedures a client set (kept and given back; our runtime sends its
 * events itself, through aem_script_send). */
static uint32_t active_proc, active_refcon, send_proc, send_refcon, create_proc, create_refcon, resume_proc,
    resume_refcon;

/* A descriptor of text or a long into a guest AEDesc. */
static int16_t put_desc(uint32_t type, const void *data, uint32_t n, gaddr out)
{
    gaddr p = tb_NewPtr(n ? (int32_t)n : 1, false, false);
    if (n)
        gput(p, data, n);
    int16_t err = tb_AECreateDesc((int32_t)type, p, (int32_t)n, out);
    tb_DisposePtr(p);
    return err;
}

static int16_t put_long(int32_t v, gaddr out)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return put_desc(T("long"), b, 4, out);
}

/* A script's data from an AEDesc (OSALoad's 'scpt' descriptor). */
static int32_t load_desc(gaddr desc, uint32_t *id)
{
    gaddr h = rd32(desc + AEDesc_dataHandle_off);
    if (!h || !rd32(h))
        return errOSABadStorageType;
    int32_t n = tb_GetHandleSize(h);
    uint8_t *data = malloc((size_t)(n > 0 ? n : 1));
    gget(data, rd32(h), (uint32_t)n);
    int32_t err = as_load_script(data, (size_t)n, id);
    free(data);
    return err ? (err == asErrCorruptData ? -1702 : err) : noErr;
}

static int32_t script_error(int32_t err) { return err ? errOSAScriptError : noErr; }

/* The handler for an Apple Event in a script (OSAExecuteEvent, OSADoEvent):
 * its direct parameter passed on. */
static int32_t execute_event(uint32_t context, gaddr evt, uint32_t *result)
{
    gaddr d = tb_NewPtr(AEDesc_SIZEOF, false, true), type = tb_NewPtr(8, false, true);
    uint32_t cls = 0, id = 0;
    tb_AEGetAttributePtr(evt, (int32_t)T("evcl"), (int32_t)T("type"), type, type + 4, 4, 0);
    cls = rd32(type + 4);
    tb_AEGetAttributePtr(evt, (int32_t)T("evid"), (int32_t)T("type"), type, type + 4, 4, 0);
    id = rd32(type + 4);
    uint32_t direct = 0;
    if (tb_AEGetParamDesc(evt, (int32_t)T("----"), (int32_t)T("****"), d) == noErr) {
        direct = as_new_from_desc(d);
        tb_AEDisposeDesc(d);
    }
    tb_DisposePtr(d);
    tb_DisposePtr(type);
    int found = 0;
    int32_t err = as_run_event(context, cls, id, direct, result, &found);
    as_dispose(direct);
    if (!err && !found)
        return errAEEventNotHandled;
    return script_error(err);
}

static int32_t cant_compile(void)
{
    as_clear_error();
    as_raise(asErrCantCompile, NULL, "Blue Box's AppleScript runs compiled scripts but can't compile them yet.");
    return errOSAScriptError;
}

#define ARG(k) rd32(params + 4 + 4 * (k)) /* the arguments, the last one first */
/* An argument at a byte offset into them (when a short is among them). */
#define P16(off) ((int16_t)rd16(params + 4 + (off)))
#define P32(off) rd32(params + 4 + (off))

/* The source styles: a TextEdit style table (STHandle) of nine
 * (uncompiled text, normal text, language and application keywords,
 * comments, literals, user symbols, object specifiers, reserved), as
 * AppleScript set them out.  Script Editor asks for them at its start and
 * takes a handle left unset for a damaged heap. */
static int32_t source_styles(gaddr out)
{
    static const struct { int16_t font, size; uint8_t face; } st[9] = {
        { 22, 10, 0 }, { 3, 10, 0 }, { 3, 10, 1 }, { 3, 10, 0 }, { 3, 9, 2 },
        { 3, 10, 0 }, { 3, 10, 0 }, { 3, 10, 0 }, { 3, 10, 0 },
    };
    gaddr h = tb_NewHandle(9 * 18, false, true);
    if (!h)
        return memFullErr;
    for (int i = 0; i < 9; i++) {
        gaddr e = rd32(h) + (uint32_t)i * 18; /* STElement */
        wr16(e, 1);                           /* stCount */
        wr16(e + 2, (uint16_t)(st[i].size + 2));
        wr16(e + 4, (uint16_t)st[i].size);    /* stAscent */
        wr16(e + 6, (uint16_t)st[i].font);
        wr8(e + 8, st[i].face);
        wr16(e + 10, (uint16_t)st[i].size);   /* stColor stays black */
    }
    wr32(out, h);
    return noErr;
}

/* ---- Dialects: AppleScript has English alone ---- */

enum { errOSANoSuchDialect = -1757, errOSACantGetTerminology = -2709, errAENoSuchObject = -1728 };
enum { kOSAModeDontDefine = 1 };

static int16_t put_short(int16_t v, gaddr out)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return put_desc(T("shor"), b, 2, out);
}

/* The English dialect's information as a typeOSADialectInfo record. */
static int32_t dialect_info_record(gaddr out)
{
    int32_t err = tb_AECreateList(0, 0, 1, out);
    gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
    if (!err && !(err = put_desc(T("TEXT"), "English", 7, item)))
        err = tb_AEPutParamDesc(out, (int32_t)T("dnam"), item), tb_AEDisposeDesc(item);
    static const uint32_t keys[3] = { 0x64636F64 /* dcod */, 0x646C6364 /* dlcd */, 0x64736364 /* dscd */ };
    for (int i = 0; !err && i < 3; i++) {
        err = put_short(0, item); /* English, Roman script */
        if (!err)
            err = tb_AEPutParamDesc(out, (int32_t)keys[i], item), tb_AEDisposeDesc(item);
    }
    tb_DisposePtr(item);
    if (!err)
        wr32(out, T("difo"));
    else
        tb_AEDisposeDesc(out);
    return err;
}

static int32_t dialect_call(uint16_t what, gaddr params)
{
    switch (what) {
    case 0x0701: /* OSASetCurrentDialect (dialectCode) */
        return P16(0) == 0 ? noErr : errOSANoSuchDialect;
    case 0x0702: /* OSAGetCurrentDialect (*dialectCode) */
        if (ARG(0))
            wr16(ARG(0), 0);
        return noErr;
    case 0x0703: { /* OSAAvailableDialects (*list of dialect infos) */
        gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
        int32_t err = tb_AECreateList(0, 0, 0, ARG(0));
        if (!err && !(err = dialect_info_record(item))) {
            err = tb_AEPutDesc(ARG(0), 0, item);
            tb_AEDisposeDesc(item);
        }
        tb_DisposePtr(item);
        return err;
    }
    case 0x0704: { /* OSAGetDialectInfo (dialectCode, selector, *result) */
        if (P16(8) != 0)
            return errOSANoSuchDialect;
        uint32_t sel = P32(4);
        if (sel == T("dnam"))
            return put_desc(T("TEXT"), "English", 7, P32(0));
        if (sel == T("dcod") || sel == T("dlcd") || sel == T("dscd"))
            return put_short(0, P32(0));
        if (sel == T("difo"))
            return dialect_info_record(P32(0));
        return errOSABadSelector;
    }
    case 0x0705: /* OSAAvailableDialectCodeList (*list of codes) */
        {
            gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
            int32_t err = tb_AECreateList(0, 0, 0, ARG(0));
            if (!err && !(err = put_short(0, item))) {
                err = tb_AEPutDesc(ARG(0), 0, item);
                tb_AEDisposeDesc(item);
            }
            tb_DisposePtr(item);
            return err;
        }
    }
    return badComponentSelector;
}

/* ---- Contexts and terminology (ASDebugging.h) ---- */

/* The data of a descriptor (a variable's name), malloc'd. */
static uint8_t *desc_bytes(gaddr desc, size_t *n)
{
    *n = 0;
    gaddr h = desc ? rd32(desc + AEDesc_dataHandle_off) : 0;
    if (!h || !rd32(h))
        return NULL;
    int32_t len = tb_GetHandleSize(h);
    uint8_t *b = malloc((size_t)(len > 0 ? len : 1));
    gget(b, rd32(h), (uint32_t)len);
    *n = (size_t)len;
    return b;
}

/* A list of texts as an AEDescList of TEXT. */
static int32_t names_to_desc(uint32_t list_id, gaddr out)
{
    int32_t err = tb_AECreateList(0, 0, 0, out);
    gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
    for (size_t i = 0, n = as_list_length(list_id); !err && i < n; i++) {
        const uint8_t *t;
        size_t len = as_list_text(list_id, i, &t);
        if (!(err = put_desc(T("TEXT"), t, (uint32_t)len, item))) {
            err = tb_AEPutDesc(out, 0, item);
            tb_AEDisposeDesc(item);
        }
    }
    tb_DisposePtr(item);
    as_dispose(list_id);
    return err;
}

static int32_t context_names(uint32_t ctx, bool handlers, gaddr out)
{
    uint32_t l;
    int32_t err = as_context_names(ctx, handlers, &l);
    return err ? err : names_to_desc(l, out);
}

static int32_t context_get(uint32_t ctx, gaddr name, bool handler, gaddr out)
{
    size_t n;
    uint8_t *b = desc_bytes(name, &n);
    uint32_t id = 0;
    int32_t err = b ? as_context_get(ctx, b, n, handler, &id) : errOSAInvalidID;
    free(b);
    wr32(out, id);
    return err;
}

static int32_t context_set(uint32_t mode, uint32_t ctx, gaddr name, bool handler, uint32_t value)
{
    size_t n;
    uint8_t *b = desc_bytes(name, &n);
    int32_t err = b ? as_context_set(ctx, b, n, handler, value, (mode & kOSAModeDontDefine) != 0) : errOSAInvalidID;
    free(b);
    return err;
}

/* The terminology resources of type `type` (id; 0 also takes the first)
 * in a file, as a list of typeAETE descriptors. */
static int32_t terminology_of(gaddr spec, uint32_t type, int16_t id, gaddr out)
{
    int16_t before = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(spec, 1);
    if (rn < 0)
        return (int16_t)rd16(LM_ResErr) ? (int16_t)rd16(LM_ResErr) : (int32_t)fnfErr;
    gaddr h = tb_Get1Resource((int32_t)type, id);
    if (!h || !rd32(h))
        h = id == 0 ? tb_Get1IndResource((int32_t)type, 1) : 0;
    int32_t err = errOSACantGetTerminology;
    if (h && rd32(h)) {
        gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
        err = tb_AECreateList(0, 0, 0, out);
        if (!err && !(err = tb_AECreateDesc((int32_t)T("aete"), rd32(h), tb_GetHandleSize(h), item))) {
            err = tb_AEPutDesc(out, 0, item);
            tb_AEDisposeDesc(item);
        }
        tb_DisposePtr(item);
    }
    tb_CloseResFile(rn);
    if (before)
        tb_UseResFile(before);
    set_res_err(noErr);
    return err;
}

/* The system's terminology ('aeut') is in the AppleScript extension. */
static int32_t sys_terminology(int16_t id, gaddr out)
{
    gaddr p = tb_NewPtr(8, false, true), name = tb_NewPtr(16, false, true), spec = tb_NewPtr(FSSpec_SIZEOF, true, true);
    int32_t err = errOSACantGetTerminology;
    if (p && name && spec) {
        wr8(name, 11);
        gput(name + 1, "AppleScript", 11);
        if (tb_FindFolder(-1, (int32_t)T("extn"), 0, p, p + 2) == noErr &&
            tb_FSMakeFSSpec((int16_t)rd16(p), (int32_t)rd32(p + 2), name, spec) == noErr)
            err = terminology_of(spec, T("aeut"), id, out);
    }
    if (p)
        tb_DisposePtr(p);
    if (name)
        tb_DisposePtr(name);
    if (spec)
        tb_DisposePtr(spec);
    return err == fnfErr ? errOSACantGetTerminology : err;
}

static int32_t app_terminology(gaddr spec, int16_t id, gaddr launched, gaddr out)
{
    if (launched)
        wr8(launched, 0);
    if (!spec)
        return paramErr;
    return terminology_of(spec, T("aete"), id, out);
}

static int32_t as_extra_call(uint16_t what, gaddr params, bool *handled)
{
    *handled = true;
    switch (what) {
    case 0x1004: /* ASGetSourceStyleNames (modeFlags, *list) */
    {
        static const char *const names[] = { "Uncompiled Text", "Normal Text", "Language Keyword",
                                             "Application Keyword", "Comment", "Literal", "User Symbol",
                                             "Object Specifier" };
        gaddr item = tb_NewPtr(AEDesc_SIZEOF, false, true);
        int32_t err = tb_AECreateList(0, 0, 0, ARG(0));
        for (size_t i = 0; !err && i < sizeof names / sizeof *names; i++)
            if (!(err = put_desc(T("TEXT"), names[i], (uint32_t)strlen(names[i]), item))) {
                err = tb_AEPutDesc(ARG(0), 0, item);
                tb_AEDisposeDesc(item);
            }
        tb_DisposePtr(item);
        return err;
    }
    /* The older forms, without mode flags */
    case 0x1101: /* ASSetProperty (contextID, name, value) */
        return context_set(0, ARG(2), ARG(1), false, ARG(0));
    case 0x1102: /* ASGetProperty (contextID, name, *value) */
        return context_get(ARG(2), ARG(1), false, ARG(0));
    case 0x1103: /* ASSetHandler (contextID, name, compiledScriptID) */
        return context_set(0, ARG(2), ARG(1), true, ARG(0));
    case 0x1104: /* ASGetHandler (contextID, name, *compiledScriptID) */
        return context_get(ARG(2), ARG(1), true, ARG(0));
    case 0x1105: /* ASGetAppTerminology (fileSpec, terminologyID, *didLaunch, *list) */
        return app_terminology(P32(10), P16(8), P32(4), P32(0));
    case 0x1106: /* OSASetProperty (modeFlags, contextID, name, value) */
        return context_set(ARG(3), ARG(2), ARG(1), false, ARG(0));
    case 0x1107: /* OSAGetProperty (modeFlags, contextID, name, *value) */
        return context_get(ARG(2), ARG(1), false, ARG(0));
    case 0x1108: /* OSASetHandler */
        return context_set(ARG(3), ARG(2), ARG(1), true, ARG(0));
    case 0x1109: /* OSAGetHandler */
        return context_get(ARG(2), ARG(1), true, ARG(0));
    case 0x110A: /* OSAGetAppTerminology (modeFlags, fileSpec, terminologyID, *didLaunch, *list) */
        return app_terminology(P32(10), P16(8), P32(4), P32(0));
    case 0x110B: /* OSAGetSysTerminology (modeFlags, terminologyID, *list) */
        return sys_terminology(P16(4), P32(0));
    case 0x110C: /* OSAGetPropertyNames (modeFlags, contextID, *list) */
        return context_names(ARG(1), false, ARG(0));
    case 0x110D: /* OSAGetHandlerNames */
        return context_names(ARG(1), true, ARG(0));
    }
    *handled = false;
    return badComponentSelector;
}

static int32_t osa_call(int16_t what, gaddr params)
{
    char text[2048];
    int32_t err;
    uint32_t id = 0;
    switch ((uint16_t)what) {
    case 0xFFFF: /* open */
    case 0xFFFE: /* close */
        return noErr;
    case 0xFFFD: { /* can do */
        static const uint16_t sels[] = {
            kOSASelectLoad, kOSASelectStore, kOSASelectExecute, kOSASelectDisplay, kOSASelectScriptError,
            kOSASelectDispose, kOSASelectSetScriptInfo, kOSASelectGetScriptInfo, kOSASelectSetActiveProc,
            kOSASelectGetActiveProc, kOSASelectScriptingComponentName, kOSASelectCompile, kOSASelectCopyID, 0x0105,
            kOSASelectGetSource, kOSASelectCoerceFromDesc, kOSASelectCoerceToDesc, kOSASelectSetSendProc,
            kOSASelectGetSendProc, kOSASelectSetCreateProc, kOSASelectGetCreateProc, kOSASelectSetDefaultTarget,
            kOSASelectStartRecording, kOSASelectStopRecording, kOSASelectLoadExecute, kOSASelectCompileExecute,
            kOSASelectDoScript, kOSASelectSetResumeDispatchProc, kOSASelectGetResumeDispatchProc,
            kOSASelectExecuteEvent, kOSASelectDoEvent, kOSASelectMakeContext,
        };
        uint16_t s = rd16(params + 4);
        for (size_t i = 0; i < sizeof sels / sizeof sels[0]; i++)
            if (sels[i] == s)
                return 1;
        return (int16_t)s < 0 && (int16_t)s >= -4;
    }
    case 0xFFFC: /* version: AppleScript 1.1 */
        return 0x01100000;
    case kOSASelectLoad: /* (scriptData, modeFlags, *resultingScriptID) */
        err = load_desc(ARG(2), &id);
        wr32(ARG(0), id);
        return err;
    case kOSASelectStore: { /* (scriptID, desiredType, modeFlags, *resultingScriptData) */
        size_t len;
        const uint8_t *data = as_script_data(ARG(3), &len);
        if (!data)
            return errOSAInvalidID;
        return put_desc(T("scpt"), data, (uint32_t)len, ARG(0));
    }
    case kOSASelectExecute: /* (compiledScriptID, contextID, modeFlags, *resultingScriptValueID) */
        err = as_run_script(ARG(3), &id);
        wr32(ARG(0), id);
        return script_error(err);
    case kOSASelectDisplay: /* (scriptValueID, desiredType, modeFlags, *resultingText) */
        if (as_display(ARG(3), text, sizeof text))
            return errOSAInvalidID;
        return put_desc(T("TEXT"), text, (uint32_t)strlen(text), ARG(0));
    case kOSASelectScriptError: { /* (selector, desiredType, *resultingErrorDescription) */
        uint32_t sel = ARG(2);
        gaddr out = ARG(0);
        if (sel == T("errn"))
            return put_long(as_error_number(), out);
        if (sel == T("errs") || sel == T("errb")) {
            const char *m = as_error_message();
            return put_desc(T("TEXT"), m, (uint32_t)strlen(m), out);
        }
        if (sel == T("erng")) { /* the source range: we have no source */
            uint8_t z[4] = { 0 };
            return put_desc(T("null"), z, 0, out);
        }
        return errOSABadSelector;
    }
    case kOSASelectDispose:
        as_dispose(ARG(0));
        return noErr;
    case kOSASelectSetScriptInfo:
        return noErr;
    case kOSASelectGetScriptInfo: /* (scriptID, selector, *result) */
        wr32(ARG(0), 0);
        return noErr;
    case kOSASelectSetActiveProc:
        active_proc = ARG(1);
        active_refcon = ARG(0);
        return noErr;
    case kOSASelectGetActiveProc:
        wr32(ARG(1), active_proc);
        wr32(ARG(0), active_refcon);
        return noErr;
    case kOSASelectScriptingComponentName:
        return put_desc(T("TEXT"), "AppleScript", 11, ARG(0));
    case kOSASelectGetSource:
        return errOSASourceNotAvailable;
    case kOSASelectCompile: /* (sourceData, modeFlags, *previousAndResultingScriptID) */
    case kOSASelectCompileExecute:
    case kOSASelectDoScript:
        return cant_compile();
    case kOSASelectCopyID: /* (fromID, *toID) */
        id = as_copy_id(ARG(1));
        wr32(ARG(0), id);
        return id ? noErr : errOSAInvalidID;
    case 0x0105: /* OSACopyScript (fromID, *toID) */
        id = as_copy_script(ARG(1));
        wr32(ARG(0), id);
        return id ? noErr : errOSAInvalidID;
    case kOSASelectCoerceFromDesc: /* (scriptData, modeFlags, *resultingScriptID) */
        id = as_new_from_desc(ARG(2));
        wr32(ARG(0), id);
        return noErr;
    case kOSASelectCoerceToDesc: /* (scriptID, desiredType, modeFlags, *result) */
        if (as_display(ARG(3), text, sizeof text))
            return errOSAInvalidID;
        return put_desc(T("TEXT"), text, (uint32_t)strlen(text), ARG(0));
    case kOSASelectSetSendProc:
        send_proc = ARG(1);
        send_refcon = ARG(0);
        return noErr;
    case kOSASelectGetSendProc:
        wr32(ARG(1), send_proc);
        wr32(ARG(0), send_refcon);
        return noErr;
    case kOSASelectSetCreateProc:
        create_proc = ARG(1);
        create_refcon = ARG(0);
        return noErr;
    case kOSASelectGetCreateProc:
        wr32(ARG(1), create_proc);
        wr32(ARG(0), create_refcon);
        return noErr;
    case kOSASelectSetResumeDispatchProc:
        resume_proc = ARG(1);
        resume_refcon = ARG(0);
        return noErr;
    case kOSASelectGetResumeDispatchProc:
        wr32(ARG(1), resume_proc);
        wr32(ARG(0), resume_refcon);
        return noErr;
    case kOSASelectSetDefaultTarget:
    case kOSASelectStartRecording:
    case kOSASelectStopRecording:
        return noErr;
    case kOSASelectLoadExecute: { /* (scriptData, contextID, modeFlags, *resultingScriptValueID) */
        err = load_desc(ARG(3), &id);
        if (err)
            return err;
        uint32_t r = 0;
        err = as_run_script(id, &r);
        as_dispose(id);
        wr32(ARG(0), r);
        return script_error(err);
    }
    case kOSASelectExecuteEvent: /* (theAppleEvent, contextID, modeFlags, *resultingScriptValueID) */
        err = execute_event(ARG(2), ARG(3), &id);
        wr32(ARG(0), id);
        return err;
    case kOSASelectDoEvent: { /* (theAppleEvent, contextID, modeFlags, reply) */
        err = execute_event(ARG(2), ARG(3), &id);
        gaddr reply = ARG(0);
        /* The handler's result goes back as text in the reply. */
        if (!err && id && reply && rd32(reply + AEDesc_descriptorType_off) == T("aevt") &&
            !as_display(id, text, sizeof text)) {
            gaddr p = tb_NewPtr((int32_t)strlen(text) + 1, false, false);
            gput(p, text, (uint32_t)strlen(text));
            tb_AEPutParamPtr(reply, (int32_t)T("----"), (int32_t)T("TEXT"), p, (int32_t)strlen(text));
            tb_DisposePtr(p);
        }
        as_dispose(id);
        return err;
    }
    case kOSASelectMakeContext: /* (contextName, parentContext, *resultingContextID) */
        return errOSABadSelector;
    }
    if ((uint16_t)what >= 0x0701 && (uint16_t)what <= 0x0705)
        return dialect_call((uint16_t)what, params);
    {
        bool handled;
        int32_t r = as_extra_call((uint16_t)what, params, &handled);
        if (handled)
            return r;
    }
    switch ((uint16_t)what) {
    case 0x1001: /* ASInit */
    case 0x1002: /* ASSetSourceStyles: kept to itself */
        return noErr;
    case 0x1003: /* ASGetSourceStyles (*resultingSourceStyles) */
        return source_styles(ARG(0));
    }
    return badComponentSelector;
}

/* pascal ComponentResult (ComponentParameters *params, Handle storage) */
static void osa_component(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    int32_t r = osa_call(what, params);
    LOGF(LOG_TRAPS, "  AppleScript component: selector %04X -> %d\n", (uint16_t)what, r);
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* ---------------------------------------------------------------------- */
/* The applet shell */

typedef struct {
    uint32_t script;
    bool ran, quit, stay_open;
} applet_state;

static applet_state applet;

/* A script's error, told as an applet tells it (not User Canceled). */
static void report_error(int32_t err)
{
    if (!err || err == asErrUserCanceled)
        return;
    LOGF(LOG_UNIMPL, "AppleScript: the script stopped with error %d: %s\n", err, as_error_message());
    machine_script_dialog d;
    memset(&d, 0, sizeof d);
    d.kind = MACHINE_SCRIPT_DIALOG;
    d.icon = MACHINE_ALERT_STOP;
    snprintf(d.message, sizeof d.message, "%s", as_error_message());
    d.nbuttons = 1;
    strcpy(d.buttons[0], "OK");
    d.default_button = 1;
    as_ask(&d);
}

/* The event handlers the applet installs: pascal OSErr (evt, reply, refcon). */
static void applet_event(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    uint32_t refcon = cpu_pop32(c);
    cpu_pop32(c); /* reply */
    gaddr evt = cpu_pop32(c);
    int32_t err = noErr;
    uint32_t result = 0;
    int found = 0;
    switch (refcon) {
    case 0x6F617070: /* 'oapp' */
        err = as_run_event(applet.script, T("aevt"), T("oapp"), 0, &result, &found);
        applet.ran = true;
        break;
    case 0x6F646F63: { /* 'odoc': the open handler, with the list of aliases */
        gaddr d = tb_NewPtr(AEDesc_SIZEOF, false, true);
        uint32_t direct = 0;
        if (tb_AEGetParamDesc(evt, (int32_t)T("----"), (int32_t)T("list"), d) == noErr) {
            direct = as_new_from_desc(d);
            tb_AEDisposeDesc(d);
        }
        tb_DisposePtr(d);
        err = as_run_event(applet.script, T("aevt"), T("odoc"), direct, &result, &found);
        if (!err && !found) /* no open handler: a droplet run as an applet */
            err = as_run_event(applet.script, T("aevt"), T("oapp"), 0, &result, &found);
        as_dispose(direct);
        applet.ran = true;
        break;
    }
    case 0x71756974: /* 'quit': the quit handler decides (continue quit) */
        as_quit_requested = false;
        err = as_run_event(applet.script, T("aevt"), T("quit"), 0, &result, &found);
        if (!found || as_quit_requested || err)
            applet.quit = true;
        break;
    }
    as_dispose(result);
    report_error(err);
    if (err == asErrUserCanceled)
        applet.quit = true;
    wr16(c->a[7], 0);
    cpu_push32(c, ret);
}

/* The startup screen: the description (TEXT 1128) with Quit and Run. */
static bool startup_screen(void)
{
    gaddr h = tb_Get1Resource((int32_t)T("TEXT"), 1128);
    if (!h || !rd32(h))
        return true;
    machine_script_dialog d;
    memset(&d, 0, sizeof d);
    d.kind = MACHINE_SCRIPT_DIALOG;
    int32_t n = tb_GetHandleSize(h);
    if (n > (int32_t)sizeof d.message - 1)
        n = sizeof d.message - 1;
    gget(d.message, rd32(h), (uint32_t)n);
    d.nbuttons = 2;
    strcpy(d.buttons[0], "Quit");
    strcpy(d.buttons[1], "Run");
    d.default_button = 2;
    return as_ask(&d) != 1;
}

/* A Pascal string in a new guest block. */
static gaddr new_pstring(const uint8_t *text, size_t n)
{
    if (n > 255)
        n = 255;
    gaddr p = tb_NewPtr((int32_t)n + 1, false, false);
    wr8(p, (uint8_t)n);
    gput(p + 1, text, (uint32_t)n);
    return p;
}

static gaddr new_menu(int16_t id, const char *title, const char *items)
{
    gaddr t = new_pstring((const uint8_t *)title, strlen(title));
    gaddr m = tb_NewMenu(id, t);
    tb_DisposePtr(t);
    if (*items) {
        gaddr i = new_pstring((const uint8_t *)items, strlen(items));
        tb_AppendMenu(m, i);
        tb_DisposePtr(i);
    }
    tb_InsertMenu(m, 0);
    return m;
}

enum { APPLE_MENU = 128, FILE_MENU = 129, EDIT_MENU = 130 };

/* The applet shell's menu bar: the Apple menu with About, File with
 * Quit, and Edit for the script's dialogs, its items dimmed. */
static void applet_menus(void)
{
    gaddr apple = new_menu(APPLE_MENU, "\x14", "About");
    uint8_t about[64] = "About ";
    size_t n = 6, len = rd8(LM_CurApName);
    if (len > 31)
        len = 31;
    gget(about + n, LM_CurApName + 1, (uint32_t)len);
    n += len;
    about[n++] = 0xC9; /* ... */
    gaddr t = new_pstring(about, n);
    tb_SetMenuItemText(apple, 1, t);
    tb_DisposePtr(t);
    new_menu(FILE_MENU, "File", "Quit/Q");
    gaddr edit = new_menu(EDIT_MENU, "Edit", "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");
    for (int16_t i = 1; i <= 6; i++)
        tb_DisableItem(edit, i);
    tb_DrawMenuBar();
}

/* A choice from the applet's menus: Quit quits; the rest does nothing. */
static void applet_choice(int32_t choice)
{
    if ((int16_t)(choice >> 16) == FILE_MENU && (int16_t)choice == 1)
        applet.quit = true;
    tb_HiliteMenu(0);
}

static int32_t applet_main(void)
{
    gaddr h = tb_Get1Resource((int32_t)T("scpt"), 128);
    if (!h || !rd32(h)) {
        LOGF(LOG_UNIMPL, "AppleScript: the applet has no script ('scpt' 128)\n");
        return resNotFound;
    }
    int32_t n = tb_GetHandleSize(h);
    uint8_t *data = malloc((size_t)(n > 0 ? n : 1));
    gget(data, rd32(h), (uint32_t)n);
    memset(&applet, 0, sizeof applet);
    int32_t err = as_load_script(data, (size_t)n, &applet.script);
    free(data);
    if (err) {
        report_error(err);
        return noErr;
    }
    gaddr pref = tb_Get1Resource((int32_t)T("pref"), 128);
    uint16_t flags = pref && rd32(pref) && tb_GetHandleSize(pref) >= 2 ? rd16(rd32(pref)) : 0;
    applet.stay_open = (flags & 1) != 0;
    if (!(flags & 2) && !startup_screen()) {
        as_dispose(applet.script);
        return noErr;
    }

    static uint32_t handler;
    if (!handler)
        handler = native_register(applet_event, "AppletEvent", 0);
    tb_AEInstallEventHandler((int32_t)T("aevt"), (int32_t)T("oapp"), handler, (int32_t)T("oapp"), 0);
    tb_AEInstallEventHandler((int32_t)T("aevt"), (int32_t)T("odoc"), handler, (int32_t)T("odoc"), 0);
    tb_AEInstallEventHandler((int32_t)T("aevt"), (int32_t)T("quit"), handler, (int32_t)T("quit"), 0);

    applet_menus();
    gaddr evt = tb_NewPtr(EventRecord_SIZEOF, false, true), wp = tb_NewPtr(4, false, true);
    uint32_t next_idle = 0;
    int waited = 0;
    as_quit_requested = false;
    while (!g_cpu->halted && !applet.quit) {
        bool got = tb_WaitNextEvent((int16_t)0xFFFF, evt, applet.stay_open ? 10 : 2, 0) != 0;
        int16_t what = got ? (int16_t)rd16(evt) : 0;
        if (what == 23) /* kHighLevelEvent */
            tb_AEProcessAppleEvent(evt);
        else if (what == 3 && (rd16(evt + 14) & 0x0100))
            applet_choice(tb_MenuKey((int16_t)(rd32(evt + 2) & 0xFF))); /* Command-Q */
        else if (what == 1 && tb_FindWindow(rd32(evt + 10), wp) == 1) /* inMenuBar */
            applet_choice(tb_MenuSelect(rd32(evt + 10)));
        if (!applet.ran && ++waited > 120) {
            /* No Open Application came (a launch without one): run anyway. */
            uint32_t r = 0;
            int found;
            err = as_run_event(applet.script, T("aevt"), T("oapp"), 0, &r, &found);
            as_dispose(r);
            report_error(err);
            applet.ran = true;
            if (err == asErrUserCanceled)
                applet.quit = true;
        }
        if (applet.ran && !applet.stay_open)
            break;
        if (applet.ran && as_quit_requested)
            break;
        if (applet.ran && applet.stay_open && rd32(LM_Ticks) >= next_idle) {
            int found = 0;
            int32_t secs = as_run_idle(applet.script, &found);
            if (secs < 0) {
                report_error(secs);
                if (secs == asErrUserCanceled)
                    break;
                secs = 30;
            }
            next_idle = rd32(LM_Ticks) + (uint32_t)(found ? secs : 30) * 60;
        }
    }
    tb_DisposePtr(evt);
    tb_DisposePtr(wp);
    tb_ClearMenuBar();
    tb_AERemoveEventHandler((int32_t)T("aevt"), (int32_t)T("oapp"), handler, 0);
    tb_AERemoveEventHandler((int32_t)T("aevt"), (int32_t)T("odoc"), handler, 0);
    tb_AERemoveEventHandler((int32_t)T("aevt"), (int32_t)T("quit"), handler, 0);
    as_dispose(applet.script);
    return noErr;
}

static void applet_component(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    int32_t r;
    switch (what) {
    case -1: /* open */
    case -2: /* close */
        r = noErr;
        break;
    case -3: /* can do */
        r = (int16_t)rd16(params + 4) == 1 || ((int16_t)rd16(params + 4) < 0 && (int16_t)rd16(params + 4) >= -4);
        break;
    case -4: /* version */
        r = 0x01100000;
        break;
    case 1: /* run the applet */
        r = applet_main();
        break;
    default:
        r = osa_call(what, params);
        break;
    }
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* The generic scripting component ('osa ' 'scpt'): its own calls name
 * AppleScript as the scripting component there is (Script Editor asks for
 * it by subtype and talks to it); the OSA's calls are AppleScript's. */
enum { errOSAComponentMismatch = -1761 };
static uint32_t ascr_component;

static int32_t generic_call(int16_t what, gaddr params)
{
    switch ((uint16_t)what) {
    case 0x1001: /* GSSGetDefaultScriptingComponent (*scriptingSubType) */
    case 0x1004: /* GSSGetScriptingComponentFromStored (storedScriptID, *scriptingSubType) */
        wr32(ARG(0), T("ascr"));
        return noErr;
    case 0x1002: /* GSSSetDefaultScriptingComponent (scriptingSubType) */
        return ARG(0) == T("ascr") ? noErr : errOSAComponentMismatch;
    case 0x1003: { /* GSSGetScriptingComponent (scriptingSubType, *scriptingInstance) */
        uint32_t ci = ARG(1) == T("ascr") && ascr_component ? components_open(ascr_component) : 0;
        wr32(ARG(0), ci);
        return ci ? noErr : errOSAComponentMismatch;
    }
    case 0x1005: /* GSSGenericToRealID (*theScriptID, *theExactComponent) */
    case 0x1006: /* GSSRealToGenericID (theScriptID, theExactComponent) */
        return noErr; /* one component: the IDs are the same */
    }
    return osa_call(what, params);
}

static void generic_component(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    int32_t r = generic_call(what, params);
    LOGF(LOG_TRAPS, "  generic scripting component: selector %04X -> %d\n", (uint16_t)what, r);
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* Register the components (a new run). */
void osa_register(void)
{
    as_reset();
    active_proc = active_refcon = send_proc = send_refcon = create_proc = create_refcon = resume_proc = resume_refcon = 0;
    uint32_t osa = native_register(osa_component, "AppleScript", 0);
    ascr_component = components_register_native(T("osa "), T("ascr"), T("appl"), osa);
    components_register_native(T("osa "), T("scpt"), T("appl"), native_register(generic_component, "GenericScripting", 0));
    components_register_native(T("aplt"), T("scpt"), T("appl"), native_register(applet_component, "AppletShell", 0));
    /* AppleScript 1.1 is here (gestaltAppleScriptVersion, gestaltAppleScriptAttr). */
    gestalt_set(T("ascv"), 0x01108000);
    gestalt_set(T("ascr"), 0x1);
}
