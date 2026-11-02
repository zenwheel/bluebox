/*
 * The Chooser: a desk accessory of our own in the System file (DRVR 16,
 * "\0Chooser"), so every program's Apple menu has it.  It is the classic
 * window: the devices' icons on the left, the choices for the one
 * selected on the right, the AppleTalk zones below the icons when the
 * network has any, the user name and AppleTalk on or off at the bottom.
 *
 * The devices are Blue Box's own rather than Chooser extensions:
 *   AppleShare   the file servers NBP finds (type AFPServer) and servers
 *                by address; connecting logs in through our AFP client
 *                and mounts the volumes chosen (chooser_afp.c).
 *   LaserWriter  the LaserWriters NBP finds; printing goes to the one
 *                chosen as PostScript over PAP (pap.c).
 *   Printer      the host's printers; printing goes to the one chosen,
 *                without the host's print panel.
 *   Save as PDF  printing makes a PDF file in a host folder, chosen here.
 * Chooser extensions in the System Folder (files of type 'PRES', 'PRER'
 * and 'RDEV') are listed after them and called as the Chooser called
 * them (chooser_ext.c).
 *
 * The choices are the run's settings: the host gives them before boot
 * (machine_set_printer and the rest) and keeps what the Chooser changes
 * (machine_set_chooser_host).  Programs see them as they did: the System
 * file's 'STR ' -8192 is the chosen printer's driver name and 'STR '
 * -16096 the user name; the Printing Manager prints where the printer
 * chosen says; SPConfig sets the printer port aside for AppleTalk while
 * AppleTalk is active, and Gestalt's 'atlk' is its version (0 inactive).
 *
 * The desk accessory's routines are 68k stubs in its DRVR that call C:
 * Open makes the window, Control takes accEvent, accRun, accCursor, the
 * Edit commands and goodBye, Close puts everything away.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chooser.h"
#include "forks.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    nullEvent = 0, mouseDown = 1, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt,
};
enum { goodbye = -1, accEvent = 64, accRun = 65, accCursor = 66, accMenu = 67, accUndo = 68, accCut = 70,
       accCopy = 71, accPaste = 72, accClear = 73 };
enum { cmdKey = 0x0100, shiftKey = 0x0200, optionKey = 0x0800, activeFlag = 1 };
enum { inButton = 10, inCheckBox = 11, inUpButton = 20, inDownButton = 21, inPageUp = 22, inPageDown = 23,
       inThumb = 129 };
enum { pushButProc = 0, radioButProc = 2, scrollBarProc = 16 };
enum { ttNone = 0, ttDisabled = 1, ttSelected = 0x4000 };
enum { CHOOSER_DRVR = 16, ICON_BASE = -15872 }; /* DRVR 16's owned resources: -16384 + 16 * 32 */

/* ---------------------------------------------------------------------- */
/* The settings */

chooser_config g_chooser;
static machine_printers_fn printers_fn;
static machine_setting_fn setting_fn;
static void *host_ctx;

void machine_set_chooser_host(machine_printers_fn printers, machine_setting_fn changed, void *ctx)
{
    printers_fn = printers;
    setting_fn = changed;
    host_ctx = ctx;
}

void machine_set_user_name(const char *utf8)
{
    uint8_t mac[64];
    size_t n = utf8_to_macroman(utf8 ? utf8 : "", mac, 31);
    memcpy(g_chooser.user, mac, n);
    g_chooser.user[n] = 0;
}

static bool valid_printer(const char *spec)
{
    return !spec[0] || !strcmp(spec, "panel") || !strcmp(spec, "pdf") || (!strncmp(spec, "host:", 5) && spec[5]) ||
           (!strncmp(spec, "laserwriter:", 12) && strchr(spec + 12, '@')) || (!strncmp(spec, "driver:", 7) && spec[7]);
}

bool machine_set_printer(const char *spec)
{
    if (!spec || !valid_printer(spec))
        return false;
    snprintf(g_chooser.printer, sizeof g_chooser.printer, "%s", spec[0] ? spec : "panel");
    return true;
}

void machine_set_print_folder(const char *path)
{
    snprintf(g_chooser.folder, sizeof g_chooser.folder, "%s", path ? path : "");
}

void chooser_changed(const char *key, const char *value)
{
    LOGF(LOG_TRAPS, "  Chooser: %s = %s\n", key, value);
    if (setting_fn)
        setting_fn(key, value, host_ctx);
}

/* The driver name programs see in 'STR ' -8192: the chosen device's. */
void chooser_driver_name(char *out, size_t size)
{
    const char *p = g_chooser.printer;
    if (!strncmp(p, "laserwriter:", 12))
        snprintf(out, size, "LaserWriter");
    else if (!strcmp(p, "pdf"))
        snprintf(out, size, "Save as PDF");
    else if (!strncmp(p, "driver:", 7)) {
        uint8_t mac[64];
        size_t n = utf8_to_macroman(p + 7, mac, 31);
        memcpy(out, mac, n < size - 1 ? n : size - 1);
        out[n < size - 1 ? n : size - 1] = 0;
    } else
        snprintf(out, size, "Printer");
}

/* A string resource of the System file set to a MacRoman C string. */
static void set_system_string(int16_t id, const char *mac)
{
    int16_t cur = tb_CurResFile();
    tb_UseResFile((int16_t)rd16(LM_SysMap));
    gaddr h = tb_Get1Resource((int32_t)fourcc("STR "), id);
    tb_UseResFile(cur);
    if (!h)
        return;
    size_t n = strlen(mac);
    if (n > 255)
        n = 255;
    tb_SetHandleSize(h, (int32_t)n + 1);
    if (!rd32(h))
        return;
    wr8(rd32(h), (uint8_t)n);
    gput(rd32(h) + 1, mac, (uint32_t)n);
}

void chooser_set_printer(const char *spec)
{
    if (!valid_printer(spec) || !strcmp(spec, g_chooser.printer))
        return;
    snprintf(g_chooser.printer, sizeof g_chooser.printer, "%s", spec);
    char name[64];
    chooser_driver_name(name, sizeof name);
    set_system_string(-8192, name);
    chooser_changed("printer", spec);
}

void chooser_set_user(const char *mac)
{
    if (!strcmp(mac, g_chooser.user))
        return;
    snprintf(g_chooser.user, sizeof g_chooser.user, "%s", mac);
    set_system_string(-16096, mac);
    char utf8[128];
    macroman_to_utf8((const uint8_t *)mac, strlen(mac), utf8, sizeof utf8);
    chooser_changed("user-name", utf8);
}

/* SPConfig: the printer port (port B, the low four bits) set aside for
 * AppleTalk while it is active, unless it connects to something. */
void chooser_note_appletalk(void)
{
    bool on = appletalk_enabled();
    uint8_t cfg = (uint8_t)(rd8(LM_SPConfig) & 0xF0);
    if (on && !serial_port_connected(1))
        cfg |= 1; /* useATalk */
    wr8(LM_SPConfig, cfg);
    xpram_set(0x13, cfg); /* parameter RAM's copy */
}

/* Where a print job goes: the destination for the host, and the folder or
 * printer named.  A LaserWriter on AppleTalk is ours to print to (pap.c). */
int chooser_print_destination(machine_print_job *job)
{
    const char *p = g_chooser.printer;
    job->printer[0] = job->folder[0] = 0;
    if (!strcmp(p, "pdf") && g_chooser.folder[0]) {
        snprintf(job->folder, sizeof job->folder, "%s", g_chooser.folder);
        return job->destination = MACHINE_PRINT_TO_PDF;
    }
    if (!strncmp(p, "host:", 5)) {
        snprintf(job->printer, sizeof job->printer, "%s", p + 5);
        return job->destination = MACHINE_PRINT_TO_HOST;
    }
    return job->destination = MACHINE_PRINT_TO_PANEL;
}

