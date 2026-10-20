/*
 * The Communications Toolbox: _CommToolboxDispatch (A08B), its routines
 * exported to PowerPC programs as InterfaceLib has them, the calls all
 * three managers share (reference constants, tool names, configurations,
 * versions, Choose), the utilities and the Communications Resource
 * Manager.  The managers are ctb_cm.c (connections), ctb_tm.c (terminals)
 * and ctb_ft.c (file transfers), each with tools of our own.
 *
 * The glue pushes the selector after the arguments and points A0 at it:
 * the last argument comes first after the selector.  The result goes back
 * in D0, which the glue stores in the function result.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctb.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "qd_internal.h"

enum { chooseFailed = -1, chooseOKMinor = 1, chooseOKMajor = 2, chooseCancel = 3 };
enum { nlCancel = 1 };

typedef struct {
    const char *name;
    uint16_t sel;
    const char *args;
    char res;
    ctb_fn fn;
} ctb_routine;

#define CTB_ROW(name, sel, args, res, fn, ppc) { #name, sel, args, res, fn },
static const ctb_routine routines[] = { CTB_ROUTINES(CTB_ROW) };
#undef CTB_ROW

static const ctb_routine *routine_for(uint16_t sel)
{
    for (size_t i = 0; i < sizeof routines / sizeof routines[0]; i++)
        if (routines[i].sel == sel)
            return &routines[i];
    return NULL;
}

static uint32_t result_of(char res, uint32_t r)
{
    switch (res) {
    case 'w': return (uint32_t)(int32_t)(int16_t)r;
    case 'b': return r ? 1 : 0;
    case 'v': return 0;
    default: return r;
    }
}

uint32_t ctb_call(uint16_t sel, const uint32_t *args, int nargs)
{
    const ctb_routine *r = routine_for(sel);
    if (!r)
        return 0;
    uint32_t a[16] = { 0 };
    int n = (int)strlen(r->args);
    for (int i = 0; i < n && i < nargs; i++)
        a[i] = r->args[i] == 'w' ? (uint32_t)(int32_t)(int16_t)args[i] : r->args[i] == 'b' ? args[i] != 0 : args[i];
    return result_of(r->res, r->fn(sel, a));
}

static void ctb_dispatch(cpu68k *c, uint16_t tw)
{
    gaddr p = c->a[0];
    uint16_t sel = rd16(p);
    const ctb_routine *r = routine_for(sel);
    if (!r) {
        LOGF(LOG_UNIMPL, "CommToolboxDispatch selector %04X\n", sel);
        c->d[0] = 0;
        return;
    }
    uint32_t a[16];
    int n = (int)strlen(r->args);
    gaddr at = p + 2; /* the last argument */
    for (int i = n - 1; i >= 0; i--) {
        switch (r->args[i]) {
        case 'b': a[i] = rd8(at) != 0; at += 2; break;
        case 'w': a[i] = (uint32_t)(int32_t)(int16_t)rd16(at); at += 2; break;
        default: a[i] = rd32(at); at += 4; break;
        }
    }
    uint32_t v = result_of(r->res, r->fn(sel, a));
    /* A Boolean the glue stores as a word (TMDoTermKey) reads its high byte. */
    c->d[0] = r->res == 'b' && v ? 0x0101 : v;
}

/* ---------------------------------------------------------------------- */
/* PowerPC: InterfaceLib's exports */

static void ppc_ctb(ppc_cpu *c, uint16_t sel)
{
    const ctb_routine *r = routine_for(sel);
    uint32_t a[16];
    int slot = r->res == 'p' ? 1 : 0; /* a Point result: its address first */
    gaddr result = c->gpr[3];
    for (int i = 0; r->args[i]; i++, slot++) {
        uint32_t v = slot < 8 ? c->gpr[3 + slot] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)slot);
        a[i] = r->args[i] == 'w' ? (uint32_t)(int32_t)(int16_t)v : r->args[i] == 'b' ? (v & 0xFF) != 0 : v;
    }
    uint32_t v = result_of(r->res, r->fn(sel, a));
    if (r->res == 'p')
        ppc_struct_result(c, result, v, 4);
    else
        c->gpr[3] = v;
    c->pc = c->lr;
}

