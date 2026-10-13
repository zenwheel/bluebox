/* Booting the environment and launching an application. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "forks.h"
#include "qd_internal.h"
#include "sysfonts.h"
#include "tb_internal.h"
#include "sysres_gen.h"
#include "machine.h"

cpu68k *g_cpu;
char g_app_dir[1024];
char g_app_path[1024];
uint32_t g_app_creator;

static cpu68k cpu;
static int screen_w = 1024, screen_h = 768;

void machine_set_screen(int width, int height)
{
    if (width >= 512 && height >= 342 && width <= 4096 && height <= 4096) {
        screen_w = width;
        screen_h = height;
    }
}
static uint64_t ticks_base_ns;
static uint64_t skipped_ns; /* Delay's skips (see machine_time_ns) */

uint32_t mac_now(void);

/* The date at boot: Time follows the machine's clock from it. */
static uint32_t boot_time;

static uint16_t hilite_rgb[3] = { 0xCCCC, 0xCCCC, 0xFFFF };

void machine_set_hilite_color(uint16_t r, uint16_t g, uint16_t b)
{
    hilite_rgb[0] = r;
    hilite_rgb[1] = g;
    hilite_rgb[2] = b;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The processor we present: a 68040 with its FPU (a Quadra 800), or a
 * 68020 with no FPU for applications that misbehave on the 68040. */
static int cpu_model = 68040;
void machine_set_cpu(int model) { cpu_model = model == 68020 ? 68020 : 68040; }

static void init_lowmem(void)
{
    wr32(LM_MemTop, GA_ROM);
    wr32(LM_BufPtr, GA_A5WORLD_TOP);
    wr32(LM_ROMBase, GA_ROM);
    wr16(LM_ROM85, 0x3FFF);           /* new ROMs, Color QuickDraw */
    wr8(LM_CPUFlag, g_cpu->model == 68040 ? 4 : 2);
    /* The highlight colour: the host's selection colour when the host gave
     * one, else Mac OS 8's lavender (the Color control panel's). */
    wr16(LM_HiliteRGB, hilite_rgb[0]);
    wr16(LM_HiliteRGB + 2, hilite_rgb[1]);
    wr16(LM_HiliteRGB + 4, hilite_rgb[2]);
    wr8(LM_HiliteMode, 0xFF); /* no highlighting pending */
    wr8(LM_MMU32Bit, 1);              /* 32-bit addressing */
    wr16(LM_SysVersion, 0x0710);
    wr32(LM_Lo3Bytes, 0x00FFFFFF);
    wr32(LM_MinusOne, 0xFFFFFFFF);
    wr32(LM_OneOne, 0x00010001);
    boot_time = mac_now();
    wr32(LM_Time, boot_time);
    wr32(LM_Ticks, 0);
    wr16(LM_monkeylives, 0xFFFF);     /* negative: the Monkey is not running */
    wr8(LM_KbdType, 2);               /* Apple Extended Keyboard (ADB) */
    wr8(LM_KbdLast, 2);               /* its ADB address */
    wr8(LM_SdVolume, 7);              /* the speaker at full volume */
    wr8(LM_SEvtEnb, 0xFF);            /* SystemEvent enabled */
    gput(LM_FinderName, (const uint8_t *)"\006Finder", 7); /* the Finder's name */
    wr32(LM_RndSeed, boot_time);      /* a seed that varies from run to run */
    /* Loop counts per millisecond, for programs that time short delays by
     * them (an emulated 68040 runs about ten instructions a microsecond). */
    wr16(LM_TimeDBRA, 10000);
    wr16(LM_TimeSCCDB, 2500);
    wr16(LM_TimeSCSIDB, 2500);
    wr16(0x0CEA, 1500);               /* TimeVIADB */
    /* The parameter RAM copy (SysParam): valid, the serial ports at 9600
     * baud 8N2, no alarm, Geneva as the application font, key repeat,
     * speaker volume 7, double-click and caret times (8 x 4 ticks). */
    wr8(LM_SPValid, 0xA8);
    wr16(LM_SPPortA, 0xCC0A);
    wr16(LM_SPPortB, 0xCC0A);
    wr16(LM_SPFont, 0);               /* applFont less one */
    wr8(LM_SPKbd, 0x63);
    wr8(LM_SPVolCtl, 7);
    wr8(LM_SPClikCaret, 0x88);
    wr8(LM_SPMisc2, 0x4C);
    wr32(LM_MinStack, 0x400);
    wr32(LM_DefltStack, 0x2000);
    gput(LM_SysResName, (const uint8_t *)"\006System", 7);
    wr16(LM_PaintWhite, 0xFFFF);      /* erase what is exposed */
    wr16(LM_SaveUpdate, 0xFFFF);      /* and queue its update */
    wr8(LM_QDExist, 0xFF);            /* QuickDraw not initialised */
    wr8(LM_WWExist, 0xFF);            /* Window Manager not initialised */
    wr32(LM_DoubleTime, 30);
    wr32(LM_CaretTime, 32);
    wr16(LM_KeyThresh, 16);
    wr16(LM_KeyRepThresh, 4);
    wr16(LM_SysFontFam, 0);
    wr16(LM_SysFontSiz, 12);
    wr16(LM_MBarHeight, 20);
    wr32(LM_ScrnBase, GA_SCREEN);
    wr16(LM_ApFontID, 3); /* applFont is Geneva (our Classic Sans) */
    wr8(LM_ScrDmpEnb, 0xFF);

    /* Fake ROM header: version word at ROMBase+8. */
    wr32(GA_ROM, 0x420DBFF3);
    wr16(GA_ROM + 8, 0x077D);
}

void threads_reset(void);
void help_reset(void);
void dialog_reset(void);
void wdef0_native(cpu68k *c, uint16_t tw);
void mdef0_native(cpu68k *c, uint16_t tw);
void cdef0_native(cpu68k *c, uint16_t tw);
void cdef1_native(cpu68k *c, uint16_t tw);
void cdef63_native(cpu68k *c, uint16_t tw);
void theme_system_icons(rsrc_spec *specs, size_t *n, size_t max);
void ldef0_native(cpu68k *c, uint16_t tw);
void events_reset(void);
void fm_init(const char *root);
void display_reset(void);
void aem_reset(void);
void aem_reset_handlers(void);

/* A definition procedure resource: `NATIVE index; RTS`. */
static void defproc_stub(uint8_t out[8], native_fn fn, const char *name)
{
    uint32_t stub = native_register(fn, name, 0);
    int idx = native_index_for(stub);
    uint8_t s[8] = { 0x71, 0xFF, (uint8_t)(idx >> 8), (uint8_t)idx, 0x4E, 0x75, 0x4E, 0x71 };
    memcpy(out, s, 8);
}

static bool open_system_file(void)
{
    uint8_t wdef0[8], mdef0[8], cdef0[8], cdef1[8], cdef63[8], ldef0[8], itl0[32], itl1[332];
    intl_default_itl0(itl0);
    intl_default_itl1(itl1);
    defproc_stub(wdef0, wdef0_native, "WDEF 0");
    defproc_stub(mdef0, mdef0_native, "MDEF 0");
    defproc_stub(cdef0, cdef0_native, "CDEF 0");
    defproc_stub(cdef1, cdef1_native, "CDEF 1");
    defproc_stub(cdef63, cdef63_native, "CDEF 63");
    defproc_stub(ldef0, ldef0_native, "LDEF 0");
    rsrc_spec specs[512] = {
        { fourcc("WDEF"), 0, NULL, RES_SYS_HEAP, wdef0, 8 },
        { fourcc("MDEF"), 0, NULL, RES_SYS_HEAP, mdef0, 8 },
        { fourcc("CDEF"), 0, NULL, RES_SYS_HEAP, cdef0, 8 },
        { fourcc("CDEF"), 1, NULL, RES_SYS_HEAP, cdef1, 8 },
        { fourcc("CDEF"), 63, NULL, RES_SYS_HEAP, cdef63, 8 },
        { fourcc("LDEF"), 0, NULL, RES_SYS_HEAP, ldef0, 8 },
        { fourcc("itl0"), 0, NULL, RES_SYS_HEAP, itl0, sizeof itl0 },
        { fourcc("itl1"), 0, NULL, RES_SYS_HEAP, itl1, sizeof itl1 },
    };
    size_t n = 8;
    /* Standard cursors and alert icons (Generators/sysres.py). */
    for (unsigned i = 0; i < sysres_count && n < 512; i++)
        specs[n++] = (rsrc_spec){ sysres[i].type, sysres[i].id, NULL, RES_SYS_HEAP | RES_PURGEABLE, sysres[i].data,
                                  sysres[i].len };
    /* Bitmap fonts: generated from the OFL faces (Generators/fonts.py). */
    for (unsigned i = 0; i < sysfont_count && n < 512; i++)
        specs[n++] = (rsrc_spec){ sysfonts[i].type, sysfonts[i].id, sysfonts[i].name, RES_SYS_HEAP | RES_PURGEABLE,
                                  sysfonts[i].data, sysfonts[i].len };
    /* A theme's own Finder and system icons replace ours. */
    theme_system_icons(specs, &n, 512);
    uint8_t *fork;
    size_t len;
    if (!rsrc_build(specs, n, &fork, &len))
        return false;
    int16_t rn = rm_open_host_file(NULL, fork, (uint32_t)len, true);
    free(fork);
    if (rn < 0)
        return false;
    rm_set_system(rn);
    return true;
}

/* Hardware a program may address through low memory (the VIA, the serial
 * chip, the floppy controller, the sound buffer) and the driver unit
 * table: harmless memory of their own, so a program poking them writes
 * there and not over the vectors near address 0. */
static void hardware_globals(void)
{
    gaddr via = mm_new_ptr_in(GA_SYSHEAP, 16 * 512, true);
    if (via)
        wr8(via, 0x80); /* vBufA: sound off */
    wr32(LM_VIA, via);
    gaddr scc = mm_new_ptr_in(GA_SYSHEAP, 64, true);
    wr32(LM_SCCRd, scc);
    wr32(LM_SCCWr, scc);
    wr32(LM_IWM, mm_new_ptr_in(GA_SYSHEAP, 16 * 512, true));
    wr32(LM_SoundBase, mm_new_ptr_in(GA_SYSHEAP, 370 * 2, true));
    /* No drivers live in the table: every unit empty. */
    wr32(LM_UTableBase, mm_new_ptr_in(GA_SYSHEAP, 64 * 4, true));
    wr16(LM_UnitNtryCnt, 64);
    /* ScrapName: the Clipboard file's name. */
    static const uint8_t scrap[] = "\016Clipboard File";
    gaddr name = mm_new_ptr_in(GA_SYSHEAP, sizeof scrap, true);
    if (name)
        gput(name, scrap, sizeof scrap - 1);
    wr32(LM_ScrapName, name);
}

bool machine_boot(bool census)
{
    if (!mem_init())
        return false;
    mem_reset();
    cpu_init(&cpu);
    cpu.model = cpu_model;
    g_cpu = &cpu;
    cpu.aline = trap_aline;
    cpu.native = trap_native;
    cpu.sr = 0x2000; /* supervisor, interrupts enabled */

    g_trap_census = census;
    init_lowmem();
    trap_init();
    gestalt_init();

    /* Heaps: the system zone, then a growable application zone. */
    mm_init_zone(GA_SYSHEAP, GA_SYSHEAP_END, 64);
    wr32(LM_SysZone, GA_SYSHEAP);
    mm_init_zone(GA_APPHEAP, GA_APPHEAP + 0x00100000u, 64);
    wr32(LM_ApplZone, GA_APPHEAP);
    wr32(LM_TheZone, GA_APPHEAP);
    wr32(LM_ApplLimit, GA_APPLLIMIT);
    wr32(LM_HeapEnd, Zone_bkLim(GA_APPHEAP));
    mm_set_appl_growth(GA_APPLLIMIT);
    hardware_globals();

    /* The System file: built here, with our definition procedures as
     * 68k stubs that call native code. */
    rm_init();
    fm_reset();
    sane_reset();
    time_reset();
    threads_reset();
    help_reset();
    dialog_reset();
    wr32(LM_TheZone, GA_SYSHEAP);
    if (!open_system_file())
        return false;
    wr32(LM_TheZone, GA_APPHEAP);
    events_reset();
    display_reset();
    aem_reset();
    aem_reset_handlers();
    dialogs_reset();
    sound_reset();
    components_reset();
    quicktime_reset();
    notify_reset();

    qd_screen_init(screen_w, screen_h);
    /* CrsrPin: the mouse stays on the screen. */
    qrect_put(LM_CrsrPin, (qrect){ 0, 0, (int16_t)screen_h, (int16_t)screen_w });

    /* No documents to open. */
    gaddr parms = tb_NewHandle(4, false, true);
    wr32(LM_AppParmHandle, parms);

    ticks_base_ns = now_ns();
    skipped_ns = 0;
    return true;
}

bool machine_add_resource_file(const char *path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return false;
    /* Only fonts: a System file's code resources must not shadow ours. */
    static const uint32_t font_types[] = { 0x464F4E44 /* FOND */, 0x4E464E54 /* NFNT */, 0x464F4E54 /* FONT */ };
    uint8_t *fonts;
    size_t flen;
    bool ok = rsrc_filter(mf.rsrc, mf.rsrc_len, font_types, 3, &fonts, &flen, err, errlen);
    mac_file_free(&mf);
    if (!ok)
        return false;
    gaddr saved = rd32(LM_TheZone);
    wr32(LM_TheZone, rd32(LM_SysZone));
    int16_t rn = rm_open_host_file(path, fonts, (uint32_t)flen, true);
    wr32(LM_TheZone, saved);
    free(fonts);
    if (rn < 0) {
        snprintf(err, errlen, "%s: no usable resource fork", path);
        return false;
    }
    return true;
}

int g_appearance = MACHINE_APPEARANCE_BW;

void machine_set_appearance(int appearance)
{
    g_appearance = appearance;
    machine_set_screen_depth(32);
}

static char **launch_docs;
static int nlaunch_docs;

void machine_set_launch_documents(const char *const *paths, int n)
{
    for (int i = 0; i < nlaunch_docs; i++)
        free(launch_docs[i]);
    free(launch_docs);
    launch_docs = n ? calloc((size_t)n, sizeof *launch_docs) : NULL;
    for (int i = 0; i < n; i++)
        launch_docs[i] = strdup(paths[i]);
    nlaunch_docs = n;
}

int16_t fm_wd_for_path(const char *path, gaddr name_out);
uint32_t fm_type_of_path(const char *path);

/* The Finder's launch parameters: message, count, then AppFile records
 * (vRefNum, type, version, filler, name padded to even). */
static void build_app_parms(void)
{
    /* Apps that take high-level events get an Open Documents event instead. */
    int n = (g_app_size_flags & 0x0040) ? 0 : nlaunch_docs;
    gaddr h = rd32(LM_AppParmHandle);
    tb_SetHandleSize(h, 4);
    wr16(rd32(h), 0); /* appOpen */
    wr16(rd32(h) + 2, (uint16_t)n);
    gaddr name = tb_NewPtr(256, false, true);
    for (int i = 0; i < n; i++) {
        fm_grant(launch_docs[i], false);
        int16_t vref = fm_wd_for_path(launch_docs[i], name);
        uint32_t nl = rd8(name);
        uint32_t rec = 8 + ((nl + 2) & ~1u);
        int32_t size = tb_GetHandleSize(h);
        tb_SetHandleSize(h, size + (int32_t)rec);
        gaddr p = rd32(h) + (uint32_t)size;
        wr16(p, (uint16_t)vref);
        wr32(p + 2, fm_type_of_path(launch_docs[i]));
        wr8(p + 6, 0);
        wr8(p + 7, 0);
        gmemcpy(p + 8, name, nl + 1);
    }
    tb_DisposePtr(name);
}

bool machine_launch(const char *app_path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(app_path, &mf, err, errlen))
        return false;
    if (mf.rsrc_len == 0) {
        snprintf(err, errlen, "%s has no resource fork", app_path);
        mac_file_free(&mf);
        return false;
    }
    /* The app's folder stands in for the default volume until the File Manager exists. */
    snprintf(g_app_dir, sizeof g_app_dir, "%s", app_path);
    char *slash = strrchr(g_app_dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(g_app_dir, ".");

    fm_init(g_app_dir);
    snprintf(g_app_path, sizeof g_app_path, "%s", app_path);
    g_app_creator = mac_file_creator(&mf);

    const char *base = strrchr(app_path, '/');
    base = base ? base + 1 : app_path;
    pstr_from_c(LM_CurApName, mf.name[0] ? mf.name : base);
    if (rd8(LM_CurApName) > 31)
        wr8(LM_CurApName, 31);

    int16_t rn = rm_open_host_file(app_path, mf.rsrc, (uint32_t)mf.rsrc_len, true);
    mac_file_free(&mf);
    if (rn < 0) {
        snprintf(err, errlen, "cannot open the application's resource fork");
        return false;
    }
    wr16(LM_CurApRefNum, (uint16_t)rn);

    /* SIZE -1 (user-modified) takes precedence over SIZE 0. */
    gaddr size = tb_Get1Resource((int32_t)fourcc("SIZE"), -1);
    if (!size)
        size = tb_Get1Resource((int32_t)fourcc("SIZE"), 0);
    g_app_size_flags = size && rd32(size) ? rd16(rd32(size)) : 0;
    build_app_parms();

    uint32_t a5;
    uint32_t entry = seg_launch(rn, &a5, err, errlen);
    if (!entry)
        return false;
    cpu.pc = entry;
    return true;
}

/* The machine's clock.  With a host attached it is the wall clock; in a
 * headless run nobody is watching, so it follows the instructions executed
 * (100 ns each, a 10 MIPS machine) and runs are fast and repeatable.
 * Delay moves it forward either way. */
bool events_host_attached(void);

uint64_t machine_time_ns(void)
{
    if (!events_host_attached())
        return cpu.icount * 100u + skipped_ns;
    return now_ns() - ticks_base_ns + skipped_ns;
}

void machine_skip_time(uint64_t ns) { skipped_ns += ns; }

/* Bring the 60 Hz tick counter up to the machine's time. */
void machine_advance_ticks(void)
{
    uint64_t ns = machine_time_ns();
    uint64_t ticks = ns * 60 / 1000000000u;
    if ((uint32_t)ticks > rd32(LM_Ticks))
        wr32(LM_Ticks, (uint32_t)ticks);
    /* Time, the seconds since 1904, which programs also read in place. */
    uint32_t secs = boot_time + (uint32_t)(ns / 1000000000u);
    if (secs > rd32(LM_Time))
        wr32(LM_Time, secs);
}

run_status machine_run(uint64_t max)
{
    const uint64_t slice = 20000;
    uint64_t done = 0;
    while (!cpu.halted && done < max) {
        uint64_t n = max - done < slice ? max - done : slice;
        done += cpu_run(&cpu, n);
        machine_advance_ticks();
        events_script_pump(cpu.icount);
        events_sync_lowmem();
        time_pump();
        if (cpu.stopped)
            break;
    }
    return cpu.halted ? RUN_HALTED : RUN_LIMIT;
}

const char *machine_halt_reason(void) { return cpu.halt_reason; }
bool machine_halted(void) { return cpu.halted; }
uint64_t machine_instruction_count(void) { return cpu.icount; }
uint32_t machine_pc(void) { return cpu.pc; }

static void trace_cpu(cpu68k *c)
{
    char buf[64];
    cpu_disasm(c->pc, buf, sizeof buf);
    log_printf("%08X  %-16s d0=%08X d1=%08X a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X a7=%08X sr=%04X\n", c->pc, buf,
               c->d[0], c->d[1], c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[7], c->sr);
}

void machine_set_log(unsigned mask)
{
    g_log_mask = mask;
    cpu.trace = (mask & LOG_CPU) ? trace_cpu : NULL;
}

unsigned machine_census(census_row *rows, unsigned max)
{
    unsigned n;
    const trap_count *tc = trap_census(&n);
    for (unsigned i = 0; i < n && i < max; i++) {
        const char *name = NULL;
        if (tc[i].selector >= 0) {
            for (unsigned j = 0; j < dispatch_sig_count; j++) {
                uint16_t t = dispatch_sigs[j].trap;
                bool same = (t & 0x0800) ? (tc[i].trap & 0x3FF) == (t & 0x3FF) && (tc[i].trap & 0x0800)
                                         : (tc[i].trap & 0xFF) == (t & 0xFF) && !(tc[i].trap & 0x0800);
                if (same && dispatch_sigs[j].selector == tc[i].selector) {
                    name = dispatch_sigs[j].name;
                    break;
                }
            }
        }
        rows[i] = (census_row){ tc[i].trap, tc[i].selector, name ? name : trap_name(tc[i].trap), tc[i].calls,
                                tc[i].implemented };
    }
    return n;
}

void machine_describe_cpu(char *buf, size_t len)
{
    snprintf(buf, len,
             "pc %08X  sr %04X  instructions %llu\n"
             "d0 %08X d1 %08X d2 %08X d3 %08X d4 %08X d5 %08X d6 %08X d7 %08X\n"
             "a0 %08X a1 %08X a2 %08X a3 %08X a4 %08X a5 %08X a6 %08X a7 %08X",
             cpu.pc, cpu.sr, (unsigned long long)cpu.icount, cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3], cpu.d[4],
             cpu.d[5], cpu.d[6], cpu.d[7], cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5], cpu.a[6],
             cpu.a[7]);
}
