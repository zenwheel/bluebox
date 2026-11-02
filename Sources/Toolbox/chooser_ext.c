/*
 * Chooser extensions: files of type 'PRES' (serial printers), 'PRER'
 * (other printers) and 'RDEV' (other devices) in the System Folder or its
 * Extensions folder, listed after our own devices with the icon their
 * bundle gives, and called as the Chooser called them (Inside Macintosh:
 * Devices, "Writing a Chooser Extension"; Technote OV 01).
 *
 * The device package, 'PACK' -4096, starts with a branch to its code, a
 * device ID, 'PACK', -4096, a version and its flags: bit 31 an AppleTalk
 * device, 28 several may be selected, 27 and 26 it uses the left and
 * right buttons, 24 it wants zone names rather than "*", 20 its On and
 * Off radio buttons; 17 to 11 the messages it takes: chooserInitMsg (11),
 * newSelMsg (12), fillListMsg (13), getSelMsg (14), selectMsg (15),
 * deselectMsg (16), terminateMsg (17); buttonMsg (19) it always gets.
 * It is called as
 *     FUNCTION Device(message, caller: INTEGER; objName, zoneName:
 *                     StringPtr; p1, p2: LONGINT): OSErr;
 * with caller 1 (the Chooser), p1 the list, its file the current resource
 * file and the Chooser's window the port.  Its strings: 'STR ' -4096 the
 * NBP type of its devices, -4091 the list's label, -4093 and -4092 the
 * buttons' titles, -4089, -4088 and -4087 the radio buttons' and their
 * label; 'nrct' -4096 where they go; 'GNRL' -4096 NBP's retry interval
 * (in 8 ticks) and count; 'LDEF' -4096 a list definition of its own.
 *
 * Printing still goes where our Printing Manager sends it (the host's
 * print panel): a printer chosen here is the driver programs see named in
 * 'STR ' -8192.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chooser.h"
#include "tb_internal.h"
#include "trap.h"

enum { chooserInitMsg = 11, newSelMsg = 12, fillListMsg = 13, getSelMsg = 14, selectMsg = 15, deselectMsg = 16,
       terminateMsg = 17, buttonMsg = 19 };
enum { chooserID = 1 };
#define F_APPLETALK 0x80000000u
#define F_MULTIPLE 0x10000000u
#define F_LEFT 0x08000000u
#define F_RIGHT 0x04000000u
#define F_ZONES 0x01000000u
#define F_RADIOS 0x00100000u
#define F_INIT 0x00020000u
#define F_NEWSEL 0x00010000u
#define F_FILL 0x00008000u
#define F_GETSEL 0x00004000u
#define F_SELECT 0x00002000u
#define F_DESELECT 0x00001000u
#define F_TERMINATE 0x00000800u

static struct {
    ch_device *d;
    int16_t rn;
    gaddr pack;
    uint32_t flags;
    gaddr ctls;   /* size word and the four controls' handles */
    gaddr zone;   /* the zone name passed */
    gaddr name;   /* objName's string */
    bool serial;
} ext;

static uint32_t type_pres, type_prer, type_rdev;

/* The icon the extension's bundle gives its own file type (or its
 * 'ICN#' -4096), copied out of the file. */
static gaddr extension_icon(uint32_t type)
{
    int16_t id = -4096;
    if (!tb_Get1Resource((int32_t)fourcc("ICN#"), -4096)) {
        id = 0;
        gaddr bndl = tb_Get1IndResource((int32_t)fourcc("BNDL"), 1);
        int16_t fref_local = -1;
        int nf = tb_Count1Resources((int32_t)fourcc("FREF"));
        for (int i = 1; i <= nf; i++) {
            gaddr f = tb_Get1IndResource((int32_t)fourcc("FREF"), (int16_t)i);
            if (f && rd32(f) && rd32(rd32(f)) == type)
                fref_local = (int16_t)rd16(rd32(f) + 4);
        }
        if (bndl && rd32(bndl) && fref_local >= 0) {
            gaddr p = rd32(bndl);
            int ntypes = (int16_t)rd16(p + 6) + 1;
            gaddr q = p + 8;
            for (int t = 0; t < ntypes && t < 16; t++) {
                uint32_t rt = rd32(q);
                int n = (int16_t)rd16(q + 4) + 1;
                q += 6;
                for (int k = 0; k < n && k < 64; k++, q += 4)
                    if (rt == fourcc("ICN#") && (int16_t)rd16(q) == fref_local)
                        id = (int16_t)rd16(q + 2);
            }
        }
        if (!id)
            return 0;
    }
    gaddr sp = ch_rect((qrect){ 0, 0, 0, 0 });
    if (tb_NewIconSuite(sp) != noErr)
        return 0;
    gaddr suite = rd32(sp);
    static const char *const kinds[] = { "ICN#", "icl4", "icl8", "ics#", "ics4", "ics8" };
    for (int i = 0; i < 6; i++) {
        gaddr h = tb_Get1Resource((int32_t)fourcc(kinds[i]), id);
        if (!h || !rd32(h))
            continue;
        tb_DetachResource(h);
        tb_AddIconToSuite(h, suite, (int32_t)fourcc(kinds[i]));
    }
    return suite;
}

