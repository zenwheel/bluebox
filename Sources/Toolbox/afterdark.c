/*
 * After Dark graphics modules: the screen saver's host.
 *
 * A module is a file of type 'ADgm' (creator 'ADrk') whose code is the
 * 'ADgm' resource 0 (After Dark 4's 'A4gm' files carry one as well).  The
 * code's first routine is
 *
 *     pascal OSErr main(Handle *storage, RgnHandle blankRgn, short message,
 *                       GMParamBlockPtr params);
 *
 * called with A0 at the code (THINK C modules find their globals from it)
 * and the module's resource file current.  An accelerated module's 'ADgm'
 * is a routine descriptor (procInfo $3BE0) in front of a PowerPC container:
 * Mixed Mode takes the call from there.  The messages (Berkeley Systems'
 * GraphicsModule_Types.h, 1989-91):
 *
 *     0 Initialize      allocate *storage; no drawing
 *     1 Close           dispose of everything; the user is waiting
 *     2 Blank           blank the screen (black), maybe a banner
 *     3 DrawFrame       the next frame, called over and over while idle
 *     4 ModuleSelected  chosen in the control panel (setup only)
 *     5 DoAbout         the credits area clicked (setup only)
 *     8+ a button's 'bVal' value: a button in the control panel was hit
 *
 * and main answers noErr, ModuleError (-1: After Dark shows the Pascal
 * string at params->errorMessage and draws no more), RestartMe (1:
 * Initialize again, no Close; the module closed itself), ImDone (2: no
 * more calls, After Dark keeps the screen) or RefreshResources (3, after a
 * button or ModuleSelected: the controls are read again).  Any other
 * negative answer (an OSErr) is an error too; other positive ones are
 * not.  A few modules' main is a C function: it leaves its arguments on
 * the stack and answers in D0.  A 'Cals'
 * resource (ID 1 as the manual has it, 0 as the Rez types have it), a byte,
 * says which of messages 0-5 the module takes (bit n for message n); with
 * none it takes the first four.
 *
 * The parameter block, 68k alignment (offsets in bytes):
 *
 *     0  short controlValues[4]   the four controls' values
 *     8  MonitorsInfoPtr monitors { short count; MonitorData list[] },
 *                                 MonitorData = { Rect bounds (global);
 *                                 Boolean synchFlag (set at each VBL);
 *                                 char curDepth } (10 bytes), main first
 *    12  Boolean colorQDAvail
 *    14  short systemConfig       bit 0 Color QuickDraw, 1 any monitor
 *                                 deeper than 1 bit, 2 all, 3 any in colour,
 *                                 4 all, 5 any a CLUT device, 6 all, 7 all can
 *                                 dim, 8 the main one can, 9 the module may
 *                                 not animate the colour table, 10 under
 *                                 MultiModule, 14 extensions (After Dark
 *                                 2.0u's call-backs), 15 sound
 *    16  QDGlobalsPtr qdGlobalsCopy { thePort; white, black, gray, ltGray,
 *                                 dkGray; arrow; screenBits; randSeed } in
 *                                 that (forward) order, 130 bytes
 *    20  short brightness         the module sets it, 0 (black) to 255,
 *                                 and the screen dims to it
 *    22  Rect demoRect            the control panel in Demo Mode, else empty
 *    30  StringPtr errorMessage   a Str255 the module fills on an error
 *    34  SndChannelPtr sndChannel the channel its 'Chnl' resource asked for
 *    38  short adVersion          BCD: $0200 for 2.0
 *    40  ExtensionTablePtr extensions { short count; { OSType selector;
 *                                 Ptr entryPoints } list[] }
 *
 * The extensions are tables of Pascal routines, after a short (their
 * count), that the developer kit's glue (EntryPoints.o, Sounds.o, and the
 * memory glue in the 2.0u modules) finds by selector once systemConfig's
 * bit 14 says they're there; the glue keeps the table in the first long of
 * the handle the opening call returns:
 *
 *   'ADSd' sound:  2 OpenSound(params): SoundInfoHandle { long private;
 *                  Boolean soundDisabled, hasSoundIOManager, hasPowerManager }
 *                  6 CloseSound(info, chan)   10 PlaySound(info, chan, snd)
 *                  14 QuietSound(info, chan)  18 FlushSound(info, chan)
 *                  22 GetSoundLength(info, snd): long
 *                  26 SoundBusy(info, chan): Boolean
 *   'ADmf' memory: 2 FindMemory(params, size): Handle (the module's memory,
 *                  sized from its resources; nil fails its Initialize)
 *                  6 ReleaseMemory(h)  10 UseMemory(h): allocations go there
 *                  14 RestoreMemory(h): back to the zone before
 *   'ADdl' After Dark 3's linker, a version ($0300 up) where the count is,
 *                  then its routines: After Dark 4's modules require it
 *                  (see "The linker" below), and find After Dark itself by
 *                  Gestalt 'aYmm', the event query's record
 *
 * The module's resources: 'sysz' 0 and 1 (memory wanted and the least it
 * needs), the controls 1000-1003 from the top ('sVal' slider 0-100 with
 * 'sUnt' labels, 'mVal' pop-up item with its 'MENU', 'xVal' check box,
 * 'tVal' a string of its 'STR#', 'bVal' a button sending its value as a
 * message), each with an optional 'µVal' (the systemConfig bits the control
 * needs; the next alternative is 1000 IDs on), 'Chnl' 0 (channel kind,
 * sampledSynth = 5, and volume 0-7), 'clut' 1000 + depth (the colours set
 * while it runs), 'STR ' 128 (the credit line), 'TEXT'/'PICT' 1000 (help).
 *
 * The host here runs a module on its own, as `bluebox screensaver` does
 * (the module's file is the program: the CPU starts in ad_host_native),
 * or over a running program after it has waited long enough for input
 * (`--screensaver M --idle N`), as After Dark's control panel did: the
 * screen's pixels and colours are kept and put back when it wakes.  It
 * keeps what After Dark wrote back into a module (its settings, in a file
 * of their own), runs several modules in turn as Randomize did, and
 * gives a module each display of the screen as one of its monitors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "forks.h"
#include "m68k.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { msgInitialize = 0, msgClose, msgBlank, msgDrawFrame, msgModuleSelected, msgDoAbout };
enum { ModuleError = -1, RestartMe = 1, ImDone = 2, RefreshResources = 3 };
enum {
    cfgColorQD = 1 << 0, cfgAnyMultibit = 1 << 1, cfgAllMultibit = 1 << 2, cfgAnyColor = 1 << 3,
    cfgAllColor = 1 << 4, cfgAnyCLUT = 1 << 5, cfgAllCLUT = 1 << 6, cfgAllCanDim = 1 << 7,
    cfgMainCanDim = 1 << 8, cfgMayNotAnimate = 1 << 9, cfgMultiModule = 1 << 10,
    cfgExtensions = 1 << 14, cfgSound = 1 << 15
};
enum {
    PB_controls = 0, PB_monitors = 8, PB_colorQD = 12, PB_config = 14, PB_qd = 16, PB_brightness = 20,
    PB_demoRect = 22, PB_error = 30, PB_channel = 34, PB_version = 38, PB_extensions = 40, PB_SIZE = 44
};
enum { QDCOPY_SIZE = 130, AD_VERSION = 0x0300, DL_SLOTS = 16 };
enum { nullEvent = 0, mouseDown = 1, keyDown = 3, autoKey = 5 };

/* The options (the CLI's and the screen saver's). */
static struct {
    bool sound;
    double seconds;          /* stop after this long; 0 runs until input */
    double snap_every;       /* seconds between PNGs; 0 none */
    char snap_dir[1024];
    int frames_per_tick;     /* DrawFrame calls a tick at most */
    uint32_t tick_budget;    /* instructions a tick before waiting for the next */
    bool control_given[4];
    int16_t control[4];
    int16_t buttons[8];      /* setup: buttons pressed before it runs */
    int nbuttons;
    char idle_module[1024];  /* idle blanking in a normal run */
    double idle_minutes;
    bool quiet;
    char settings_dir[1024]; /* where each module's changed resources are kept; "" not kept */
} opt = { .frames_per_tick = 4, .tick_budget = 100000 };

/* The displays the screen is made of (After Dark's monitors), in screen
 * coordinates, the main one first; none: the screen is one. */
enum { MAX_MONITORS = 16 };
static struct {
    qrect r[MAX_MONITORS];
    int n;
} monitors;

static void rotation_reset(void);

