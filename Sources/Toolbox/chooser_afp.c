/*
 * The Chooser's AppleShare: connecting to a file server it found by NBP,
 * or one by address (AFP over TCP), as AppleShare's Chooser extension did:
 * "Connect to the file server ... as" a guest or a registered user (the
 * Chooser's user name filled in), then the server's volumes to choose
 * from, each mounted as a volume of its own through our AFP client
 * (appleshare.c).  And the Chooser's small modal dialogs (a message, a
 * question).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afp.h"
#include "chooser.h"
#include "tb_internal.h"
#include "trap.h"

enum { keyDown = 3, autoKey = 5 };
enum { ctrlItem = 4, btnCtrl = 0, chkCtrl = 1, radCtrl = 2, statText = 8, editText = 16, itemDisable = 128 };

typedef struct {
    int16_t top, left, bottom, right;
    uint8_t type;
    const char *text; /* MacRoman */
} ch_item;

static gaddr make_items(const ch_item *items, int n)
{
    uint32_t size = 2;
    for (int i = 0; i < n; i++)
        size += 14 + (uint32_t)((strlen(items[i].text) + 1) & ~1u);
    gaddr h = tb_NewHandle((int32_t)size, false, true);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    wr16(p, (uint16_t)(n - 1));
    p += 2;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(items[i].text);
        wr32(p, 0);
        qrect_put(p + 4, (qrect){ items[i].top, items[i].left, items[i].bottom, items[i].right });
        wr8(p + 12, items[i].type);
        wr8(p + 13, (uint8_t)len);
        gput(p + 14, items[i].text, (uint32_t)len);
        p += 14 + (uint32_t)((len + 1) & ~1u);
    }
    return h;
}

/* The password typed (shown as bullets) and the field it is typed in. */
static char secret[64];
static int secret_item;
static int cancel_item;

/* pascal Boolean filter(DialogPtr, EventRecord *, short *item): Return
 * and Enter are the default button, Escape and Command-period cancel; the
 * password field keeps what is typed and shows bullets. */
static void filter_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    gaddr itemp = cpu_pop32(c), evt = cpu_pop32(c), d = cpu_pop32(c);
    bool took = false;
    int what = (int16_t)rd16(evt);
    if (what == keyDown || what == autoKey) {
        uint8_t ch = (uint8_t)rd32(evt + EventRecord_message_off);
        bool cmd = rd16(evt + EventRecord_modifiers_off) & 0x0100;
        if (ch == 27 || (cmd && ch == '.')) {
            if (cancel_item) {
                wr16(itemp, (uint16_t)cancel_item);
                took = true;
            }
        } else if (!cmd && secret_item && (int16_t)rd16(d + DialogRecord_editField_off) + 1 == secret_item &&
                   ch != 13 && ch != 3 && ch != 9) {
            gaddr te = rd32(d + DialogRecord_textH_off);
            int len = (int16_t)rd16(rd32(te) + TERec_teLength_off);
            size_t n = strlen(secret);
            if (ch == 8) { /* delete: the whole password, as its field can't show which */
                secret[0] = 0;
                tb_TESetSelect(0, len, te);
            } else if (ch >= 32 && n < sizeof secret - 1 && n < 8) { /* AFP's cleartext passwords: 8 */
                secret[n] = (char)ch;
                secret[n + 1] = 0;
                tb_TESetSelect(len, len, te);
                wr32(evt + EventRecord_message_off, (rd32(evt + EventRecord_message_off) & ~0xFFu) | 0xA5);
            } else {
                took = true; /* (arrows and the rest: nothing) */
                wr16(itemp, 0);
            }
        }
        if (!took && (ch == 13 || ch == 3)) {
            wr16(itemp, (uint16_t)rd16(d + DialogRecord_aDefItem_off));
            took = true;
        }
    }
    wr16(c->a[7], took ? 0x0100 : 0);
    cpu_push32(c, ret);
}

static gaddr filter_stub;