#define CTB_GLUE(name, sel, args, res, fn, ppc) static void ppc_##name(ppc_cpu *c) { ppc_ctb(c, sel); }
CTB_ROUTINES(CTB_GLUE)
#undef CTB_GLUE

#define CTB_EXPORT(name, sel, args, res, fn, ppc) { (ppc) ? #name : "", ppc_##name },
const ppc_export ctb_exports[] = { CTB_ROUTINES(CTB_EXPORT) };
#undef CTB_EXPORT
const unsigned ctb_export_count = sizeof ctb_exports / sizeof ctb_exports[0];

/* ---------------------------------------------------------------------- */
/* Helpers */

uint64_t ctb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t ctb_deadline(int32_t ticks)
{
    if (ticks < 0)
        return UINT64_MAX;
    return ctb_now_ms() + (uint64_t)ticks * 50 / 3;
}

uint32_t ctb_callback(gaddr proc, uint32_t proc_info, const uint32_t *args, int nargs)
{
    return proc ? mm_call(proc, proc_info, args, nargs) : 0;
}

static const ctb_manager *manager_of(int mgr)
{
    switch (mgr) {
    case MGR_CM: return &cm_manager;
    case MGR_TM: return &tm_manager;
    case MGR_FT: return &ft_manager;
    default: return NULL;
    }
}

const ctb_tool *ctb_tool_of(int mgr, int16_t proc_id)
{
    const ctb_manager *m = manager_of(mgr);
    return m && proc_id >= 1 && proc_id <= m->ntools ? m->tools[proc_id - 1] : NULL;
}

static const ctb_tool *tool_of_record(int mgr, gaddr h)
{
    return h && rd32(h) ? ctb_tool_of(mgr, (int16_t)rd16(rd32(h) + REC_procID)) : NULL;
}

/* A Pascal string argument as a C string. */
static void cstr(gaddr p, char *out, size_t cap)
{
    size_t n = p ? rd8(p) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        gget(out, p + 1, (uint32_t)n);
    out[n] = 0;
}

static void put_pstr(gaddr p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* Configurations */

static const ctb_field *field_named(const ctb_tool *t, const char *key)
{
    for (int i = 0; i < t->nfields; i++)
        if (!strcasecmp(t->fields[i].key, key))
            return &t->fields[i];
    return NULL;
}

/* One field from text; false if the value isn't one it takes. */
static bool set_field(const ctb_field *f, gaddr block, const char *v)
{
    switch (f->type) {
    case CF_INT: {
        char *end;
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < f->lo || n > f->hi)
            return false;
        wr32(block + f->off, (uint32_t)n);
        return true;
    }
    case CF_BOOL:
        if (!strcasecmp(v, "True") || !strcasecmp(v, "Yes") || !strcmp(v, "1"))
            wr8(block + f->off, 1);
        else if (!strcasecmp(v, "False") || !strcasecmp(v, "No") || !strcmp(v, "0"))
            wr8(block + f->off, 0);
        else
            return false;
        return true;
    case CF_ENUM:
        for (int i = 0; f->names[i]; i++)
            if (!strcasecmp(f->names[i], v)) {
                wr16(block + f->off, (uint16_t)i);
                return true;
            }
        return false;
    case CF_STR: {
        size_t n = strlen(v);
        if ((int32_t)n > f->hi)
            return false;
        wr8(block + f->off, (uint8_t)n);
        gput(block + f->off + 1, v, (uint32_t)n);
        return true;
    }
    }
    return false;
}

void ctb_config_defaults(const ctb_tool *t, gaddr block)
{
    gmemset(block, 0, t->size);
    wr32(block, t->tag);
    for (int i = 0; i < t->nfields; i++)
        set_field(&t->fields[i], block, t->fields[i].def);
}