void machine_screensaver_reset(void)
{
    rotation_reset();
    monitors.n = 0;
    memset(&opt, 0, sizeof opt);
    opt.frames_per_tick = 4;
    opt.tick_budget = 100000;
}
void machine_screensaver_set_sound(bool on) { opt.sound = on; }
void machine_screensaver_set_seconds(double s) { opt.seconds = s; }
void machine_screensaver_set_pace(int frames_per_tick, uint32_t tick_budget)
{
    opt.frames_per_tick = frames_per_tick > 0 ? frames_per_tick : 4;
    opt.tick_budget = tick_budget ? tick_budget : 100000;
}
void machine_screensaver_set_snapshots(double every, const char *dir)
{
    opt.snap_every = every;
    snprintf(opt.snap_dir, sizeof opt.snap_dir, "%s", dir ? dir : "");
}
bool machine_screensaver_set_control(int index, int value)
{
    if (index >= 1000)
        index -= 1000;
    if (index < 0 || index > 3)
        return false;
    opt.control_given[index] = true;
    opt.control[index] = (int16_t)value;
    return true;
}
/* What the screen shows before the module starts (the screen saver's
 * desktop picture): RGBA, scaled to the screen. */
static uint8_t *backdrop;
static int backdrop_w, backdrop_h;

void machine_screensaver_set_backdrop(const uint8_t *rgba, int w, int h)
{
    free(backdrop);
    backdrop = NULL;
    if (!rgba || w <= 0 || h <= 0)
        return;
    size_t n = (size_t)w * (size_t)h * 4;
    backdrop = malloc(n);
    if (!backdrop)
        return;
    memcpy(backdrop, rgba, n);
    backdrop_w = w;
    backdrop_h = h;
}

bool machine_screensaver_add_monitor(int left, int top, int right, int bottom)
{
    if (monitors.n == MAX_MONITORS || right <= left || bottom <= top)
        return false;
    monitors.r[monitors.n++] = (qrect){ .top = (int16_t)top, .left = (int16_t)left, .bottom = (int16_t)bottom,
                                        .right = (int16_t)right };
    return true;
}

bool machine_screensaver_press_button(int button)
{
    if (opt.nbuttons == 8 || button < 8)
        return false;
    opt.buttons[opt.nbuttons++] = (int16_t)button;
    return true;
}

void machine_screensaver_set_idle(const char *module, double minutes)
{
    snprintf(opt.idle_module, sizeof opt.idle_module, "%s", module ? module : "");
    opt.idle_minutes = minutes;
}

/* ------------------------------------------------------------------ */
/* Settings: what After Dark wrote back into a module's file (the controls
 * set in its panel, and whatever the module itself changed in its
 * resources at a button or in its setup) is kept in a file of its own,
 * the resources that differ from the module's, and laid over the
 * module's resources when it runs again.  The module file itself is
 * never written: it may be read-only (the sandboxed screen saver's are)
 * or shared.  The file is named for the module and its resources' hash,
 * so copies of the same module share their settings and other versions
 * don't. */

void machine_screensaver_set_settings_dir(const char *dir)
{
    snprintf(opt.settings_dir, sizeof opt.settings_dir, "%s", dir ? dir : "");
}

static uint32_t fnv32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool settings_path_for(const char *module, const uint8_t *fork, size_t len, char *out, size_t outsize)
{
    if (!opt.settings_dir[0])
        return false;
    const char *base = strrchr(module, '/');
    base = base ? base + 1 : module;
    snprintf(out, outsize, "%s/%s %08X.rsrc", opt.settings_dir, base, fnv32(fork, len));
    return true;
}

bool machine_screensaver_settings_path(const char *module, char *out, size_t outsize)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(module, &mf, err, sizeof err))
        return false;
    bool ok = settings_path_for(module, mf.rsrc, mf.rsrc_len, out, outsize);
    mac_file_free(&mf);
    return ok;
}

static const rsrc_entry *find_entry(const rsrc_index *idx, uint32_t type, int16_t id)
{
    for (size_t i = 0; i < idx->count; i++)
        if (idx->entries[i].type == type && idx->entries[i].id == id)
            return &idx->entries[i];
    return NULL;
}

/* base with over's resources replacing or added to its own (malloc'd). */
static bool merge_forks(const uint8_t *base, size_t blen, const uint8_t *over, size_t olen, uint8_t **out,
                        size_t *outlen)
{
    rsrc_index bi, oi;
    char err[160];
    if (!rsrc_parse(base, blen, &bi, err, sizeof err))
        return false;
    if (!rsrc_parse(over, olen, &oi, err, sizeof err)) {
        rsrc_index_free(&bi);
        return false;
    }
    rsrc_spec *specs = calloc(bi.count + oi.count + 1, sizeof *specs);
    size_t n = 0;
    for (size_t i = 0; i < bi.count; i++) {
        const rsrc_entry *e = &bi.entries[i];
        if (find_entry(&oi, e->type, e->id))
            continue;
        specs[n++] = (rsrc_spec){ e->type, e->id, e->name, e->attrs, rsrc_data(base, &bi, e), e->data_len };
    }
    for (size_t i = 0; i < oi.count; i++) {
        const rsrc_entry *e = &oi.entries[i];
        specs[n++] = (rsrc_spec){ e->type, e->id, e->name, e->attrs, rsrc_data(over, &oi, e), e->data_len };
    }
    bool ok = rsrc_build(specs, n, out, outlen);
    free(specs);
    rsrc_index_free(&bi);
    rsrc_index_free(&oi);
    return ok;
}

/* The module's resources with its settings over them, if it has any
 * (malloc'd); NULL if not, or none are kept. */
uint8_t *machine_screensaver_module_fork(const char *module, size_t *len)
{
    mac_file mf;
    char err[256], path[1400];
    *len = 0;
    if (!appledouble_read(module, &mf, err, sizeof err))
        return NULL;
    uint8_t *out = NULL, *over = NULL;
    size_t olen = 0;
    if (mf.rsrc_len && settings_path_for(module, mf.rsrc, mf.rsrc_len, path, sizeof path) &&
        read_file(path, &over, &olen) && !merge_forks(mf.rsrc, mf.rsrc_len, over, olen, &out, len))
        out = NULL;
    if (!out && mf.rsrc_len) {
        out = malloc(mf.rsrc_len);
        memcpy(out, mf.rsrc, mf.rsrc_len);
        *len = mf.rsrc_len;
    }
    free(over);
    mac_file_free(&mf);
    return out;
}

/* A launch's module file: its settings laid over its resources. */
void afterdark_apply_settings(const char *path, uint32_t type, uint8_t **rsrc, size_t *len)
{
    if (!opt.settings_dir[0] || !*len)
        return;
    if (type != fourcc("ADgm") && type != fourcc("A4gm")) {
        /* By its resources, as afterdark_is_module_file decides. */
        rsrc_index idx;
        char e[160];
        if (!rsrc_parse(*rsrc, *len, &idx, e, sizeof e))
            return;
        bool module = false;
        for (size_t i = 0; i < idx.count; i++)
            if (idx.entries[i].type == fourcc("ADgm"))
                module = true;
        if (find_entry(&idx, fourcc("CODE"), 0))
            module = false;
        rsrc_index_free(&idx);
        if (!module)
            return;
    }
    char sp[1400];
    uint8_t *over = NULL, *out = NULL;
    size_t olen = 0, outlen = 0;
    if (!settings_path_for(path, *rsrc, *len, sp, sizeof sp) || !read_file(sp, &over, &olen))
        return;
    if (merge_forks(*rsrc, *len, over, olen, &out, &outlen)) {
        LOGF(LOG_RSRC, "After Dark: settings from %s\n", sp);
        free(*rsrc);
        *rsrc = out;
        *len = outlen;
    }
    free(over);
}