/* The job dialog's words for where the pages go (MacRoman). */
void chooser_job_text(char *out, size_t size)
{
    const char *p = g_chooser.printer;
    char name[300] = "";
    const char *from = !strncmp(p, "host:", 5) ? p + 5 : !strncmp(p, "laserwriter:", 12) ? p + 12 : g_chooser.folder;
    uint8_t m[300];
    size_t n = utf8_to_macroman(from, m, sizeof m - 1);
    for (size_t i = 0; i < n; i++)
        name[i] = (char)(m[i] == '_' && !strncmp(p, "host:", 5) ? ' ' : m[i]);
    name[n] = 0;
    char *at = !strncmp(p, "laserwriter:", 12) ? strrchr(name, '@') : NULL;
    if (at)
        *at = 0;
    if (!strncmp(p, "host:", 5))
        snprintf(out, size, "The pages are printed on \xD2%s\xD3, as chosen in the Chooser.", name);
    else if (!strncmp(p, "laserwriter:", 12))
        snprintf(out, size, "The pages are printed on the LaserWriter \xD2%s\xD3, as chosen in the Chooser.", name);
    else if (!strcmp(p, "pdf") && g_chooser.folder[0])
        snprintf(out, size, "The pages are saved as a PDF file in \xD2%s\xD3, as chosen in the Chooser.", name);
    else
        snprintf(out, size, "The printer, the number of copies and the pages to print are chosen in the next panel.");
}

bool chooser_laserwriter(char *nbp, size_t size)
{
    if (strncmp(g_chooser.printer, "laserwriter:", 12))
        return false;
    /* NAME@ZONE: the NBP entity NAME:LaserWriter@ZONE, in MacRoman */
    const char *at = strrchr(g_chooser.printer + 12, '@');
    char obj[160], zone[160];
    snprintf(obj, sizeof obj, "%.*s", (int)(at - g_chooser.printer - 12), g_chooser.printer + 12);
    snprintf(zone, sizeof zone, "%s", at + 1);
    uint8_t mo[40], mz[40];
    size_t no = utf8_to_macroman(obj, mo, 32), nz = utf8_to_macroman(zone, mz, 32);
    snprintf(nbp, size, "%.*s:LaserWriter@%.*s", (int)no, (const char *)mo, (int)nz, (const char *)mz);
    return true;
}

/* ---------------------------------------------------------------------- */
/* The System file's resources: the desk accessory, its icons, the strings */

/* Our own icons, 32 by 32 (masks filled in from the outline). */
static const char *const icon_art[4][32] = {
    { /* AppleShare: a file server on the network */
      "................................", ".........##############.........", ".........#............#.........",
      ".........#.##########.#.........", ".........#.#........#.#.........", ".........#.##########.#.........",
      ".........#............#.........", ".........#.##########.#.........", ".........#.#........#.#.........",
      ".........#.##########.#.........", ".........#............#.........", ".........#............#.........",
      ".........#............#.........", ".........#.......##.#.#.........", ".........#............#.........",
      ".........#............#.........", ".........#............#.........", ".........#............#.........",
      ".........##############.........", "..........##........##..........", "................................",
      "...............##...............", "...............##...............", "...............##...............",
      "..############################..", "..#..........................#..", "..############################..",
      "................................", "................................", "................................",
      "................................", "................................" },
    { /* LaserWriter: a laser printer, a page in its tray */
      "................................", "................................", "..........############..........",
      "..........#..........#..........", "..........#.######...#..........", "..........#..........#..........",
      "..........#.########.#..........", "..........#..........#..........", "...##########################...",
      "..#..........................#..", ".#............................#.", ".##############################.",
      ".#............................#.", ".#..########################..#.", ".#..#......................#..#.",
      ".#..########################..#.", ".#............................#.", ".#....................##..##..#.",
      ".#....................##..##..#.", ".#............................#.", ".##############################.",
      ".#............................#.", ".#............................#.", ".#............................#.",
      "..############################..", "..###......................###..", "................................",
      "................................", "................................", "................................",
      "................................", "................................" },
    { /* Printer: the host's, a printer with a page coming out */
      "................................", "................................", "........################........",
      "........#..............#........", "........#.##########...#........", "........#..............#........",
      "........#.########.....#........", "........#..............#........", "....########################....",
      "...#........................#...", "..#..........................#..", ".#............................#.",
      ".##############################.", ".#............................#.", ".#.......................##...#.",
      ".#.......................##...#.", ".#............................#.", ".#....##################......#.",
      ".##..#..................#....##.", "..##.#..................#...##..", "...###..................#####...",
      ".....#..#############...#.......", ".....#..................#.......", ".....#..##########......#.......",
      ".....#..................#.......", ".....####################.......", "................................",
      "................................", "................................", "................................",
      "................................", "................................" },
    { /* Save as PDF: a page with a folded corner */
      ".....##################.........", ".....#................##........", ".....#................#.#.......",
      ".....#................#..#......", ".....#................#...#.....", ".....#................######....",
      ".....#.....................#....", ".....#..###########........#....", ".....#.....................#....",
      ".....#..##############.....#....", ".....#.....................#....", "..######################....#....",
      "..#....................#...#....", "..#.###...###....####...#...#....", "..#.#..#..#..#...#......#...#....",
      "..#.#..#..#...#..#......#...#....", "..#.###...#...#..###....#...#....", "..#.#.....#...#..#......#...#....",
      "..#.#.....#..#...#......#...#....", "..#.#.....###....#......#...#....", "..#....................#....#....",
      "..######################....#....", ".....#.....................#....", ".....#..###########........#....",
      ".....#.....................#....", ".....#..#########..........#....", ".....#.....................#....",
      ".....#######################....", "................................", "................................",
      "................................", "................................" },
};

/* An ICN#: the icon, and its mask (everything not reachable from the
 * edge through blank pixels). */
static void make_icn(int which, uint8_t out[256])
{
    uint8_t px[32][32], mask[32][32];
    memset(out, 0, 256);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            px[y][x] = icon_art[which][y][x] == '#';
            mask[y][x] = 1;
        }
    /* flood the outside */
    int stack[32 * 32][2], sp = 0;
    for (int i = 0; i < 32; i++) {
        int edge[4][2] = { { 0, i }, { 31, i }, { i, 0 }, { i, 31 } };
        for (int k = 0; k < 4; k++)
            if (!px[edge[k][0]][edge[k][1]] && mask[edge[k][0]][edge[k][1]]) {
                mask[edge[k][0]][edge[k][1]] = 0;
                stack[sp][0] = edge[k][0];
                stack[sp++][1] = edge[k][1];
            }
    }
    while (sp) {
        sp--;
        int y = stack[sp][0], x = stack[sp][1];
        int nb[4][2] = { { y - 1, x }, { y + 1, x }, { y, x - 1 }, { y, x + 1 } };
        for (int k = 0; k < 4; k++) {
            int ny = nb[k][0], nx = nb[k][1];
            if (ny < 0 || ny > 31 || nx < 0 || nx > 31 || px[ny][nx] || !mask[ny][nx])
                continue;
            mask[ny][nx] = 0;
            stack[sp][0] = ny;
            stack[sp++][1] = nx;
        }
    }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            if (px[y][x])
                out[y * 4 + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            if (mask[y][x])
                out[128 + y * 4 + x / 8] |= (uint8_t)(0x80 >> (x & 7));
        }
}

static void chooser_open_native(cpu68k *c, uint16_t tw);
static void chooser_control_native(cpu68k *c, uint16_t tw);
static void chooser_close_native(cpu68k *c, uint16_t tw);

/* The DRVR: flags, delay, event mask, menu, the routines' offsets, the
 * name; then Open (NATIVE; RTS), Control (NATIVE; MOVE.L JIODone,-(SP);
 * RTS) and Close.  It needs time (the network's answers), locking and a
 * goodbye when the program quits. */
static uint8_t drvr[64];
static uint8_t icns[4][256];
static uint8_t str_driver[64], str_user[64];

static void put_native(uint8_t *p, native_fn fn, const char *name)
{
    int idx = native_index_for(native_register(fn, name, 0));
    p[0] = 0x71;
    p[1] = 0xFF;
    p[2] = (uint8_t)(idx >> 8);
    p[3] = (uint8_t)idx;
}

