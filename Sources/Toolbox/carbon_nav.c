/*
 * Navigation Services' Carbon dialogs (NavCreate...Dialog, NavDialogRun
 * and the rest), on the same host panels and alerts as NavGetFile and
 * its family (navigation.c, stdfile.c).
 *
 * A NavDialogRef is a block holding what the dialog was made with, its
 * creation options turned into a NavDialogOptions as the older calls
 * take them.  NavDialogRun presents it and returns once it is dismissed,
 * whatever the modality asked (the panels are the host's, and modal):
 * the event procedure hears kNavCBStart, then kNavCBUserAction with the
 * user's action (when it may call NavDialogGetReply), then
 * kNavCBTerminate (when programs commonly dispose of the dialog).
 */
#include <string.h>

#include "carbon_svc.h"

enum { kNavWrongDialogStateErr = -5694, kNavWrongDialogClassErr = -5695 };
enum { kNavDefaultNavDlogOptions = 0x000000E4 };
enum { kWindowModalityAppModal = 2 };
enum { kNavCBStart = 2, kNavCBTerminate = 3, kNavCBUserAction = 12 };
enum {
    kNavUserActionNone = 0,
    kNavUserActionCancel = 1,
    kNavUserActionOpen = 2,
    kNavUserActionSaveAs = 3,
    kNavUserActionChoose = 4,
    kNavUserActionNewFolder = 5,
    kNavUserActionSaveChanges = 6,
    kNavUserActionDontSaveChanges = 7,
    kNavUserActionDiscardChanges = 8,
};
enum { kNavAskSaveChangesSave = 1, kNavAskSaveChangesCancel = 2, kNavAskSaveChangesDontSave = 3 };
enum { kNavAskDiscardChanges = 1 };
enum { uppNavEventProcInfo = 0x00000FC0 };
#define typeFSS 0x66737320u /* 'fss ' */

/* NavDialogCreationOptions */
enum {
    CO_VERSION = 0, CO_FLAGS = 2, CO_LOCATION = 6, CO_CLIENT = 10, CO_TITLE = 14, CO_ACTION = 18, CO_CANCEL = 22,
    CO_SAVENAME = 26, CO_MESSAGE = 30, CO_PREFKEY = 34, CO_POPUP = 38, CO_MODALITY = 42, CO_PARENT = 46, CO_SIZE = 66
};
/* NavReplyRecord, version 1: saveFileName after reserved1 */
enum { REPLY_SAVENAME = 24, REPLY_SIZE = 256 };
/* NavCBRec */
enum { CB_VERSION = 0, CB_CONTEXT = 2, CB_WINDOW = 6, CB_USERACTION = 32, CB_SIZE = 254 };

enum { GET_FILE = 1, PUT_FILE, ASK_SAVE, ASK_DISCARD, CHOOSE_FILE, CHOOSE_FOLDER, CHOOSE_VOLUME, CHOOSE_OBJECT,
       NEW_FOLDER };

/* The dialog */
#define NAV_TAG 0x6E617664u /* 'navd' */
enum {
    D_TAG = 0, D_KIND = 4, D_TYPES = 8, D_EVENT = 12, D_PREVIEW = 16, D_FILTER = 20, D_CLIENT = 24, D_FILETYPE = 28,
    D_CREATOR = 32, D_ACTION = 36, D_USER_ACTION = 40, D_SAVENAME = 44, D_HAS_REPLY = 48, D_RUNNING = 49,
    D_REPLY = 52, D_OPTIONS = D_REPLY + REPLY_SIZE, D_SIZE = D_OPTIONS + NavDialogOptions_SIZEOF
};

void carbon_nav_reset(void) {}

static bool is_dialog(gaddr d) { return d && rd32(d + D_TAG) == NAV_TAG; }

/* NavGetDefaultDialogCreationOptions(&options): the default flags, an
 * application-modal dialog named for the application. */