static gaddr new_dialog(const ch_item *items, int n, int width, int height)
{
    gaddr ditl = make_items(items, n);
    int top = (g_screen_height - height) / 3, left = (g_screen_width - width) / 2;
    qrect r = { (int16_t)top, (int16_t)left, (int16_t)(top + height), (int16_t)(left + width) };
    gaddr d = tb_NewDialog(0, ch_rect(r), ch_pstr(""), 1, 1 /* dBoxProc */, 0xFFFFFFFFu, 0, 0, ditl);
    if (d)
        tb_SetPort(d);
    if (!filter_stub)
        filter_stub = native_register(filter_native, "Chooser dialog filter", 0);
    return d;
}

static gaddr item_handle(gaddr d, int item)
{
    gaddr t = ch_rect((qrect){ 0, 0, 0, 0 }), h = t + 2, r = ch_rect((qrect){ 0, 0, 0, 0 });
    tb_GetDialogItem(d, (int16_t)item, t, h, r);
    return rd32(h);
}

static void set_text(gaddr d, int item, const char *mac) { tb_SetDialogItemText(item_handle(d, item), ch_pstr(mac)); }

static void get_text(gaddr d, int item, char *out, size_t size)
{
    gaddr s = ch_pstr("");
    tb_GetDialogItemText(item_handle(d, item), s);
    int n = rd8(s);
    if ((size_t)n >= size)
        n = (int)size - 1;
    gget(out, s + 1, (uint32_t)n);
    out[n] = 0;
}

/* The default button's outline. */
static void outline(gaddr d, int item)
{
    gaddr t = ch_rect((qrect){ 0, 0, 0, 0 }), h = t + 2, rp = ch_rect((qrect){ 0, 0, 0, 0 });
    tb_GetDialogItem(d, (int16_t)item, t, h, rp);
    qrect r = qrect_get(rp);
    r.top -= 4, r.left -= 4, r.bottom += 4, r.right += 4;
    tb_PenSize(3, 3);
    tb_FrameRoundRect(ch_rect(r), 16, 16);
    tb_PenNormal();
}

static int run_dialog(gaddr d, int default_item, int cancel)
{
    wr16(d + DialogRecord_aDefItem_off, (uint16_t)default_item);
    cancel_item = cancel;
    tb_DrawDialog(d);
    outline(d, default_item);
    gaddr itemp = ch_rect((qrect){ 0, 0, 0, 0 });
    wr16(itemp, 0);
    tb_ModalDialog(filter_stub, itemp);
    return (int16_t)rd16(itemp);
}

static void end_dialog(gaddr d, gaddr port)
{
    tb_DisposeDialog(d);
    tb_SetPort(port);
}

void chooser_message(const char *mac_text)
{
    static const ch_item items[] = {
        { 92, 260, 112, 320, ctrlItem + btnCtrl, "OK" },
        { 10, 60, 82, 320, statText | itemDisable, "" },
        { 10, 14, 42, 46, 32 /* iconItem */ | itemDisable, "" },
    };
    gaddr port = qd_port();
    gaddr d = new_dialog(items, 2, 336, 124);
    if (!d)
        return;
    set_text(d, 2, mac_text);
    for (;;) {
        int hit = run_dialog(d, 1, 1);
        if (hit == 1 || g_cpu->halted)
            break;
    }
    end_dialog(d, port);
}

bool chooser_confirm(const char *mac_text, const char *ok)
{
    ch_item items[] = {
        { 92, 260, 112, 320, ctrlItem + btnCtrl, ok },
        { 92, 180, 112, 240, ctrlItem + btnCtrl, "Cancel" },
        { 10, 20, 82, 320, statText | itemDisable, "" },
    };
    gaddr port = qd_port();
    gaddr d = new_dialog(items, 3, 336, 124);
    if (!d)
        return false;
    set_text(d, 3, mac_text);
    int hit;
    do
        hit = run_dialog(d, 1, 2);
    while (hit != 1 && hit != 2 && !g_cpu->halted);
    end_dialog(d, port);
    return hit == 1;
}

