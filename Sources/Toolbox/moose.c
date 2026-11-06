/*
 * The Talking Moose's host (Steve Halls's desk accessory, 1987-88, and
 * EDUCORP's "New Talking Moose!"): the program the desk accessory host
 * (dahost.c) becomes for a file holding the Moose.
 *
 * The Moose is a desk accessory that, when opened, copies its code into
 * the system heap and stays there after its window (Talk Settings: rate,
 * pitch, delay, volume) closes.  Its globals are a VBL task element with
 * the signature 'TALK' (the MooseRec of its MooseUnit.p); a jGNEFilter
 * watches the events, and once the Mac has had nothing but null events
 * for Delay ticks the Moose draws itself on the screen and says one of
 * the "Pause" phrases of the Moose Phrases file through MacinTalk.
 *
 * MooseUnit.p is its interface for programs, and this host uses only
 * that: StartMoose(TRUE) (ApplScratch says 'Hide', OpenDeskAcc and
 * CloseDeskAcc, the result back in ApplScratch), GetMoose (the VBL queue
 * searched for 'TALK'), NowID/NowNum to have it say a phrase from a
 * 'STR#' at the next null event, SayCode for its own phrases, MoosFlags'
 * PauseMask for the idle remarks, and DoMoose (CallMooseToSpeak).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "forks.h"
#include "m68k.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

/* MooseRec: a VBL task element (qLink, qType, vblAddr, vblCount,
 * vblPhase) with the Moose's globals after it. */
enum {
    moo_Sig = 14, moo_Rate = 18, moo_Pitch = 20, moo_Delay = 22, moo_Robot = 24, moo_Enable = 26,
    moo_Animate = 28, moo_TheSpeech = 30, moo_Error = 34, moo_NowID = 44, moo_NowNum = 46, moo_NowHandle = 56,
    moo_SayCode = 60, moo_PauseNum = 62, moo_PauseMax = 64, moo_Inter = 66, moo_DoReader = 80,
    moo_DoMoose = 86, moo_MoosFlags = 92,
};
enum { PauseMask = 1 };
enum { sayTime = 1, sayGoodBye = 2, saySwitch = 3, sayHello = 4 };
enum { pauseSTR = 1988 }; /* the Moose Phrases file's "Pause" 'STR#' */
enum { mooseID = 131 };
enum { itemJoke = 1, itemQuiet = 2, itemSettings = 4 };

extern char g_app_path[];

static const uint8_t moose_name[] = "\016\000Talking Moose"; /* the DA's name, as MooseUnit opens it */

/* GetMoose: the Moose's globals, or 0 if it isn't installed. */
gaddr moose_globals(void)
{
    gaddr m = rd32(LM_VBLQueue + 2);
    for (int n = 0; m && n < 256; n++, m = rd32(m))
        if (rd32(m + moo_Sig) == fourcc("TALK"))
            return m;
    return 0;
}

int16_t moose_menu_id(void) { return mooseID; }

static gaddr name_ptr(void)
{
    gaddr s = tb_NewPtr(sizeof moose_name, false, true);
    gput(s, moose_name, sizeof moose_name - 1);
    return s;
}

/* StartMoose(hide): 0 installed, -1 already installed, 1 no MacinTalk,
 * 2 no Moose Phrases, 3 no room in the system heap, 4 (or 5) the desk
 * accessory didn't open. */
int moose_start(bool hide)
{
    int result = moose_globals() ? -1 : 0;
    uint8_t saved[4];
    gget(saved, LM_ApplScratch, 4);
    wr32(LM_ApplScratch, fourcc(hide ? "Hide" : "Norm"));
    gaddr name = name_ptr();
    int16_t rn = tb_OpenDeskAcc(name);
    if (hide && rn)
        tb_CloseDeskAcc(rn);
    tb_DisposePtr(name);
    uint32_t code = rd32(LM_ApplScratch);
    if (code == fourcc("Norm") || code == fourcc("Hide"))
        result = 5;
    else if (code & 0xFFFF)
        result = (int16_t)(code >> 16);
    gput(LM_ApplScratch, saved, 4);
    return result;
}

/* CallMooseToSpeak: what it was asked to say, now. */
static void moose_call(gaddr m)
{
    gaddr fn = rd32(m + moo_DoMoose);
    if (!fn)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
}