size_t chooser_system_resources(rsrc_spec *specs, size_t n, size_t max)
{
    static const uint8_t name[] = "\0Chooser";
    enum { HDR = 18, NAME_LEN = 8, CODE = 28 };
    memset(drvr, 0, sizeof drvr);
    uint16_t flags = 0x4000 | 0x2000 | 0x1000 | 0x0400; /* dNeedLock, dNeedTime, dNeedGoodBye, dCtlEnable */
    uint16_t emask = (1 << mouseDown) | (1 << keyDown) | (1 << autoKey) | (1 << updateEvt) | (1 << activateEvt);
    uint16_t open_at = CODE, ctl_at = CODE + 6, close_at = CODE + 16;
    uint16_t hdr[9] = { flags, 30, emask, 0, open_at, 0, ctl_at, 0, close_at };
    for (int i = 0; i < 9; i++) {
        drvr[2 * i] = (uint8_t)(hdr[i] >> 8);
        drvr[2 * i + 1] = (uint8_t)hdr[i];
    }
    drvr[HDR] = NAME_LEN;
    memcpy(drvr + HDR + 1, name, NAME_LEN);
    put_native(drvr + open_at, chooser_open_native, "Chooser Open");
    drvr[open_at + 4] = 0x4E;
    drvr[open_at + 5] = 0x75;
    put_native(drvr + ctl_at, chooser_control_native, "Chooser Control");
    static const uint8_t iodone[6] = { 0x2F, 0x38, 0x08, 0xFC, 0x4E, 0x75 };
    memcpy(drvr + ctl_at + 4, iodone, 6);
    put_native(drvr + close_at, chooser_close_native, "Chooser Close");
    drvr[close_at + 4] = 0x4E;
    drvr[close_at + 5] = 0x75;

    if (n + 7 > max)
        return n;
    specs[n++] = (rsrc_spec){ fourcc("DRVR"), CHOOSER_DRVR, "\0Chooser", 0x40 /* resSysHeap */, drvr, close_at + 6, 8 };
    for (int i = 0; i < 4; i++) {
        make_icn(i, icns[i]);
        specs[n++] = (rsrc_spec){ fourcc("ICN#"), (int16_t)(ICON_BASE + i), NULL, 0x40, icns[i], 256 };
    }
    char driver[64];
    chooser_driver_name(driver, sizeof driver);
    str_driver[0] = (uint8_t)strlen(driver);
    memcpy(str_driver + 1, driver, str_driver[0]);
    specs[n++] = (rsrc_spec){ fourcc("STR "), -8192, NULL, 0x40, str_driver, (uint32_t)str_driver[0] + 1 };
    str_user[0] = (uint8_t)strlen(g_chooser.user);
    memcpy(str_user + 1, g_chooser.user, str_user[0]);
    specs[n++] = (rsrc_spec){ fourcc("STR "), -16096, NULL, 0x40, str_user, (uint32_t)str_user[0] + 1 };
    return n;
}

/* ---------------------------------------------------------------------- */
/* The window */

enum { WIN_W = 410, WIN_H = 252 };
enum { DEV_APPLESHARE, DEV_LASERWRITER, DEV_PRINTER, DEV_PDF, DEV_EXTENSION };

static const qrect icons_frame = { 8, 8, 168, 151 }, icons_area = { 9, 9, 167, 150 };
static const qrect icons_bar = { 8, 150, 168, 166 };
enum { CELL_W = 70, CELL_H = 52 };
static const qrect zones_view = { 192, 9, 244, 150 };
static const qrect list_view = { 27, 171, 107, 386 };
static const qrect user_text = { 149, 250, 165, 396 };
static const qrect at_active = { 210, 272, 226, 332 }, at_inactive = { 210, 335, 226, 404 };
static const qrect default_rects[5] = {
    { 112, 206, 132, 266 }, { 112, 296, 132, 356 }, { 114, 272, 130, 332 }, { 114, 335, 130, 395 }, { 114, 170, 130, 267 },
};

chooser_state g_ch;

/* The window's visible region, for the lists' updates. */
static gaddr vis_rgn(void) { return rd32(g_ch.window + GrafPort_visRgn_off); }

/* Guest scratch space: Pascal strings and rectangles, in turn. */
gaddr ch_pstr(const char *mac)
{
    static int next;
    gaddr p = g_ch.scratch + (gaddr)(next++ % 6) * 256;
    size_t n = strlen(mac);
    if (n > 255)
        n = 255;
    wr8(p, (uint8_t)n);
    gput(p + 1, mac, (uint32_t)n);
    return p;
}

gaddr ch_rect(qrect r)
{
    static int next;
    gaddr p = g_ch.scratch + 6 * 256 + (gaddr)(next++ % 8) * 8;
    qrect_put(p, r);
    return p;
}

static gaddr scratch_word(void) { return g_ch.scratch + 6 * 256 + 64; }
static gaddr scratch_long(void) { return g_ch.scratch + 6 * 256 + 68; }

static void use_font(int font, int size, int face)
{
    tb_TextFont((int16_t)font);
    tb_TextSize((int16_t)size);
    tb_TextFace((int16_t)face);
}

static int text_width(const char *mac) { return tb_StringWidth(ch_pstr(mac)); }

/* Text cut to a width with an ellipsis. */
static void fit_text(const char *mac, int width, char *out, size_t size)
{
    snprintf(out, size, "%s", mac);
    if (text_width(out) <= width)
        return;
    size_t n = strlen(out);
    while (n > 0) {
        out[--n] = 0;
        char t[300];
        snprintf(t, sizeof t, "%s\xC9", out);
        if (text_width(t) <= width) {
            snprintf(out, size, "%s", t);
            return;
        }
    }
}

static void draw_text_at(int h, int v, const char *mac)
{
    tb_MoveTo((int16_t)h, (int16_t)v);
    tb_DrawString(ch_pstr(mac));
}

/* The devices: ours first, then the Chooser extensions found. */
static void add_device(const char *name, int kind, bool atalk, int16_t icon)
{
    if (g_ch.ndevices >= CH_MAX_DEVICES)
        return;
    ch_device *d = &g_ch.devices[g_ch.ndevices++];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->kind = kind;
    d->appletalk = atalk;
    d->icon = icon;
}

static void find_devices(void)
{
    g_ch.ndevices = 0;
    add_device("AppleShare", DEV_APPLESHARE, true, ICON_BASE);
    add_device("LaserWriter", DEV_LASERWRITER, true, ICON_BASE + 1);
    add_device("Printer", DEV_PRINTER, false, ICON_BASE + 2);
    add_device("Save as PDF", DEV_PDF, false, ICON_BASE + 3);
    chooser_find_extensions();
}

void chooser_add_extension(const char *name, const char *path, uint32_t type)
{
    add_device(name, DEV_EXTENSION, false, 0);
    ch_device *d = &g_ch.devices[g_ch.ndevices - 1];
    snprintf(d->path, sizeof d->path, "%s", path);
    d->type = type;
    d->appletalk = type == fourcc("PRER") || type == fourcc("RDEV"); /* until its flags say */
}

/* The device the current printer belongs to: selected when the window opens. */
static int printer_device(void)
{
    const char *p = g_chooser.printer;
    for (int i = 0; i < g_ch.ndevices; i++) {
        ch_device *d = &g_ch.devices[i];
        if ((d->kind == DEV_LASERWRITER && !strncmp(p, "laserwriter:", 12)) ||
            (d->kind == DEV_PRINTER && !strncmp(p, "host:", 5)) || (d->kind == DEV_PDF && !strcmp(p, "pdf")))
            return i;
        if (d->kind == DEV_EXTENSION && !strncmp(p, "driver:", 7)) {
            char utf8[128];
            macroman_to_utf8((const uint8_t *)d->name, strlen(d->name), utf8, sizeof utf8);
            if (!strcmp(utf8, p + 7))
                return i;
        }
    }
    return -1;
}

bool chooser_device_dimmed(int i)
{
    return g_ch.devices[i].appletalk && !appletalk_enabled();
}

/* ---- The icons on the left ---- */

static int icon_rows(void) { return (g_ch.ndevices + 1) / 2; }
static int visible_icon_rows(void) { return (icons_area.bottom - icons_area.top) / CELL_H; }

static qrect icon_cell(int i)
{
    int row = i / 2 - g_ch.icon_top, col = i % 2;
    int top = icons_area.top + row * CELL_H, left = icons_area.left + col * CELL_W;
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + CELL_H), (int16_t)(left + CELL_W) };
}

