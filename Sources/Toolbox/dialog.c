/*
 * Dialog Manager.
 *
 * Alerts are shown by the host as native alerts, built from the ALRT and
 * DITL resources with ParamText substituted.  Without a host the default
 * button is chosen (and the message logged).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "tb_internal.h"

enum { itemButton = 4, itemStatText = 8, itemDisabled = 0x80 };

static gaddr resume_proc;
static machine_alert_fn alert_fn;
static void *alert_ctx;
static bool depth_alerts;
static int wanted_depth;

void machine_set_depth_alerts(bool on) { depth_alerts = on; }
int machine_wanted_depth(void) { return wanted_depth; }

/* The depths an alert says a program runs at ("requires 256 colors", "set
 * your monitor to 16 colors (4 bit)", "only runs in Black and White, 4
 * color mode ..."), as a set of 1 << depth; 0 when it isn't about depth. */
uint64_t machine_alert_depths(const char *message)
{
    char m[1024];
    size_t n = 0;
    for (const char *p = message; *p && n < sizeof m - 1; p++)
        m[n++] = (char)tolower((unsigned char)*p);
    m[n] = 0;
    static const char *const asks[] = { "requir", "need", "must", "only", "set ", "switch", "at least", "find",
                                        "please", "greater than", "more than", "too ", "depth", NULL };
    bool asking = false;
    for (int i = 0; asks[i]; i++)
        asking |= strstr(m, asks[i]) != NULL;
    if (!asking)
        return 0;
    static const struct { const char *text; int depth; } words[] = {
        { "256", 8 }, { "8-bit", 8 }, { "8 bit", 8 },
        { "16 colo", 4 }, { "16-colo", 4 }, { "16 gr", 4 }, { "16 shade", 4 }, { "4-bit", 4 }, { "4 bit", 4 },
        { "4 colo", 2 }, { "4-colo", 2 }, { "4 gr", 2 }, { "2-bit", 2 }, { "2 bit", 2 },
        { "thousands", 16 }, { "16-bit", 16 }, { "16 bit", 16 },
        { "millions", 32 }, { "24-bit", 32 }, { "24 bit", 32 }, { "32-bit", 32 }, { "32 bit", 32 },
        { "black and white", 1 }, { "black & white", 1 }, { "b&w", 1 }, { "monochrome", 1 },
        { "2-colo", 1 }, { "2 colo", 1 }, { "1-bit", 1 }, { "1 bit", 1 },
        /* in words: Dark Castle's "set the main screen to two colors" */
        { "two colo", 1 }, { "four colo", 2 }, { "sixteen colo", 4 },
    };
    uint64_t set = 0;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        for (const char *p = strstr(m, words[i].text); p; p = strstr(p + 1, words[i].text)) {
            const char *after = p + strlen(words[i].text);
            while (*after == ' ' || *after == '-')
                after++;
            /* "32 Bit QuickDraw", "32-bit addressing": not a depth. */
            if (!strncmp(after, "quickdraw", 9) || !strncmp(after, "address", 7) || !strncmp(after, "clean", 5))
                continue;
            /* "16 colors" is not "6 colors"; "256" not "56". */
            if (p > m && isdigit((unsigned char)p[-1]))
                continue;
            set |= 1ull << words[i].depth;
        }
    }
    return set;
}

/* A program that refuses the screen's depth before anyone has seen it
 * run: when the host allows, the run ends here, without the alert, and
 * the host starts it again at the depth it asked for (the highest of
 * those it names up to 256 colours, as games mostly mean).  Returns
 * whether it did. */
bool depth_alert(const char *message)
{
    if (!depth_alerts)
        return false;
    uint64_t set = machine_alert_depths(message);
    if (!set || (set & (1ull << qd_screen_depth())))
        return false;
    int pick = 0;
    for (int d = 1; d <= 32; d <<= 1) {
        if ((set & (1ull << d)) && (d <= 8 || !(set & 0x1FF)))
            pick = d;
    }
    wanted_depth = pick;
    LOGF(LOG_UNIMPL, "alert asks for depth %d: %s\n", pick, message);
    cpu_halt(g_cpu, "asks for depth %d", pick);
    return true;
}

void machine_set_alert_handler(machine_alert_fn fn, void *ctx)
{
    alert_fn = fn;
    alert_ctx = ctx;
}

/* The standard alert sound procedure, pascal (sound: INTEGER): that many
 * beeps (0 to 3). ErrorSound replaces it in DABeeper. */
static void standard_beeper(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    int16_t n = (int16_t)cpu_pop16(c);
    cpu_push32(c, ret);
    for (int i = 0; i < n && i < 3; i++)
        tb_SysBeep(30);
}