/* ---- More phrases ----
 *
 * Phrase files of Uli's Moose (creator 'MOSe': Moose Phraseology's Cats,
 * Geekdom, Witticisms ...) hold English in 'STR#' resources.  Those in
 * the Moose's folder, or a folder in it, are jokes too: MacinTalk's Reader
 * makes their phonemes, which the Moose says from its NowHandle. */

static uint8_t (*more)[256]; /* Pascal strings */
static int nmore, more_room;

static void add_phrases(const char *path)
{
    mac_file f;
    char err[128];
    memset(&f, 0, sizeof f);
    if (!appledouble_read_meta(path, &f))
        return;
    rsrc_index idx;
    if (mac_file_creator(&f) == fourcc("MOSe") && f.rsrc && rsrc_parse(f.rsrc, f.rsrc_len, &idx, err, sizeof err)) {
        for (size_t i = 0; i < idx.count; i++) {
            const rsrc_entry *e = &idx.entries[i];
            if (e->type != fourcc("STR#") || e->data_len < 2)
                continue;
            const uint8_t *d = rsrc_data(f.rsrc, &idx, e);
            int n = d[0] << 8 | d[1];
            uint32_t at = 2;
            for (int k = 0; k < n && at < e->data_len && at + 1 + d[at] <= e->data_len; k++, at += 1 + d[at]) {
                if (!d[at])
                    continue;
                if (nmore == more_room) {
                    int room = more_room ? more_room * 2 : 256;
                    void *grown = bb_realloc(more, (size_t)room * sizeof *more, BB_BYTES);
                    if (!grown)
                        break;
                    more = grown;
                    more_room = room;
                }
                memcpy(more[nmore++], d + at, 1 + (size_t)d[at]);
            }
        }
        rsrc_index_free(&idx);
    }
    mac_file_free(&f);
}

static void find_phrases(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2)
                find_phrases(path, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            add_phrases(path);
        }
    }
    closedir(d);
}

/* The Moose's own Pause phrases (its phonemes) and how many: the jokes
 * numbered 1 to that, the phrase files' after them. */
static int pause_count(gaddr m)
{
    int max = m ? (int16_t)rd16(m + moo_PauseMax) : 0;
    gaddr list = tb_GetResource((int32_t)fourcc("STR#"), pauseSTR);
    if (max <= 0 && list && rd32(list))
        max = (int16_t)rd16(rd32(list));
    return max > 0 ? max : 0;
}

/* The next joke: all of them told once, in a shuffled order, before any is
 * told again. */