static uint32_t pack_flags(void)
{
    gaddr p = tb_Get1Resource((int32_t)fourcc("PACK"), -4096);
    if (!p || !rd32(p) || tb_GetHandleSize(p) < 16 || rd32(rd32(p) + 4) != fourcc("PACK"))
        return 0;
    return rd32(rd32(p) + 12);
}

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp(((const char(*)[PATH_MAX])a)[0], ((const char(*)[PATH_MAX])b)[0]);
}

/* The System Folder's extension files, and its Extensions folder's. */
void chooser_find_extensions(void)
{
    type_pres = fourcc("PRES");
    type_prer = fourcc("PRER");
    type_rdev = fourcc("RDEV");
    const char *sys = fm_system_folder_host();
    if (!sys)
        return;
    char dirs[2][PATH_MAX];
    snprintf(dirs[0], sizeof dirs[0], "%s", sys);
    snprintf(dirs[1], sizeof dirs[1], "%s/Extensions", sys);
    static char found[CH_MAX_DEVICES][2][PATH_MAX]; /* name (MacRoman), path */
    int n = 0;
    for (int k = 0; k < 2; k++) {
        DIR *dir = opendir(dirs[k]);
        if (!dir)
            continue;
        struct dirent *e;
        while ((e = readdir(dir)) && n < CH_MAX_DEVICES - 8) {
            if (e->d_name[0] == '.')
                continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", dirs[k], e->d_name);
            uint32_t t = fm_type_of_path(path);
            if (t != type_pres && t != type_prer && t != type_rdev)
                continue;
            uint8_t mac[64];
            size_t len = utf8_to_macroman(e->d_name, mac, 31);
            for (size_t i = 0; i < len; i++)
                if (mac[i] == ':')
                    mac[i] = '/';
            memcpy(found[n][0], mac, len);
            found[n][0][len] = 0;
            snprintf(found[n][1], PATH_MAX, "%s", path);
            n++;
        }
        closedir(dir);
    }
    qsort(found, (size_t)n, sizeof found[0], cmp_names);
    int16_t cur = tb_CurResFile();
    for (int i = 0; i < n; i++) {
        uint32_t t = fm_type_of_path(found[i][1]);
        chooser_add_extension(found[i][0], found[i][1], t);
        ch_device *d = &g_ch.devices[g_ch.ndevices - 1];
        int16_t rn = rm_open_path(found[i][1], 1 /* fsRdPerm */);
        if (rn < 0)
            continue;
        tb_UseResFile(rn);
        uint32_t flags = pack_flags();
        d->appletalk = (flags & F_APPLETALK) != 0;
        d->suite = extension_icon(t);
        tb_CloseResFile(rn);
        tb_UseResFile(cur);
    }
}

/* ---- Calling the package ---- */

static int16_t call(int16_t msg, gaddr obj, uint32_t p1, uint32_t p2)
{
    if (!ext.pack || !rd32(ext.pack))
        return 0;
    gaddr port = qd_port();
    int16_t cur = tb_CurResFile();
    tb_UseResFile(ext.rn);
    tb_SetPort(g_ch.window);
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push16(c, (uint16_t)msg);
    cpu_push16(c, chooserID);
    cpu_push32(c, obj);
    cpu_push32(c, ext.zone);
    cpu_push32(c, p1);
    cpu_push32(c, p2);
    LOGF(LOG_TRAPS, "  Chooser: %.*s message %d\n", (int)strlen(ext.d->name), ext.d->name, msg);
    guest_call(rd32(ext.pack));
    int16_t r = (int16_t)cpu_pop16(c);
    tb_UseResFile(cur);
    tb_SetPort(port);
    return r;
}

static bool takes(uint32_t bit) { return (ext.flags & bit) != 0; }

static void get_string(int16_t id, char *out, size_t size)
{
    out[0] = 0;
    gaddr h = tb_Get1Resource((int32_t)fourcc("STR "), id);
    if (!h || !rd32(h))
        return;
    int n = rd8(rd32(h));
    if ((size_t)n >= size)
        n = (int)size - 1;
    gget(out, rd32(h) + 1, (uint32_t)n);
    out[n] = 0;
}