gaddr ctb_config_new(const ctb_tool *t)
{
    gaddr p = tb_NewPtr(t->size, false, true);
    if (p)
        ctb_config_defaults(t, p);
    return p;
}

/* Whether a block holds settings the tool can use. */
static bool config_valid(const ctb_tool *t, gaddr block)
{
    if (!block || rd32(block) != t->tag)
        return false;
    for (int i = 0; i < t->nfields; i++) {
        const ctb_field *f = &t->fields[i];
        gaddr p = block + f->off;
        switch (f->type) {
        case CF_INT:
            if ((int32_t)rd32(p) < f->lo || (int32_t)rd32(p) > f->hi)
                return false;
            break;
        case CF_BOOL:
            if (rd8(p) > 1)
                return false;
            break;
        case CF_ENUM: {
            int n = 0;
            while (f->names[n])
                n++;
            if (rd16(p) >= n)
                return false;
            break;
        }
        case CF_STR:
            if (rd8(p) > f->hi)
                return false;
            break;
        }
    }
    return true;
}

/* Words, or "quoted strings" (with \" inside), separated by blanks. */
static const char *next_token(const char *s, char *out, size_t cap)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1])
                s++;
            if (n < cap - 1)
                out[n++] = *s;
            s++;
        }
        if (*s == '"')
            s++;
    } else {
        while (*s && !isspace((unsigned char)*s)) {
            if (n < cap - 1)
                out[n++] = *s;
            s++;
        }
    }
    out[n] = 0;
    return s;
}

int16_t ctb_config_parse(const ctb_tool *t, gaddr block, const char *s)
{
    const char *start = s;
    char key[64], val[256];
    for (;;) {
        while (isspace((unsigned char)*s))
            s++;
        if (!*s)
            return 0;
        const char *at = s;
        s = next_token(s, key, sizeof key);
        const ctb_field *f = field_named(t, key);
        if (!f)
            return (int16_t)(at - start + 1);
        while (isspace((unsigned char)*s))
            s++;
        if (!*s)
            return (int16_t)(at - start + 1);
        s = next_token(s, val, sizeof val);
        if (!set_field(f, block, val))
            return (int16_t)(at - start + 1);
    }
}

void ctb_config_str(gaddr block, uint16_t off, char *out, size_t cap) { cstr(block + off, out, cap); }

void ctb_config_string(const ctb_tool *t, gaddr block, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < t->nfields && n < cap; i++) {
        const ctb_field *f = &t->fields[i];
        gaddr p = block + f->off;
        char v[300];
        switch (f->type) {
        case CF_INT: snprintf(v, sizeof v, "%d", (int32_t)rd32(p)); break;
        case CF_BOOL: snprintf(v, sizeof v, "%s", rd8(p) ? "True" : "False"); break;
        case CF_ENUM: snprintf(v, sizeof v, "%s", f->names[rd16(p)]); break;
        case CF_STR: {
            char s[256];
            cstr(p, s, sizeof s);
            size_t k = 0;
            v[k++] = '"';
            for (char *q = s; *q && k < sizeof v - 3; q++) {
                if (*q == '"' || *q == '\\')
                    v[k++] = '\\';
                v[k++] = *q;
            }
            v[k++] = '"';
            v[k] = 0;
            break;
        }
        }
        n += (size_t)snprintf(out + n, cap - n, "%s%s %s", i ? " " : "", f->key, v);
    }
}

/* The records' defProc: our tools are native, so a definition procedure
 * called directly (hConn, msg, p1, p2, p3) does nothing. */
static gaddr def_stub;

static void def_native(cpu68k *c, uint16_t tw)
{
    uint32_t ret = cpu_pop32(c);
    c->a[7] += 18;
    wr32(c->a[7], 0);
    cpu_push32(c, ret);
}