static int next_joke(int total)
{
    static int *order;
    static int count, next, last;
    if (count != total || next >= count) {
        int *o = bb_realloc(order, (size_t)total * sizeof *o, BB_BYTES);
        if (!o)
            return 1;
        order = o;
        for (int i = 0; i < total; i++)
            order[i] = i + 1;
        for (int i = total - 1; i > 0; i--) {
            int j = (int)arc4random_uniform((uint32_t)i + 1);
            int t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        if (total > 1 && order[0] == last) { /* not the last round's last joke again */
            order[0] = order[total - 1];
            order[total - 1] = last;
        }
        count = total;
        next = 0;
    }
    return last = order[next++];
}

static bool joke_asked; /* Tell a Joke's Pause phrase is on its way to MacinTalk */

/* A joke: the next of the Pause phrases and the phrase files', said at the
 * next null event (the Moose waits for the user to finish what they are
 * doing). */
bool moose_tell_joke(void)
{
    gaddr m = moose_globals();
    if (!m)
        return false;
    int max = pause_count(m), total = max + nmore;
    if (total <= 0)
        return false;
    int n = next_joke(total);
    if (n <= max) {
        wr16(m + moo_NowNum, (uint16_t)n);
        wr16(m + moo_NowID, pauseSTR);
        joke_asked = true;
        return true;
    }
    const uint8_t *s = more[n - max - 1];
    gaddr h = macintalk_read(s + 1, s[0]);
    if (!h)
        return false;
    gaddr old = rd32(m + moo_NowHandle);
    if (old)
        tb_DisposeHandle(old);
    wr32(m + moo_NowHandle, h);
    return true;
}

/* The Moose's idle remarks, as they reach MacinTalk: one of its Pause
 * phrases, picked by itself, becomes the next joke of them all. */
static bool swap_remark(const uint8_t *ph, size_t n, uint8_t **out, size_t *out_len, bool *english)
{
    gaddr m = moose_globals();
    gaddr list = tb_GetResource((int32_t)fourcc("STR#"), pauseSTR);
    if (!m || !list || !rd32(list))
        return false;
    gaddr p = rd32(list);
    int count = (int16_t)rd16(p);
    uint32_t size = (uint32_t)tb_GetHandleSize(list), at = 2;
    while (n && *ph == ' ') { /* the Moose puts a space before its phrases */
        ph++;
        n--;
    }
    bool pause = false;
    for (int k = 0; k < count && at < size && !pause; k++, at += 1 + rd8(p + at))
        pause = rd8(p + at) == n && at + 1 + n <= size && !memcmp(gptr(p + at + 1), ph, n);
    if (!pause)
        return false;
    if (joke_asked) { /* Tell a Joke's own pick */
        joke_asked = false;
        return false;
    }
    int max = pause_count(m), j = next_joke(max + nmore);
    const uint8_t *s;
    if (j <= max) { /* one of its own after all */
        at = 2;
        for (int k = 1; k < j && at < size; k++)
            at += 1 + rd8(p + at);
        if (at >= size)
            return false;
        *out_len = rd8(p + at);
        *out = bb_malloc(*out_len ? *out_len : 1, BB_BYTES);
        gget(*out, p + at + 1, (uint32_t)*out_len);
        *english = false;
        return true;
    }
    s = more[j - max - 1];
    *out_len = s[0];
    *out = bb_malloc(s[0] ? s[0] : 1, BB_BYTES);
    memcpy(*out, s + 1, s[0]);
    *english = true;
    return true;
}

/* Be Quiet: no remarks of its own when idle (it still says what it is
 * asked to). */
void moose_set_quiet(bool quiet)
{
    gaddr m = moose_globals();
    if (!m)
        return;
    uint32_t f = rd32(m + moo_MoosFlags);
    wr32(m + moo_MoosFlags, quiet ? f & ~(uint32_t)PauseMask : f | PauseMask);
}

bool moose_quiet(void)
{
    gaddr m = moose_globals();
    return m && !(rd32(m + moo_MoosFlags) & PauseMask);
}

/* ---- Its settings, kept between runs ----
 *
 * The desk accessory kept its settings only while the Mac was on (Switch
 * Moose set them again at startup); its host keeps them in the System
 * Folder's Preferences, "Talking Moose Prefs", a line for each: what Talk
 * Settings sets (rate, pitch, delay, robotic, talking, animation, quick
 * stop) and Be Quiet. */

static const struct {
    const char *name;
    int off;
} settings[] = {
    { "rate", moo_Rate }, { "pitch", moo_Pitch }, { "delay", moo_Delay }, { "robotic", moo_Robot },
    { "talking", moo_Enable }, { "animation", moo_Animate }, { "quickstop", moo_Inter },
};
enum { NSETTINGS = sizeof settings / sizeof settings[0] };
static int16_t saved_values[NSETTINGS + 1]; /* and Be Quiet */
static bool have_saved;

static void prefs_path(char *out, size_t n) { snprintf(out, n, "%s/Preferences/Talking Moose Prefs", fm_system_folder_host()); }

static void current_values(gaddr m, int16_t *v)
{
    for (int i = 0; i < NSETTINGS; i++)
        v[i] = (int16_t)rd16(m + (uint32_t)settings[i].off);
    v[NSETTINGS] = moose_quiet();
}

static void load_settings(gaddr m)
{
    char path[1024], line[128];
    prefs_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char name[32];
        int v;
        if (sscanf(line, "%31[a-z] %d", name, &v) != 2)
            continue;
        for (int i = 0; i < NSETTINGS; i++)
            if (!strcmp(name, settings[i].name))
                wr16(m + (uint32_t)settings[i].off, (uint16_t)v);
        if (!strcmp(name, "quiet"))
            moose_set_quiet(v != 0);
    }
    fclose(f);
}

/* Rate, pitch and Robotic to MacinTalk: the Moose's own speaking never
 * passes them on (it makes no Control calls), so Talk Settings would
 * change nothing that is heard. */
static void pass_settings(gaddr m)
{
    macintalk_settings((int16_t)rd16(m + moo_Rate), (int16_t)rd16(m + moo_Pitch), rd16(m + moo_Robot) != 0);
}