static void mkdirs(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

/* Keep the resources of the file (refnum rn, the module at path) that
 * differ from the module's own; none: no settings file. */
static void save_settings_of(int16_t rn, const char *path)
{
    mac_file mf;
    char err[256], sp[1400];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return;
    if (!settings_path_for(path, mf.rsrc, mf.rsrc_len, sp, sizeof sp)) {
        mac_file_free(&mf);
        return;
    }
    tb_UpdateResFile(rn); /* the map and the changed resources into the fork in memory */
    const uint8_t *cur;
    uint32_t clen;
    rsrc_index bi, ci;
    if (rm_fork_of(rn, &cur, &clen) && rsrc_parse(mf.rsrc, mf.rsrc_len, &bi, err, sizeof err)) {
        if (rsrc_parse(cur, clen, &ci, err, sizeof err)) {
            rsrc_spec *specs = calloc(ci.count + 1, sizeof *specs);
            size_t n = 0;
            for (size_t i = 0; i < ci.count; i++) {
                const rsrc_entry *e = &ci.entries[i], *b = find_entry(&bi, e->type, e->id);
                const uint8_t *d = rsrc_data(cur, &ci, e);
                if (b && b->data_len == e->data_len && !memcmp(rsrc_data(mf.rsrc, &bi, b), d, e->data_len) &&
                    !strcmp(b->name, e->name))
                    continue;
                specs[n++] = (rsrc_spec){ e->type, e->id, e->name, e->attrs, d, e->data_len };
            }
            uint8_t *out;
            size_t olen;
            if (!n) {
                remove(sp);
            } else if (rsrc_build(specs, n, &out, &olen)) {
                mkdirs(opt.settings_dir);
                FILE *fp = fopen(sp, "wb");
                if (fp) {
                    fwrite(out, 1, olen, fp);
                    fclose(fp);
                    LOGF(LOG_RSRC, "After Dark: %zu changed resources kept in %s\n", n, sp);
                }
                free(out);
            }
            free(specs);
            rsrc_index_free(&ci);
        }
        rsrc_index_free(&bi);
    }
    mac_file_free(&mf);
}

/* Store one resource in a module's settings (the screen saver's Options
 * sheet sets the controls so); one the same as the module's own is
 * dropped from them. */
bool machine_screensaver_store_resource(const char *module, uint32_t type, int16_t id, const uint8_t *data,
                                        uint32_t len)
{
    mac_file mf;
    char err[256], sp[1400];
    if (!appledouble_read(module, &mf, err, sizeof err))
        return false;
    rsrc_index bi, oi = { 0 };
    if (!settings_path_for(module, mf.rsrc, mf.rsrc_len, sp, sizeof sp) ||
        !rsrc_parse(mf.rsrc, mf.rsrc_len, &bi, err, sizeof err)) {
        mac_file_free(&mf);
        return false;
    }
    uint8_t *over = NULL;
    size_t olen = 0;
    if (!read_file(sp, &over, &olen) || !rsrc_parse(over, olen, &oi, err, sizeof err))
        memset(&oi, 0, sizeof oi);
    const rsrc_entry *b = find_entry(&bi, type, id);
    bool same = b && b->data_len == len && !memcmp(rsrc_data(mf.rsrc, &bi, b), data, len);
    rsrc_spec *specs = calloc(oi.count + 2, sizeof *specs);
    size_t n = 0;
    for (size_t i = 0; i < oi.count; i++) {
        const rsrc_entry *e = &oi.entries[i];
        if (e->type != type || e->id != id)
            specs[n++] = (rsrc_spec){ e->type, e->id, e->name, e->attrs, rsrc_data(over, &oi, e), e->data_len };
    }
    if (!same)
        specs[n++] = (rsrc_spec){ type, id, b ? b->name : "", b ? b->attrs : 0, data, len };
    bool ok = true;
    uint8_t *out;
    size_t outlen;
    if (!n) {
        remove(sp);
    } else if ((ok = rsrc_build(specs, n, &out, &outlen))) {
        mkdirs(opt.settings_dir);
        FILE *fp = fopen(sp, "wb");
        ok = fp && fwrite(out, 1, outlen, fp) == outlen;
        if (fp)
            fclose(fp);
        free(out);
    }
    free(specs);
    if (oi.entries)
        rsrc_index_free(&oi);
    rsrc_index_free(&bi);
    free(over);
    mac_file_free(&mf);
    return ok;
}

/* What the module host learned, for the CLI's report and the tests. */
static struct {
    uint32_t calls[9];       /* by message (8: any button) */
    uint32_t frames;
    int16_t last_result;
    char error[256];
    uint32_t snaps;
} stats;

uint32_t machine_screensaver_frames(void) { return stats.frames; }
uint32_t machine_screensaver_calls(int message) { return message >= 0 && message < 9 ? stats.calls[message] : 0; }
const char *machine_screensaver_error(void) { return stats.error; }

/* One module, running. */
typedef struct {
    bool active;
    int16_t rn;          /* its resource file */
    bool own_file;       /* opened here (idle blanking): closed at the end */
    gaddr code, entry;
    uint8_t cals;
    gaddr cell;          /* the Handle its storage goes in */
    gaddr rgn, pb, mons, qd, ext, sd, mf, dl, err, port, vbl, evt, chanp;
    bool color, stopped; /* stopped: ImDone or an error; no more calls */
    bool time_up;        /* the seconds asked for have passed */
    gaddr zone;          /* the zone UseMemory gives */
    uint32_t snap_tick, start_tick;
    /* idle blanking: what goes back */
    uint8_t *saved_screen;
    uint32_t saved_len, saved_base;
    gaddr saved_port, saved_ctab;
    int16_t saved_res;
    int brightness;
} ad_state;

static ad_state ad;

/* The module's settings kept: after its setup and when it closes. */
static void save_settings(void)
{
    char path[1024];
    if (opt.settings_dir[0] && ad.rn > 0 && rm_host_path(ad.rn, path, sizeof path))
        save_settings_of(ad.rn, path);
}
static bool natives_registered; /* since the boot */
static gaddr sd_stubs[7], mf_stubs[4], dl_stubs[DL_SLOTS], vbl_stub;

/* ------------------------------------------------------------------ */
/* The extensions */

/* SoundInfo: { long private; Boolean soundDisabled, hasSoundIOManager, hasPowerManager } */
static void sd_open(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* params */
    LOGF(LOG_TRAPS, "  After Dark: OpenSound\n");
    gaddr h = tb_NewHandle(8, false, true);
    if (h) {
        gaddr p = rd32(h);
        wr8(p + 4, opt.sound ? 0 : 1);
        wr8(p + 5, 1);
    }
    wr32(c->a[7], h);
    cpu_push32(c, ret);
}

static void quiet(gaddr chan, bool flush)
{
    if (!chan)
        return;
    gaddr cmd = tb_NewPtr(8, false, true);
    if (flush) {
        wr16(cmd, 4); /* flushCmd */
        tb_SndDoImmediate(chan, cmd);
    }
    wr16(cmd, 3); /* quietCmd */
    tb_SndDoImmediate(chan, cmd);
    tb_DisposePtr(cmd);
}

static void sd_close(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr chan = cpu_pop32(c), info = cpu_pop32(c);
    quiet(chan, true);
    if (info)
        tb_DisposeHandle(info);
    cpu_push32(c, ret);
}

static void sd_play(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr snd = cpu_pop32(c), chan = cpu_pop32(c);
    cpu_pop32(c); /* info */
    LOGF(LOG_TRAPS, "  After Dark: PlaySound %08X on channel %08X\n", snd, chan);
    if (opt.sound && chan && snd && rd32(snd)) {
        quiet(chan, true);
        tb_SndPlay(chan, snd, 1);
    }
    cpu_push32(c, ret);
}

static void sd_quiet(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr chan = cpu_pop32(c);
    cpu_pop32(c);
    quiet(chan, false);
    cpu_push32(c, ret);
}

static void sd_flush(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr chan = cpu_pop32(c);
    cpu_pop32(c);
    quiet(chan, true);
    cpu_push32(c, ret);
}

/* The samples in a sampled sound: its header's length (an extended or
 * compressed header counts frames). */
static int32_t sound_length(gaddr snd)
{
    if (!snd || !rd32(snd))
        return 0;
    gaddr p = mem_strip(rd32(snd));
    int32_t size = tb_GetHandleSize(snd);
    uint16_t format = rd16(p);
    gaddr q = p + 2;
    if (format == 1) {
        int n = (int16_t)rd16(q);
        q += 2 + (uint32_t)n * 6;
    } else {
        q += 2;
    }
    int ncmds = (int16_t)rd16(q);
    q += 2;
    for (int i = 0; i < ncmds && q + 8 <= p + (uint32_t)size; i++, q += 8) {
        uint16_t cmd = rd16(q) & 0x7FFF;
        if (cmd != 80 && cmd != 81) /* soundCmd, bufferCmd */
            continue;
        gaddr hdr = p + rd32(q + 4);
        uint8_t enc = rd8(hdr + 20);
        return enc == 0 ? (int32_t)rd32(hdr + 4) : (int32_t)rd32(hdr + 22) * (int32_t)(rd32(hdr + 4) ? rd32(hdr + 4) : 1);
    }
    return 0;
}

static void sd_length(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr snd = cpu_pop32(c);
    cpu_pop32(c);
    wr32(c->a[7], (uint32_t)sound_length(snd));
    cpu_push32(c, ret);
}

static void sd_busy(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr chan = cpu_pop32(c);
    cpu_pop32(c);
    bool busy = false;
    if (chan && opt.sound) {
        gaddr st = tb_NewPtr(24, false, true);
        if (tb_SndChannelStatus(chan, 24, st) == 0)
            busy = rd8(st + 18) != 0; /* scChannelBusy */
        tb_DisposePtr(st);
    }
    wr16(c->a[7], busy ? 0x0100 : 0);
    cpu_push32(c, ret);
}

/* The memory handle: { long private (the glue's); THz saved; } */
static void mf_find(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* size: the zone the host gives has room */
    cpu_pop32(c); /* params */
    wr32(c->a[7], tb_NewHandle(8, false, true));
    cpu_push32(c, ret);
}

static void mf_release(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr h = cpu_pop32(c);
    if (h)
        tb_DisposeHandle(h);
    cpu_push32(c, ret);
}

static void mf_use(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr h = cpu_pop32(c);
    if (h && rd32(h)) {
        wr32(mem_strip(rd32(h)) + 4, rd32(LM_TheZone));
        wr32(LM_TheZone, ad.zone ? ad.zone : rd32(LM_ApplZone));
    }
    cpu_push32(c, ret);
}

static void mf_restore(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr h = cpu_pop32(c);
    if (h && rd32(h) && rd32(mem_strip(rd32(h)) + 4))
        wr32(LM_TheZone, rd32(mem_strip(rd32(h)) + 4));
    cpu_push32(c, ret);
}

/* The VBL task: each monitor's synchFlag goes true, as After Dark's did:
 * $FF, as ST sets it (Color Swarm waits in DrawFrame with TST.B and BGE
 * until the flag is negative). */
static void snapshot_due(void);
static void time_up_check(void);
static uint32_t run_start; /* Ticks when the screen saver started (the modules' turns within it) */

static void vbl_native(cpu68k *c, uint16_t tw)
{
    gaddr task = c->a[0];
    wr16(task + VBLTask_vblCount_off, 1);
    if (ad.active && ad.mons) {
        int n = (int16_t)rd16(ad.mons);
        for (int i = 0; i < n; i++)
            wr8(ad.mons + 2 + (uint32_t)i * 10 + 8, 0xFF);
    }
    /* A module may stay in DrawFrame until it sees input (Horizontal
     * Hold, Color Swarm): its pictures and its time are kept from here. */
    if (ad.active && !ad.own_file) {
        snapshot_due();
        time_up_check();
    }
}

/* The linker: 'ADdl', After Dark 3's.  Its 68k half links a module's own
 * segments ('CODE', 'DATA', 'CREL', 'DREL', 'JUMP') to the "After Dark
 * 4.0 Library"; its other half, which the PowerPC modules use (the After
 * Dark 4.0 Shared library's MacXResourceFile), opens a module's resource
 * databases and makes one current around each resource call:
 *
 *   26 OpenDatabase(params, name, *ref): OSErr  name nil: the module's own
 *   30 CloseDatabase(ref)   34 UseDatabase(ref)   38 RestoreDatabase(): long
 *
 * A named database is a resource file in the module's folder, in a
 * "Module Resources" folder beside it or one up, or in the System Folder's
 * "After Dark Files".  The 68k half (2 to 22, and 62, which links a
 * library by its 'dll#') isn't here: After Dark 4's modules carry PowerPC
 * code too, and a 68k one that needs it (Flying Toilets Demo) needs After
 * Dark 3's libraries as well, which the archive doesn't have.  A call to
 * it halts, naming the caller. */
enum { DL_DEPTH = 16, DL_FILES = 16 };
static struct {
    int16_t rn[DL_FILES];       /* by ref - 1; 0 free */
    bool own[DL_FILES];         /* opened here: closed with the module */
    int16_t stack[DL_DEPTH];    /* the files current before each Use */
    int depth;
} dl;

static void dl_unknown(cpu68k *c, uint16_t tw)
{
    cpu_halt(c, "After Dark's linker: a call it doesn't know, from %08X", rd32(c->a[7]));
}

static bool dl_find(const char *dir, const char *name, char *out, size_t outsize)
{
    static const char *const places[] = { "%s/%s", "%s/Module Resources/%s", "%s/../Module Resources/%s",
                                          "%s/After Dark Files/%s", "%s/After Dark Files/Module Resources/%s" };
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++) {
        snprintf(out, outsize, places[i], dir, name);
        if (fm_host_exists(out))
            return true;
    }
    return false;
}

