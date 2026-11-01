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
 * button or ModuleSelected: the controls are read again).  A 'Cals'
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
 * screen's pixels and colours are kept and put back when it wakes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
enum { QDCOPY_SIZE = 130, AD_VERSION = 0x0300 };
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
} opt = { .frames_per_tick = 4, .tick_budget = 100000 };

void machine_screensaver_reset(void)
{
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
    gaddr rgn, pb, mons, qd, ext, sd, mf, err, port, vbl, evt, chanp;
    bool color, stopped; /* stopped: ImDone or an error; no more calls */
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
static bool natives_registered; /* since the boot */
static gaddr sd_stubs[7], mf_stubs[4], vbl_stub;

/* ------------------------------------------------------------------ */
/* The extensions */

/* SoundInfo: { long private; Boolean soundDisabled, hasSoundIOManager, hasPowerManager } */
static void sd_open(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* params */
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

/* The VBL task: each monitor's synchFlag goes true, as After Dark's did. */
static void vbl_native(cpu68k *c, uint16_t tw)
{
    gaddr task = c->a[0];
    wr16(task + VBLTask_vblCount_off, 1);
    if (ad.active && ad.mons) {
        int n = (int16_t)rd16(ad.mons);
        for (int i = 0; i < n; i++)
            wr8(ad.mons + 2 + (uint32_t)i * 10 + 8, 1);
    }
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
static int16_t control_value(int i, uint16_t config)
{
    static const char *const kinds[] = { "sVal", "mVal", "xVal", "tVal", "bVal" };
    int16_t value = 0;
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
    cpu_push16(c, 0); /* the result */
    cpu_push32(c, ad.cell);
    cpu_push32(c, ad.rgn);
    cpu_push16(c, (uint16_t)message);
    cpu_push32(c, ad.pb);
    c->a[0] = ad.entry;
    guest_call_regs(ad.entry);
    int16_t result = (int16_t)cpu_pop16(c);
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
            int16_t again = call_module(msgInitialize);
            if (again != RestartMe) {
                handle_result(again, msgInitialize);
                return;
            }
        }
    }
    if (r == RefreshResources)
        return;
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

/* Set everything up and call Initialize and Blank; false if the module
 * can't run (no code). */
static bool ad_begin(int16_t rn)
{
    cpu68k *c = g_cpu;
    register_natives();
    memset(&ad, 0, sizeof ad);
    ad.rn = rn;
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
    ad.rgn = tb_NewRgn();
    tb_RectRgn(ad.rgn, bounds);
    tb_SetPort(ad.port);
    tb_ClipRect(bounds);

    ad.mons = tb_NewPtr(2 + 10, false, true);
    wr16(ad.mons, 1);
    gmemcpy(ad.mons + 2, bounds, 8);
    wr8(ad.mons + 2 + 9, (uint8_t)qd_screen_depth());

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
    ad.ext = tb_NewPtr(2 + 2 * 8, false, true);
    wr16(ad.ext, 2);
    wr32(ad.ext + 2, fourcc("ADSd"));
    wr32(ad.ext + 6, ad.sd);
    wr32(ad.ext + 10, fourcc("ADmf"));
    wr32(ad.ext + 14, ad.mf);
    ad.err = tb_NewPtr(256, false, true);

    ad.pb = tb_NewPtr(PB_SIZE, false, true);
    uint16_t cfg = system_config();
    for (int i = 0; i < 4; i++)
        wr16(ad.pb + PB_controls + 2 * (uint32_t)i,
             (uint16_t)(opt.control_given[i] ? opt.control[i] : control_value(i, cfg)));
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
    stats.frames = 0;

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
    if (opt.snap_every > 0 && opt.snap_dir[0]) {
        uint32_t every = (uint32_t)(opt.snap_every * 60 + 0.5);
        if (!every)
            every = 1;
        if (rd32(LM_Ticks) - ad.snap_tick >= every) {
            ad.snap_tick += every;
            snapshot();
        }
    }
}

static void ad_end(void)
{
    if (!ad.active)
        return;
    if (!ad.stopped && understands(msgClose))
        call_module(msgClose);
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
    gaddr ptrs[] = { ad.port, ad.mons, ad.qd, ad.sd, ad.mf, ad.ext, ad.err, ad.pb, ad.chanp, ad.cell, ad.evt, ad.vbl };
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

/* ------------------------------------------------------------------ */
/* A module on its own (bluebox screensaver) */

static void ad_host_native(cpu68k *c, uint16_t tw)
{
    gaddr world = tb_NewPtr(1024, false, true);
    gaddr a5 = world + 768;
    wr32(LM_CurrentA5, a5);
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
    if (!ad_begin(rn)) {
        cpu_halt(c, "ExitToShell (no ADgm code in the module)");
        return;
    }
    uint32_t limit = opt.seconds > 0 ? (uint32_t)(opt.seconds * 60 + 0.5) : 0;
    if (opt.snap_every > 0 && opt.snap_dir[0])
        snapshot(); /* the blanked screen */
    while (!c->halted) {
        if (limit && rd32(LM_Ticks) - ad.start_tick >= limit)
            break;
        if (woken())
            break;
        if (!ad.stopped)
            ad_tick();
        /* Wait for the next tick: input ends it. */
        tb_WaitNextEvent((int16_t)(1 << mouseDown | 1 << keyDown | 1 << autoKey), ad.evt, 1, 0);
    }
    if (c->halted)
        return;
    bool timed = limit && rd32(LM_Ticks) - ad.start_tick >= limit;
    ad_end();
    tb_ShowCursor();
    cpu_halt(c, timed ? "ExitToShell (the screen saver ran its time)" : "ExitToShell (woken by input)");
}

bool afterdark_is_module_file(void)
{
    if (tb_Get1Resource((int32_t)fourcc("CODE"), 0))
        return false;
    return tb_Count1Resources((int32_t)fourcc("ADgm")) > 0;
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
    bool begun = ad_begin(rn);
    ad.own_file = true;
    ad.zone = rd32(LM_SysZone);
    while (begun && !c->halted && !woken()) {
        if (!ad.stopped)
            ad_tick();
        tb_WaitNextEvent(0, ad.evt, 1, 0); /* no events taken: the program gets the one that wakes it */
    }
    if (begun && !c->halted)
        ad_end();
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

bool afterdark_answer(gaddr evt)
{
    if (!evt || rd32(evt + 2) != fourcc("aYmm") || !(ad.active || opt.idle_module[0]))
        return false;
    if (!ad_info) {
        ad_info = tb_NewPtr(32, true, true);
        wr16(ad_info + 12, AD_VERSION);
    }
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