/* "Connect to the file server ... as": a guest, or a registered user with
 * a name and password.  False if cancelled. */
static bool login(const char *server, bool guest_allowed, char *user, size_t usize, char *password, size_t psize)
{
    char prompt[160];
    snprintf(prompt, sizeof prompt, "Connect to the file server \xD2%s\xD3 as:", server);
    ch_item items[] = {
        { 178, 266, 198, 336, ctrlItem + btnCtrl, "Connect" },
        { 178, 180, 198, 250, ctrlItem + btnCtrl, "Cancel" },
        { 12, 20, 30, 340, statText | itemDisable, prompt },
        { 40, 60, 56, 200, ctrlItem + radCtrl, "Guest" },
        { 58, 60, 74, 240, ctrlItem + radCtrl, "Registered User" },
        { 88, 60, 104, 140, statText | itemDisable, "Name:" },
        { 88, 144, 104, 330, editText, "" },
        { 118, 60, 134, 140, statText | itemDisable, "Password:" },
        { 118, 144, 134, 330, editText, "" },
        { 140, 144, 156, 330, statText | itemDisable, "(Clear text)" },
    };
    gaddr port = qd_port();
    gaddr d = new_dialog(items, 10, 356, 212);
    if (!d)
        return false;
    set_text(d, 7, g_chooser.user);
    bool guest = !g_chooser.user[0] && guest_allowed;
    tb_SetControlValue(item_handle(d, 4), guest);
    tb_SetControlValue(item_handle(d, 5), !guest);
    if (!guest_allowed)
        tb_HiliteControl(item_handle(d, 4), 255);
    tb_SelectDialogItemText(d, guest ? 7 : 9, 0, 32767);
    secret[0] = 0;
    secret_item = 9;
    int hit;
    for (;;) {
        hit = run_dialog(d, 1, 2);
        if (hit == 1 || hit == 2 || g_cpu->halted)
            break;
        if (hit == 4 || hit == 5) {
            guest = hit == 4;
            tb_SetControlValue(item_handle(d, 4), guest);
            tb_SetControlValue(item_handle(d, 5), !guest);
        }
    }
    if (hit == 1) {
        if (guest) {
            user[0] = password[0] = 0;
        } else {
            get_text(d, 7, user, usize);
            snprintf(password, psize, "%s", secret);
        }
    }
    secret_item = 0;
    end_dialog(d, port);
    return hit == 1;
}

/* The volumes to mount, checked in a list of check boxes. */
static int choose_volumes(const char *server, char (*names)[28], int count, bool *chosen)
{
    enum { MAX_SHOWN = 10 };
    if (count > MAX_SHOWN)
        count = MAX_SHOWN;
    char prompt[160];
    snprintf(prompt, sizeof prompt, "%s", server);
    ch_item items[3 + MAX_SHOWN];
    items[0] = (ch_item){ 0, 266, 0, 336, ctrlItem + btnCtrl, "OK" };
    items[1] = (ch_item){ 0, 180, 0, 250, ctrlItem + btnCtrl, "Cancel" };
    items[2] = (ch_item){ 12, 20, 46, 340, statText | itemDisable, "" };
    for (int i = 0; i < count; i++)
        items[3 + i] = (ch_item){ (int16_t)(52 + 20 * i), 40, (int16_t)(68 + 20 * i), 330, ctrlItem + chkCtrl, names[i] };
    int bottom = 52 + 20 * count + 16;
    items[0].top = items[1].top = (int16_t)bottom;
    items[0].bottom = items[1].bottom = (int16_t)(bottom + 20);
    gaddr port = qd_port();
    gaddr d = new_dialog(items, 3 + count, 356, bottom + 34);
    if (!d)
        return 0;
    char text[200];
    snprintf(text, sizeof text, "%s\rSelect the items you want to use:", prompt);
    set_text(d, 3, text);
    for (int i = 0; i < count; i++) {
        chosen[i] = i == 0;
        tb_SetControlValue(item_handle(d, 4 + i), chosen[i]);
    }
    int hit;
    for (;;) {
        hit = run_dialog(d, 1, 2);
        if (hit == 1 || hit == 2 || g_cpu->halted)
            break;
        if (hit >= 4 && hit < 4 + count) {
            chosen[hit - 4] = !chosen[hit - 4];
            tb_SetControlValue(item_handle(d, hit), chosen[hit - 4]);
        }
    }
    end_dialog(d, port);
    if (hit != 1)
        return 0;
    int n = 0;
    for (int i = 0; i < count; i++)
        n += chosen[i];
    return n;
}