void ctb_record_init(gaddr h, int mgr, int16_t proc_id, uint32_t flags, int32_t refcon, int32_t user_data)
{
    const ctb_tool *t = ctb_tool_of(mgr, proc_id);
    gaddr cfg = ctb_config_new(t), old = ctb_config_new(t);
    gaddr r = rd32(h);
    wr16(r + REC_procID, (uint16_t)proc_id);
    wr32(r + REC_flags, flags);
    wr32(r + REC_refCon, (uint32_t)refcon);
    wr32(r + REC_userData, (uint32_t)user_data);
    wr32(r + REC_config, cfg);
    wr32(r + REC_oldConfig, old);
    wr32(r + REC_defProc, def_stub);
}

void ctb_record_free(gaddr h)
{
    gaddr r = rd32(h);
    if (rd32(r + REC_config))
        tb_DisposePtr(rd32(r + REC_config));
    if (rd32(r + REC_oldConfig))
        tb_DisposePtr(rd32(r + REC_oldConfig));
    tb_DisposeHandle(h);
}

/* ---------------------------------------------------------------------- */
/* The calls every manager has */

uint32_t ctb_nothing(uint16_t sel, const uint32_t *a) { return 0; }
uint32_t ctb_zero(uint16_t sel, const uint32_t *a) { return 0; }

uint32_t ctb_set_refcon(uint16_t sel, const uint32_t *a)
{
    if (a[0] && rd32(a[0]))
        wr32(rd32(a[0]) + REC_refCon, a[1]);
    return 0;
}

uint32_t ctb_get_refcon(uint16_t sel, const uint32_t *a) { return a[0] && rd32(a[0]) ? rd32(rd32(a[0]) + REC_refCon) : 0; }

uint32_t ctb_set_userdata(uint16_t sel, const uint32_t *a)
{
    if (a[0] && rd32(a[0]))
        wr32(rd32(a[0]) + REC_userData, a[1]);
    return 0;
}

uint32_t ctb_get_userdata(uint16_t sel, const uint32_t *a)
{
    return a[0] && rd32(a[0]) ? rd32(rd32(a[0]) + REC_userData) : 0;
}

/* (procID, name) */
uint32_t ctb_get_tool_name(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = ctb_tool_of(ctb_mgr(sel), (int16_t)a[0]);
    if (a[1])
        put_pstr(a[1], t ? t->name : "");
    return 0;
}

/* (name): -1 for a tool we haven't */
uint32_t ctb_get_proc_id(uint16_t sel, const uint32_t *a)
{
    const ctb_manager *m = manager_of(ctb_mgr(sel));
    char name[256];
    cstr(a[0], name, sizeof name);
    for (int i = 0; m && i < m->ntools; i++)
        if (!strcasecmp(m->tools[i]->name, name))
            return (uint32_t)(i + 1);
    return (uint32_t)-1;
}

/* (hConn): true when the configuration was bad and is now the defaults */
uint32_t ctb_validate(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = tool_of_record(ctb_mgr(sel), a[0]);
    if (!t)
        return 1;
    gaddr cfg = rd32(rd32(a[0]) + REC_config);
    if (config_valid(t, cfg))
        return 0;
    if (!cfg) {
        cfg = ctb_config_new(t);
        wr32(rd32(a[0]) + REC_config, cfg);
    } else {
        ctb_config_defaults(t, cfg);
    }
    manager_of(ctb_mgr(sel))->config_changed(a[0]);
    return 1;
}

/* (Ptr *theConfig, procID, allocate) */
uint32_t ctb_default(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = ctb_tool_of(ctb_mgr(sel), (int16_t)a[1]);
    if (!t || !a[0])
        return 0;
    if (a[2])
        wr32(a[0], ctb_config_new(t));
    else if (rd32(a[0]))
        ctb_config_defaults(t, rd32(a[0]));
    return 0;
}

/* (hConn): a new pointer with the configuration as a string */
uint32_t ctb_get_config(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = tool_of_record(ctb_mgr(sel), a[0]);
    if (!t)
        return 0;
    char s[1024];
    ctb_config_string(t, rd32(rd32(a[0]) + REC_config), s, sizeof s);
    gaddr p = tb_NewPtr((int32_t)strlen(s) + 1, false, false);
    if (p)
        gput(p, s, (uint32_t)strlen(s) + 1);
    return p;
}

