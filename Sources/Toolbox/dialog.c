/*
 * Dialog Manager.
 *
 * Alerts are shown by the host as native alerts, built from the ALRT and
 * DITL resources with ParamText substituted.  Without a host the default
 * button is chosen (and the message logged).
 */
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "tb_internal.h"

enum { itemButton = 4, itemStatText = 8, itemDisabled = 0x80 };

static gaddr resume_proc;
static machine_alert_fn alert_fn;
static void *alert_ctx;

void machine_set_alert_handler(machine_alert_fn fn, void *ctx)
{
    alert_fn = fn;
    alert_ctx = ctx;
}

void tb_InitDialogs(gaddr rp)
{
    resume_proc = rp;
    wr16(LM_ANumber, 0);
    wr16(LM_ACount, (uint16_t)-1);
    wr16(LM_DlgFont, 0);
}

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
static size_t append_text(char *out, size_t n, size_t cap, const uint8_t *s, size_t len)
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
    if (stage & 3)
        tb_SysBeep(30);
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

int16_t tb_Alert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_PLAIN); }
int16_t tb_StopAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_STOP); }
int16_t tb_NoteAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_NOTE); }
int16_t tb_CautionAlert(int16_t id, gaddr fp) { return run_alert(id, MACHINE_ALERT_CAUTION); }
void tb_ErrorSound(gaddr sp) {}
int16_t tb_GetAlertStage(void) { return (int16_t)rd16(LM_ACount); }
void tb_ResetAlertStage(void) { wr16(LM_ACount, (uint16_t)-1); }

/* Loading dialog and alert resources ahead of time: nothing to do now. */
void tb_CouldDialog(int16_t id) {}
void tb_FreeDialog(int16_t id) {}
void tb_CouldAlert(int16_t id) {}
void tb_FreeAlert(int16_t id) {}