/* The serial ports, for a serial printer. */
static const char *const ports[2] = { "Modem Port", "Printer Port" };

void chooser_ext_select(ch_device *d)
{
    memset(&ext, 0, sizeof ext);
    ext.d = d;
    ext.serial = d->type == type_pres;
    ext.rn = rm_open_path(d->path, 0);
    if (ext.rn < 0) {
        chooser_set_label("This device can't be opened.");
        ext.d = NULL;
        return;
    }
    int16_t cur = tb_CurResFile();
    tb_UseResFile(ext.rn);
    ext.pack = tb_Get1Resource((int32_t)fourcc("PACK"), -4096);
    if (ext.pack && rd32(ext.pack)) {
        tb_HNoPurge(ext.pack);
        tb_HLock(ext.pack);
        ext.flags = pack_flags();
    } else {
        ext.pack = 0;
    }
    d->appletalk = takes(F_APPLETALK);
    char label[256], b1[64], b2[64], on[64], off[64];
    get_string(-4091, label, sizeof label);
    get_string(-4093, b1, sizeof b1);
    get_string(-4092, b2, sizeof b2);
    get_string(-4089, on, sizeof on);
    get_string(-4088, off, sizeof off);
    get_string(-4087, g_ch.radio_label, sizeof g_ch.radio_label);
    qrect rects[5] = { { 112, 206, 132, 266 }, { 112, 296, 132, 356 }, { 114, 272, 130, 332 },
                       { 114, 335, 130, 395 }, { 114, 170, 130, 267 } };
    gaddr nrct = tb_Get1Resource((int32_t)fourcc("nrct"), -4096);
    if (nrct && rd32(nrct)) {
        int n = (int16_t)rd16(rd32(nrct));
        for (int i = 0; i < n && i < 5; i++) {
            qrect r = qrect_get(rd32(nrct) + 2 + 8 * (gaddr)i);
            if (!qrect_empty(r))
                rects[i] = r;
        }
    }
    bool has_ldef = tb_Get1Resource((int32_t)fourcc("LDEF"), -4096) != 0;
    uint8_t gnrl[2] = { 8, 3 };
    gaddr g = tb_Get1Resource((int32_t)fourcc("GNRL"), -4096);
    if (g && rd32(g) && tb_GetHandleSize(g) >= 2)
        gget(gnrl, rd32(g), 2);
    char nbp_type[40];
    get_string(-4096, nbp_type, sizeof nbp_type);
    tb_UseResFile(cur);

    chooser_set_label(label);
    chooser_layout_controls(rects, b1, b2, on, off, takes(F_RADIOS));
    chooser_show_button(1, takes(F_LEFT));
    chooser_show_button(2, takes(F_RIGHT));
    chooser_new_list(takes(F_FILL) || ext.serial ? 1 : 2);
    if (has_ldef && g_ch.list) {
        /* its own list definition: the list made again with it */
        tb_LDispose(g_ch.list);
        int16_t c2 = tb_CurResFile();
        tb_UseResFile(ext.rn);
        qrect view = { 27, 171, 107, 386 };
        qrect bounds = { 0, 0, 0, (int16_t)(takes(F_FILL) ? 1 : 2) };
        g_ch.list = tb_LNew(ch_rect(view), ch_rect(bounds), make_pt(0, (int16_t)(view.right - view.left)), -4096,
                            g_ch.window, 1, 0, 0, 1);
        tb_UseResFile(c2);
    }
    if (g_ch.list && takes(F_MULTIPLE))
        wr8(rd32(g_ch.list) + ListRec_selFlags_off, 0);
    g_ch.button1_needs_sel = true;

    ext.ctls = tb_NewPtr(18, false, true);
    wr16(ext.ctls, 18);
    wr32(ext.ctls + 2, g_ch.button1);
    wr32(ext.ctls + 6, g_ch.button2);
    wr32(ext.ctls + 10, g_ch.radio_on);
    wr32(ext.ctls + 14, g_ch.radio_off);
    ext.zone = tb_NewPtr(34, false, true);
    ext.name = tb_NewPtr(34, false, true);
    char zone[33] = "*";
    if (takes(F_ZONES))
        at_my_zone(zone);
    if (g_ch.zone[0])
        snprintf(zone, sizeof zone, "%s", g_ch.zone);
    wr8(ext.zone, (uint8_t)strlen(zone));
    gput(ext.zone + 1, zone, (uint32_t)strlen(zone));

    if (!ext.pack) {
        if (!label[0])
            chooser_set_label("");
    }
    if (takes(F_INIT))
        call(chooserInitMsg, ext.ctls, g_ch.list, 0);
    if (ext.serial) {
        for (int i = 0; i < 2; i++)
            chooser_add_row(ports[i], NULL, 0);
        chooser_select_row(1, true); /* the printer port */
    } else if (takes(F_FILL)) {
        call(fillListMsg, 0, g_ch.list, 0);
        chooser_ext_get_selection();
    } else if (takes(F_APPLETALK)) {
        if (appletalk_enabled() && nbp_type[0])
            chooser_start_lookup(nbp_type);
        else if (!appletalk_enabled())
            chooser_set_label("AppleTalk is inactive.");
        (void)gnrl;
    }
    chooser_update_buttons();
}