/* Written when they change: the Moose may be stopped by the host at any time. */
static void save_settings(void)
{
    gaddr m = moose_globals();
    if (!m)
        return;
    int16_t v[NSETTINGS + 1];
    current_values(m, v);
    if (have_saved && !memcmp(v, saved_values, sizeof v))
        return;
    pass_settings(m);
    char path[1024], tmp[1100];
    prefs_path(path, sizeof path);
    char *slash = strrchr(path, '/');
    *slash = 0;
    mkdir(path, 0755);
    *slash = '/';
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < NSETTINGS; i++)
        fprintf(f, "%s %d\n", settings[i].name, v[i]);
    fprintf(f, "quiet %d\n", v[NSETTINGS]);
    if (fclose(f) == 0 && rename(tmp, path) == 0) {
        memcpy(saved_values, v, sizeof v);
        have_saved = true;
    }
}

/* ---- The host's part ---- */

static void put_menu(void)
{
    gaddr s = tb_NewPtr(256, false, true);
    pstr_from_c(s, "Moose");
    gaddr mh = tb_NewMenu(mooseID, s);
    pstr_from_c(s, "Tell a Joke/J;Be Quiet;(-;Moose Settings\xC9");
    tb_AppendMenu(mh, s);
    tb_DisposePtr(s);
    tb_InsertMenu(mh, 0);
}

/* Whether the program's file holds the Moose; if so it is started,
 * hidden, and gets its menu. */
bool moose_host_start(void)
{
    gaddr name = name_ptr();
    gaddr h = tb_Get1NamedResource((int32_t)fourcc("DRVR"), name);
    tb_DisposePtr(name);
    if (!h)
        return false;
    /* QuickDraw starts randSeed at 1, so the Moose would make its idle
     * remarks in the same order at every start: a seed from the host. */
    gaddr qd = rd32(rd32(LM_CurrentA5)) - QDGlobals_thePort_off;
    wr32(qd + QDGlobals_randSeed_off, arc4random() | 1);
    int r = moose_start(true);
    gaddr m = moose_globals();
    LOGF(LOG_TRAPS, "StartMoose(TRUE) = %d: globals %08X rate %d pitch %d delay %d enable %d animate %d pauses %d flags %08X\n",
         r, m, m ? (int16_t)rd16(m + moo_Rate) : 0, m ? (int16_t)rd16(m + moo_Pitch) : 0,
         m ? (int16_t)rd16(m + moo_Delay) : 0, m ? (int16_t)rd16(m + moo_Enable) : 0,
         m ? (int16_t)rd16(m + moo_Animate) : 0, m ? (int16_t)rd16(m + moo_PauseMax) : 0, m ? rd32(m + moo_MoosFlags) : 0);
    if (r > 0) {
        static const char *const why[] = { "", "no MacinTalk", "no Moose Phrases", "no room in the system heap",
                                           "the desk accessory did not open", "the desk accessory did not open" };
        cpu_halt(g_cpu, "ExitToShell (the Talking Moose could not start: %s)", why[r < 6 ? r : 5]);
        return true;
    }
    if (m) {
        load_settings(m);
        pass_settings(m);
        current_values(m, saved_values);
        have_saved = true; /* as they were read: nothing to write yet */
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", g_app_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        find_phrases(dir, 0);
        LOGF(LOG_TRAPS, "Moose: %d more phrases from Uli's Moose phrase files\n", nmore);
    }
    /* Its idle remarks from all of them, and a moment on the screen, quiet
     * and still, before it speaks (as MacinTalk took to get going on a Mac
     * Plus) and after. */
    macintalk_host(nmore ? swap_remark : NULL, 0.5, 1.0);
    put_menu();
    tb_CheckItem(tb_GetMenuHandle(mooseID), itemQuiet, moose_quiet());
    return true;
}

/* Each turn of the host's event loop. */
void moose_host_idle(void) { save_settings(); }

bool moose_host_menu(int16_t item)
{
    gaddr mh = tb_GetMenuHandle(mooseID);
    switch (item) {
    case itemJoke:
        return moose_tell_joke();
    case itemQuiet: {
        bool quiet = !moose_quiet();
        moose_set_quiet(quiet);
        tb_CheckItem(mh, itemQuiet, quiet);
        return true;
    }
    case itemSettings: {
        gaddr name = name_ptr();
        int16_t rn = tb_OpenDeskAcc(name); /* its Talk Settings window */
        tb_DisposePtr(name);
        return rn != 0;
    }
    }
    return false;
}

/* Quitting: the Moose says goodbye, unless it was told to be quiet. */
void moose_host_quit(void)
{
    gaddr m = moose_globals();
    save_settings();
    if (!m || moose_quiet() || !rd16(m + moo_Enable))
        return;
    wr16(m + moo_SayCode, sayGoodBye);
    moose_call(m);
}