static void draw_icon(int i)
{
    qrect cell = icon_cell(i);
    if (cell.bottom <= icons_area.top || cell.top >= icons_area.bottom)
        return;
    ch_device *d = &g_ch.devices[i];
    bool sel = i == g_ch.current, dim = chooser_device_dimmed(i);
    qrect ir = { (int16_t)(cell.top + 4), (int16_t)(cell.left + (CELL_W - 32) / 2), (int16_t)(cell.top + 36),
                 (int16_t)(cell.left + (CELL_W - 32) / 2 + 32) };
    int16_t transform = (int16_t)((sel ? ttSelected : ttNone) | (dim ? ttDisabled : ttNone));
    if (d->kind == DEV_EXTENSION && d->suite)
        tb_PlotIconSuite(ch_rect(ir), 0, transform, d->suite);
    else
        tb_PlotIconID(ch_rect(ir), 0, transform, d->kind == DEV_EXTENSION ? ICON_BASE + 2 : d->icon);
    use_font(3, 9, 0); /* Geneva 9 */
    char label[80];
    fit_text(d->name, CELL_W - 4, label, sizeof label);
    int w = text_width(label), h = cell.left + (CELL_W - w) / 2;
    qrect lr = { (int16_t)(cell.top + 38), (int16_t)(h - 2), (int16_t)(cell.top + 50), (int16_t)(h + w + 2) };
    tb_EraseRect(ch_rect(lr));
    draw_text_at(h, cell.top + 47, label);
    if (sel)
        tb_InvertRect(ch_rect(lr));
    if (dim) { /* greyed, as a disabled item */
        static const uint8_t grey[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
        gaddr pat = g_ch.scratch + 6 * 256 + 72;
        gput(pat, grey, 8);
        tb_PenPat(pat);
        tb_PenMode(11); /* patBic */
        tb_PaintRect(ch_rect(lr));
        tb_PenNormal();
    }
    use_font(0, 0, 0);
}

static void draw_icons(void)
{
    tb_FrameRect(ch_rect(icons_frame));
    gaddr saved_clip = tb_NewRgn();
    tb_GetClip(saved_clip);
    tb_ClipRect(ch_rect(icons_area));
    tb_EraseRect(ch_rect(icons_area));
    for (int i = 0; i < g_ch.ndevices; i++)
        draw_icon(i);
    tb_SetClip(saved_clip);
    tb_DisposeRgn(saved_clip);
}

static void icons_scrolled(int top)
{
    int max = icon_rows() - visible_icon_rows();
    if (max < 0)
        max = 0;
    if (top > max)
        top = max;
    if (top < 0)
        top = 0;
    if (top == g_ch.icon_top)
        return;
    g_ch.icon_top = top;
    tb_SetControlValue(g_ch.icon_bar, (int16_t)top);
    gaddr saved_clip = tb_NewRgn();
    tb_GetClip(saved_clip);
    tb_ClipRect(ch_rect(icons_area));
    tb_EraseRect(ch_rect(icons_area));
    for (int i = 0; i < g_ch.ndevices; i++)
        draw_icon(i);
    tb_SetClip(saved_clip);
    tb_DisposeRgn(saved_clip);
}

/* pascal void action(ControlHandle, short part): the arrows and the grey
 * area scroll a row and a page as they are held. */
static void icon_bar_action(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t part = (int16_t)cpu_pop16(c);
    cpu_pop32(c);
    int step = part == inUpButton ? -1 : part == inDownButton ? 1 : part == inPageUp ? -visible_icon_rows()
             : part == inPageDown                             ? visible_icon_rows()
                                                              : 0;
    if (step)
        icons_scrolled(g_ch.icon_top + step);
    cpu_push32(c, ret);
}

static void update_icon_bar(void)
{
    int max = icon_rows() - visible_icon_rows();
    if (max < 0)
        max = 0;
    tb_SetControlMaximum(g_ch.icon_bar, (int16_t)max);
    tb_HiliteControl(g_ch.icon_bar, (int16_t)(max > 0 && g_ch.active ? 0 : 255));
}

/* ---- The right side ---- */

void chooser_set_label(const char *mac)
{
    snprintf(g_ch.label, sizeof g_ch.label, "%s", mac);
    qrect r = { 6, 170, 24, 404 };
    tb_EraseRect(ch_rect(r));
    use_font(0, 0, 0);
    char fitted[300];
    fit_text(g_ch.label, r.right - r.left, fitted, sizeof fitted);
    draw_text_at(r.left, 20, fitted);
}

static gaddr new_control(qrect r, const char *title, int proc, int value)
{
    return tb_NewControl(g_ch.window, ch_rect(r), ch_pstr(title), 0, (int16_t)value, 0, 1, (int16_t)proc, 0);
}

/* The two buttons and the radio buttons below the list, as a device wants
 * them: rects from its 'nrct' (or ours), titles, shown or not. */
void chooser_layout_controls(const qrect rects[5], const char *b1, const char *b2, const char *on, const char *off,
                             bool radios)
{
    gaddr ctls[4] = { g_ch.button1, g_ch.button2, g_ch.radio_on, g_ch.radio_off };
    const char *titles[4] = { b1, b2, on, off };
    for (int i = 0; i < 4; i++) {
        tb_HideControl(ctls[i]);
        qrect r = rects ? rects[i] : default_rects[i];
        tb_MoveControl(ctls[i], r.left, r.top);
        tb_SizeControl(ctls[i], (int16_t)(r.right - r.left), (int16_t)(r.bottom - r.top));
        tb_SetControlTitle(ctls[i], ch_pstr(titles[i] ? titles[i] : ""));
    }
    g_ch.radio_rect = rects ? rects[4] : default_rects[4];
    g_ch.show_radios = radios;
    qrect area = { 108, 168, 140, 406 };
    tb_EraseRect(ch_rect(area));
    tb_InvalRect(ch_rect(area));
}

void chooser_show_button(int which, bool show)
{
    gaddr c = which == 1 ? g_ch.button1 : g_ch.button2;
    if (show)
        tb_ShowControl(c);
    else
        tb_HideControl(c);
    if (which == 1)
        g_ch.button1_default = show;
}

static void draw_default_outline(void)
{
    gaddr c = g_ch.button1;
    if (!c || !ControlRecord_contrlVis(rd32(c)))
        return;
    qrect r = qrect_get(rd32(c) + ControlRecord_contrlRect_off);
    r.top -= 4, r.left -= 4, r.bottom += 4, r.right += 4;
    bool on = rd8(rd32(c) + ControlRecord_contrlHilite_off) == 0;
    tb_PenSize(3, 3);
    if (!on) {
        static const uint8_t grey[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
        gaddr pat = g_ch.scratch + 6 * 256 + 72;
        gput(pat, grey, 8);
        tb_PenPat(pat);
    }
    tb_FrameRoundRect(ch_rect(r), 16, 16);
    tb_PenNormal();
}

/* Enabled while something in the list is selected (the left button is the
 * default, as Return and a double click are). */
void chooser_update_buttons(void)
{
    bool any = g_ch.list && chooser_selected_row() >= 0;
    if (g_ch.button1 && ControlRecord_contrlVis(rd32(g_ch.button1)))
        tb_HiliteControl(g_ch.button1, (int16_t)(any && g_ch.active && g_ch.button1_needs_sel ? 0
                                                  : !g_ch.button1_needs_sel && g_ch.active ? 0
                                                                                          : 255));
    draw_default_outline();
}

/* The list on the right, made new for each device. */
void chooser_new_list(int columns)
{
    if (g_ch.list)
        tb_LDispose(g_ch.list);
    qrect bounds = { 0, 0, 0, (int16_t)columns };
    gaddr b = ch_rect(bounds);
    int w = list_view.right - list_view.left;
    g_ch.list = tb_LNew(ch_rect(list_view), b, make_pt(0, (int16_t)w), 0, g_ch.window, 1, 0, 0, 1);
    if (g_ch.list)
        wr8(rd32(g_ch.list) + ListRec_selFlags_off, 0x80); /* lOnlyOne */
    qrect frame = { (int16_t)(list_view.top - 1), (int16_t)(list_view.left - 1), (int16_t)(list_view.bottom + 1),
                    (int16_t)(list_view.right + 1) };
    tb_EraseRect(ch_rect(frame));
    tb_FrameRect(ch_rect(frame));
    if (g_ch.list) {
        tb_LActivate((int8_t)g_ch.active, g_ch.list);
        tb_LUpdate(vis_rgn(), g_ch.list);
    }
}

int chooser_list_rows(void)
{
    if (!g_ch.list)
        return 0;
    qrect b = qrect_get(rd32(g_ch.list) + ListRec_dataBounds_off);
    return b.bottom - b.top;
}

int chooser_selected_row(void)
{
    if (!g_ch.list)
        return -1;
    gaddr cell = scratch_long();
    wr32(cell, 0);
    if (!tb_LGetSelect(1, cell, g_ch.list))
        return -1;
    return (int16_t)rd16(cell);
}

void chooser_select_row(int row, bool on)
{
    if (g_ch.list && row >= 0 && row < chooser_list_rows())
        tb_LSetSelect((int8_t)on, make_pt((int16_t)row, 0), g_ch.list);
}

void chooser_cell_text(int row, int col, char *out, size_t size)
{
    out[0] = 0;
    if (!g_ch.list)
        return;
    gaddr buf = g_ch.scratch + 6 * 256 + 128, len = scratch_word();
    wr16(len, 255);
    tb_LGetCell(buf, len, make_pt((int16_t)row, (int16_t)col), g_ch.list);
    int n = (int16_t)rd16(len);
    if (n < 0)
        n = 0;
    if ((size_t)n >= size)
        n = (int)size - 1;
    gget(out, buf, (uint32_t)n);
    out[n] = 0;
}

int chooser_add_row(const char *mac, const void *col1, int col1_len)
{
    if (!g_ch.list)
        return -1;
    int row = tb_LAddRow(1, (int16_t)chooser_list_rows(), g_ch.list);
    gaddr buf = g_ch.scratch + 6 * 256 + 128;
    size_t n = strlen(mac);
    gput(buf, mac, (uint32_t)n);
    tb_LSetCell(buf, (int16_t)n, make_pt((int16_t)row, 0), g_ch.list);
    if (col1 && col1_len > 0) {
        gput(buf, col1, (uint32_t)col1_len);
        tb_LSetCell(buf, (int16_t)col1_len, make_pt((int16_t)row, 1), g_ch.list);
    }
    return row;
}

void chooser_clear_rows(void)
{
    int n = chooser_list_rows();
    if (n)
        tb_LDelRow((int16_t)n, 0, g_ch.list);
}

/* ---- Our devices' panes ---- */

static int sort_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* The host's printers. */
static void fill_printers(void)
{
    static char names[64][256];
    int n = printers_fn ? printers_fn(names, 64, host_ctx) : 0;
    static struct {
        char shown[40]; /* MacRoman, its queue's underscores as spaces */
        char queue[256];
    } p[64];
    int count = 0;
    for (int i = 0; i < n && count < 64; i++) {
        uint8_t m[64];
        size_t len = utf8_to_macroman(names[i], m, 32);
        for (size_t k = 0; k < len; k++)
            if (m[k] == '_')
                m[k] = ' ';
        memcpy(p[count].shown, m, len);
        p[count].shown[len] = 0;
        snprintf(p[count].queue, sizeof p[count].queue, "%s", names[i]);
        count++;
    }
    qsort(p, (size_t)count, sizeof p[0], sort_names);
    for (int i = 0; i < count; i++)
        chooser_add_row(p[i].shown, p[i].queue, (int)strlen(p[i].queue));
    if (!strncmp(g_chooser.printer, "host:", 5))
        for (int i = 0; i < count; i++)
            if (!strcmp(p[i].queue, g_chooser.printer + 5))
                chooser_select_row(i, true);
    if (!count)
        chooser_set_label("No printers are set up.");
}

/* The PDF folder: its name in the list, chosen when selected. */
static void fill_pdf(void)
{
    chooser_clear_rows();
    if (!g_chooser.folder[0])
        return;
    const char *home = getenv("HOME");
    char shown[1100];
    size_t hl = home ? strlen(home) : 0;
    if (hl && !strncmp(g_chooser.folder, home, hl) && (g_chooser.folder[hl] == '/' || !g_chooser.folder[hl]))
        snprintf(shown, sizeof shown, "~%s", g_chooser.folder + hl);
    else
        snprintf(shown, sizeof shown, "%s", g_chooser.folder);
    uint8_t mac[300];
    size_t n = utf8_to_macroman(shown, mac, sizeof mac - 1);
    mac[n] = 0;
    char fitted[300];
    use_font(0, 0, 0);
    fit_text((const char *)mac, list_view.right - list_view.left - 8, fitted, sizeof fitted);
    chooser_add_row(fitted, NULL, 0);
    if (!strcmp(g_chooser.printer, "pdf"))
        chooser_select_row(0, true);
}

static void choose_pdf_folder(void)
{
    machine_file_request rq;
    memset(&rq, 0, sizeof rq);
    rq.folders = true;
    snprintf(rq.prompt, sizeof rq.prompt, "Choose the folder for the PDF files Blue Box prints:");
    snprintf(rq.directory, sizeof rq.directory, "%s", g_chooser.folder);
    char path[PATH_MAX];
    if (!stdfile_request(&rq, path, sizeof path))
        return;
    machine_set_print_folder(path);
    chooser_changed("print-folder", path);
    fill_pdf();
    chooser_select_row(0, true);
    chooser_set_printer("pdf");
    chooser_update_buttons();
}

/* NBP: the entities of a type in the zone, as they answer. */
static void nbp_answer(void *ctx, const uint8_t *tuple, int len)
{
    (void)ctx;
    if (len < 6 || g_ch.nfound >= CH_MAX_FOUND)
        return;
    int k = 5, olen = tuple[k];
    if (k + 1 + olen > len)
        return;
    ch_found f;
    memset(&f, 0, sizeof f);
    memcpy(f.name, tuple + k + 1, (size_t)olen);
    f.addr[0] = tuple[0];
    f.addr[1] = tuple[1];
    f.addr[2] = tuple[2];
    f.addr[3] = tuple[3];
    for (int i = 0; i < g_ch.nfound; i++)
        if (!strcmp(g_ch.found[i].name, f.name)) {
            memcpy(g_ch.found[i].addr, f.addr, 4);
            return;
        }
    g_ch.found[g_ch.nfound++] = f;
    g_ch.found_changed = true;
}

void chooser_start_lookup(const char *type)
{
    if (g_ch.lookup)
        at_nbp_stop(g_ch.lookup);
    g_ch.lookup = 0;
    snprintf(g_ch.lookup_type, sizeof g_ch.lookup_type, "%s", type);
    g_ch.nfound = 0;
    g_ch.found_changed = true;
    if (!appletalk_enabled() || at_open() != noErr)
        return;
    uint8_t packed[100];
    int k = 0;
    packed[k++] = 1;
    packed[k++] = '=';
    size_t tl = strlen(type);
    packed[k++] = (uint8_t)tl;
    memcpy(packed + k, type, tl);
    k += (int)tl;
    const char *zone = g_ch.zone[0] ? g_ch.zone : "*";
    size_t zl = strlen(zone);
    packed[k++] = (uint8_t)zl;
    memcpy(packed + k, zone, zl);
    k += (int)zl;
    g_ch.lookup = at_nbp_start(packed, k, 4, 750, nbp_answer, NULL);
    g_ch.lookup_ticks = rd32(LM_Ticks);
}

/* The answers so far into the list (sorted; the selection kept), and the
 * device told which are chosen. */
static void show_found(void)
{
    if (!g_ch.found_changed || !g_ch.list)
        return;
    g_ch.found_changed = false;
    char selected[40] = "";
    int row = chooser_selected_row();
    if (row >= 0)
        chooser_cell_text(row, 0, selected, sizeof selected);
    qsort(g_ch.found, (size_t)g_ch.nfound, sizeof g_ch.found[0], sort_names);
    tb_LSetDrawingMode(0, g_ch.list);
    chooser_clear_rows();
    for (int i = 0; i < g_ch.nfound; i++)
        chooser_add_row(g_ch.found[i].name, g_ch.found[i].addr, 4);
    chooser_get_selection(selected);
    tb_LSetDrawingMode(1, g_ch.list);
    qrect frame = list_view;
    tb_EraseRect(ch_rect(frame));
    tb_LUpdate(vis_rgn(), g_ch.list);
    chooser_update_buttons();
}

/* Which rows are chosen: the LaserWriter printed to, or the row that was
 * selected before the list changed; an extension's getSelMsg. */
void chooser_get_selection(const char *was)
{
    ch_device *d = &g_ch.devices[g_ch.current];
    if (d->kind == DEV_EXTENSION) {
        chooser_ext_get_selection();
        return;
    }
    char want[64] = "";
    if (d->kind == DEV_LASERWRITER && !strncmp(g_chooser.printer, "laserwriter:", 12)) {
        const char *at = strrchr(g_chooser.printer + 12, '@');
        char obj[160];
        snprintf(obj, sizeof obj, "%.*s", (int)(at - g_chooser.printer - 12), g_chooser.printer + 12);
        uint8_t m[40];
        size_t n = utf8_to_macroman(obj, m, 32);
        memcpy(want, m, n);
        want[n] = 0;
    } else if (was) {
        snprintf(want, sizeof want, "%s", was);
    }
    for (int i = 0; i < chooser_list_rows(); i++) {
        char t[40];
        chooser_cell_text(i, 0, t, sizeof t);
        if (want[0] && !strcmp(t, want))
            chooser_select_row(i, true);
    }
}

/* A device selected: its pane on the right. */
static void select_device(int i)
{
    if (i < 0 || i >= g_ch.ndevices)
        return;
    int was = g_ch.current;
    if (was >= 0 && g_ch.devices[was].kind == DEV_EXTENSION)
        chooser_ext_terminate();
    if (g_ch.lookup)
        at_nbp_stop(g_ch.lookup);
    g_ch.lookup = 0;
    g_ch.nfound = 0;
    g_ch.current = i;
    if (was >= 0)
        draw_icon(was);
    draw_icon(i);
    ch_device *d = &g_ch.devices[i];
    g_ch.button1_needs_sel = true;
    chooser_layout_controls(NULL, "", "", "", "", false);
    chooser_new_list(d->kind == DEV_PDF ? 1 : 2);
    switch (d->kind) {
    case DEV_APPLESHARE:
        chooser_set_label("Select a file server:");
        chooser_layout_controls(NULL, "OK", "Server IP Address\xC9", "", "", false);
        {
            qrect r[5];
            memcpy(r, default_rects, sizeof r);
            r[0] = (qrect){ 112, 182, 132, 242 };
            r[1] = (qrect){ 112, 258, 132, 398 };
            chooser_layout_controls(r, "OK", "Server IP Address\xC9", "", "", false);
        }
        chooser_show_button(1, true);
        chooser_show_button(2, true);
        if (appletalk_enabled())
            chooser_start_lookup("AFPServer");
        break;
    case DEV_LASERWRITER:
        chooser_set_label(appletalk_enabled() ? "Select a LaserWriter:" : "AppleTalk is inactive.");
        if (appletalk_enabled())
            chooser_start_lookup("LaserWriter");
        break;
    case DEV_PRINTER:
        chooser_set_label("Select a printer:");
        fill_printers();
        break;
    case DEV_PDF:
        chooser_set_label("Save PDF files in:");
        {
            qrect r[5];
            memcpy(r, default_rects, sizeof r);
            r[0] = (qrect){ 112, 262, 132, 386 };
            chooser_layout_controls(r, "Choose Folder\xC9", "", "", "", false);
        }
        g_ch.button1_needs_sel = false;
        chooser_show_button(1, true);
        fill_pdf();
        break;
    case DEV_EXTENSION:
        chooser_ext_select(d);
        break;
    }
    if (d->kind == DEV_APPLESHARE && !appletalk_enabled())
        chooser_set_label("AppleTalk is inactive; servers by address:");
    chooser_update_buttons();
}

/* The list's selection changed (a click): the device's choice. */
static void selection_changed(int row_was)
{
    ch_device *d = &g_ch.devices[g_ch.current];
    int row = chooser_selected_row();
    if (d->kind == DEV_EXTENSION) {
        chooser_ext_selection_changed(row_was, row);
    } else if (row != row_was) {
        char name[64] = "";
        if (row >= 0)
            chooser_cell_text(row, 0, name, sizeof name);
        char utf8[200];
        macroman_to_utf8((const uint8_t *)name, strlen(name), utf8, sizeof utf8);
        char spec[600];
        switch (d->kind) {
        case DEV_LASERWRITER: {
            char zone[100];
            macroman_to_utf8((const uint8_t *)(g_ch.zone[0] ? g_ch.zone : "*"), strlen(g_ch.zone[0] ? g_ch.zone : "*"),
                             zone, sizeof zone);
            snprintf(spec, sizeof spec, "laserwriter:%s@%s", utf8, zone);
            chooser_set_printer(row >= 0 ? spec : "panel");
            break;
        }
        case DEV_PRINTER: {
            char queue[256] = "";
            if (row >= 0)
                chooser_cell_text(row, 1, queue, sizeof queue);
            snprintf(spec, sizeof spec, "host:%s", queue);
            chooser_set_printer(row >= 0 && queue[0] ? spec : "panel");
            break;
        }
        case DEV_PDF: chooser_set_printer(row >= 0 ? "pdf" : "panel"); break;
        default: break;
        }
    }
    chooser_update_buttons();
}

static void button_hit(int which, int16_t modifiers)
{
    ch_device *d = &g_ch.devices[g_ch.current];
    switch (d->kind) {
    case DEV_APPLESHARE:
        if (which == 1) {
            int row = chooser_selected_row();
            if (row >= 0)
                chooser_afp_connect_found(row);
        } else {
            chooser_afp_connect_address();
        }
        break;
    case DEV_PDF:
        if (which == 1)
            choose_pdf_folder();
        break;
    case DEV_EXTENSION: chooser_ext_button(which, modifiers); break;
    default: break;
    }
}

/* ---- AppleTalk on or off, the zones ---- */

static void draw_bottom(void)
{
    use_font(0, 0, 0);
    draw_text_at(172, 162, "User Name:");
    qrect f = { (int16_t)(user_text.top - 3), (int16_t)(user_text.left - 3), (int16_t)(user_text.bottom + 3),
                (int16_t)(user_text.right + 3) };
    tb_FrameRect(ch_rect(f));
    if (g_ch.te)
        tb_TEUpdate(ch_rect(user_text), g_ch.te);
    draw_text_at(196, 222, "AppleTalk");
    qrect line = { 182, 170, 183, 404 };
    static const uint8_t grey[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    gaddr pat = g_ch.scratch + 6 * 256 + 72;
    gput(pat, grey, 8);
    tb_FillRect(ch_rect(line), pat);
}

static void draw_zones(void)
{
    if (!g_ch.zones)
        return;
    use_font(0, 0, 0);
    draw_text_at(9, 186, "AppleTalk Zones:");
    qrect frame = { (int16_t)(zones_view.top - 1), (int16_t)(zones_view.left - 1), (int16_t)(zones_view.bottom + 1),
                    (int16_t)(zones_view.right + 1) };
    tb_FrameRect(ch_rect(frame));
    tb_LUpdate(vis_rgn(), g_ch.zones);
}

/* The zones, when the network has a router to ask. */
static void fill_zones(void)
{
    static char names[256][33];
    int n = at_zone_list(names, 256);
    if (n <= 0) {
        if (g_ch.zones)
            tb_LDispose(g_ch.zones);
        g_ch.zones = 0;
        return;
    }
    if (!g_ch.zones) {
        qrect bounds = { 0, 0, 0, 1 };
        g_ch.zones = tb_LNew(ch_rect(zones_view), ch_rect(bounds),
                             make_pt(0, (int16_t)(zones_view.right - zones_view.left)), 0, g_ch.window, 1, 0, 0, 1);
        if (!g_ch.zones)
            return;
        wr8(rd32(g_ch.zones) + ListRec_selFlags_off, 0x80);
    }
    tb_LSetDrawingMode(0, g_ch.zones);
    qrect b = qrect_get(rd32(g_ch.zones) + ListRec_dataBounds_off);
    if (b.bottom > b.top)
        tb_LDelRow((int16_t)(b.bottom - b.top), 0, g_ch.zones);
    qsort(names, (size_t)n, sizeof names[0], sort_names);
    char mine[33];
    at_my_zone(mine);
    if (!g_ch.zone[0])
        snprintf(g_ch.zone, sizeof g_ch.zone, "%s", mine);
    gaddr buf = g_ch.scratch + 6 * 256 + 128;
    for (int i = 0; i < n; i++) {
        int row = tb_LAddRow(1, (int16_t)i, g_ch.zones);
        size_t len = strlen(names[i]);
        gput(buf, names[i], (uint32_t)len);
        tb_LSetCell(buf, (int16_t)len, make_pt((int16_t)row, 0), g_ch.zones);
        if (!strcmp(names[i], g_ch.zone))
            tb_LSetSelect(1, make_pt((int16_t)row, 0), g_ch.zones);
    }
    tb_LSetDrawingMode(1, g_ch.zones);
    tb_LAutoScroll(g_ch.zones);
}

static void set_appletalk(bool on)
{
    if (on == appletalk_enabled())
        return;
    if (!on && appletalk_in_use()) {
        /* As the Chooser asked: the program's connections end. */
        if (!chooser_confirm("Are you sure you want to make AppleTalk inactive? The program's network "
                             "connections will be closed.",
                             "OK"))
            return;
    }
    const char *mode = appletalk_set_active(on);
    tb_SetControlValue(g_ch.at_on, on);
    tb_SetControlValue(g_ch.at_off, !on);
    chooser_note_appletalk();
    chooser_changed("appletalk", mode);
    for (int i = 0; i < g_ch.ndevices; i++)
        draw_icon(i);
    if (g_ch.current >= 0 && g_ch.devices[g_ch.current].appletalk)
        select_device(g_ch.current);
    fill_zones();
    qrect zr = { 170, 0, WIN_H, 168 };
    tb_EraseRect(ch_rect(zr));
    draw_zones();
}

/* ---- The window's life ---- */

static void commit_user_name(void)
{
    if (!g_ch.te)
        return;
    gaddr te = rd32(g_ch.te);
    int n = (int16_t)rd16(te + TERec_teLength_off);
    if (n > 31)
        n = 31;
    char name[40];
    gget(name, rd32(rd32(te + TERec_hText_off)), (uint32_t)n);
    name[n] = 0;
    chooser_set_user(name);
}

static void draw_window(void)
{
    use_font(0, 0, 0);
    draw_icons();
    draw_zones();
    use_font(0, 0, 0);
    char fitted[300];
    fit_text(g_ch.label, 234, fitted, sizeof fitted);
    draw_text_at(170, 20, fitted);
    qrect frame = { (int16_t)(list_view.top - 1), (int16_t)(list_view.left - 1), (int16_t)(list_view.bottom + 1),
                    (int16_t)(list_view.right + 1) };
    tb_FrameRect(ch_rect(frame));
    if (g_ch.list)
        tb_LUpdate(vis_rgn(), g_ch.list);
    if (g_ch.show_radios) {
        draw_text_at(g_ch.radio_rect.left, g_ch.radio_rect.top + 12, g_ch.radio_label);
    }
    draw_bottom();
    tb_DrawControls(g_ch.window);
    draw_default_outline();
    chooser_ext_draw();
}

static void activate(bool on)
{
    g_ch.active = on;
    if (g_ch.list)
        tb_LActivate((int8_t)on, g_ch.list);
    if (g_ch.zones)
        tb_LActivate((int8_t)on, g_ch.zones);
    if (g_ch.te) {
        if (on)
            tb_TEActivate(g_ch.te);
        else
            tb_TEDeactivate(g_ch.te);
    }
    gaddr ctls[4] = { g_ch.button2, g_ch.radio_on, g_ch.radio_off, g_ch.at_on };
    for (int i = 0; i < 4; i++)
        if (ctls[i])
            tb_HiliteControl(ctls[i], (int16_t)(on ? 0 : 255));
    if (g_ch.at_off)
        tb_HiliteControl(g_ch.at_off, (int16_t)(on ? 0 : 255));
    if (!(rd8(0x0946) & 0x80)) { /* ChooserBits: AppleTalk's state locked */
        tb_HiliteControl(g_ch.at_on, 255);
        tb_HiliteControl(g_ch.at_off, 255);
    }
    update_icon_bar();
    chooser_update_buttons();
    if (!on)
        commit_user_name();
}

static void make_window(gaddr dce)
{
    memset(&g_ch, 0, sizeof g_ch);
    g_ch.current = -1;
    g_ch.dce = dce;
    g_ch.scratch = tb_NewPtr(6 * 256 + 512, true, true);
    find_devices();
    int top = 40 + 20, left = (g_screen_width - WIN_W) / 2;
    if (left < 4)
        left = 4;
    qrect bounds = { (int16_t)top, (int16_t)left, (int16_t)(top + WIN_H), (int16_t)(left + WIN_W) };
    g_ch.window = tb_NewCWindow(0, ch_rect(bounds), ch_pstr("Chooser"), 0, 4 /* noGrowDocProc */, 0xFFFFFFFFu, 1, 0);
    if (!g_ch.window)
        return;
    wr16(g_ch.window + WindowRecord_windowKind_off, rd16(dce + DCtlEntry_dCtlRefNum_off));
    wr32(dce + DCtlEntry_dCtlWindow_off, g_ch.window);
        tb_SetPort(g_ch.window);
    use_font(0, 0, 0);

    gaddr action = native_register(icon_bar_action, "Chooser icons scroll", 0);
    g_ch.icon_action = action;
    g_ch.icon_bar = tb_NewControl(g_ch.window, ch_rect(icons_bar), ch_pstr(""), 1, 0, 0, 0, scrollBarProc, 0);
    g_ch.button1 = new_control(default_rects[0], "", pushButProc, 0);
    g_ch.button2 = new_control(default_rects[1], "", pushButProc, 0);
    g_ch.radio_on = new_control(default_rects[2], "", radioButProc, 0);
    g_ch.radio_off = new_control(default_rects[3], "", radioButProc, 0);
    g_ch.at_on = tb_NewControl(g_ch.window, ch_rect(at_active), ch_pstr("Active"), 1, appletalk_enabled(), 0, 1,
                               radioButProc, 0);
    g_ch.at_off = tb_NewControl(g_ch.window, ch_rect(at_inactive), ch_pstr("Inactive"), 1, !appletalk_enabled(),
                                0, 1, radioButProc, 0);

    g_ch.te = tb_TENew(ch_rect(user_text), ch_rect(user_text));
    if (g_ch.te) {
        wr16(rd32(g_ch.te) + TERec_crOnly_off, 0xFFFF); /* one line */
        size_t n = strlen(g_chooser.user);
        gaddr buf = g_ch.scratch + 6 * 256 + 128;
        gput(buf, g_chooser.user, (uint32_t)n);
        tb_TESetText(buf, (int32_t)n, g_ch.te);
        tb_TESetSelect(0, 32767, g_ch.te);
    }
    fill_zones();
    update_icon_bar();
    int pd = printer_device();
    select_device(pd >= 0 ? pd : 0);
    /* the chosen device's row in sight */
    if (g_ch.current >= 0)
        icons_scrolled(g_ch.current / 2 - visible_icon_rows() + 1);
    tb_ShowWindow(g_ch.window);
    tb_SelectWindow(g_ch.window);
}

static void close_window(void)
{
    if (!g_ch.window)
        return;
    commit_user_name();
    if (g_ch.current >= 0 && g_ch.devices[g_ch.current].kind == DEV_EXTENSION)
        chooser_ext_terminate();
    chooser_ext_forget();
    if (g_ch.lookup)
        at_nbp_stop(g_ch.lookup);
    if (g_ch.list)
        tb_LDispose(g_ch.list);
    if (g_ch.zones)
        tb_LDispose(g_ch.zones);
    if (g_ch.te)
        tb_TEDispose(g_ch.te);
    tb_DisposeWindow(g_ch.window);
    wr32(g_ch.dce + DCtlEntry_dCtlWindow_off, 0);
    tb_DisposePtr(g_ch.scratch);
    memset(&g_ch, 0, sizeof g_ch);
    g_ch.current = -1;
}

/* ---- Events ---- */

static bool in(qrect r, int h, int v) { return v >= r.top && v < r.bottom && h >= r.left && h < r.right; }

static void click(gaddr evt)
{
    gaddr pt = scratch_long();
    wr32(pt, rd32(evt + EventRecord_where_off));
    tb_GlobalToLocal(pt);
    uint32_t where = rd32(pt);
    int h = pt_h(where), v = pt_v(where);
    int16_t mods = (int16_t)rd16(evt + EventRecord_modifiers_off);
    gaddr chp = scratch_long() + 4;
    int16_t part = tb_FindControl(where, g_ch.window, chp);
    gaddr ctl = rd32(chp);
    if (part && ctl == g_ch.icon_bar) {
        if (part == inThumb) {
            tb_TrackControl(ctl, where, 0);
            int top = tb_GetControlValue(ctl);
            g_ch.icon_top = -1;
            icons_scrolled(top);
        } else {
            tb_TrackControl(ctl, where, g_ch.icon_action);
        }
        return;
    }
    if (part && (ctl == g_ch.at_on || ctl == g_ch.at_off)) {
        if (tb_TrackControl(ctl, where, 0))
            set_appletalk(ctl == g_ch.at_on);
        return;
    }
    if (part && (ctl == g_ch.button1 || ctl == g_ch.button2 || ctl == g_ch.radio_on || ctl == g_ch.radio_off)) {
        if (tb_TrackControl(ctl, where, 0))
            button_hit(ctl == g_ch.button1 ? 1 : ctl == g_ch.button2 ? 2 : ctl == g_ch.radio_on ? 3 : 4, mods);
        return;
    }
    if (in(icons_area, h, v)) {
        int col = (h - icons_area.left) / CELL_W, row = (v - icons_area.top) / CELL_H + g_ch.icon_top;
        int i = row * 2 + col;
        if (col < 2 && i < g_ch.ndevices && i != g_ch.current)
            select_device(i);
        return;
    }
    if (g_ch.zones && in((qrect){ zones_view.top, zones_view.left, zones_view.bottom, (int16_t)(zones_view.right + 16) }, h, v)) {
        tb_LClick(where, mods, g_ch.zones);
        gaddr cell = scratch_long() + 8;
        wr32(cell, 0);
        if (tb_LGetSelect(1, cell, g_ch.zones)) {
            gaddr buf = g_ch.scratch + 6 * 256 + 128, len = scratch_word();
            wr16(len, 33);
            tb_LGetCell(buf, len, rd32(cell), g_ch.zones);
            char z[40];
            int n = (int16_t)rd16(len);
            gget(z, buf, (uint32_t)n);
            z[n] = 0;
            if (strcmp(z, g_ch.zone)) {
                snprintf(g_ch.zone, sizeof g_ch.zone, "%s", z);
                if (g_ch.current >= 0 && g_ch.devices[g_ch.current].appletalk)
                    select_device(g_ch.current);
            }
        }
        return;
    }
    qrect list_and_bar = { list_view.top, list_view.left, list_view.bottom, (int16_t)(list_view.right + 16) };
    if (g_ch.list && in(list_and_bar, h, v)) {
        int was = chooser_selected_row();
        bool dbl = tb_LClick(where, mods, g_ch.list);
        selection_changed(was);
        if (dbl && chooser_selected_row() >= 0 && g_ch.button1 && ControlRecord_contrlVis(rd32(g_ch.button1)) &&
            !rd8(rd32(g_ch.button1) + ControlRecord_contrlHilite_off))
            button_hit(1, mods);
        return;
    }
    if (g_ch.te && in(user_text, h, v)) {
        tb_TEClick(where, (int8_t)((mods & shiftKey) != 0), g_ch.te);
        return;
    }
    chooser_ext_click(where, mods);
}

static void key(gaddr evt)
{
    uint8_t ch = (uint8_t)rd32(evt + EventRecord_message_off);
    if (ch == 13 || ch == 3) { /* Return, Enter: the default button */
        if (g_ch.button1 && ControlRecord_contrlVis(rd32(g_ch.button1)) &&
            !rd8(rd32(g_ch.button1) + ControlRecord_contrlHilite_off)) {
            tb_HiliteControl(g_ch.button1, 1);
            tb_HiliteControl(g_ch.button1, 0);
            button_hit(1, 0);
        }
        return;
    }
    if (ch == 9 || !g_ch.te)
        return;
    gaddr te = rd32(g_ch.te);
    int len = (int16_t)rd16(te + TERec_teLength_off);
    int sel = (int16_t)rd16(te + TERec_selEnd_off) - (int16_t)rd16(te + TERec_selStart_off);
    if (ch >= 32 && ch != 127 && len - sel >= 31) { /* 31 characters, as the Chooser allowed */
        tb_SysBeep(1);
        return;
    }
    tb_TEKey(ch, g_ch.te);
}

static void handle_event(gaddr evt)
{
    switch ((int16_t)rd16(evt + EventRecord_what_off)) {
    case mouseDown: click(evt); break;
    case keyDown:
    case autoKey: key(evt); break;
    case updateEvt:
        tb_BeginUpdate(g_ch.window);
        draw_window();
        tb_EndUpdate(g_ch.window);
        break;
    case activateEvt: activate(rd16(evt + EventRecord_modifiers_off) & activeFlag); break;
    default: break;
    }
}

static void edit(int code)
{
    if (!g_ch.te)
        return;
    switch (code) {
    case accCut:
        tb_TECut(g_ch.te);
        tb_ZeroScrap();
        tb_TEToScrap();
        break;
    case accCopy:
        tb_TECopy(g_ch.te);
        tb_ZeroScrap();
        tb_TEToScrap();
        break;
    case accPaste:
        tb_TEFromScrap();
        tb_TEPaste(g_ch.te);
        break;
    case accClear: tb_TEDelete(g_ch.te); break;
    default: break;
    }
}

/* Time: the caret, the network's answers, lookups asked again. */
static void run(void)
{
    if (g_ch.te && g_ch.active)
        tb_TEIdle(g_ch.te);
    if (g_ch.lookup || g_ch.lookup_type[0]) {
        show_found();
        if (!at_nbp_busy(g_ch.lookup) && rd32(LM_Ticks) - g_ch.lookup_ticks > 5 * 60 && g_ch.current >= 0 &&
            g_ch.devices[g_ch.current].appletalk && appletalk_enabled()) {
            /* asked again every few seconds, as the Chooser kept looking */
            int keep = g_ch.nfound;
            char type[40];
            snprintf(type, sizeof type, "%s", g_ch.lookup_type);
            ch_found saved[CH_MAX_FOUND];
            memcpy(saved, g_ch.found, sizeof saved);
            chooser_start_lookup(type);
            memcpy(g_ch.found, saved, sizeof saved);
            g_ch.nfound = keep;
            g_ch.found_changed = false;
        }
    }
    chooser_ext_run();
}

/* ---- The driver's routines ---- */

static void chooser_open_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr dce = c->a[1];
    gaddr saved = qd_port();
    if (!g_ch.window)
        make_window(dce);
    else
        tb_SelectWindow(g_ch.window);
    tb_SetPort(saved && saved != g_ch.window ? saved : g_ch.window);
    c->d[0] = g_ch.window ? 0 : (uint32_t)(int32_t)memFullErr;
}

static void chooser_control_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    int16_t code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    gaddr param = rd32(pb + CntrlParam_csParam_off);
    c->d[0] = 0;
    if (!g_ch.window)
        return;
    gaddr saved = qd_port();
    tb_SetPort(g_ch.window);
    use_font(0, 0, 0);
    switch (code) {
    case accEvent: handle_event(param); break;
    case accRun: run(); break;
    case accCursor: break;
    case accCut:
    case accCopy:
    case accPaste:
    case accClear: edit(code); break;
    case goodbye: commit_user_name(); break;
    default: c->d[0] = (uint32_t)(int32_t)-17; break; /* controlErr */
    }
    if (g_ch.window)
        tb_SetPort(saved && saved != g_ch.window ? saved : g_ch.window);
}

static void chooser_close_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr saved = qd_port();
    bool was_ours = saved == g_ch.window;
    close_window();
    if (!was_ours)
        tb_SetPort(saved);
    c->d[0] = 0;
}

int machine_open_chooser(void)
{
    gaddr s = tb_NewPtr(16, false, true);
    static const uint8_t name[] = "\x08\0Chooser";
    gput(s, name, 9);
    int16_t rn = tb_OpenDeskAcc(s);
    tb_DisposePtr(s);
    return rn;
}

void chooser_reset(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    g_ch.current = -1;
    wr8(0x0946, 0xFF); /* ChooserBits: nothing locked */
    chooser_note_appletalk();
    chooser_ext_reset();
}