static void nv_NavGetDefaultDialogCreationOptions(ppc_cpu *c)
{
    gaddr o = A(0);
    if (!o)
        RET(paramErr);
    gmemset(o, 0, CO_SIZE);
    wr32(o + CO_FLAGS, kNavDefaultNavDlogOptions);
    wr32(o + CO_LOCATION, 0xFFFFFFFFu);
    wr32(o + CO_CLIENT, svc_cfstring_pascal(LM_CurApName));
    wr32(o + CO_MODALITY, kWindowModalityAppModal);
    RET(noErr);
}

/* A CFString field of the creation options into a Str255 of the old ones. */
static void option_string(gaddr co, uint32_t co_off, gaddr no, uint32_t no_off)
{
    uint8_t p[256];
    gaddr s = rd32(co + co_off);
    p[0] = 0;
    if (s)
        svc_cfstring_to_pascal(s, p);
    gput(no + no_off, p, (uint32_t)p[0] + 1);
}

static gaddr new_dialog(int kind, gaddr co, gaddr event, gaddr client, gaddr out)
{
    if (!out)
        return 0;
    wr32(out, 0);
    gaddr d = tb_NewPtr(D_SIZE, false, true);
    if (!d)
        return 0;
    wr32(d + D_TAG, NAV_TAG);
    wr32(d + D_KIND, (uint32_t)kind);
    wr32(d + D_EVENT, event);
    wr32(d + D_CLIENT, client);
    gaddr no = d + D_OPTIONS;
    tb_NavGetDefaultDialogOptions(no);
    if (co) {
        wr32(no + NavDialogOptions_dialogOptionFlags_off, rd32(co + CO_FLAGS));
        wr32(no + NavDialogOptions_location_off, rd32(co + CO_LOCATION));
        if (rd32(co + CO_CLIENT))
            option_string(co, CO_CLIENT, no, NavDialogOptions_clientName_off);
        option_string(co, CO_TITLE, no, NavDialogOptions_windowTitle_off);
        option_string(co, CO_ACTION, no, NavDialogOptions_actionButtonLabel_off);
        option_string(co, CO_CANCEL, no, NavDialogOptions_cancelButtonLabel_off);
        option_string(co, CO_SAVENAME, no, NavDialogOptions_savedFileName_off);
        option_string(co, CO_MESSAGE, no, NavDialogOptions_message_off);
        wr32(no + NavDialogOptions_preferenceKey_off, rd32(co + CO_PREFKEY));
        gaddr name = rd32(co + CO_SAVENAME);
        svc_cfretain(name);
        wr32(d + D_SAVENAME, name);
    }
    wr32(out, d);
    return d;
}

#define CREATE(kind, co, event, client, out)                                                                           \
    gaddr d = new_dialog(kind, co, event, client, out);                                                                \
    if (!d)                                                                                                            \
        RET((out) ? memFullErr : paramErr);

/* NavCreateGetFileDialog(options, typeList, eventProc, previewProc,
 * filterProc, clientData, &dialog), and NavCreateChooseFileDialog. */
static void nv_NavCreateGetFileDialog(ppc_cpu *c)
{
    CREATE(GET_FILE, A(0), A(2), A(5), A(6));
    wr32(d + D_TYPES, A(1));
    wr32(d + D_PREVIEW, A(3));
    wr32(d + D_FILTER, A(4));
    RET(noErr);
}

static void nv_NavCreateChooseFileDialog(ppc_cpu *c)
{
    CREATE(CHOOSE_FILE, A(0), A(2), A(5), A(6));
    wr32(d + D_TYPES, A(1));
    wr32(d + D_PREVIEW, A(3));
    wr32(d + D_FILTER, A(4));
    RET(noErr);
}

/* NavCreatePutFileDialog(options, fileType, fileCreator, eventProc,
 * clientData, &dialog) */
static void nv_NavCreatePutFileDialog(ppc_cpu *c)
{
    CREATE(PUT_FILE, A(0), A(3), A(4), A(5));
    wr32(d + D_FILETYPE, A(1));
    wr32(d + D_CREATOR, A(2));
    RET(noErr);
}