static void dl_open(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    gaddr refp = cpu_pop32(c), name = cpu_pop32(c);
    cpu_pop32(c); /* params */
    int16_t err = 0, rn = ad.rn;
    bool own = false;
    char cname[256] = "";
    if (name && rd8(name)) {
        snprintf(cname, sizeof cname, "%.*s", rd8(name), (const char *)gptr(name + 1));
        char module[1024], dir[1024], path[1100];
        err = -43; /* fnfErr */
        if (rm_host_path(ad.rn, module, sizeof module)) {
            snprintf(dir, sizeof dir, "%s", module);
            char *slash = strrchr(dir, '/');
            if (slash)
                *slash = 0;
            bool found = dl_find(dir, cname, path, sizeof path);
            if (!found) {
                snprintf(dir, sizeof dir, "%s", fm_system_folder_host());
                found = dl_find(dir, cname, path, sizeof path);
            }
            mac_file mf;
            char e[256];
            if (found && appledouble_read(path, &mf, e, sizeof e)) {
                int16_t saved = tb_CurResFile();
                rn = mf.rsrc_len ? rm_open_host_file(path, mf.rsrc, (uint32_t)mf.rsrc_len, true) : -1;
                tb_UseResFile(saved);
                mac_file_free(&mf);
                err = rn > 0 ? 0 : -39; /* eofErr: no resource fork */
                own = rn > 0;
            }
        }
    }
    int ref = 0;
    for (int i = 0; !err && i < DL_FILES && !ref; i++)
        if (!dl.rn[i]) {
            dl.rn[i] = rn;
            dl.own[i] = own;
            ref = i + 1;
        }
    if (!err && !ref)
        err = -42; /* tmfoErr */
    LOGF(LOG_TRAPS, "  After Dark: OpenDatabase \"%s\" -> %d (file %d)\n", cname, err ? err : ref, rn);
    if (refp)
        wr32(refp, (uint32_t)ref);
    wr16(c->a[7], (uint16_t)err);
    cpu_push32(c, ret);
}

static void dl_close_ref(int ref)
{
    if (ref < 1 || ref > DL_FILES || !dl.rn[ref - 1])
        return;
    if (dl.own[ref - 1])
        tb_CloseResFile(dl.rn[ref - 1]);
    dl.rn[ref - 1] = 0;
    dl.own[ref - 1] = false;
}

static void dl_close(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    dl_close_ref((int)cpu_pop32(c));
    cpu_push32(c, ret);
}

static void dl_use(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int ref = (int)cpu_pop32(c);
    if (dl.depth < DL_DEPTH)
        dl.stack[dl.depth++] = tb_CurResFile();
    if (ref >= 1 && ref <= DL_FILES && dl.rn[ref - 1])
        tb_UseResFile(dl.rn[ref - 1]);
    cpu_push32(c, ret);
}

static void dl_restore(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t was = tb_CurResFile();
    if (dl.depth > 0)
        tb_UseResFile(dl.stack[--dl.depth]);
    wr32(c->a[7], (uint32_t)(int32_t)was);
    cpu_push32(c, ret);
}

static void dl_end(void)
{
    for (int i = 1; i <= DL_FILES; i++)
        dl_close_ref(i);
    dl.depth = 0;
}

