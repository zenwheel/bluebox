/*
 * Navigation Services (NavigationLib), on the host's open and save panels
 * as Standard File shows them (stdfile.c).
 *
 * The program's object filter decides which files an open panel offers,
 * called for each with the file's FSSpec and NavFileOrFolderInfo as the
 * browser list would; its event and preview procedures are not called,
 * the panel being the host's own.  A reply's selection is an AEDescList
 * of FSSpecs (one: the panels choose one item).  Asking to save or
 * discard changes is the host's alert, as other alerts are.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "carbon_svc.h"
#include "forks.h"
#include "machine.h"
#include "tb_internal.h"

bool dialog_host_alert(machine_alert *a, int *result); /* dialog.c */

enum { userCanceledErr = -128 };
enum { kNavDefaultNavDlogOptions = 0x000000E4 }; /* no translate items; stationery, previews, several files */
enum { kNavSaveChangesClosingDocument = 1, kNavSaveChangesQuittingApplication = 2 };
enum { kNavAskSaveChangesSave = 1, kNavAskSaveChangesCancel = 2, kNavAskSaveChangesDontSave = 3 };
enum { kNavAskDiscardChanges = 1, kNavAskDiscardChangesCancel = 2 };
enum { kNavFilteringBrowserList = 0 };
#define typeFSS 0x66737320u /* 'fss ' */

/* NavObjectFilterProcPtr: pascal Boolean (AEDesc *, void *info, void *ud, NavFilterModes) */
enum { uppNavObjectFilterProcInfo = 0x00002FD0 };

/* ---------------------------------------------------------------------- */
/* Options */

static void option_text(gaddr options, uint32_t off, char *out, size_t size)
{
    out[0] = 0;
    if (!options || !rd8(options + off))
        return;
    uint8_t s[256];
    uint32_t n = rd8(options + off);
    gget(s, options + off + 1, n);
    macroman_to_utf8(s, n, out, size);
}

/* NavGetDefaultDialogOptions(options): the defaults, the client's name
 * the application's. */
int16_t tb_NavGetDefaultDialogOptions(gaddr options)
{
    if (!options)
        return paramErr;
    gmemset(options, 0, NavDialogOptions_SIZEOF);
    wr32(options + NavDialogOptions_dialogOptionFlags_off, kNavDefaultNavDlogOptions);
    wr32(options + NavDialogOptions_location_off, 0xFFFFFFFFu); /* (-1, -1): the default place */
    gmemcpy(options + NavDialogOptions_clientName_off, LM_CurApName, 32);
    return noErr;
}

int16_t tb_NavLoad(void) { return noErr; }
int16_t tb_NavUnload(void) { return noErr; }
uint32_t tb_NavLibraryVersion(void) { return 0x01108000; } /* 1.1 */
int8_t tb_NavServicesCanRun(void) { return 1; }
int8_t tb_NavServicesAvailable(void) { return 1; }

/* ---------------------------------------------------------------------- */
/* Panels */

/* A request from the options, starting where defaultLocation (an FSSpec)
 * says. */
static void request(machine_file_request *rq, gaddr location, gaddr options, bool save, bool folders)
{
    memset(rq, 0, sizeof *rq);
    rq->save = save;
    rq->folders = folders;
    option_text(options, NavDialogOptions_message_off, rq->prompt, sizeof rq->prompt);
    if (!rq->prompt[0])
        option_text(options, NavDialogOptions_windowTitle_off, rq->prompt, sizeof rq->prompt);
    if (save) {
        option_text(options, NavDialogOptions_savedFileName_off, rq->default_name, sizeof rq->default_name);
        for (char *c = rq->default_name; *c; c++)
            if (*c == '/')
                *c = ':'; /* a Mac name's '/' is ':' on the host */
    }
    gaddr h = location && AEDesc_descriptorType(location) == (int32_t)typeFSS ? AEDesc_dataHandle(location) : 0;
    if (h && rd32(h)) {
        gaddr spec = rd32(h);
        char path[PATH_MAX];
        struct stat st;
        if (fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path) &&
            stat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) { /* a file: its folder, as the browser shows it */
                char *slash = strrchr(path, '/');
                if (slash)
                    *slash = 0;
            }
            snprintf(rq->directory, sizeof rq->directory, "%s", path);
        }
    }
}

/* The reply: valid, its selection the one item chosen. */
static void fill_reply(gaddr reply, const char *path, bool save)
{
    struct stat st;
    bool exists = stat(path, &st) == 0, dir = exists && S_ISDIR(st.st_mode);
    fm_grant(path, dir); /* the user chose it */
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    fm_spec_for_path(path, spec);
    gaddr list = reply + NavReplyRecord_selection_off;
    tb_AECreateList(0, 0, false, list);
    tb_AEPutPtr(list, 0, (int32_t)typeFSS, spec, FSSpec_SIZEOF);
    tb_DisposePtr(spec);
    wr8(reply + NavReplyRecord_validRecord_off, 1);
    wr8(reply + NavReplyRecord_replacing_off, save && exists ? 1 : 0);
}

