/*
 * The Text Services Manager (_TSMDispatch, $AA54), as on a Mac with no
 * input methods installed: documents are made, activated and fixed but no
 * text service takes their events, the service list is empty, and the
 * default input method of every script is none.  A program can still open
 * a component of its own as a text service, send Apple events through
 * the Manager, and make service windows, which float in front of its
 * other windows.
 */
#include <string.h>

#include "tb_internal.h"

enum {
    tsmUnsupScriptLanguageErr = -2500,
    tsmInputMethodNotFoundErr = -2501,
    tsmInvalidDocIDErr = -2505,
    tsmDocNotActiveErr = -2507,
    tsmCantOpenComponentErr = -2509,
    tsmScriptHasNoIMErr = -2517,
};

#define TSM_DOC_SIGNATURE 0x74646F63u /* 'tdoc' */
enum { MAX_DOCS = 32, MAX_SERVICE_WINDOWS = 16, MAX_SERVICES = 16 };

/* A document: a small block of ours ('tdoc', its refcon) whose address is
 * the TSMDocumentID. */
static gaddr docs[MAX_DOCS];
static gaddr active_doc;
static struct {
    gaddr doc;
    uint32_t ci;
} services[MAX_SERVICES];
static gaddr service_windows[MAX_SERVICE_WINDOWS];
static int16_t service_script, service_language; /* SetTextServiceLanguage's */
static uint32_t default_im;                      /* SetDefaultInputMethod's */
static int16_t default_im_script;

static bool doc_valid(gaddr d)
{
    if (!d)
        return false;
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i] == d)
            return rd32(d) == TSM_DOC_SIGNATURE;
    return false;
}

int16_t tb_NewTSMDocument(int16_t numOfInterface, gaddr supportedInterfaceTypes, gaddr idocID, int32_t refcon)
{
    if (idocID)
        wr32(idocID, 0);
    if (!idocID)
        return paramErr;
    for (int i = 0; i < MAX_DOCS; i++) {
        if (docs[i])
            continue;
        gaddr d = tb_NewPtr(8, true, true);
        if (!d)
            return memFullErr;
        wr32(d, TSM_DOC_SIGNATURE);
        wr32(d + 4, (uint32_t)refcon);
        docs[i] = d;
        wr32(idocID, d);
        return noErr;
    }
    return memFullErr;
}

int16_t tb_DeleteTSMDocument(gaddr idocID)
{
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].doc == idocID) {
            components_close(services[i].ci);
            services[i].doc = 0;
        }
    for (int i = 0; i < MAX_DOCS; i++)
        if (docs[i] == idocID)
            docs[i] = 0;
    if (active_doc == idocID)
        active_doc = 0;
    wr32(idocID, 0);
    tb_DisposePtr(idocID);
    return noErr;
}

int16_t tb_ActivateTSMDocument(gaddr idocID)
{
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    active_doc = idocID;
    return noErr;
}

int16_t tb_DeactivateTSMDocument(gaddr idocID)
{
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    if (active_doc != idocID)
        return tsmDocNotActiveErr;
    active_doc = 0;
    return noErr;
}

/* Nothing in progress to confirm: no input method is composing. */
int16_t tb_FixTSMDocument(gaddr idocID)
{
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    return active_doc == idocID ? noErr : tsmDocNotActiveErr;
}

/* No text service takes an event, a menu choice or the cursor. */
int8_t tb_TSMEvent(gaddr event) { return 0; }
int8_t tb_TSMMenuSelect(int32_t menuResult) { return 0; }
int8_t tb_SetTSMCursor(uint32_t mousePos) { return 0; }

/* The text services that offer these interfaces: none.  The list is a
 * TextServiceList with no entries. */
int16_t tb_GetServiceList(int16_t numOfInterface, gaddr supportedInterfaceTypes, gaddr serviceInfo, gaddr seedValue)
{
    gaddr h = tb_NewHandle(2, true, true);
    if (!h)
        return memFullErr;
    if (serviceInfo)
        wr32(serviceInfo, h);
    if (seedValue)
        wr32(seedValue, 1);
    return noErr;
}

int16_t tb_OpenTextService(gaddr idocID, uint32_t aComponent, gaddr aComponentInstance)
{
    if (aComponentInstance)
        wr32(aComponentInstance, 0);
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].doc)
            continue;
        uint32_t ci = aComponent ? components_open(aComponent) : 0;
        if (!ci)
            return tsmCantOpenComponentErr;
        services[i].doc = idocID;
        services[i].ci = ci;
        if (aComponentInstance)
            wr32(aComponentInstance, ci);
        return noErr;
    }
    return tsmCantOpenComponentErr;
}

int16_t tb_CloseTextService(gaddr idocID, uint32_t aComponentInstance)
{
    if (!doc_valid(idocID))
        return tsmInvalidDocIDErr;
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].doc == idocID && services[i].ci == aComponentInstance) {
            components_close(aComponentInstance);
            services[i].doc = 0;
            return noErr;
        }
    return -2514; /* tsmTSNotOpenErr */
}

/* An Apple event from a text service goes to the application. */
int16_t tb_SendAEFromTSMComponent(gaddr theAppleEvent, gaddr reply, int32_t sendMode, int16_t sendPriority,
                                  int32_t timeOutInTicks, gaddr idleProc, gaddr filterProc)
{
    return tb_AESend(theAppleEvent, reply, sendMode, sendPriority, timeOutInTicks, idleProc, filterProc);
}