/* NavCreateAskSaveChangesDialog(options, action, eventProc, clientData, &dialog) */
static void nv_NavCreateAskSaveChangesDialog(ppc_cpu *c)
{
    CREATE(ASK_SAVE, A(0), A(2), A(3), A(4));
    wr32(d + D_ACTION, A(1));
    RET(noErr);
}

/* NavCreateAskDiscardChangesDialog(options, eventProc, clientData, &dialog) */
static void nv_NavCreateAskDiscardChangesDialog(ppc_cpu *c)
{
    CREATE(ASK_DISCARD, A(0), A(1), A(2), A(3));
    RET(noErr);
}

/* NavCreateChooseFolderDialog and NavCreateChooseVolumeDialog(options,
 * eventProc, filterProc, clientData, &dialog) */
static void nv_NavCreateChooseFolderDialog(ppc_cpu *c)
{
    CREATE(CHOOSE_FOLDER, A(0), A(1), A(3), A(4));
    wr32(d + D_FILTER, A(2));
    RET(noErr);
}

static void nv_NavCreateChooseVolumeDialog(ppc_cpu *c)
{
    CREATE(CHOOSE_VOLUME, A(0), A(1), A(3), A(4));
    wr32(d + D_FILTER, A(2));
    RET(noErr);
}

/* NavCreateChooseObjectDialog(options, eventProc, previewProc, filterProc,
 * clientData, &dialog) */
static void nv_NavCreateChooseObjectDialog(ppc_cpu *c)
{
    CREATE(CHOOSE_OBJECT, A(0), A(1), A(4), A(5));
    wr32(d + D_PREVIEW, A(2));
    wr32(d + D_FILTER, A(3));
    RET(noErr);
}

/* NavCreateNewFolderDialog(options, eventProc, clientData, &dialog) */
static void nv_NavCreateNewFolderDialog(ppc_cpu *c)
{
    CREATE(NEW_FOLDER, A(0), A(1), A(2), A(3));
    RET(noErr);
}

/* The event procedure told something, with a NavCBRec. */
static void tell(gaddr d, int32_t message)
{
    gaddr proc = rd32(d + D_EVENT);
    if (!proc)
        return;
    gaddr cb = tb_NewPtr(CB_SIZE, false, true);
    if (!cb)
        return;
    wr16(cb + CB_VERSION, 1);
    wr32(cb + CB_CONTEXT, d);
    wr32(cb + CB_USERACTION, rd32(d + D_USER_ACTION));
    uint32_t args[3] = { (uint32_t)message, cb, rd32(d + D_CLIENT) };
    svc_call_upp(proc, uppNavEventProcInfo, args, 3);
    tb_DisposePtr(cb);
}

/* The name of the reply's file, as a new CFString. */
static gaddr reply_name(gaddr reply)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), key = tb_NewPtr(4, false, true),
          type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true);
    gaddr s = 0;
    if (tb_AEGetNthPtr(reply + NavReplyRecord_selection_off, 1, (int32_t)typeFSS, key, type, spec, FSSpec_SIZEOF,
                       size) == noErr)
        s = svc_cfstring_pascal(spec + FSSpec_name_off);
    tb_DisposePtr(size);
    tb_DisposePtr(type);
    tb_DisposePtr(key);
    tb_DisposePtr(spec);
    return s;
}