void dialog_reset(void)
{
    wr32(LM_DABeeper, native_register(standard_beeper, "StandardBeeper", 0));
}

void tb_InitDialogs(gaddr rp)
{
    resume_proc = rp;
    wr32(LM_ResumeProc, rp);
    wr16(LM_ANumber, 0);
    wr16(LM_ACount, (uint16_t)-1);
    wr16(LM_DlgFont, 0);
}

/* SetDialogFont: the font new dialogs' text items take. */
void tb_SetDialogFont(int16_t i) { wr16(LM_DlgFont, (uint16_t)i); }

void tb_ParamText(gaddr p0, gaddr p1, gaddr p2, gaddr p3)
{
    gaddr params[4] = { p0, p1, p2, p3 };
    for (int i = 0; i < 4; i++) {
        gaddr slot = LM_DAStrings + (uint32_t)i * 4;
        gaddr h = rd32(slot);
        if (!params[i]) /* NIL leaves the string alone */
            continue;
        if (g_log_mask & LOG_TRAPS) {
            char text[256];
            pstr_to_c(params[i], text, sizeof text);
            log_printf("  ^%d = %s\n", i, text);
        }
        if (h)
            tb_SetString(h, params[i]);
        else
            wr32(slot, tb_NewString(params[i]));
    }
}

/* Append MacRoman text, replacing ^0..^3 with the ParamText strings. */
size_t append_text(char *out, size_t n, size_t cap, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        if (s[i] == '^' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '3') {
            gaddr h = rd32(LM_DAStrings + (uint32_t)(s[i + 1] - '0') * 4);
            if (h && rd32(h)) {
                uint32_t pl = rd8(rd32(h));
                for (uint32_t j = 0; j < pl && n + 1 < cap; j++)
                    out[n++] = (char)rd8(rd32(h) + 1 + j);
            }
            i++;
        } else {
            out[n++] = (char)s[i];
        }
    }
    out[n] = 0;
    return n;
}

/* Fill an alert description from a DITL; returns false if there is none. */
static bool read_items(int16_t ditl_id, machine_alert *a)
{
    gaddr h = tb_GetResource((int32_t)fourcc("DITL"), ditl_id);
    if (!h || !rd32(h))
        return false;
    gaddr p = rd32(h);
    int count = (int16_t)rd16(p) + 1;
    uint32_t off = 2;
    size_t tn = 0;
    for (int item = 1; item <= count; item++) {
        uint8_t type = rd8(p + off + 12) & 0x7F;
        uint8_t len = rd8(p + off + 13);
        gaddr data = p + off + 14;
        uint8_t text[256];
        gget(text, data, len);
        if (type == itemStatText && len) {
            if (tn) {
                tn = append_text(a->message, tn, sizeof a->message, (const uint8_t *)"\r\r", 2);
            }
            tn = append_text(a->message, tn, sizeof a->message, text, len);
        } else if (type == itemButton && a->nbuttons < (int)(sizeof a->buttons / sizeof a->buttons[0])) {
            machine_alert_button *b = &a->buttons[a->nbuttons++];
            b->item = (int16_t)item;
            append_text(b->title, 0, sizeof b->title, text, len);
        }
        off += 14 + ((len + 1u) & ~1u);
    }
    return true;
}

bool dialog_host_alerts(void) { return alert_fn != NULL; }

/* Ask the host to show an alert; false if there is no host to ask. */
bool dialog_host_alert(machine_alert *a, int *result)
{
    if (!alert_fn)
        return false;
    *result = alert_fn(a, alert_ctx);
    return true;
}

static int16_t run_alert(int16_t id, int kind)
{
    gaddr alrt = tb_GetResource((int32_t)fourcc("ALRT"), id);
    if (!alrt || !rd32(alrt))
        return -1;
    int16_t ditl = (int16_t)rd16(rd32(alrt) + 8);
    uint16_t stages = rd16(rd32(alrt) + 10);

    /* Repeated alerts move through the stages (ACount 0..3). */
    int16_t count = (int16_t)rd16(LM_ACount);
    if ((int16_t)rd16(LM_ANumber) != id)
        count = 0;
    else if (count < 3)
        count++;
    wr16(LM_ANumber, (uint16_t)id);
    wr16(LM_ACount, (uint16_t)count);
    unsigned stage = (stages >> (count * 4)) & 0xF;
    int16_t default_item = (stage & 8) ? 2 : 1;
    if ((stage & 3) && rd32(LM_DABeeper)) { /* the stage's sound, through the sound procedure */
        cpu_push16(g_cpu, (uint16_t)(stage & 3));
        guest_call(rd32(LM_DABeeper));
    }
    if (!(stage & 4)) /* no alert box at this stage */
        return -1;

    machine_alert a;
    memset(&a, 0, sizeof a);
    a.kind = kind;
    a.default_item = default_item;
    if (!read_items(ditl, &a))
        return -1;
    for (int i = 0; i < a.nbuttons; i++) {
        if (a.buttons[i].item == default_item)
            a.buttons[i].is_default = true;
    }
    int16_t result = default_item;
    if (depth_alert(a.message))
        return result;
    if (alert_fn) {
        int r = alert_fn(&a, alert_ctx);
        if (r > 0)
            result = (int16_t)r;
    } else {
        LOGF(LOG_UNIMPL, "alert %d: %s\n", id, a.message);
        machine_idle(); /* headless: the user's answer is the wait for input */
    }
    return result;
}