/* ScriptLanguageRecord: fScript, fLanguage. */
int16_t tb_SetDefaultInputMethod(uint32_t ts, gaddr slRecordPtr)
{
    if (!ts || !slRecordPtr)
        return tsmInputMethodNotFoundErr;
    default_im = ts;
    default_im_script = (int16_t)rd16(slRecordPtr);
    return noErr;
}

int16_t tb_GetDefaultInputMethod(gaddr ts, gaddr slRecordPtr)
{
    bool have = default_im && slRecordPtr && (int16_t)rd16(slRecordPtr) == default_im_script;
    if (ts)
        wr32(ts, have ? default_im : 0);
    return have ? noErr : tsmScriptHasNoIMErr;
}

int16_t tb_SetTextServiceLanguage(gaddr slRecordPtr)
{
    if (!slRecordPtr)
        return paramErr;
    if ((int16_t)rd16(slRecordPtr) < 0)
        return tsmUnsupScriptLanguageErr;
    service_script = (int16_t)rd16(slRecordPtr);
    service_language = (int16_t)rd16(slRecordPtr + 2);
    return noErr;
}

int16_t tb_GetTextServiceLanguage(gaddr slRecordPtr)
{
    if (!slRecordPtr)
        return paramErr;
    wr16(slRecordPtr, (uint16_t)service_script);
    wr16(slRecordPtr + 2, (uint16_t)service_language);
    return noErr;
}

/* Input in a floating window or inline: with no input method, neither. */
int16_t tb_UseInputWindow(gaddr idocID, int8_t useWindow)
{
    return !idocID || doc_valid(idocID) ? noErr : tsmInvalidDocIDErr;
}

int16_t tb_InitTSMAwareApplication(void) { return noErr; }
int16_t tb_CloseTSMAwareApplication(void) { return noErr; }

/* ---- Service windows ---- */

static bool is_service_window(gaddr w)
{
    for (int i = 0; i < MAX_SERVICE_WINDOWS; i++)
        if (w && service_windows[i] == w)
            return true;
    return false;
}

static int16_t new_service_window(bool color, gaddr wStorage, gaddr boundsRect, gaddr title, int8_t visible,
                                  int16_t theProc, int8_t goAwayFlag, uint32_t ts, gaddr window)
{
    int slot = -1;
    for (int i = 0; i < MAX_SERVICE_WINDOWS && slot < 0; i++)
        if (!service_windows[i])
            slot = i;
    if (window)
        wr32(window, 0);
    if (slot < 0)
        return memFullErr;
    /* In front of the program's windows, whatever behind says; the text
     * service in its refCon. */
    gaddr w = color ? tb_NewCWindow(wStorage, boundsRect, title, visible, theProc, (gaddr)-1, goAwayFlag, (int32_t)ts)
                    : tb_NewWindow(wStorage, boundsRect, title, visible, theProc, (gaddr)-1, goAwayFlag, (int32_t)ts);
    if (!w)
        return memFullErr;
    service_windows[slot] = w;
    if (window)
        wr32(window, w);
    return noErr;
}

int16_t tb_NewServiceWindow(gaddr wStorage, gaddr boundsRect, gaddr title, int8_t visible, int16_t theProc,
                            gaddr behind, int8_t goAwayFlag, uint32_t ts, gaddr window)
{
    return new_service_window(false, wStorage, boundsRect, title, visible, theProc, goAwayFlag, ts, window);
}

int16_t tb_NewCServiceWindow(gaddr wStorage, gaddr boundsRect, gaddr title, int8_t visible, int16_t theProc,
                             gaddr behind, int8_t goAwayFlag, uint32_t ts, gaddr window)
{
    return new_service_window(true, wStorage, boundsRect, title, visible, theProc, goAwayFlag, ts, window);
}

int16_t tb_CloseServiceWindow(gaddr window)
{
    for (int i = 0; i < MAX_SERVICE_WINDOWS; i++)
        if (window && service_windows[i] == window) {
            service_windows[i] = 0;
            tb_DisposeWindow(window);
            return noErr;
        }
    return paramErr;
}

/* The frontmost of them, or nil. */
int16_t tb_GetFrontServiceWindow(gaddr window)
{
    gaddr w = rd32(LM_WindowList);
    while (w && !is_service_window(w))
        w = WindowRecord_nextWindow(w);
    if (window)
        wr32(window, w);
    return noErr;
}

/* The part of a service window at a point, as FindWindow says it; inDesk
 * when there is none. */
int16_t tb_FindServiceWindow(uint32_t thePoint, gaddr theWindow)
{
    gaddr tmp = tb_NewPtr(4, false, true);
    int16_t part = tmp ? tb_FindWindow(thePoint, tmp) : 0;
    gaddr w = tmp ? rd32(tmp) : 0;
    if (tmp)
        tb_DisposePtr(tmp);
    if (!is_service_window(w))
        part = 0, w = 0;
    if (theWindow)
        wr32(theWindow, w);
    return part;
}

/* A fresh machine. */
void tsm_reset(void)
{
    memset(docs, 0, sizeof docs);
    memset(services, 0, sizeof services);
    memset(service_windows, 0, sizeof service_windows);
    active_doc = 0;
    service_script = service_language = 0;
    default_im = 0;
    default_im_script = 0;
}