/* (hConn, the string): 0, -1, or where parsing stopped */
uint32_t ctb_set_config(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = tool_of_record(ctb_mgr(sel), a[0]);
    if (!t || !a[1])
        return (uint32_t)-1;
    char s[1024];
    uint32_t n = 0;
    while (n < sizeof s - 1 && rd8(a[1] + n))
        n++;
    gget(s, a[1], n);
    s[n] = 0;
    gaddr cfg = rd32(rd32(a[0]) + REC_config);
    int16_t r = ctb_config_parse(t, cfg, s);
    manager_of(ctb_mgr(sel))->config_changed(a[0]);
    return (uint32_t)(int32_t)r;
}

/* (hConn, input, Ptr *output, language): our strings are English already */
uint32_t ctb_intl(uint16_t sel, const uint32_t *a)
{
    if (!a[1] || !a[2])
        return (uint32_t)paramErr;
    uint32_t n = 0;
    while (rd8(a[1] + n))
        n++;
    gaddr p = tb_NewPtr((int32_t)n + 1, false, false);
    if (!p)
        return (uint32_t)memFullErr;
    gmemcpy(p, a[1], n + 1);
    wr32(a[2], p);
    return 0;
}

/* (hConn): a 'vers' resource of the tool's, as a handle */
uint32_t ctb_get_version(uint16_t sel, const uint32_t *a)
{
    const ctb_tool *t = tool_of_record(ctb_mgr(sel), a[0]);
    if (!t)
        return 0;
    static const char *const shortv = "1.0";
    char longv[128];
    snprintf(longv, sizeof longv, "1.0, %s for Blue Box", t->name);
    uint32_t n = 6 + 1 + (uint32_t)strlen(shortv) + 1 + (uint32_t)strlen(longv);
    gaddr h = tb_NewHandle((int32_t)n, false, true);
    if (!h)
        return 0;
    gaddr p = rd32(h);
    wr8(p, 0x01); /* 1.0 final */
    wr8(p + 2, 0x80);
    put_pstr(p + 6, shortv);
    put_pstr(p + 7 + (uint32_t)strlen(shortv), longv);
    return h;
}

/* The managers', the utilities' and the CRM's versions */
uint32_t ctb_mgr_version(uint16_t sel, const uint32_t *a) { return 2; }

/* (procID, &magicCookie): no items of the tool's for the program's dialog */
uint32_t ctb_setup_preflight(uint16_t sel, const uint32_t *a)
{
    if (a[1])
        wr32(a[1], 0);
    return 0;
}