/* Log in, list the volumes, mount the ones chosen. */
static void connect_server(const char *shown, const char *spec_prefix, bool atalk, const char *where)
{
    char user[64], password[64];
    if (!login(shown, true, user, sizeof user, password, sizeof password))
        return;
    char err[256] = "";
    char uu[128];
    macroman_to_utf8((const uint8_t *)user, strlen(user), uu, sizeof uu);
    afp_session *s = atalk ? afp_connect_atalk(where, uu, password, err, sizeof err)
                           : afp_connect_tcp(where, uu, password, err, sizeof err);
    if (!s) {
        char text[400];
        snprintf(text, sizeof text, "Could not connect to the file server \xD2%s\xD3: %s", shown, err);
        chooser_message(text);
        return;
    }
    char (*names)[28] = NULL;
    int count = 0;
    int r = afp_volumes(s, &names, &count);
    afp_close(s);
    if (r != 0 || count == 0) {
        chooser_message("The file server has no volumes you can use.");
        bb_free(names);
        return;
    }
    bool chosen[16] = { false };
    if (choose_volumes(shown, names, count, chosen)) {
        for (int i = 0; i < count && i < 10; i++) {
            if (!chosen[i])
                continue;
            char vol[64], spec[1200], creds[200] = "";
            macroman_to_utf8((const uint8_t *)names[i], strlen(names[i]), vol, sizeof vol);
            if (uu[0])
                snprintf(creds, sizeof creds, "%s:%s@", uu, password);
            snprintf(spec, sizeof spec, "%s%s%s/%s", spec_prefix, creds, where, vol);
            char merr[256] = "";
            if (!appleshare_mount_now(spec, merr, sizeof merr)) {
                char text[400];
                snprintf(text, sizeof text, "The volume \xD2%s\xD3 could not be used: %s", names[i], merr);
                chooser_message(text);
            }
        }
    }
    bb_free(names);
}

void chooser_afp_connect_found(int row)
{
    char name[40];
    chooser_cell_text(row, 0, name, sizeof name);
    char where[200];
    snprintf(where, sizeof where, "%s:AFPServer@%s", name, g_ch.zone[0] ? g_ch.zone : "*");
    connect_server(name, "atalk:", true, where);
}

/* A server by its address: AFP over TCP. */
void chooser_afp_connect_address(void)
{
    static const ch_item items[] = {
        { 92, 266, 112, 336, ctrlItem + btnCtrl, "Connect" },
        { 92, 180, 112, 250, ctrlItem + btnCtrl, "Cancel" },
        { 12, 20, 46, 340, statText | itemDisable, "Enter the file server's IP address or domain name:" },
        { 56, 24, 72, 330, editText, "" },
    };
    gaddr port = qd_port();
    gaddr d = new_dialog(items, 4, 356, 126);
    if (!d)
        return;
    tb_SelectDialogItemText(d, 4, 0, 32767);
    int hit;
    do
        hit = run_dialog(d, 1, 2);
    while (hit != 1 && hit != 2 && !g_cpu->halted);
    char addr[256] = "";
    if (hit == 1)
        get_text(d, 4, addr, sizeof addr);
    end_dialog(d, port);
    if (hit != 1 || !addr[0])
        return;
    char utf8[300];
    macroman_to_utf8((const uint8_t *)addr, strlen(addr), utf8, sizeof utf8);
    connect_server(addr, "tcp:", false, utf8);
}