static void register_natives(void)
{
    if (natives_registered)
        return;
    natives_registered = true;
    static const native_fn sd[7] = { sd_open, sd_close, sd_play, sd_quiet, sd_flush, sd_length, sd_busy };
    static const char *const sdn[7] = { "AD OpenSound", "AD CloseSound", "AD PlaySound", "AD QuietSound",
                                        "AD FlushSound", "AD GetSoundLength", "AD SoundBusy" };
    for (int i = 0; i < 7; i++)
        sd_stubs[i] = native_register(sd[i], sdn[i], 0);
    static const native_fn mf[4] = { mf_find, mf_release, mf_use, mf_restore };
    static const char *const mfn[4] = { "AD FindMemory", "AD ReleaseMemory", "AD UseMemory", "AD RestoreMemory" };
    for (int i = 0; i < 4; i++)
        mf_stubs[i] = native_register(mf[i], mfn[i], 0);
    vbl_stub = native_register(vbl_native, "AD VBL", 0);
    for (int i = 0; i < DL_SLOTS; i++)
        dl_stubs[i] = native_register(dl_unknown, "AD linker", 0);
    dl_stubs[6] = native_register(dl_open, "AD OpenDatabase", 0);
    dl_stubs[7] = native_register(dl_close, "AD CloseDatabase", 0);
    dl_stubs[8] = native_register(dl_use, "AD UseDatabase", 0);
    dl_stubs[9] = native_register(dl_restore, "AD RestoreDatabase", 0);
}

static gaddr make_table(const gaddr *stubs, int n)
{
    gaddr t = tb_NewPtr(2 + 4 * n, false, true);
    wr16(t, (uint16_t)n);
    for (int i = 0; i < n; i++)
        wr32(t + 2 + 4 * (uint32_t)i, stubs[i]);
    return t;
}

/* ------------------------------------------------------------------ */
/* The module's resources */

static int16_t res_word(const char *type, int16_t id, bool *found)
{
    gaddr h = tb_Get1Resource((int32_t)fourcc(type), id);
    *found = h && rd32(h) && tb_GetHandleSize(h) >= 2;
    return *found ? (int16_t)rd16(mem_strip(rd32(h))) : 0;
}

/* 'µVal' (MacRoman µ is $B5). */
static uint32_t mu_val_type(void) { return 0xB5000000u | ((uint32_t)'V' << 16) | ((uint32_t)'a' << 8) | 'l'; }

/* A control's value: the first alternative (1000 IDs apart) whose 'µVal'
 * the configuration meets, else the last there is. */
static int16_t control_value_res(int i, uint16_t config, gaddr *res)
{
    static const char *const kinds[] = { "sVal", "mVal", "xVal", "tVal", "bVal" };
    int16_t value = 0;
    *res = 0;
    for (int alt = 0; alt < 9; alt++) {
        int16_t id = (int16_t)(1000 + i + alt * 1000);
        bool any = false;
        int16_t v = 0;
        for (size_t k = 0; k < sizeof kinds / sizeof kinds[0] && !any; k++) {
            bool f;
            int16_t w = res_word(kinds[k], id, &f);
            if (f) {
                any = true;
                v = k == 4 ? 0 : w; /* a button has no value */
                *res = k < 3 ? tb_Get1Resource((int32_t)fourcc(kinds[k]), id) : 0; /* the ones with a value to set */
            }
        }
        if (!any)
            continue;
        value = v;
        gaddr mu = tb_Get1Resource((int32_t)mu_val_type(), id);
        if (!mu || !rd32(mu) || tb_GetHandleSize(mu) < 2)
            break;
        uint16_t need = rd16(mem_strip(rd32(mu)));
        if ((need & config) == need)
            break;
    }
    return value;
}

static int16_t control_value(int i, uint16_t config)
{
    gaddr res;
    return control_value_res(i, config, &res);
}