/* StandardAlert (the Appearance Manager's): the error and its explanation,
 * with the buttons an AlertStdAlertParamRec (mac68k layout) asks for, or
 * just OK.  The item hit: 1 OK, 2 Cancel, 3 the other button. */
int16_t dialog_standard_alert(int16_t type, gaddr error, gaddr explanation, gaddr param, gaddr item_hit)
{
    static const int kinds[] = { MACHINE_ALERT_STOP, MACHINE_ALERT_NOTE, MACHINE_ALERT_CAUTION, MACHINE_ALERT_PLAIN };
    machine_alert a;
    memset(&a, 0, sizeof a);
    a.kind = type >= 0 && type <= 3 ? kinds[type] : MACHINE_ALERT_PLAIN;
    size_t n = 0;
    if (error && rd8(error)) {
        uint8_t t[256];
        gget(t, error + 1, rd8(error));
        n = append_text(a.message, n, sizeof a.message, t, rd8(error));
    }
    if (explanation && rd8(explanation)) {
        uint8_t t[256];
        gget(t, explanation + 1, rd8(explanation));
        if (n)
            n = append_text(a.message, n, sizeof a.message, (const uint8_t *)"\r\r", 2);
        n = append_text(a.message, n, sizeof a.message, t, rd8(explanation));
    }
    static const char *const standard[] = { "OK", "Cancel", "Don\xD5t Save" }; /* MacRoman apostrophe */
    int16_t default_item = 1;
    for (int k = 0; k < 3; k++) {
        gaddr title = k == 0 ? 0xFFFFFFFFu : 0;
        if (param)
            title = rd32(param + 6 + (uint32_t)k * 4); /* defaultText, cancelText, otherText */
        if (!title)
            continue;
        machine_alert_button *b = &a.buttons[a.nbuttons++];
        b->item = (int16_t)(k + 1);
        if (title == 0xFFFFFFFFu) {
            append_text(b->title, 0, sizeof b->title, (const uint8_t *)standard[k], strlen(standard[k]));
        } else {
            uint8_t t[256];
            gget(t, title + 1, rd8(title));
            append_text(b->title, 0, sizeof b->title, t, rd8(title));
        }
    }
    if (!a.nbuttons) { /* none asked for: OK */
        a.buttons[a.nbuttons].item = 1;
        append_text(a.buttons[a.nbuttons++].title, 0, sizeof a.buttons[0].title, (const uint8_t *)"OK", 2);
    }
    if (param && rd16(param + 18))
        default_item = (int16_t)rd16(param + 18);
    a.default_item = default_item;
    for (int i = 0; i < a.nbuttons; i++)
        a.buttons[i].is_default = a.buttons[i].item == default_item;
    int16_t result = default_item;
    if (alert_fn) {
        int r = alert_fn(&a, alert_ctx);
        if (r > 0)
            result = (int16_t)r;
    } else {
        LOGF(LOG_UNIMPL, "alert standard: %s\n", a.message);
        machine_idle();
    }
    if (item_hit)
        wr16(item_hit, (uint16_t)result);
    return noErr;
}

int16_t tb_Alert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_PLAIN); }
int16_t tb_StopAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_STOP); }
int16_t tb_NoteAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_NOTE); }
int16_t tb_CautionAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_CAUTION); }
void tb_ErrorSound(gaddr sp) { wr32(LM_DABeeper, sp); }
int16_t tb_GetAlertStage(void) { return (int16_t)rd16(LM_ACount); }
void tb_ResetAlertStage(void) { wr16(LM_ACount, (uint16_t)-1); }

/* Loading dialog and alert resources ahead of time: nothing to do now. */
void tb_CouldDialog(int16_t id) {}
void tb_FreeDialog(int16_t id) {}
void tb_CouldAlert(int16_t id) {}
void tb_FreeAlert(int16_t id) {}