/* NavDialogRun(dialog) */
static void nv_NavDialogRun(ppc_cpu *c)
{
    gaddr d = A(0);
    if (!is_dialog(d))
        RET(paramErr);
    if (rd8(d + D_RUNNING) || rd8(d + D_HAS_REPLY))
        RET(kNavWrongDialogStateErr);
    wr8(d + D_RUNNING, 1);
    wr32(d + D_USER_ACTION, kNavUserActionNone);
    tell(d, kNavCBStart);

    gaddr reply = d + D_REPLY, opts = d + D_OPTIONS, ud = rd32(d + D_CLIENT);
    gmemset(reply, 0, REPLY_SIZE);
    int kind = (int)rd32(d + D_KIND);
    int16_t e = noErr;
    uint32_t action = kNavUserActionCancel;
    switch (kind) {
    case GET_FILE:
    case CHOOSE_FILE:
        e = tb_NavGetFile(0, reply, opts, 0, 0, rd32(d + D_FILTER), rd32(d + D_TYPES), ud);
        action = kind == GET_FILE ? kNavUserActionOpen : kNavUserActionChoose;
        break;
    case CHOOSE_OBJECT:
        e = tb_NavChooseObject(0, reply, opts, 0, rd32(d + D_FILTER), ud);
        action = kNavUserActionChoose;
        break;
    case CHOOSE_FOLDER:
        e = tb_NavChooseFolder(0, reply, opts, 0, rd32(d + D_FILTER), ud);
        action = kNavUserActionChoose;
        break;
    case CHOOSE_VOLUME:
        e = tb_NavChooseVolume(0, reply, opts, 0, rd32(d + D_FILTER), ud);
        action = kNavUserActionChoose;
        break;
    case PUT_FILE:
        e = tb_NavPutFile(0, reply, opts, 0, (int32_t)rd32(d + D_FILETYPE), (int32_t)rd32(d + D_CREATOR), ud);
        action = kNavUserActionSaveAs;
        break;
    case NEW_FOLDER:
        e = tb_NavNewFolder(0, reply, opts, 0, ud);
        action = kNavUserActionNewFolder;
        break;
    case ASK_SAVE:
    case ASK_DISCARD: {
        gaddr r = tb_NewPtr(4, false, true);
        if (kind == ASK_SAVE)
            tb_NavAskSaveChanges(opts, rd32(d + D_ACTION), r, 0, ud);
        else
            tb_NavAskDiscardChanges(opts, r, 0, ud);
        uint32_t answer = rd32(r);
        tb_DisposePtr(r);
        action = kind == ASK_DISCARD ? (answer == kNavAskDiscardChanges ? kNavUserActionDiscardChanges
                                                                         : kNavUserActionCancel)
                 : answer == kNavAskSaveChangesSave     ? kNavUserActionSaveChanges
                 : answer == kNavAskSaveChangesDontSave ? kNavUserActionDontSaveChanges
                                                        : kNavUserActionCancel;
        break;
    }
    }
    if (kind != ASK_SAVE && kind != ASK_DISCARD && e != noErr)
        action = kNavUserActionCancel;
    if (action != kNavUserActionCancel && kind != ASK_SAVE && kind != ASK_DISCARD) {
        wr16(reply + NavReplyRecord_version_off, 1);
        wr8(d + D_HAS_REPLY, 1);
        if (kind == PUT_FILE) {
            gaddr name = reply_name(reply);
            if (name) {
                svc_cfrelease(rd32(d + D_SAVENAME));
                wr32(d + D_SAVENAME, name);
            }
        }
    }
    wr32(d + D_USER_ACTION, action);
    tell(d, kNavCBUserAction);
    wr8(d + D_RUNNING, 0);
    tell(d, kNavCBTerminate);
    RET(noErr);
}

/* NavDialogDispose(dialog) */
static void nv_NavDialogDispose(ppc_cpu *c)
{
    gaddr d = A(0);
    if (!is_dialog(d))
        DONE();
    if (rd8(d + D_HAS_REPLY))
        tb_AEDisposeDesc(d + D_REPLY + NavReplyRecord_selection_off);
    svc_cfrelease(rd32(d + D_SAVENAME));
    wr32(d + D_TAG, 0);
    tb_DisposePtr(d);
    DONE();
}

/* NavDialogGetWindow(dialog): the panel is the host's; no window. */
static void nv_NavDialogGetWindow(ppc_cpu *c) { RET(0); }

static void nv_NavDialogGetUserAction(ppc_cpu *c)
{
    gaddr d = A(0);
    RET(is_dialog(d) ? rd32(d + D_USER_ACTION) : kNavUserActionNone);
}