static uint16_t system_config(void)
{
    int depth = qd_screen_depth();
    uint16_t cfg = cfgAllCanDim | cfgMainCanDim | cfgExtensions;
    if (ad.color)
        cfg |= cfgColorQD;
    if (depth > 1)
        cfg |= cfgAnyMultibit | cfgAllMultibit | cfgAnyColor | cfgAllColor;
    if (depth > 1 && depth <= 8)
        cfg |= cfgAnyCLUT | cfgAllCLUT;
    if (opt.sound)
        cfg |= cfgSound;
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Calling the module */

static int16_t call_module(int16_t message)
{
    cpu68k *c = g_cpu;
    int16_t saved_res = tb_CurResFile();
    gaddr saved_port = tb_NewPtr(4, false, true);
    tb_GetPort(saved_port);
    tb_UseResFile(ad.rn);
    tb_SetPort(ad.port);
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint32_t sp = c->a[7];
    cpu_push16(c, 0); /* the result */
    cpu_push32(c, ad.cell);
    cpu_push32(c, ad.rgn);
    cpu_push16(c, (uint16_t)message);
    cpu_push32(c, ad.pb);
    c->a[0] = ad.entry;
    guest_call_regs(ad.entry);
    /* A Pascal main took its arguments off and left the result; one
     * written as a C function left them and answered in D0. */
    int16_t result = c->a[7] == sp - 2 ? (int16_t)rd16(c->a[7]) : (int16_t)c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    stats.calls[message < 8 ? message : 8]++;
    stats.last_result = result;
    tb_SetPort(rd32(saved_port));
    tb_DisposePtr(saved_port);
    tb_UseResFile(saved_res);
    /* The brightness it asked for. */
    int b = (int16_t)rd16(ad.pb + PB_brightness);
    if (b != ad.brightness) {
        ad.brightness = b < 0 ? 0 : b > 255 ? 255 : b;
        qd_screen_set_brightness(ad.brightness);
    }
    return result;
}

static bool understands(int message) { return message < 8 && (ad.cals >> message) & 1; }

/* After Dark's own screen when the module stopped: black, and the
 * module's message if it left one. */
static void show_error(void)
{
    gaddr s = rd32(ad.pb + PB_error);
    int len = s ? rd8(s) : 0;
    snprintf(stats.error, sizeof stats.error, "%.*s", len, len ? (const char *)gptr(s + 1) : "");
    tb_SetPort(ad.port);
    tb_ForeColor(33); /* blackColor */
    tb_PaintRgn(ad.rgn);
    if (!len)
        return;
    tb_ForeColor(30); /* whiteColor */
    tb_TextFont(0);
    tb_TextSize(12);
    gaddr r = ad.port + GrafPort_portRect_off;
    int16_t w = tb_StringWidth(s);
    int16_t cx = (int16_t)(((int16_t)rd16(r + 2) + (int16_t)rd16(r + 6)) / 2);
    int16_t cy = (int16_t)(((int16_t)rd16(r) + (int16_t)rd16(r + 4)) / 2);
    tb_MoveTo((int16_t)(cx - w / 2), cy);
    tb_DrawString(s);
    tb_ForeColor(33);
}

static bool rotating(void);

/* A result that ends the module's run. */
static void handle_result(int16_t r, int16_t message)
{
    if (r == 0 || ad.stopped)
        return;
    if (r == ImDone) {
        ad.stopped = true;
        return;
    }
    if (r == RestartMe && message != msgClose) {
        for (int tries = 0; tries < 4; tries++) {
            mm_clear_free(ad.zone ? ad.zone : rd32(LM_TheZone));
            int16_t again = call_module(msgInitialize);
            if (again != RestartMe) {
                handle_result(again, msgInitialize);
                return;
            }
        }
    }
    /* Other positive answers are no error (VS Screen Saver's Blank leaves
     * QDExtensions' selector, 23); an error is negative: ModuleError, or
     * an OSErr such as the kit's glue returns from MemError. */
    if (r > 0)
        return;
    /* "requires Millions of colours" (Gloss, Ice, Madman's Quilt): when
     * the host allows, the run ends to start again at that depth, as a
     * program's alert asking for one does. */
    gaddr msg = rd32(ad.pb + PB_error);
    if (msg && rd8(msg)) {
        char text[256];
        snprintf(text, sizeof text, "%.*s", rd8(msg), (const char *)gptr(msg + 1));
        if (!rotating() && depth_alert(text)) { /* taking turns, the next one comes instead */
            ad.stopped = true;
            return;
        }
    }
    ad.stopped = true;
    if (!rd8(rd32(ad.pb + PB_error)))
        snprintf(stats.error, sizeof stats.error, "error %d", r);
    if (!opt.quiet)
        fprintf(stderr, "bluebox: the module stopped (%d) on message %d%s%.*s\n", r, message,
                rd8(rd32(ad.pb + PB_error)) ? ": " : "", rd8(rd32(ad.pb + PB_error)),
                (const char *)gptr(rd32(ad.pb + PB_error) + 1));
    show_error();
}

/* The colours the module's 'clut' for this depth asks for. */
static void set_module_colors(void)
{
    int depth = qd_screen_depth();
    if (depth < 2 || depth > 8)
        return;
    gaddr h = tb_Get1Resource((int32_t)fourcc("clut"), (int16_t)(1000 + depth));
    if (!h || !rd32(h) || tb_GetHandleSize(h) < 8)
        return;
    gaddr p = mem_strip(rd32(h));
    int16_t n = (int16_t)rd16(p + 6);
    if (n < 0 || n >= (1 << depth) || tb_GetHandleSize(h) < 8 + (n + 1) * 8)
        return;
    gaddr spec = tb_NewPtr((n + 1) * 8, false, true);
    gmemcpy(spec, p + 8, (uint32_t)(n + 1) * 8);
    tb_SetEntries(0, n, spec);
    tb_DisposePtr(spec);
}

/* The screen's colour table now, to put back. */
static gaddr copy_screen_colors(void)
{
    gaddr t = qd_screen_ctab();
    if (!t || !rd32(t) || qd_screen_depth() > 8)
        return 0;
    int32_t sz = tb_GetHandleSize(t);
    gaddr copy = tb_NewPtr(sz, true, false);
    if (copy)
        gmemcpy(copy, mem_strip(rd32(t)), (uint32_t)sz);
    return copy;
}

static void restore_screen_colors(gaddr copy)
{
    if (!copy)
        return;
    int16_t n = (int16_t)rd16(copy + 6);
    tb_SetEntries(0, n, copy + 8);
    tb_DisposePtr(copy);
}

static gaddr info_record(void);

/* Set everything up and call Initialize and Blank; false if the module
 * can't run (no code). */
static bool ad_begin(int16_t rn, bool over_program)
{
    cpu68k *c = g_cpu;
    register_natives();
    /* After Dark 3 and 4 answered Gestalt 'aYmm' with the record the
     * event query gets (After Dark 4's AfterDarkExists asks so). */
    gestalt_set(fourcc("aYmm"), info_record());
    memset(&ad, 0, sizeof ad);
    ad.rn = rn;
    ad.own_file = over_program;
    ad.zone = over_program ? rd32(LM_SysZone) : 0;
    ad.brightness = 255;
    ad.color = !(rd16(LM_ROM85) & 0x4000);
    int16_t saved_res = tb_CurResFile();
    tb_UseResFile(rn);
    ad.code = tb_Get1Resource((int32_t)fourcc("ADgm"), 0);
    if (!ad.code)
        ad.code = tb_Get1IndResource((int32_t)fourcc("ADgm"), 1);
    if (!ad.code || !rd32(ad.code)) {
        tb_UseResFile(saved_res);
        snprintf(stats.error, sizeof stats.error, "no ADgm code");
        return false;
    }
    tb_DetachResource(ad.code); /* ours now: a module's ReleaseResource can't take it */
    tb_MoveHHi(ad.code);
    tb_HLock(ad.code);
    ad.entry = mem_strip(rd32(ad.code));
    /* The messages it takes: a byte, bit n for message n; none, or one
     * naming none of the four it must take, is the first four. */
    gaddr cals = tb_Get1Resource((int32_t)fourcc("Cals"), 1);
    if (!cals)
        cals = tb_Get1Resource((int32_t)fourcc("Cals"), 0);
    ad.cals = cals && rd32(cals) && tb_GetHandleSize(cals) >= 1 ? rd8(mem_strip(rd32(cals))) : 0x0F;
    if (!(ad.cals & 0x0F))
        ad.cals |= 0x0F;

    /* The port: the whole screen, every monitor in it. */
    ad.port = tb_NewPtr(CGrafPort_SIZEOF, false, true);
    if (ad.color)
        tb_OpenCPort(ad.port);
    else
        tb_OpenPort(ad.port);
    gaddr bounds = tb_NewPtr(8, false, true);
    int w = 0, h = 0;
    machine_screen_size(&w, &h);
    wr16(bounds + 4, (uint16_t)h);
    wr16(bounds + 6, (uint16_t)w);
    tb_SetPort(ad.port);
    tb_ClipRect(bounds);

    /* The monitors, main first: the displays given, else the screen.  The
     * region to blank is all of them, and each starts from the backdrop. */
    int nmon = monitors.n ? monitors.n : 1;
    ad.mons = tb_NewPtr(2 + 10 * nmon, false, true);
    wr16(ad.mons, (uint16_t)nmon);
    ad.rgn = tb_NewRgn();
    gaddr one = tb_NewRgn();
    for (int i = 0; i < nmon; i++) {
        gaddr m = ad.mons + 2 + 10 * (uint32_t)i;
        if (monitors.n) {
            const qrect *r = &monitors.r[i];
            wr16(m, (uint16_t)r->top);
            wr16(m + 2, (uint16_t)r->left);
            wr16(m + 4, (uint16_t)r->bottom);
            wr16(m + 6, (uint16_t)r->right);
        } else {
            gmemcpy(m, bounds, 8);
        }
        wr8(m + 9, (uint8_t)qd_screen_depth());
        tb_RectRgn(one, m);
        tb_UnionRgn(ad.rgn, one, ad.rgn);
        if (backdrop && !ad.own_file)
            qd_draw_rgba(backdrop, backdrop_w, backdrop_h,
                         (qrect){ .top = (int16_t)rd16(m), .left = (int16_t)rd16(m + 2), .bottom = (int16_t)rd16(m + 4),
                                  .right = (int16_t)rd16(m + 6) });
    }
    tb_DisposeRgn(one);
    tb_DisposePtr(bounds);

    /* The QuickDraw globals, a copy in their record's order. */
    ad.qd = tb_NewPtr(QDCOPY_SIZE, false, true);
    gaddr g = rd32(c->a[5]); /* &thePort */
    wr32(ad.qd, ad.port);
    for (int i = 0; i < 5; i++) /* white, black, gray, ltGray, dkGray */
        gmemcpy(ad.qd + 4 + (uint32_t)i * 8, g - 8 - (uint32_t)i * 8, 8);
    gmemcpy(ad.qd + 44, g - 108, 68);  /* arrow */
    gmemcpy(ad.qd + 112, g - 122, 14); /* screenBits */
    wr32(ad.qd + 126, rd32(g - 126));  /* randSeed */

    ad.sd = make_table(sd_stubs, 7);
    ad.mf = make_table(mf_stubs, 4);
    ad.dl = make_table(dl_stubs, DL_SLOTS);
    wr16(ad.dl, AD_VERSION); /* the linker's version, not a count */
    ad.ext = tb_NewPtr(2 + 3 * 8, false, true);
    wr16(ad.ext, 3);
    wr32(ad.ext + 18, fourcc("ADdl"));
    wr32(ad.ext + 22, ad.dl);
    wr32(ad.ext + 2, fourcc("ADSd"));
    wr32(ad.ext + 6, ad.sd);
    wr32(ad.ext + 10, fourcc("ADmf"));
    wr32(ad.ext + 14, ad.mf);
    ad.err = tb_NewPtr(256, false, true);

    ad.pb = tb_NewPtr(PB_SIZE, false, true);
    uint16_t cfg = system_config();
    for (int i = 0; i < 4; i++) {
        gaddr res;
        int16_t v = control_value_res(i, cfg, &res);
        /* A value given is set in the module's resource, as After Dark's
         * panel did: it is kept with the module's settings. */
        if (opt.control_given[i]) {
            v = opt.control[i];
            if (res && rd32(res) && tb_GetHandleSize(res) >= 2 && (int16_t)rd16(mem_strip(rd32(res))) != v) {
                wr16(mem_strip(rd32(res)), (uint16_t)v);
                tb_ChangedResource(res);
            }
        }
        wr16(ad.pb + PB_controls + 2 * (uint32_t)i, (uint16_t)v);
    }
    wr32(ad.pb + PB_monitors, ad.mons);
    wr8(ad.pb + PB_colorQD, ad.color ? 1 : 0);
    wr16(ad.pb + PB_config, cfg);
    wr32(ad.pb + PB_qd, ad.qd);
    wr16(ad.pb + PB_brightness, 255);
    wr32(ad.pb + PB_error, ad.err);
    wr16(ad.pb + PB_version, AD_VERSION);
    wr32(ad.pb + PB_extensions, ad.ext);

    /* The sound channel its 'Chnl' asks for. */
    gaddr chnl = tb_Get1Resource((int32_t)fourcc("Chnl"), 0);
    if (opt.sound && chnl && rd32(chnl)) {
        ad.chanp = tb_NewPtr(4, false, true);
        int16_t kind = (int16_t)rd16(mem_strip(rd32(chnl)));
        if (tb_SndNewChannel(ad.chanp, kind == 1 || kind == 3 ? kind : 5, 0, 0) == 0)
            wr32(ad.pb + PB_channel, rd32(ad.chanp));
    }
    set_module_colors();
    tb_UseResFile(saved_res);

    ad.cell = tb_NewPtr(4, false, true);
    ad.evt = tb_NewPtr(EventRecord_SIZEOF, false, true);
    ad.vbl = tb_NewPtr(VBLTask_SIZEOF, false, true);
    wr16(ad.vbl + VBLTask_qType_off, 1); /* vType */
    wr32(ad.vbl + VBLTask_vblAddr_off, vbl_stub);
    wr16(ad.vbl + VBLTask_vblCount_off, 1);
    tb_VInstall(ad.vbl);
    ad.active = true;
    ad.start_tick = ad.snap_tick = rd32(LM_Ticks);

    LOGF(LOG_TRAPS, "After Dark: module code at %08X, Cals %02X\n", ad.entry, ad.cals);
    /* Setup first, as in the control panel: the module chosen, then its
     * buttons (a button given by its 'bVal' ID sends that one's value). */
    if (opt.nbuttons) {
        if (understands(msgModuleSelected))
            call_module(msgModuleSelected);
        for (int i = 0; i < opt.nbuttons && !c->halted; i++) {
            int16_t b = opt.buttons[i];
            if (b >= 1000) {
                tb_UseResFile(rn);
                bool f;
                int16_t v = res_word("bVal", b, &f);
                tb_UseResFile(saved_res);
                if (!f) {
                    fprintf(stderr, "bluebox: the module has no button %d\n", b);
                    continue;
                }
                b = v;
            }
            if (call_module(b) == RefreshResources) {
                tb_UseResFile(rn);
                for (int k = 0; k < 4; k++)
                    if (!opt.control_given[k])
                        wr16(ad.pb + PB_controls + 2 * (uint32_t)k, (uint16_t)control_value(k, cfg));
                tb_UseResFile(saved_res);
            }
        }
        tb_SetPort(ad.port);
        tb_ForeColor(33);
        tb_PaintRgn(ad.rgn);
    }
    save_settings(); /* the controls given, and what the setup changed */
    /* The memory it gets is as fresh as the heap After Dark made it (and
     * again at each RestartMe: Peek-a-boo, starting over, reads a field of
     * its new storage as nil, its sound being off). */
    mm_clear_free(ad.zone ? ad.zone : rd32(LM_TheZone));
    int16_t r = call_module(msgInitialize);
    handle_result(r, msgInitialize);
    if (!ad.stopped) {
        if (understands(msgBlank)) {
            handle_result(call_module(msgBlank), msgBlank);
        } else {
            tb_SetPort(ad.port);
            tb_ForeColor(33);
            tb_PaintRgn(ad.rgn);
        }
    }
    return true;
}

static void snapshot(void)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/frame-%05u.png", opt.snap_dir, stats.snaps++);
    machine_screenshot(path);
}