static void clear_reply(gaddr reply)
{
    if (reply)
        gmemset(reply, 0, NavReplyRecord_SIZEOF);
}

/* The program's object filter, for each file the open panel would offer. */
static gaddr filter_proc, filter_ud;

static bool nav_filtered(const char *path)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), desc = tb_NewPtr(AEDesc_SIZEOF, false, true);
    gaddr info = tb_NewPtr(NavFileOrFolderInfo_SIZEOF, false, true), pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true);
    gaddr name = tb_NewPtr(256, false, true);
    fm_spec_for_path(path, spec);
    tb_AECreateDesc((int32_t)typeFSS, spec, FSSpec_SIZEOF, desc);
    gmemcpy(name, spec + FSSpec_name_off, 64);
    wr32(pb + HFileInfo_ioNamePtr_off, name);
    wr16(pb + HFileInfo_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
    wr32(pb + HFileInfo_ioDirID_off, rd32(spec + FSSpec_parID_off));
    wr8(info + NavFileOrFolderInfo_visible_off, 1);
    if (tb_PBGetCatInfo(pb, false) == noErr) {
        bool folder = (rd8(pb + HFileInfo_ioFlAttrib_off) & 0x10) != 0;
        wr8(info + NavFileOrFolderInfo_isFolder_off, folder);
        wr32(info + NavFileOrFolderInfo_creationDate_off, rd32(pb + HFileInfo_ioFlCrDat_off));
        wr32(info + NavFileOrFolderInfo_modificationDate_off, rd32(pb + HFileInfo_ioFlMdDat_off));
        if (folder) {
            wr32(info + NavFileOrFolderInfo_fileAndFolder_folderInfo_numberOfFiles_off,
                 rd16(pb + DirInfo_ioDrNmFls_off));
            gmemcpy(info + NavFileOrFolderInfo_fileAndFolder_folderInfo_finderDInfo_off, pb + HFileInfo_ioFlFndrInfo_off, 16);
        } else {
            wr8(info + NavFileOrFolderInfo_fileAndFolder_fileInfo_locked_off, rd8(pb + HFileInfo_ioFlAttrib_off) & 1);
            wr32(info + NavFileOrFolderInfo_fileAndFolder_fileInfo_dataSize_off, rd32(pb + HFileInfo_ioFlLgLen_off));
            wr32(info + NavFileOrFolderInfo_fileAndFolder_fileInfo_resourceSize_off, rd32(pb + HFileInfo_ioFlRLgLen_off));
            gmemcpy(info + NavFileOrFolderInfo_fileAndFolder_fileInfo_finderInfo_off, pb + HFileInfo_ioFlFndrInfo_off, 16);
        }
    }
    uint32_t args[4] = { desc, info, filter_ud, kNavFilteringBrowserList };
    uint32_t shown = mm_is_descriptor(filter_proc) ? mm_call(filter_proc, uppNavObjectFilterProcInfo, args, 4)
                                                   : ppc_call(filter_proc, args, 4);
    tb_AEDisposeDesc(desc);
    tb_DisposePtr(name);
    tb_DisposePtr(pb);
    tb_DisposePtr(info);
    tb_DisposePtr(desc);
    tb_DisposePtr(spec);
    return (shown & 0xFF) == 0;
}

/* An open panel for a file (types from the NavTypeList: component
 * signature, reserved, count, the types) or a folder. */
static int16_t choose(gaddr location, gaddr reply, gaddr options, gaddr filter, gaddr types, gaddr ud, bool folders)
{
    if (!reply)
        return paramErr;
    clear_reply(reply);
    machine_file_request rq;
    request(&rq, location, options, false, folders);
    gaddr tl = types ? rd32(types) : 0;
    if (tl) {
        int n = (int16_t)rd16(tl + 6);
        rq.ntypes = n < 0 ? 0 : n > 16 ? 16 : n;
        for (int i = 0; i < rq.ntypes; i++)
            rq.types[i] = rd32(tl + 8 + (uint32_t)i * 4);
    }
    filter_proc = filter;
    filter_ud = ud;
    if (filter)
        stdfile_set_filter(nav_filtered);
    char path[PATH_MAX];
    bool chosen = stdfile_request(&rq, path, sizeof path);
    stdfile_set_filter(NULL);
    filter_proc = 0;
    if (!chosen)
        return userCanceledErr;
    fill_reply(reply, path, false);
    return noErr;
}