void chooser_ext_get_selection(void)
{
    if (ext.d && takes(F_GETSEL) && !ext.serial)
        call(getSelMsg, 0, g_ch.list, 0);
}

/* A row's name and its p2: the row, or its address (column 1). */
static uint32_t row_param(int row)
{
    char name[40];
    chooser_cell_text(row, 0, name, sizeof name);
    wr8(ext.name, (uint8_t)strlen(name));
    gput(ext.name + 1, name, (uint32_t)strlen(name));
    if (takes(F_FILL))
        return (uint32_t)row;
    char addr[8] = { 0 };
    chooser_cell_text(row, 1, addr, sizeof addr);
    return (uint32_t)(uint8_t)addr[0] << 24 | (uint32_t)(uint8_t)addr[1] << 16 | (uint32_t)(uint8_t)addr[2] << 8 |
           (uint8_t)addr[3];
}

void chooser_ext_selection_changed(int was, int now)
{
    if (!ext.d)
        return;
    if (takes(F_MULTIPLE)) {
        if (takes(F_NEWSEL))
            call(newSelMsg, 0, g_ch.list, 0);
    } else if (was != now) {
        if (was >= 0 && takes(F_DESELECT)) {
            uint32_t p2 = row_param(was);
            call(deselectMsg, takes(F_FILL) ? 0 : ext.name, g_ch.list, p2);
        }
        if (now >= 0 && takes(F_SELECT)) {
            uint32_t p2 = row_param(now);
            call(selectMsg, takes(F_FILL) ? 0 : ext.name, g_ch.list, p2);
        }
    }
    /* A printer chosen: the driver programs see named. */
    if (now >= 0 && (ext.d->type == type_pres || ext.d->type == type_prer)) {
        char spec[200], utf8[128];
        macroman_to_utf8((const uint8_t *)ext.d->name, strlen(ext.d->name), utf8, sizeof utf8);
        snprintf(spec, sizeof spec, "driver:%s", utf8);
        chooser_set_printer(spec);
    }
}

void chooser_ext_button(int which, int16_t modifiers)
{
    if (ext.d)
        call(buttonMsg, ext.ctls, g_ch.list, (uint32_t)(uint16_t)modifiers << 16 | (uint32_t)which);
}

void chooser_ext_click(uint32_t where, int16_t modifiers)
{
    (void)where;
    (void)modifiers;
}

void chooser_ext_draw(void) {}

void chooser_ext_run(void) {}

/* Another device, or the window closed: terminateMsg, its file written
 * and closed. */
void chooser_ext_terminate(void)
{
    if (!ext.d)
        return;
    if (takes(F_TERMINATE))
        call(terminateMsg, 0, g_ch.list, 0);
    if (ext.rn >= 0) {
        tb_UpdateResFile(ext.rn);
        tb_CloseResFile(ext.rn);
    }
    if (ext.ctls)
        tb_DisposePtr(ext.ctls);
    if (ext.zone)
        tb_DisposePtr(ext.zone);
    if (ext.name)
        tb_DisposePtr(ext.name);
    gaddr ctls[4] = { g_ch.button1, g_ch.button2, g_ch.radio_on, g_ch.radio_off };
    for (int i = 0; i < 4; i++)
        if (ctls[i])
            tb_HideControl(ctls[i]);
    g_ch.show_radios = false;
    memset(&ext, 0, sizeof ext);
    ext.rn = -1;
}

/* The icons copied out of the extensions, when the window closes. */
void chooser_ext_forget(void)
{
    for (int i = 0; i < g_ch.ndevices; i++)
        if (g_ch.devices[i].suite) {
            tb_DisposeIconSuite(g_ch.devices[i].suite, 1);
            g_ch.devices[i].suite = 0;
        }
}

void chooser_ext_reset(void)
{
    memset(&ext, 0, sizeof ext);
    ext.rn = -1;
}