/* The PNG due every snap_every seconds, if it is. */
static void snapshot_due(void)
{
    if (opt.snap_every <= 0 || !opt.snap_dir[0])
        return;
    uint32_t every = (uint32_t)(opt.snap_every * 60 + 0.5);
    if (!every)
        every = 1;
    if (rd32(LM_Ticks) - ad.snap_tick >= every) {
        ad.snap_tick += every;
        snapshot();
    }
}

/* The seconds are up: a module still in DrawFrame is woken as by a key. */
static void time_up_check(void)
{
    if (opt.seconds <= 0 || ad.time_up)
        return;
    if (rd32(LM_Ticks) - run_start >= (uint32_t)(opt.seconds * 60 + 0.5)) {
        ad.time_up = true;
        machine_post_key(' ', 49, 0, 1, 0);
        machine_post_key(' ', 49, 0, 0, 0);
    }
}

/* Frames for one tick, then wait for the next. */
static void ad_tick(void)
{
    cpu68k *c = g_cpu;
    uint32_t tick = rd32(LM_Ticks);
    uint64_t start = c->icount;
    for (int n = 0; n < opt.frames_per_tick && !ad.stopped && !c->halted; n++) {
        handle_result(call_module(msgDrawFrame), msgDrawFrame);
        stats.frames++;
        if (rd32(LM_Ticks) != tick || c->icount - start >= opt.tick_budget)
            break;
    }
    snapshot_due();
}

static void ad_end(void)
{
    if (!ad.active)
        return;
    if (!ad.stopped && understands(msgClose))
        call_module(msgClose);
    save_settings();
    tb_VRemove(ad.vbl);
    if (ad.chanp && rd32(ad.chanp))
        tb_SndDisposeChannel(rd32(ad.chanp), 1);
    if (ad.brightness != 255)
        qd_screen_set_brightness(255);
    if (ad.color)
        tb_CloseCPort(ad.port);
    else
        tb_ClosePort(ad.port);
    tb_DisposeRgn(ad.rgn);
    dl_end();
    gaddr ptrs[] = { ad.port, ad.mons, ad.qd, ad.sd, ad.mf, ad.dl, ad.ext, ad.err, ad.pb, ad.chanp, ad.cell, ad.evt, ad.vbl };
    for (size_t i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++)
        if (ptrs[i])
            tb_DisposePtr(ptrs[i]);
    tb_HUnlock(ad.code);
    tb_DisposeHandle(ad.code);
    ad.active = false;
}

/* Input since the module started: a click, a key, or the mouse moved. */
static uint32_t input_mark;
static bool woken(void) { return ev_input_serial() != input_mark; }

/* Modules in turn, as After Dark 3 and 4's own Randomize (and the 2.0
 * Randomizer module) ran them: the first module (the one launched, or the
 * idle one), then each added, every so many seconds, in order or at
 * random.  Each one is closed (its Close, its settings kept) and the next
 * opened from its file and begun as After Dark began it. */
enum { MAX_ROTATION = 512 };
static struct {
    char *paths[MAX_ROTATION]; /* the ones after the first */
    int n;
    double seconds;
    bool random;
    int cur;                   /* 0: the first */
    int16_t rn;                /* the file opened here for the current one; 0 the first's */
    uint32_t seed;
} rot;

bool machine_screensaver_add_module(const char *path)
{
    if (!path || rot.n == MAX_ROTATION)
        return false;
    rot.paths[rot.n++] = strdup(path);
    return true;
}

void machine_screensaver_set_rotation(double seconds, bool random)
{
    rot.seconds = seconds;
    rot.random = random;
}

static void rotation_reset(void)
{
    for (int i = 0; i < rot.n; i++)
        free(rot.paths[i]);
    memset(&rot, 0, sizeof rot);
}

/* A module's file opened for its turn (its settings over it): its
 * resource file, or -1. */
static int16_t open_module(const char *path)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err) || !mf.rsrc_len) {
        fprintf(stderr, "bluebox: %s: %s\n", path, mf.rsrc_len ? err : "no resource fork");
        return -1;
    }
    int16_t saved = tb_CurResFile();
    afterdark_apply_settings(path, mac_file_type(&mf), &mf.rsrc, &mf.rsrc_len);
    int16_t rn = rm_open_host_file(path, mf.rsrc, (uint32_t)mf.rsrc_len, true);
    mac_file_free(&mf);
    tb_UseResFile(saved);
    /* One with PowerPC code runs on a Power Mac, as at a launch. */
    if (rn > 0 && afterdark_has_powerpc(path))
        gestalt_powerpc();
    return rn;
}

static bool rotating(void) { return rot.n > 0; }

static bool rotation_due(void)
{
    if (!rot.n || rot.seconds <= 0)
        return false;
    uint32_t ran = rd32(LM_Ticks) - ad.start_tick;
    /* One that stopped with its message has it seen a moment, then goes. */
    if (ad.stopped && ran >= 180)
        return true;
    return ran >= (uint32_t)(rot.seconds * 60 + 0.5);
}

/* The next module's turn.  first_rn is the first's file (the launched
 * program's, or the idle module's); over_program as for ad_begin. */