/* (hConn, id, errMsg) */
uint32_t ctb_error_string(uint16_t sel, const uint32_t *a)
{
    static const char *const cm[] = { "No error", "The connection was refused", "The connection failed",
                                      "The connection timed out", "The connection isn't open",
                                      "The connection isn't closed", "No request is pending",
                                      "Not supported", "No connection tools", "Cancelled" };
    static const char *const ft[] = { "No error", "The transfer was refused", "The transfer failed",
                                      "The transfer timed out", "Too many retries", "The disk is full",
                                      "Cancelled by the other end", "Wrong format", "No file transfer tools",
                                      "Cancelled", "Not supported" };
    static const char *const tm[] = { "No error", "Not sent", "The environment changed", "", "", "", "",
                                      "Not supported", "No terminal tools" };
    int16_t id = (int16_t)a[1];
    const char *s = "";
    switch (ctb_mgr(sel)) {
    case MGR_CM: s = id >= 0 && id < 10 ? cm[id] : "Connection error"; break;
    case MGR_FT: s = id >= 0 && id < 11 ? ft[id] : "File transfer error"; break;
    case MGR_TM: s = id >= 0 && id < 9 ? tm[id] : "Terminal error"; break;
    }
    if (a[2])
        put_pstr(a[2], s);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Choose: a dialog of the manager's tools, with the settings as their
 * configuration string */

enum { itemOK = 1, itemCancel, itemToolTitle, itemSettingsTitle, itemSettings, itemFirstTool };

static uint32_t ditl_item(gaddr p, qrect r, uint8_t type, const char *text)
{
    size_t n = strlen(text);
    wr32(p, 0);
    qrect_put(p + 4, r);
    wr8(p + 12, type);
    wr8(p + 13, (uint8_t)n);
    gput(p + 14, text, (uint32_t)n);
    return 14 + (((uint32_t)n + 1) & ~1u);
}

/* (Handle *h, where, idleProc or ChooseRec *) */
uint32_t ctb_choose(uint16_t sel, const uint32_t *a)
{
    int mgr = ctb_mgr(sel);
    const ctb_manager *m = manager_of(mgr);
    gaddr hp = a[0];
    gaddr h = hp ? rd32(hp) : 0;
    const ctb_tool *cur = tool_of_record(mgr, h);
    if (!m || !h || !cur)
        return (uint32_t)chooseFailed;
    int ntools = m->ntools, height = 150 + 20 * ntools;
    char settings[1024];
    ctb_config_string(cur, rd32(rd32(h) + REC_config), settings, sizeof settings);

    gaddr ditl = tb_NewHandle(2 + (5 + ntools) * (14 + 256), false, true);
    gaddr p = rd32(ditl) + 2;
    uint32_t at = 0;
    at += ditl_item(p + at, (qrect){ (int16_t)(height - 30), 300, (int16_t)(height - 10), 370 }, 4, "OK");
    at += ditl_item(p + at, (qrect){ (int16_t)(height - 30), 210, (int16_t)(height - 10), 280 }, 4, "Cancel");
    at += ditl_item(p + at, (qrect){ 10, 10, 26, 370 },
                    8 | 0x80, mgr == MGR_CM ? "Connection tool:" : mgr == MGR_TM ? "Terminal tool:" : "File transfer tool:");
    at += ditl_item(p + at, (qrect){ (int16_t)(34 + 20 * ntools), 10, (int16_t)(50 + 20 * ntools), 370 }, 8 | 0x80,
                    "Settings:");
    settings[255] = 0;
    at += ditl_item(p + at, (qrect){ (int16_t)(56 + 20 * ntools), 13, (int16_t)(height - 42), 367 }, 16, settings);
    for (int i = 0; i < ntools; i++)
        at += ditl_item(p + at, (qrect){ (int16_t)(32 + 20 * i), 20, (int16_t)(50 + 20 * i), 360 }, 4 | 2,
                        m->tools[i]->name);
    wr16(rd32(ditl), (uint16_t)(5 + ntools - 1));

    int16_t top = pt_v(a[1]) > 20 ? pt_v(a[1]) : 60, left = pt_h(a[1]) > 0 ? pt_h(a[1]) : 40;
    gaddr scratch = tb_NewPtr(64, false, true);
    qrect_put(scratch, (qrect){ top, left, (int16_t)(top + height), (int16_t)(left + 380) });
    gaddr d = tb_NewDialog(0, scratch, scratch + 8, 1, 1 /* dBoxProc */, 0xFFFFFFFF, 0, 0, ditl);
    if (!d) {
        tb_DisposePtr(scratch);
        return (uint32_t)chooseFailed;
    }
    int chosen = rd16(rd32(h) + REC_procID);
    gaddr itype = scratch + 16, item = scratch + 20, r = scratch + 24, hit = scratch + 32;
    for (int i = 0; i < ntools; i++) {
        tb_GetDialogItem(d, (int16_t)(itemFirstTool + i), itype, item, r);
        tb_SetControlValue(rd32(item), i + 1 == chosen);
    }
    tb_SetDialogDefaultItem(d, itemOK);
    tb_SetDialogCancelItem(d, itemCancel);
    int16_t n;
    do {
        tb_ModalDialog(0, hit);
        n = (int16_t)rd16(hit);
        if (n >= itemFirstTool && n < itemFirstTool + ntools && n - itemFirstTool + 1 != chosen) {
            chosen = n - itemFirstTool + 1;
            for (int i = 0; i < ntools; i++) {
                tb_GetDialogItem(d, (int16_t)(itemFirstTool + i), itype, item, r);
                tb_SetControlValue(rd32(item), i + 1 == chosen);
            }
            /* Another tool: its defaults, or the record's own settings. */
            const ctb_tool *t = m->tools[chosen - 1];
            gaddr cfg = ctb_config_new(t);
            if (t == cur)
                gmemcpy(cfg, rd32(rd32(h) + REC_config), t->size);
            ctb_config_string(t, cfg, settings, sizeof settings);
            tb_DisposePtr(cfg);
            settings[255] = 0;
            tb_GetDialogItem(d, itemSettings, itype, item, r);
            gaddr str = tb_NewPtr(256, false, false);
            put_pstr(str, settings);
            tb_SetDialogItemText(rd32(item), str);
            tb_DisposePtr(str);
        }
    } while (n != itemOK && n != itemCancel);
    gaddr str = tb_NewPtr(256, false, false);
    tb_GetDialogItem(d, itemSettings, itype, item, r);
    tb_GetDialogItemText(rd32(item), str);
    cstr(str, settings, sizeof settings);
    tb_DisposePtr(str);
    tb_DisposeDialog(d);
    tb_DisposePtr(scratch);
    if (n == itemCancel)
        return chooseCancel;
    uint32_t result = chooseOKMinor;
    if (chosen != (int16_t)rd16(rd32(h) + REC_procID)) {
        gaddr nh = m->renew(h, (int16_t)chosen);
        if (!nh)
            return (uint32_t)chooseFailed;
        wr32(hp, nh);
        h = nh;
        result = chooseOKMajor;
    }
    ctb_config_parse(m->tools[chosen - 1], rd32(rd32(h) + REC_config), settings);
    m->config_changed(h);
    return result;
}

/* ---------------------------------------------------------------------- */
/* The utilities */

uint32_t ctb_init(uint16_t sel, const uint32_t *a) { return 0; }
uint32_t ctb_append_ditl(uint16_t sel, const uint32_t *a)
{
    tb_AppendDITL(a[0], a[1], (int16_t)a[2]);
    return 0;
}
uint32_t ctb_count_ditl(uint16_t sel, const uint32_t *a) { return (uint32_t)tb_CountDITL(a[0]); }
uint32_t ctb_shorten_ditl(uint16_t sel, const uint32_t *a)
{
    tb_ShortenDITL(a[0], (int16_t)a[1]);
    return 0;
}

/* StandardNBP and the rest: there is no AppleTalk to look names up on. */
uint32_t ctb_nbp(uint16_t sel, const uint32_t *a) { return nlCancel; }

/* ---------------------------------------------------------------------- */
/* The Communications Resource Manager: a queue of device records */

static gaddr crm_header;
static int32_t crm_next_id;

void crm_install(gaddr rec)
{
    if (!rec || !crm_header)
        return;
    CRMRec_set_qType(rec, 9); /* crmType */
    CRMRec_set_crmDeviceID(rec, crm_next_id++);
    tb_Enqueue(rec, crm_header);
}

gaddr crm_search(gaddr req)
{
    if (!req || !crm_header)
        return 0;
    int32_t type = CRMRec_crmDeviceType(req), after = CRMRec_crmDeviceID(req);
    for (gaddr e = QHdr_qHead(crm_header); e; e = rd32(e))
        if (CRMRec_crmDeviceType(e) == type && CRMRec_crmDeviceID(e) > after)
            return e;
    return 0;
}

uint32_t crm_get_header(uint16_t sel, const uint32_t *a) { return crm_header; }
uint32_t crm_install_r(uint16_t sel, const uint32_t *a)
{
    crm_install(a[0]);
    return 0;
}
uint32_t crm_remove(uint16_t sel, const uint32_t *a)
{
    return crm_header ? (uint32_t)(int32_t)tb_Dequeue(a[0], crm_header) : (uint32_t)-1;
}
uint32_t crm_search_r(uint16_t sel, const uint32_t *a) { return crm_search(a[0]); }

/* (type, id or index or name) */
uint32_t crm_get_resource(uint16_t sel, const uint32_t *a)
{
    switch (sel) {
    case 0x0507: return tb_GetResource((int32_t)a[0], (int16_t)a[1]);
    case 0x0508: return tb_Get1Resource((int32_t)a[0], (int16_t)a[1]);
    case 0x0509: return tb_GetIndResource((int32_t)a[0], (int16_t)a[1]);
    case 0x050A: return tb_Get1IndResource((int32_t)a[0], (int16_t)a[1]);
    case 0x050B: return tb_GetNamedResource((int32_t)a[0], a[1]);
    default: return tb_Get1NamedResource((int32_t)a[0], a[1]);
    }
}

uint32_t crm_release_resource(uint16_t sel, const uint32_t *a)
{
    tb_ReleaseResource(a[0]);
    return 0;
}

/* Our tools have no resources to renumber. */
uint32_t crm_local_id(uint16_t sel, const uint32_t *a) { return a[3]; }

/* (bundleType 'cbnd' 'tbnd' 'fbnd', index from 1, name) */
uint32_t crm_get_ind_tool_name(uint16_t sel, const uint32_t *a)
{
    int mgr = a[0] == fourcc("cbnd") ? MGR_CM : a[0] == fourcc("tbnd") ? MGR_TM : a[0] == fourcc("fbnd") ? MGR_FT : 0;
    const ctb_tool *t = ctb_tool_of(mgr, (int16_t)a[1]);
    if (!t)
        return (uint32_t)resNotFound;
    if (a[2])
        put_pstr(a[2], t->name);
    return 0;
}

/* (&vRefNum, &dirID): the System Folder, as the Communications Folder */
uint32_t crm_find_communications(uint16_t sel, const uint32_t *a)
{
    if (a[0])
        wr16(a[0], (uint16_t)fm_system_vref());
    if (a[1])
        wr32(a[1], 0);
    return 0;
}

/* (driverName): whether a serial port's driver is open */
uint32_t crm_is_driver_open(uint16_t sel, const uint32_t *a)
{
    char name[64];
    cstr(a[0], name, sizeof name);
    if (!strcasecmp(name, ".AIn") || !strcasecmp(name, ".AOut"))
        return serial_port_in_use(0);
    if (!strcasecmp(name, ".BIn") || !strcasecmp(name, ".BOut"))
        return serial_port_in_use(1);
    return 0;
}

uint32_t crm_parse_caps(uint16_t sel, const uint32_t *a) { return (uint32_t)-1; }

/* ---------------------------------------------------------------------- */

static bool polling;

void ctb_poll(void)
{
    if (polling || !g_cpu)
        return;
    polling = true;
    cm_poll();
    polling = false;
}

void ctb_reset(void)
{
    polling = false;
    crm_header = tb_NewPtr(QHdr_SIZEOF, true, true);
    crm_next_id = 1;
    cm_reset_all();
    tm_reset_all();
    ft_reset_all();
    wr32(OS_TRAP_TABLE + 0x8Bu * 4, native_register(ctb_dispatch, "CommToolboxDispatch", 0xA08B));
    def_stub = native_register(def_native, "CTBToolDef", 0);
    gestalt_set(fourcc("ctbv"), 2);
    gestalt_set(fourcc("conn"), 0xF); /* present, CMAddSearch fixed, CMGetErrorString, CMNewIOPB and the rest */
    gestalt_set(fourcc("term"), 0x5); /* present, TMGetErrorString */
    gestalt_set(fourcc("fxfr"), 0x7); /* present, FTSend and FTReceive, FTGetErrorString */
    gestalt_set(fourcc("crm "), 0x7); /* present, persistent tools, CRMGetToolResource */
    gestalt_set(fourcc("ditl"), 0x1); /* AppendDITL, CountDITL and ShortenDITL */
}
