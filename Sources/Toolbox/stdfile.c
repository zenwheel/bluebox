/*
 * Standard File Package.
 *
 * The host shows its own open and save panels.  Dialog hooks, filters and
 * custom items are not run; replies are filled as the classic package would.
 * Without a host every request is cancelled.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"

int16_t fm_wd_for_path(const char *path, gaddr name_out);
uint32_t fm_type_of_path(const char *path);

static machine_file_fn file_fn;
static void *file_ctx;
static char last_dir[PATH_MAX]; /* where the last panel was left */

void machine_set_file_handler(machine_file_fn fn, void *ctx)
{
    file_fn = fn;
    file_ctx = ctx;
}

uint32_t machine_file_type(const char *path) { return fm_type_of_path(path); }

static void utf8_from_pstr(gaddr p, char *out, size_t outsize)
{
    out[0] = 0;
    if (!p)
        return;
    uint8_t s[256];
    uint32_t n = rd8(p);
    gget(s, p + 1, n);
    macroman_to_utf8(s, n, out, outsize);
}

/* Ask the host; returns true with a path if the user did not cancel. */
static bool ask(bool save, gaddr prompt, gaddr default_name, gaddr types, int16_t ntypes, char *path, size_t pathsize)
{
    machine_file_request rq;
    memset(&rq, 0, sizeof rq);
    rq.save = save;
    utf8_from_pstr(prompt, rq.prompt, sizeof rq.prompt);
    utf8_from_pstr(default_name, rq.default_name, sizeof rq.default_name);
    for (char *c = rq.default_name; *c; c++)
        if (*c == '/')
            *c = ':'; /* a Mac name's '/' is ':' on the host */
    if (types && ntypes > 0) {
        rq.ntypes = ntypes > 16 ? 16 : ntypes;
        for (int i = 0; i < rq.ntypes; i++)
            rq.types[i] = rd32(types + (uint32_t)i * 4);
    }
    snprintf(rq.directory, sizeof rq.directory, "%s", last_dir);
    if (!file_fn) {
        LOGF(LOG_UNIMPL, "Standard File: no host, %s cancelled\n", save ? "save" : "open");
        return false;
    }
    if (!file_fn(&rq, path, pathsize, file_ctx) || !path[0])
        return false;
    snprintf(last_dir, sizeof last_dir, "%s", path);
    char *slash = strrchr(last_dir, '/');
    if (slash)
        *slash = 0;
    return true;
}

/* System 7 reply.  The chosen folder also becomes the default for the next
 * call, as with the classic package (SFSaveDisk, CurDirStore). */
static void fill_reply(gaddr reply, bool good, const char *path, bool save)
{
    gmemset(reply, 0, StandardFileReply_SIZEOF);
    if (!good)
        return;
    gaddr spec = reply + StandardFileReply_sfFile_off;
    fm_spec_for_path(path, spec);
    struct stat st;
    bool exists = stat(path, &st) == 0;
    wr8(reply + StandardFileReply_sfGood_off, 1);
    wr8(reply + StandardFileReply_sfReplacing_off, save && exists ? 1 : 0);
    wr32(reply + StandardFileReply_sfType_off, exists ? fm_type_of_path(path) : 0);
    wr8(reply + StandardFileReply_sfIsFolder_off, exists && S_ISDIR(st.st_mode) ? 1 : 0);
    wr16(LM_SFSaveDisk, (uint16_t)(-(int16_t)rd16(spec + FSSpec_vRefNum_off)));
    wr32(LM_CurDirStore, rd32(spec + FSSpec_parID_off));
}

/* Old-style reply: a working directory and a name. */
static void fill_old_reply(gaddr reply, bool good, const char *path, bool save)
{
    gmemset(reply, 0, SFReply_SIZEOF);
    if (!good)
        return;
    gaddr name = tb_NewPtr(256, false, true);
    int16_t wd = fm_wd_for_path(path, name);
    uint32_t n = rd8(name) > 63 ? 63 : rd8(name);
    wr8(reply + SFReply_fName_off, (uint8_t)n);
    gmemcpy(reply + SFReply_fName_off + 1, name + 1, n);
    tb_DisposePtr(name);
    struct stat st;
    bool exists = stat(path, &st) == 0;
    wr8(reply + SFReply_good_off, 1);
    wr16(reply + SFReply_vRefNum_off, (uint16_t)wd);
    wr32(reply + SFReply_fType_off, exists ? fm_type_of_path(path) : 0);
    (void)save;
}

void tb_StandardGetFile(gaddr filef, int16_t numt, gaddr tl, gaddr replyp)
{
    char path[PATH_MAX];
    fill_reply(replyp, ask(false, 0, 0, tl, numt, path, sizeof path), path, false);
}

void tb_StandardPutFile(gaddr prompt, gaddr defaultname, gaddr replyp)
{
    char path[PATH_MAX];
    fill_reply(replyp, ask(true, prompt, defaultname, 0, 0, path, sizeof path), path, true);
}

void tb_CustomGetFile(gaddr filefilter, int16_t numtypes, gaddr typelist, gaddr replyp, int16_t dlgid, uint32_t where,
                      gaddr dlghook, gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    tb_StandardGetFile(filefilter, numtypes, typelist, replyp);
}

void tb_CustomPutFile(gaddr prompt, gaddr defaultName, gaddr replyp, int16_t dlgid, uint32_t where, gaddr dlghook,
                      gaddr filterproc, gaddr activeList, gaddr activateproc, gaddr yourdatap)
{
    tb_StandardPutFile(prompt, defaultName, replyp);
}

void tb_SFGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    fill_old_reply(rep, ask(false, prompt, 0, tl, numt, path, sizeof path), path, false);
}

void tb_SFPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep)
{
    char path[PATH_MAX];
    fill_old_reply(rep, ask(true, prompt, name, 0, 0, path, sizeof path), path, true);
}

void tb_SFPGetFile(uint32_t p, gaddr prompt, gaddr filef, int16_t numt, gaddr tl, gaddr dh, gaddr rep, int16_t dig,
                   gaddr fp)
{
    tb_SFGetFile(p, prompt, filef, numt, tl, dh, rep);
}

void tb_SFPPutFile(uint32_t p, gaddr prompt, gaddr name, gaddr dh, gaddr rep, int16_t dig, gaddr fp)
{
    tb_SFPutFile(p, prompt, name, dh, rep);
}