static bool rotate(int16_t first_rn, bool over_program)
{
    int count = rot.n + 1, next = rot.cur;
    if (rot.random) {
        if (!rot.seed)
            rot.seed = rd32(LM_Ticks) * 2654435761u + 1;
        for (int tries = 0; tries < 8 && next == rot.cur; tries++) {
            rot.seed = rot.seed * 1103515245u + 12345u;
            next = (int)((rot.seed >> 8) % (uint32_t)count);
        }
    } else {
        next = (rot.cur + 1) % count;
    }
    ad_end();
    if (rot.rn > 0)
        tb_CloseResFile(rot.rn);
    rot.rn = 0;
    int16_t rn = first_rn;
    if (next > 0) {
        rn = open_module(rot.paths[next - 1]);
        if (rn < 0) {
            rn = first_rn;
            next = 0;
        } else {
            rot.rn = rn;
        }
    }
    rot.cur = next;
    LOGF(LOG_TRAPS, "After Dark: the next module, %s\n", next ? rot.paths[next - 1] : "the first");
    return ad_begin(rn, over_program);
}

/* ------------------------------------------------------------------ */
/* A module on its own (bluebox screensaver) */

static void ad_host_native(cpu68k *c, uint16_t tw)
{
    gaddr a5 = rd32(LM_CurrentA5); /* the launch made the A5 world, QuickDraw's globals below it */
    c->a[5] = a5;
    tb_InitGraf(a5 - 4);
    tb_InitFonts();
    tb_FlushEvents(-1, 0);
    tb_InitWindows();
    tb_InitMenus();
    tb_TEInit();
    tb_InitDialogs(0);
    tb_InitCursor();
    tb_HideCursor();
    int16_t rn = (int16_t)rd16(LM_CurApRefNum);
    input_mark = ev_input_serial();
    run_start = rd32(LM_Ticks);
    rot.cur = 0;
    if (!ad_begin(rn, false)) {
        cpu_halt(c, "ExitToShell (no ADgm code in the module)");
        return;
    }
    uint32_t limit = opt.seconds > 0 ? (uint32_t)(opt.seconds * 60 + 0.5) : 0;
    if (opt.snap_every > 0 && opt.snap_dir[0])
        snapshot(); /* the blanked screen */
    while (!c->halted) {
        if (limit && rd32(LM_Ticks) - run_start >= limit)
            break;
        if (woken())
            break;
        if (rotation_due() && !rotate(rn, false))
            break;
        if (!ad.stopped)
            ad_tick();
        /* Wait for the next tick: input ends it. */
        tb_WaitNextEvent((int16_t)(1 << mouseDown | 1 << keyDown | 1 << autoKey), ad.evt, 1, 0);
    }
    if (c->halted)
        return;
    bool timed = ad.time_up || (limit && rd32(LM_Ticks) - run_start >= limit);
    ad_end();
    if (rot.rn > 0)
        tb_CloseResFile(rot.rn);
    rot.rn = 0;
    tb_ShowCursor();
    cpu_halt(c, timed ? "ExitToShell (the screen saver ran its time)" : "ExitToShell (woken by input)");
}

bool afterdark_is_module_file(void)
{
    if (tb_Get1Resource((int32_t)fourcc("CODE"), 0))
        return false;
    return tb_Count1Resources((int32_t)fourcc("ADgm")) > 0;
}

/* Whether the module file holds PowerPC code: an accelerated 'ADgm', a
 * PowerPC container in a resource (the 68k stubs of the "PPC" modules load
 * one) or in its data fork (After Dark 4's). */
bool afterdark_has_powerpc(const char *path)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return false;
    static const uint8_t pef[8] = { 'J', 'o', 'y', '!', 'p', 'e', 'f', 'f' };
    bool found = mf.data_len >= 8 && !memcmp(mf.data, pef, 8);
    for (size_t i = 0; !found && mf.rsrc_len >= 8 && i + 8 <= mf.rsrc_len; i++)
        found = mf.rsrc[i] == 'J' && !memcmp(mf.rsrc + i, pef, 8);
    mac_file_free(&mf);
    return found;
}

uint32_t afterdark_module_entry(void)
{
    memset(&stats, 0, sizeof stats);
    return native_register(ad_host_native, "After Dark", 0);
}

/* ------------------------------------------------------------------ */
/* Idle blanking inside a run (--screensaver M --idle N) */

static uint32_t idle_since;   /* Ticks at the last input */
static uint32_t idle_serial;
static bool in_saver;

/* Called where a program waits for events: after the minutes without
 * input, the module blanks the screen until input comes; the program's
 * screen comes back as it was. */
void afterdark_idle(void)
{
    if (!opt.idle_module[0] || in_saver || !g_cpu || g_cpu->halted)
        return;
    uint32_t now = rd32(LM_Ticks);
    uint32_t serial = ev_input_serial();
    if (serial != idle_serial || !idle_since) {
        idle_serial = serial;
        idle_since = now ? now : 1;
        return;
    }
    if ((double)(now - idle_since) < opt.idle_minutes * 3600)
        return;
    cpu68k *c = g_cpu;
    mac_file mf;
    char err[256];
    if (!appledouble_read(opt.idle_module, &mf, err, sizeof err) || !mf.rsrc_len) {
        fprintf(stderr, "bluebox: --screensaver %s: %s\n", opt.idle_module, mf.rsrc_len ? err : "no resource fork");
        opt.idle_module[0] = 0;
        return;
    }
    in_saver = true;
    int16_t saved_res = tb_CurResFile();
    afterdark_apply_settings(opt.idle_module, mac_file_type(&mf), &mf.rsrc, &mf.rsrc_len);
    int16_t rn = rm_open_host_file(opt.idle_module, mf.rsrc, (uint32_t)mf.rsrc_len, true);
    mac_file_free(&mf);
    tb_UseResFile(saved_res);
    if (rn < 0) {
        in_saver = false;
        opt.idle_module[0] = 0;
        return;
    }
    LOGF(LOG_TRAPS, "After Dark: blanking after %.1f minutes without input\n", opt.idle_minutes);
    /* What goes back: the screen's pixels and colours. */
    uint32_t base = rd32(LM_ScrnBase);
    uint32_t len = qd_screen_row_bytes() * (uint32_t)g_screen_height;
    uint8_t *saved = malloc(len);
    if (saved)
        memcpy(saved, gptr(base), len);
    gaddr colors = copy_screen_colors();
    gaddr zone = rd32(LM_TheZone);
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint8_t crsr = rd8(LM_CrsrVis);
    tb_HideCursor();
    input_mark = ev_input_serial();
    run_start = rd32(LM_Ticks);
    rot.cur = 0;
    bool begun = ad_begin(rn, true);
    while (begun && !c->halted && !woken()) {
        if (rotation_due() && !(begun = rotate(rn, true)))
            break;
        if (!ad.stopped)
            ad_tick();
        tb_WaitNextEvent(0, ad.evt, 1, 0); /* no events taken: the program gets the one that wakes it */
    }
    if (begun && !c->halted)
        ad_end();
    if (rot.rn > 0)
        tb_CloseResFile(rot.rn);
    rot.rn = 0;
    /* The key or click that woke it was for the screen saver, not the
     * program under it. */
    tb_FlushEvents((int16_t)(1 << mouseDown | 1 << 2 /* mouseUp */ | 1 << keyDown | 1 << 4 /* keyUp */ | 1 << autoKey), 0);
    tb_CloseResFile(rn);
    tb_UseResFile(saved_res);
    restore_screen_colors(colors);
    if (saved) {
        memcpy(gptr(base), saved, len);
        free(saved);
    }
    if (crsr)
        tb_ShowCursor();
    wr32(LM_TheZone, zone);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    idle_serial = ev_input_serial();
    idle_since = rd32(LM_Ticks);
    in_saver = false;
}

bool afterdark_running(void) { return in_saver || ad.active; }

/* After Dark's answer to "are you there?": a module (Berkeley Systems'
 * glue, AfterDarkExists) puts 'aYmm' in an event record's message and asks
 * for an event with a mask of 0; After Dark's patch put 'ADrk' there and
 * in the where field a pointer to its record, whose word at 12 is its
 * version (BCD), which the module compares with the one it needs. */
static gaddr ad_info;

static gaddr info_record(void)
{
    if (!ad_info) {
        ad_info = tb_NewPtr(32, true, true);
        wr16(ad_info + 12, AD_VERSION);
    }
    return ad_info;
}

bool afterdark_answer(gaddr evt)
{
    if (!evt || rd32(evt + 2) != fourcc("aYmm") || !(ad.active || opt.idle_module[0]))
        return false;
    info_record();
    wr16(evt, 0); /* nullEvent */
    wr32(evt + 2, fourcc("ADrk"));
    wr32(evt + 10, ad_info);
    return true;
}

void afterdark_reset(void)
{
    idle_since = 0;
    in_saver = false;
    natives_registered = false;
    ad_info = 0;
    memset(&ad, 0, sizeof ad);
}