int16_t tb_NavGetFile(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc, gaddr previewProc,
                      gaddr filterProc, gaddr typeList, gaddr callBackUD)
{
    return choose(defaultLocation, reply, dialogOptions, filterProc, typeList, callBackUD, false);
}

int16_t tb_NavChooseFile(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc, gaddr previewProc,
                         gaddr filterProc, gaddr typeList, gaddr callBackUD)
{
    return choose(defaultLocation, reply, dialogOptions, filterProc, typeList, callBackUD, false);
}

int16_t tb_NavChooseFolder(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc,
                           gaddr filterProc, gaddr callBackUD)
{
    return choose(defaultLocation, reply, dialogOptions, filterProc, 0, callBackUD, true);
}

int16_t tb_NavChooseObject(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc,
                           gaddr filterProc, gaddr callBackUD)
{
    return choose(defaultLocation, reply, dialogOptions, filterProc, 0, callBackUD, false);
}

/* NavChooseVolume: a folder chosen, and its disk the answer. */
int16_t tb_NavChooseVolume(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc,
                           gaddr filterProc, gaddr callBackUD)
{
    int16_t e = choose(defaultLocation, reply, dialogOptions, filterProc, 0, callBackUD, true);
    if (e != noErr)
        return e;
    gaddr list = reply + NavReplyRecord_selection_off, spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    gaddr type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true), key = tb_NewPtr(4, false, true);
    gaddr pb = tb_NewPtr(HParamBlockRec_SIZEOF, false, true), name = tb_NewPtr(256, false, true);
    if (tb_AEGetNthPtr(list, 1, (int32_t)typeFSS, key, type, spec, FSSpec_SIZEOF, size) == noErr) {
        wr32(pb + HVolumeParam_ioNamePtr_off, name);
        wr16(pb + HVolumeParam_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
        if (tb_PBHGetVInfo(pb, false) == noErr) { /* the volume: its root's parent, and its name */
            wr32(spec + FSSpec_parID_off, 1);
            gmemcpy(spec + FSSpec_name_off, name, 64);
            tb_AEPutPtr(list, 1, (int32_t)typeFSS, spec, FSSpec_SIZEOF);
        }
    }
    tb_DisposePtr(name);
    tb_DisposePtr(pb);
    tb_DisposePtr(key);
    tb_DisposePtr(size);
    tb_DisposePtr(type);
    tb_DisposePtr(spec);
    return noErr;
}

/* NavPutFile: a save panel; the reply is the file to write (replacing
 * says it is there already). */
int16_t tb_NavPutFile(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc, int32_t fileType,
                      int32_t fileCreator, gaddr callBackUD)
{
    if (!reply)
        return paramErr;
    clear_reply(reply);
    machine_file_request rq;
    request(&rq, defaultLocation, dialogOptions, true, false);
    if (!rq.default_name[0])
        snprintf(rq.default_name, sizeof rq.default_name, "untitled");
    char path[PATH_MAX];
    if (!stdfile_request(&rq, path, sizeof path))
        return userCanceledErr;
    fill_reply(reply, path, true);
    return noErr;
}

/* NavNewFolder: a save panel for the name, and the folder made. */
int16_t tb_NavNewFolder(gaddr defaultLocation, gaddr reply, gaddr dialogOptions, gaddr eventProc, gaddr callBackUD)
{
    if (!reply)
        return paramErr;
    clear_reply(reply);
    machine_file_request rq;
    request(&rq, defaultLocation, dialogOptions, true, false);
    if (!rq.default_name[0])
        snprintf(rq.default_name, sizeof rq.default_name, "untitled folder");
    char path[PATH_MAX];
    if (!stdfile_request(&rq, path, sizeof path))
        return userCanceledErr;
    struct stat st;
    if (stat(path, &st) != 0 && mkdir(path, 0777) != 0)
        return -34; /* dskFulErr: it couldn't be made */
    fill_reply(reply, path, false);
    return noErr;
}

int16_t tb_NavDisposeReply(gaddr reply)
{
    if (!reply)
        return paramErr;
    tb_AEDisposeDesc(reply + NavReplyRecord_selection_off);
    wr8(reply + NavReplyRecord_validRecord_off, 0);
    if (rd16(reply + NavReplyRecord_version_off) >= 1) { /* Carbon's: its saveFileName, a CFString */
        svc_cfrelease(rd32(reply + 24));
        wr32(reply + 24, 0);
    }
    return noErr;
}