/* NavDialogGetReply(dialog, &reply): a copy of its own, which the program
 * disposes of with NavDisposeReply. */
static void nv_NavDialogGetReply(ppc_cpu *c)
{
    gaddr d = A(0), out = A(1);
    if (!is_dialog(d) || !out)
        RET(paramErr);
    int kind = (int)rd32(d + D_KIND);
    if (kind == ASK_SAVE || kind == ASK_DISCARD)
        RET(kNavWrongDialogClassErr);
    if (!rd8(d + D_HAS_REPLY))
        RET(kNavWrongDialogStateErr);
    gmemcpy(out, d + D_REPLY, REPLY_SIZE);
    int16_t e = tb_AEDuplicateDesc(d + D_REPLY + NavReplyRecord_selection_off, out + NavReplyRecord_selection_off);
    if (e != noErr) {
        gmemset(out, 0, REPLY_SIZE);
        RET(e);
    }
    gaddr name = kind == PUT_FILE ? rd32(d + D_SAVENAME) : 0;
    svc_cfretain(name);
    wr32(out + REPLY_SAVENAME, name);
    RET(noErr);
}

/* NavDialogGetSaveFileName(dialog): the dialog's, not retained for the
 * caller. */
static void nv_NavDialogGetSaveFileName(ppc_cpu *c)
{
    gaddr d = A(0);
    RET(is_dialog(d) && rd32(d + D_KIND) == PUT_FILE ? rd32(d + D_SAVENAME) : 0);
}

/* NavDialogSetSaveFileName(dialog, name) */
static void nv_NavDialogSetSaveFileName(ppc_cpu *c)
{
    gaddr d = A(0), name = A(1);
    if (!is_dialog(d) || !name)
        RET(paramErr);
    if (rd32(d + D_KIND) != PUT_FILE)
        RET(kNavWrongDialogClassErr);
    svc_cfretain(name);
    svc_cfrelease(rd32(d + D_SAVENAME));
    wr32(d + D_SAVENAME, name);
    uint8_t p[256];
    svc_cfstring_to_pascal(name, p);
    gput(d + D_OPTIONS + NavDialogOptions_savedFileName_off, p, (uint32_t)p[0] + 1);
    RET(noErr);
}

const ppc_export carbon_nav_exports[] = {
    { "NavCreateAskDiscardChangesDialog", nv_NavCreateAskDiscardChangesDialog },
    { "NavCreateAskSaveChangesDialog", nv_NavCreateAskSaveChangesDialog },
    { "NavCreateChooseFileDialog", nv_NavCreateChooseFileDialog },
    { "NavCreateChooseFolderDialog", nv_NavCreateChooseFolderDialog },
    { "NavCreateChooseObjectDialog", nv_NavCreateChooseObjectDialog },
    { "NavCreateChooseVolumeDialog", nv_NavCreateChooseVolumeDialog },
    { "NavCreateGetFileDialog", nv_NavCreateGetFileDialog },
    { "NavCreateNewFolderDialog", nv_NavCreateNewFolderDialog },
    { "NavCreatePutFileDialog", nv_NavCreatePutFileDialog },
    { "NavDialogDispose", nv_NavDialogDispose },
    { "NavDialogGetReply", nv_NavDialogGetReply },
    { "NavDialogGetSaveFileName", nv_NavDialogGetSaveFileName },
    { "NavDialogGetUserAction", nv_NavDialogGetUserAction },
    { "NavDialogGetWindow", nv_NavDialogGetWindow },
    { "NavDialogRun", nv_NavDialogRun },
    { "NavDialogSetSaveFileName", nv_NavDialogSetSaveFileName },
    { "NavGetDefaultDialogCreationOptions", nv_NavGetDefaultDialogCreationOptions },
};
const unsigned carbon_nav_export_count = sizeof carbon_nav_exports / sizeof carbon_nav_exports[0];