/* Nothing is translated: the files are the program's own kind. */
int16_t tb_NavCompleteSave(gaddr reply, uint32_t howToTranslate) { return noErr; }
int16_t tb_NavTranslateFile(gaddr reply, uint32_t howToTranslate) { return noErr; }
/* NavCreatePreview (the preview stored in the file): os9left_misc.c. */
/* Only an event procedure calls it, during a dialog of Navigation's own. */
int16_t tb_NavCustomControl(gaddr context, int32_t selector, gaddr parms) { return paramErr; }

/* ---------------------------------------------------------------------- */
/* Asking about changes */

static size_t append(char *out, size_t n, size_t size, const char *s, size_t len)
{
    for (size_t i = 0; i < len && n + 1 < size; i++)
        out[n++] = s[i];
    out[n] = 0;
    return n;
}

static size_t append_pstr(char *out, size_t n, size_t size, gaddr p)
{
    if (!p)
        return n;
    char s[256];
    uint32_t len = p ? rd8(p) : 0;
    gget(s, p + 1, len);
    return append(out, n, size, s, len);
}

static void button(machine_alert *a, int16_t item, const char *title, bool is_default)
{
    machine_alert_button *b = &a->buttons[a->nbuttons++];
    b->item = item;
    snprintf(b->title, sizeof b->title, "%s", title);
    b->is_default = is_default;
    if (is_default)
        a->default_item = item;
}

/* The host's alert, or (headless) its default button. */
static int ask(machine_alert *a)
{
    int r = 0;
    if (!dialog_host_alert(a, &r)) {
        LOGF(LOG_UNIMPL, "Navigation alert: %s\n", a->message);
        machine_idle_answered(); /* headless: the user's answer is the wait for input */
    }
    return r > 0 ? r : a->default_item;
}

/* MacRoman: curly quotes and the apostrophe. */
#define LQ "\xD2"
#define RQ "\xD3"
#define APOS "\xD5"

static int save_changes(machine_alert *a, gaddr reply)
{
    a->kind = MACHINE_ALERT_CAUTION;
    button(a, kNavAskSaveChangesSave, "Save", true);
    button(a, kNavAskSaveChangesDontSave, "Don" APOS "t Save", false);
    button(a, kNavAskSaveChangesCancel, "Cancel", false);
    int r = ask(a);
    if (reply)
        wr32(reply, (uint32_t)r);
    return noErr;
}

/* NavAskSaveChanges(options, action, &reply, eventProc, ud): "Save changes
 * to the document ... before closing?" (or quitting). */
int16_t tb_NavAskSaveChanges(gaddr dialogOptions, uint32_t action, gaddr reply, gaddr eventProc, gaddr callBackUD)
{
    machine_alert a;
    memset(&a, 0, sizeof a);
    size_t n = append(a.message, 0, sizeof a.message, "Save changes to the document " LQ, strlen("Save changes to the document " LQ));
    n = append_pstr(a.message, n, sizeof a.message, dialogOptions ? dialogOptions + NavDialogOptions_savedFileName_off : 0);
    const char *end = action == kNavSaveChangesQuittingApplication ? RQ " before quitting?"
                      : action == kNavSaveChangesClosingDocument   ? RQ " before closing?"
                                                                   : RQ "?";
    append(a.message, n, sizeof a.message, end, strlen(end));
    return (int16_t)save_changes(&a, reply);
}

/* NavCustomAskSaveChanges(options, &reply, eventProc, ud): the program's
 * own message. */
int16_t tb_NavCustomAskSaveChanges(gaddr dialogOptions, gaddr reply, gaddr eventProc, gaddr callBackUD)
{
    machine_alert a;
    memset(&a, 0, sizeof a);
    append_pstr(a.message, 0, sizeof a.message, dialogOptions ? dialogOptions + NavDialogOptions_message_off : 0);
    return (int16_t)save_changes(&a, reply);
}

/* NavAskDiscardChanges(options, &reply, eventProc, ud): revert? */
int16_t tb_NavAskDiscardChanges(gaddr dialogOptions, gaddr reply, gaddr eventProc, gaddr callBackUD)
{
    machine_alert a;
    memset(&a, 0, sizeof a);
    a.kind = MACHINE_ALERT_CAUTION;
    size_t n = append(a.message, 0, sizeof a.message, "Discard changes to the document " LQ, strlen("Discard changes to the document " LQ));
    n = append_pstr(a.message, n, sizeof a.message, dialogOptions ? dialogOptions + NavDialogOptions_savedFileName_off : 0);
    append(a.message, n, sizeof a.message, RQ "?", strlen(RQ "?"));
    button(&a, kNavAskDiscardChanges, "OK", true);
    button(&a, kNavAskDiscardChangesCancel, "Cancel", false);
    int r = ask(&a);
    if (reply)
        wr32(reply, (uint32_t)r);
    return noErr;
}
