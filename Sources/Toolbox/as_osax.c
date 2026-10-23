/*
 * AppleScript's commands (M14): the scripting additions, written here
 * rather than loaded from Apple's 'osax' files, and the Apple Events a
 * script sends to applications.
 *
 * Everything stays inside Blue Box.  Dialogs are shown by the host as
 * native ones (as classic alerts are), only to show text and take the
 * answer; files are the guest's (its File Manager), the clipboard is the
 * guest's scrap, speech the guest's Speech Manager.  Commands that would
 * reach outside (do shell script, open location, mount volume ...) are
 * refused, and `tell application` reaches only the program itself and
 * other Blue Box runs (aem_script_send), never the host's applications.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "as_internal.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

int16_t aem_script_send(gaddr evt, gaddr reply); /* aem.c */
void machine_pass_tick(void);                    /* trap.c */

/* ---------------------------------------------------------------------- */
/* Dialogs, shown by the host */

static machine_script_dialog_fn dialog_fn;
static void *dialog_ctx;

void machine_set_script_dialog_handler(machine_script_dialog_fn fn, void *ctx)
{
    dialog_fn = fn;
    dialog_ctx = ctx;
}

static void copy_text(char *out, size_t cap, as_obj *t)
{
    size_t n = t && t->kind == V_TEXT ? (t->n < cap - 1 ? t->n : cap - 1) : 0;
    if (n)
        memcpy(out, t->bytes, n);
    out[n] = 0;
}

static as_obj *param(as_obj *params, const char *code)
{
    return params ? as_record_get_code(params, T(code)) : NULL;
}

static as_obj *text_param(as_obj *v)
{
    if (!v)
        return NULL;
    v = as_deref(v);
    return v ? as_to_text(v) : NULL;
}

/* Ask the host, or answer as the user would at once when there is none
 * (headless runs): the default button, the default answer. */
/* BLUEBOX_SCRIPT_ANSWERS: a headless run's answers to the dialogs, in
 * order, separated by "|": a button's title chooses it, anything else is
 * typed into the dialog's text field (or picks a list's item) and the
 * default button chosen.  Once they run out, the defaults again. */
static bool scripted_answer(machine_script_dialog *d, int *button)
{
    static const char *next;
    static bool started;
    if (!started) {
        started = true;
        next = getenv("BLUEBOX_SCRIPT_ANSWERS");
    }
    if (!next || !*next)
        return false;
    const char *end = strchr(next, '|');
    size_t n = end ? (size_t)(end - next) : strlen(next);
    char answer[256];
    snprintf(answer, sizeof answer, "%.*s", (int)(n < 255 ? n : 255), next);
    next = end ? end + 1 : next + n;
    for (int i = 0; i < d->nbuttons; i++)
        if (!strcmp(d->buttons[i], answer)) {
            *button = i + 1;
            return true;
        }
    if (d->kind == MACHINE_SCRIPT_LIST) {
        for (int i = 0; i < d->nitems; i++)
            if (!strcmp(d->items[i], answer))
                d->selected = i;
    } else if (d->has_answer) {
        snprintf(d->answer, sizeof d->answer, "%s", answer);
    }
    *button = d->default_button ? d->default_button : 1;
    return true;
}

int as_ask(machine_script_dialog *d)
{
    if (dialog_fn)
        return dialog_fn(d, dialog_ctx);
    int b = d->default_button;
    if (!b && d->kind == MACHINE_SCRIPT_DIALOG) {
        /* No default: the rightmost button that isn't Cancel, as a hurried user would. */
        for (int i = d->nbuttons; i >= 1 && !b; i--)
            if (i != d->cancel_button)
                b = i;
        if (!b)
            b = d->nbuttons;
        if (d->giving_up)
            b = 0;
    }
    if (d->kind == MACHINE_SCRIPT_LIST) {
        b = 1;
        if (d->selected < 0)
            d->selected = 0;
    }
    scripted_answer(d, &b);
    char msg[160];
    snprintf(msg, sizeof msg, "%s", d->message);
    for (char *p = msg; *p; p++)
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    if (d->kind == MACHINE_SCRIPT_DIALOG)
        LOGF(LOG_UNIMPL | LOG_TRAPS, "AppleScript: display dialog \"%s\" -> %s%s%s\n", msg,
             b ? d->buttons[b - 1] : "(gave up)", d->has_answer ? ", answer " : "", d->has_answer ? d->answer : "");
    else
        LOGF(LOG_UNIMPL | LOG_TRAPS, "AppleScript: choose from list \"%s\" -> item %d\n", msg, d->selected + 1);
    machine_idle(); /* the user's answer takes time */
    return b;
}

static as_obj *display_dialog(as_obj *direct, as_obj *params)
{
    machine_script_dialog d;
    memset(&d, 0, sizeof d);
    d.kind = MACHINE_SCRIPT_DIALOG;
    d.icon = MACHINE_ALERT_PLAIN;
    as_obj *msg = text_param(direct ? direct : as_cstr(""));
    if (!msg)
        return NULL;
    copy_text(d.message, sizeof d.message, msg);
    as_obj *btns = param(params, "btns");
    if (btns) {
        btns = as_deref(btns);
        if (btns && btns->kind != V_LIST)
            btns = as_coerce(btns, T("list"));
        if (!btns)
            return NULL;
        if (btns->n > 3)
            return as_raise(-50, btns, "A dialog can have at most three buttons.");
        for (uint32_t i = 0; i < btns->n; i++) {
            as_obj *t = as_to_text(btns->items[i]);
            if (!t)
                return NULL;
            copy_text(d.buttons[i], sizeof d.buttons[i], t);
        }
        d.nbuttons = (int)btns->n;
    } else {
        strcpy(d.buttons[0], "Cancel");
        strcpy(d.buttons[1], "OK");
        d.nbuttons = 2;
        d.default_button = 2;
    }
    for (int i = 0; i < d.nbuttons; i++)
        if (!strcasecmp(d.buttons[i], "Cancel"))
            d.cancel_button = i + 1;
    as_obj *dflt = param(params, "dflt");
    if (dflt) {
        dflt = as_deref(dflt);
        double n;
        bool is_int;
        if (dflt && dflt->kind == V_TEXT) {
            for (int i = 0; i < d.nbuttons; i++)
                if (strlen(d.buttons[i]) == dflt->n && !strncasecmp(d.buttons[i], (const char *)dflt->bytes, dflt->n))
                    d.default_button = i + 1;
        } else if (dflt && as_to_number(dflt, &n, &is_int) && n >= 1 && n <= d.nbuttons) {
            d.default_button = (int)n;
        } else if (!dflt) {
            return NULL;
        }
        if (!d.default_button)
            return as_raise(-50, dflt, "The default button isn't one of the dialog's buttons.");
    }
    as_obj *icon = param(params, "disp");
    if (icon) {
        double n;
        bool is_int;
        int k = -1;
        if (icon->kind == V_CONST)
            k = (int)(icon->code & 0xFF);
        else if (as_to_number(icon, &n, &is_int))
            k = (int)n;
        d.icon = k == 0 ? MACHINE_ALERT_STOP : k == 1 ? MACHINE_ALERT_NOTE : k == 2 ? MACHINE_ALERT_CAUTION
                                                                             : MACHINE_ALERT_PLAIN;
    }
    as_obj *answer = param(params, "dtxt");
    if (answer) {
        as_obj *t = text_param(answer);
        if (!t)
            return NULL;
        d.has_answer = true;
        copy_text(d.answer, sizeof d.answer, t);
    }
    as_obj *givu = param(params, "givu");
    if (givu) {
        double n;
        bool is_int;
        givu = as_deref(givu);
        if (!givu || !as_to_number(givu, &n, &is_int))
            return givu ? as_raise(asErrCantCoerce, givu, "Can't make the giving up time a number.") : NULL;
        d.giving_up = n < 1 ? 1 : (int)n;
    }
    int b = as_ask(&d);
    if (b < 0 || b > d.nbuttons)
        b = d.default_button;
    if (b && b == d.cancel_button)
        return as_raise(asErrUserCanceled, NULL, "User canceled.");
    as_obj *r = as_record();
    as_record_set(r, as_const(T("bhit")), as_cstr(b ? d.buttons[b - 1] : ""));
    if (d.has_answer)
        as_record_set(r, as_const(T("ttxt")), as_cstr(d.answer));
    if (d.giving_up)
        as_record_set(r, as_const(T("gavu")), as_bool(b == 0));
    return r;
}

static as_obj *choose_from_list(as_obj *direct, as_obj *params)
{
    as_obj *l = direct ? as_deref(direct) : NULL;
    if (!l)
        return direct ? NULL : as_raise(-50, NULL, "choose from list needs a list.");
    if (l->kind != V_LIST)
        l = as_coerce(l, T("list"));
    if (!l)
        return NULL;
    machine_script_dialog d;
    memset(&d, 0, sizeof d);
    d.kind = MACHINE_SCRIPT_LIST;
    as_obj *prompt = text_param(param(params, "prmp"));
    copy_text(d.message, sizeof d.message, prompt ? prompt : as_cstr("Please make your selection:"));
    as_obj *ok = text_param(param(params, "okbt")), *cancel = text_param(param(params, "cnbt"));
    copy_text(d.buttons[0], sizeof d.buttons[0], ok ? ok : as_cstr("OK"));
    copy_text(d.buttons[1], sizeof d.buttons[1], cancel ? cancel : as_cstr("Cancel"));
    d.nbuttons = 2;
    d.default_button = 1;
    d.cancel_button = 2;
    char **items = calloc(l->n ? l->n : 1, sizeof *items);
    for (uint32_t i = 0; i < l->n; i++) {
        as_obj *t = as_to_text(l->items[i]);
        if (!t) {
            for (uint32_t k = 0; k < i; k++)
                free(items[k]);
            free(items);
            return NULL;
        }
        items[i] = strndup((const char *)t->bytes, t->n);
    }
    d.items = (const char *const *)items;
    d.nitems = (int)l->n;
    d.selected = -1;
    as_obj *initial = param(params, "inSL");
    if (initial) {
        initial = as_deref(initial);
        if (initial && initial->kind == V_LIST && initial->n)
            initial = initial->items[0];
        for (uint32_t i = 0; initial && i < l->n && d.selected < 0; i++)
            if (as_equal(l->items[i], initial))
                d.selected = (int)i;
    }
    int b = l->n ? as_ask(&d) : 2;
    for (uint32_t i = 0; i < l->n; i++)
        free(items[i]);
    free(items);
    if (b != 1 || d.selected < 0 || d.selected >= (int)l->n)
        return as_bool(false);
    as_obj *r = as_list(1);
    as_list_push(r, l->items[d.selected]);
    return r;
}

/* ---------------------------------------------------------------------- */
/* The guest's files, by alias or path */

/* A guest Pascal string of text, in a pointer the caller disposes of. */
static gaddr guest_pstr(as_obj *t)
{
    gaddr p = tb_NewPtr(256, false, true);
    uint32_t n = t->n > 255 ? 255 : t->n;
    wr8(p, (uint8_t)n);
    gput(p + 1, t->bytes, n);
    return p;
}

/* A file as a script names it (an alias, or a path) as an FSSpec in spec. */
static bool file_spec(as_obj *v, gaddr spec)
{
    v = as_deref(v);
    if (!v)
        return false;
    if (v->kind == V_DESC && v->code == T("alis")) {
        gaddr h = tb_NewHandle((int32_t)v->n, false, false);
        if (!h || !rd32(h)) {
            as_raise(memFullErr, v, "Out of memory.");
            return false;
        }
        gput(rd32(h), v->bytes, v->n);
        gaddr changed = tb_NewPtr(2, false, true);
        int16_t err = tb_ResolveAlias(0, h, spec, changed);
        tb_DisposePtr(changed);
        tb_DisposeHandle(h);
        if (err) {
            as_raise(err, v, "Can't find the file of an alias (error %d).", err);
            return false;
        }
        return true;
    }
    if (v->kind == V_DESC && v->code == T("fss ") && v->n >= FSSpec_SIZEOF) {
        gput(spec, v->bytes, FSSpec_SIZEOF);
        return true;
    }
    as_obj *t = as_to_text(v);
    if (!t)
        return false;
    gaddr name = guest_pstr(t);
    int16_t err = tb_FSMakeFSSpec(0, 0, name, spec);
    tb_DisposePtr(name);
    if (err) {
        char s[128];
        as_show(v, s, sizeof s);
        as_raise(err, v, "Can't find the file %s (error %d).", s, err);
        return false;
    }
    return true;
}

static as_obj *alias_of_spec(gaddr spec)
{
    gaddr hp = tb_NewPtr(4, false, true);
    int16_t err = tb_NewAlias(0, spec, hp);
    gaddr h = rd32(hp);
    tb_DisposePtr(hp);
    if (err || !h || !rd32(h))
        return as_raise(err ? err : memFullErr, NULL, "Can't make an alias (error %d).", err);
    int32_t n = tb_GetHandleSize(h);
    uint8_t *buf = malloc((size_t)n);
    gget(buf, rd32(h), (uint32_t)n);
    as_obj *a = as_desc(T("alis"), buf, (size_t)n);
    free(buf);
    tb_DisposeHandle(h);
    return a;
}

/* An FSSpec's full path ("Disk:Folder:File"), through the catalog. */
static as_obj *spec_path(gaddr spec)
{
    char parts[16][64];
    int np = 0;
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true), name = tb_NewPtr(256, false, true);
    int16_t vref = (int16_t)rd16(spec + FSSpec_vRefNum_off);
    int32_t dir = (int32_t)rd32(spec + FSSpec_parID_off);
    uint8_t len = rd8(spec + FSSpec_name_off);
    gget(parts[np], spec + FSSpec_name_off + 1, len > 63 ? 63 : len);
    parts[np++][len > 63 ? 63 : len] = 0;
    while (dir > 1 && np < 16) {
        for (uint32_t i = 0; i < CInfoPBRec_SIZEOF; i++)
            wr8(pb + i, 0);
        wr32(pb + DirInfo_ioNamePtr_off, name);
        wr16(pb + DirInfo_ioVRefNum_off, (uint16_t)vref);
        wr16(pb + DirInfo_ioFDirIndex_off, (uint16_t)-1);
        wr32(pb + DirInfo_ioDrDirID_off, (uint32_t)dir);
        if (tb_PBGetCatInfo(pb, false))
            break;
        uint8_t l = rd8(name);
        gget(parts[np], name + 1, l > 63 ? 63 : l);
        parts[np++][l > 63 ? 63 : l] = 0;
        dir = (int32_t)rd32(pb + DirInfo_ioDrParID_off);
    }
    tb_DisposePtr(pb);
    tb_DisposePtr(name);
    as_obj *t = as_text("", 0);
    for (int i = np; i-- > 0;) {
        as_text_append(t, parts[i], strlen(parts[i]));
        if (i)
            as_text_append(t, ":", 1);
    }
    return t;
}

/* The path an alias record keeps (its full path, tag 2), when the file
 * isn't there to resolve it. */
static as_obj *recorded_path(as_obj *a)
{
    const uint8_t *p = a->bytes;
    uint32_t n = a->n, off = 150;
    while (off + 4 <= n) {
        int16_t tag = (int16_t)(p[off] << 8 | p[off + 1]);
        uint32_t len = (uint32_t)(p[off + 2] << 8 | p[off + 3]);
        if (tag == -1 || off + 4 + len > n)
            break;
        if (tag == 2)
            return as_text(p + off + 4, len);
        off += 4 + ((len + 1) & ~1u);
    }
    if (n >= 114 && p[10] < 28 && p[50] < 64) {
        as_obj *t = as_text(p + 11, p[10]);
        as_text_append(t, ":", 1);
        as_text_append(t, p + 51, p[50]);
        return t;
    }
    return NULL;
}

as_obj *as_file_path(as_obj *file)
{
    if (file->kind != V_DESC || (file->code != T("alis") && file->code != T("fss ")))
        return NULL;
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    bool ok = file_spec(file, spec);
    as_obj *r = ok ? spec_path(spec) : NULL;
    tb_DisposePtr(spec);
    if (!ok)
        as_clear_error();
    if (!r && file->code == T("alis"))
        r = recorded_path(file);
    return r;
}

static as_obj *choose_file(as_obj *params)
{
    gaddr reply = tb_NewPtr(StandardFileReply_SIZEOF, false, true);
    gaddr types = tb_NewPtr(16, false, true);
    int16_t ntypes = -1;
    as_obj *ft = param(params, "fltp");
    if (ft) {
        ft = as_deref(ft);
        if (ft && ft->kind != V_LIST)
            ft = as_coerce(ft, T("list"));
        if (!ft) {
            tb_DisposePtr(reply);
            tb_DisposePtr(types);
            return NULL;
        }
        ntypes = 0;
        for (uint32_t i = 0; i < ft->n && ntypes < 4; i++) {
            as_obj *t = as_to_text(ft->items[i]);
            if (t && t->n == 4)
                wr32(types + 4u * (uint32_t)ntypes++, (uint32_t)t->bytes[0] << 24 | (uint32_t)t->bytes[1] << 16 |
                                                          (uint32_t)t->bytes[2] << 8 | t->bytes[3]);
        }
        as_clear_error();
    }
    tb_StandardGetFile(0, ntypes, ntypes > 0 ? types : 0, reply);
    as_obj *r = rd8(reply + StandardFileReply_sfGood_off) ? alias_of_spec(reply + StandardFileReply_sfFile_off)
                                                           : as_raise(asErrUserCanceled, NULL, "User canceled.");
    tb_DisposePtr(reply);
    tb_DisposePtr(types);
    return r;
}

static int32_t pb_call(int16_t (*fn)(gaddr, bool), int16_t refnum, gaddr buf, int32_t count, int32_t *actual)
{
    gaddr pb = tb_NewPtr(IOParam_SIZEOF + 16, false, true);
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    wr32(pb + IOParam_ioBuffer_off, buf);
    wr32(pb + IOParam_ioReqCount_off, (uint32_t)count);
    int16_t err = fn(pb, false);
    if (actual)
        *actual = (int32_t)rd32(pb + (fn == tb_PBGetEOF ? IOParam_ioMisc_off : IOParam_ioActCount_off));
    tb_DisposePtr(pb);
    return err;
}

static int16_t open_file(as_obj *file, bool write, int16_t *refnum)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), rp = tb_NewPtr(2, false, true);
    int16_t err = file_spec(file, spec) ? tb_FSpOpenDF(spec, write ? 3 : 1, rp) : 1;
    *refnum = (int16_t)rd16(rp);
    tb_DisposePtr(spec);
    tb_DisposePtr(rp);
    return err;
}

/* read: a file opened for access (its reference number) or a file, all of
 * it or `for` so many bytes. */
static as_obj *read_file(as_obj *direct, as_obj *params)
{
    direct = direct ? as_deref(direct) : NULL;
    if (!direct)
        return as_raise(-50, NULL, "read needs a file.");
    int16_t refnum;
    bool opened = false;
    double d;
    bool is_int;
    if (is_int = false, as_to_number(direct, &d, &is_int) && is_int) {
        refnum = (int16_t)d;
    } else {
        int16_t err = open_file(direct, false, &refnum);
        if (err == 1)
            return NULL;
        if (err)
            return as_raise(err, direct, "Can't open the file (error %d).", err);
        opened = true;
    }
    int32_t eof = 0;
    pb_call(tb_PBGetEOF, refnum, 0, 0, &eof);
    int32_t want = eof;
    as_obj *count = param(params, "for ");
    if (count && as_to_number(as_deref(count), &d, &is_int) && d >= 0 && d < want)
        want = (int32_t)d;
    gaddr buf = tb_NewPtr(want ? want : 1, false, false);
    if (!buf) {
        if (opened)
            pb_call(tb_PBClose, refnum, 0, 0, NULL);
        return as_raise(memFullErr, NULL, "Out of memory.");
    }
    int32_t got = 0;
    int32_t err = pb_call(tb_PBRead, refnum, buf, want, &got);
    if (opened)
        pb_call(tb_PBClose, refnum, 0, 0, NULL);
    if (err && err != -39 /* eofErr */) {
        tb_DisposePtr(buf);
        return as_raise(err, direct, "Can't read the file (error %d).", err);
    }
    if (err == -39 && got == 0) {
        tb_DisposePtr(buf);
        return as_raise(-39, direct, "End of file error.");
    }
    uint8_t *host = malloc((size_t)got + 1);
    gget(host, buf, (uint32_t)got);
    tb_DisposePtr(buf);
    as_obj *t = as_text(host, (size_t)got);
    free(host);
    return t;
}

/* ---------------------------------------------------------------------- */
/* Values as Apple Event descriptors (in the guest) and back */

static gaddr new_desc(void) { return tb_NewPtr(AEDesc_SIZEOF, false, true); }

static void free_desc(gaddr d)
{
    if (d) {
        tb_AEDisposeDesc(d);
        tb_DisposePtr(d);
    }
}

static bool make_desc(uint32_t type, const void *data, uint32_t n, gaddr out)
{
    gaddr p = tb_NewPtr(n ? (int32_t)n : 1, false, false);
    if (n)
        gput(p, data, n);
    int16_t err = tb_AECreateDesc((int32_t)type, p, (int32_t)n, out);
    tb_DisposePtr(p);
    return err == noErr;
}

static bool be32_desc(uint32_t type, uint32_t v, gaddr out)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return make_desc(type, b, 4, out);
}

static bool to_desc(as_obj *v, gaddr out);

static bool put_key(gaddr rec, uint32_t key, gaddr d)
{
    int16_t err = tb_AEPutParamDesc(rec, (int32_t)key, d);
    return err == noErr;
}

static bool obj_spec(as_obj *ref, gaddr out)
{
    static const char *const forms[] = { "prop", "indx", "indx", "indx", "ID  ", "name", "rang", "rele", "test" };
    if (ref->btype == REF_BEGINNING || ref->btype == REF_END) {
        /* an insertion location */
        gaddr from = new_desc(), pos = new_desc();
        bool ok = to_desc(ref->a, from) && be32_desc(T("enum"), ref->btype == REF_END ? T("end ") : T("bgng"), pos) &&
                  tb_AECreateList(0, 0, 1, out) == noErr && put_key(out, T("kobj"), from) && put_key(out, T("kpos"), pos);
        free_desc(from);
        free_desc(pos);
        if (ok)
            wr32(out, T("insl"));
        return ok;
    }
    if (ref->btype > REF_FILTER)
        return false;
    gaddr want = new_desc(), form = new_desc(), seld = new_desc(), from = new_desc();
    uint32_t cls = ref->btype == REF_PROPERTY ? T("prop") : ref->b && ref->b->kind == V_CONST ? (uint32_t)ref->b->code
                                                                                              : T("cobj");
    bool ok = be32_desc(T("type"), cls, want) && be32_desc(T("enum"), T(forms[ref->btype]), form);
    if (ok) {
        switch (ref->btype) {
        case REF_PROPERTY:
            ok = ref->b && ref->b->kind == V_CONST && be32_desc(T("type"), (uint32_t)ref->b->code, seld);
            break;
        case REF_EVERY:
            ok = be32_desc(T("abso"), T("all "), seld);
            break;
        case REF_SOME:
            ok = be32_desc(T("abso"), T("any "), seld);
            break;
        case REF_MIDDLE:
            ok = be32_desc(T("abso"), T("midd"), seld);
            break;
        case REF_RANGE: {
            /* a range record of two specifiers on the container */
            as_obj s1 = { .kind = V_REF, .btype = REF_INDEX, .a = as_null(), .b = ref->b, .c = ref->c };
            as_obj s2 = { .kind = V_REF, .btype = REF_INDEX, .a = as_null(), .b = ref->b, .c = ref->d };
            gaddr d1 = new_desc(), d2 = new_desc();
            ok = to_desc(&s1, d1) && to_desc(&s2, d2) && tb_AECreateList(0, 0, 1, seld) == noErr &&
                 put_key(seld, T("star"), d1) && put_key(seld, T("stop"), d2);
            if (ok)
                wr32(seld, T("rang"));
            free_desc(d1);
            free_desc(d2);
            break;
        }
        default:
            ok = to_desc(ref->c, seld);
            break;
        }
    }
    ok = ok && to_desc(ref->a, from) && tb_AECreateList(0, 0, 1, out) == noErr && put_key(out, T("want"), want) &&
         put_key(out, T("form"), form) && put_key(out, T("seld"), seld) && put_key(out, T("from"), from);
    if (ok)
        wr32(out, T("obj "));
    free_desc(want);
    free_desc(form);
    free_desc(seld);
    free_desc(from);
    return ok;
}

static bool to_desc(as_obj *v, gaddr out)
{
    uint8_t b[8];
    switch (v->kind) {
    case V_TEXT:
        return make_desc(T("TEXT"), v->bytes, v->n, out);
    case V_INT:
        return be32_desc(T("long"), (uint32_t)v->i, out);
    case V_REAL: {
        uint64_t bits;
        memcpy(&bits, &v->r, 8);
        for (int i = 0; i < 8; i++)
            b[i] = (uint8_t)(bits >> (56 - 8 * i));
        return make_desc(T("doub"), b, 8, out);
    }
    case V_BOOL:
        b[0] = v->i ? 1 : 0;
        return make_desc(T("bool"), b, 1, out);
    case V_CONST:
        if (v->code >> 32)
            return be32_desc(T("enum"), (uint32_t)v->code, out);
        return be32_desc(T("type"), (uint32_t)v->code, out);
    case V_LIST: {
        if (tb_AECreateList(0, 0, 0, out) != noErr)
            return false;
        for (uint32_t i = 0; i < v->n; i++) {
            gaddr d = new_desc();
            bool ok = to_desc(v->items[i], d) && tb_AEPutDesc(out, 0, d) == noErr;
            free_desc(d);
            if (!ok)
                return false;
        }
        return true;
    }
    case V_RECORD: {
        if (tb_AECreateList(0, 0, 1, out) != noErr)
            return false;
        for (uint32_t i = 0; i < v->n; i++) {
            if (v->keys[i]->kind != V_CONST)
                continue; /* user labels ('usrf') aren't kept */
            gaddr d = new_desc();
            bool ok = to_desc(v->items[i], d) && put_key(out, (uint32_t)v->keys[i]->code, d);
            free_desc(d);
            if (!ok)
                return false;
        }
        return true;
    }
    case V_DESC:
        return make_desc((uint32_t)v->code, v->bytes, v->n, out);
    case V_REF:
        return obj_spec(v, out);
    case V_APP:
    case V_NULL:
    case V_UNDEF:
    case V_MISSING:
        return make_desc(T("null"), NULL, 0, out);
    default:
        return false;
    }
}

as_obj *as_from_desc(gaddr desc)
{
    uint32_t type = (uint32_t)rd32(desc + AEDesc_descriptorType_off);
    gaddr h = rd32(desc + AEDesc_dataHandle_off);
    if (type == T("list") || type == T("reco")) {
        gaddr cnt = tb_NewPtr(8, false, true);
        tb_AECountItems(desc, cnt);
        int32_t n = (int32_t)rd32(cnt);
        as_obj *r = type == T("list") ? as_list((uint32_t)n) : as_record();
        gaddr d = new_desc();
        for (int32_t i = 1; i <= n; i++) {
            if (tb_AEGetNthDesc(desc, i, (int32_t)T("****"), cnt + 4, d) != noErr)
                break;
            as_obj *v = as_from_desc(d);
            tb_AEDisposeDesc(d);
            if (type == T("list"))
                as_list_push(r, v);
            else
                as_record_set(r, as_const(rd32(cnt + 4)), v);
        }
        tb_DisposePtr(d);
        tb_DisposePtr(cnt);
        return r;
    }
    uint32_t n = h && rd32(h) ? (uint32_t)tb_GetHandleSize(h) : 0;
    uint8_t *b = malloc(n + 1);
    if (n)
        gget(b, rd32(h), n);
    as_obj *v;
    uint32_t be = n >= 4 ? (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3] : 0;
    switch (type) {
    case 0x54455854: /* 'TEXT' */
    case 0x63747874: /* 'ctxt' */
        v = as_text(b, n);
        break;
    case 0x6C6F6E67: /* 'long' */
        v = as_int((int32_t)be);
        break;
    case 0x73686F72: /* 'shor' */
        v = as_int(n >= 2 ? (int16_t)(b[0] << 8 | b[1]) : 0);
        break;
    case 0x646F7562: { /* 'doub' */
        uint64_t bits = 0;
        for (uint32_t i = 0; i < 8 && i < n; i++)
            bits = bits << 8 | b[i];
        double d;
        memcpy(&d, &bits, 8);
        v = as_real(d);
        break;
    }
    case 0x626F6F6C: /* 'bool' */
        v = as_bool(n && b[0]);
        break;
    case 0x74727565: /* 'true' */
        v = as_bool(true);
        break;
    case 0x66616C73: /* 'fals' */
        v = as_bool(false);
        break;
    case 0x74797065: /* 'type' */
        v = as_const(be);
        break;
    case 0x656E756D: /* 'enum' */
        v = as_const((uint64_t)T("enum") << 32 | be);
        break;
    case 0x6E756C6C: /* 'null' */
        v = as_missing();
        break;
    default:
        v = as_desc(type, b, n);
        break;
    }
    free(b);
    return v;
}

/* ---------------------------------------------------------------------- */
/* Apple Events to applications: Blue Box runs only */

static as_obj *root_app(as_obj *v)
{
    while (v && v->kind == V_REF)
        v = v->a;
    return v && v->kind == V_APP ? v : NULL;
}

static as_obj *app_error(as_obj *app, int16_t err, const char *what)
{
    char name[96] = "The application";
    if (app && app->a)
        snprintf(name, sizeof name, "%.*s", (int)app->a->n, (const char *)app->a->bytes);
    if (err == asErrProcNotFound)
        return as_raise(err, app, "%s isn't running in Blue Box.", name);
    return as_raise(err, app, "%s got an error: %s (%d).", name, what, err);
}

/* Send an event to an application (its signature), with a direct
 * parameter and others; the reply's direct parameter. */
static as_obj *send_to_app(as_obj *app, uint32_t cls, uint32_t id, as_obj *direct, as_obj *params)
{
    gaddr addr = new_desc(), evt = new_desc(), reply = new_desc(), d = new_desc();
    as_obj *result = NULL;
    if (!be32_desc(T("sign"), (uint32_t)app->code, addr) ||
        tb_AECreateAppleEvent((int32_t)cls, (int32_t)id, addr, -1, 0, evt) != noErr) {
        result = as_raise(asErrUnsupported, app, "Can't make the Apple Event.");
        goto done;
    }
    if (direct) {
        if (!to_desc(direct, d) || !put_key(evt, T("----"), d)) {
            result = as_raise(asErrCantCoerce, direct, "Can't send that in an Apple Event.");
            goto done;
        }
        tb_AEDisposeDesc(d);
    }
    for (uint32_t i = 0; params && i < params->n; i++) {
        if (params->keys[i]->kind != V_CONST)
            continue;
        if (!to_desc(params->items[i], d) || !put_key(evt, (uint32_t)params->keys[i]->code, d)) {
            result = as_raise(asErrCantCoerce, params->items[i], "Can't send that in an Apple Event.");
            goto done;
        }
        tb_AEDisposeDesc(d);
    }
    int16_t err = aem_script_send(evt, reply);
    if (err) {
        result = app_error(app, err, "the event wasn't handled");
        goto done;
    }
    if (rd32(reply + AEDesc_descriptorType_off) == T("aevt")) {
        if (tb_AEGetParamDesc(reply, (int32_t)T("errn"), (int32_t)T("long"), d) == noErr) {
            as_obj *n = as_from_desc(d);
            tb_AEDisposeDesc(d);
            if (n->kind == V_INT && n->i) {
                as_obj *msg = NULL;
                if (tb_AEGetParamDesc(reply, (int32_t)T("errs"), (int32_t)T("TEXT"), d) == noErr) {
                    msg = as_from_desc(d);
                    tb_AEDisposeDesc(d);
                }
                result = msg && msg->kind == V_TEXT ? as_raise(n->i, app, "%.*s", (int)msg->n, (const char *)msg->bytes)
                                                    : app_error(app, (int16_t)n->i, "an error");
                goto done;
            }
        }
        if (tb_AEGetParamDesc(reply, (int32_t)T("----"), (int32_t)T("****"), d) == noErr) {
            result = as_from_desc(d);
            tb_AEDisposeDesc(d);
            goto done;
        }
    }
    result = as_undef();
done:
    free_desc(addr);
    free_desc(evt);
    free_desc(reply);
    tb_DisposePtr(d);
    return result;
}

/* The Finder isn't a process in Blue Box.  Of its commands a script may
 * send, `computer` (Gestalt) is answered here, from the guest's Gestalt;
 * the others are refused, as for an application that isn't running. */
static bool is_finder(as_obj *app) { return app && app->code == T("MACS"); }

static as_obj *gestalt(as_obj *direct)
{
    as_obj *t = text_param(direct);
    if (!t)
        return NULL;
    if (t->n != 4)
        return as_raise(-50, t, "computer needs a Gestalt selector of four characters.");
    uint32_t sel = (uint32_t)t->bytes[0] << 24 | (uint32_t)t->bytes[1] << 16 | (uint32_t)t->bytes[2] << 8 | t->bytes[3];
    uint32_t resp = 0;
    int16_t err = tb_Gestalt((int32_t)sel, &resp);
    if (err)
        return as_raise(err, t, "Gestalt selector %.4s is unknown.", (const char *)t->bytes);
    return as_number((int32_t)resp);
}

as_obj *as_app_get(as_obj *ref)
{
    as_obj *app = root_app(ref);
    if (!app)
        return as_raise(asErrCantGet, ref, "Can't get that.");
    if (ref == app)
        return app;
    if (is_finder(app))
        return app_error(app, asErrProcNotFound, "");
    return send_to_app(app, T("core"), T("getd"), ref, NULL);
}

as_obj *as_app_set(as_obj *ref, as_obj *value)
{
    as_obj *app = root_app(ref);
    if (!app || is_finder(app))
        return app_error(app, asErrProcNotFound, "");
    as_obj *params = as_record();
    as_record_set(params, as_const(T("data")), value);
    if (!send_to_app(app, T("core"), T("setd"), ref, params))
        return NULL;
    return value;
}

/* ---------------------------------------------------------------------- */
/* Commands */

static as_obj *say(as_obj *direct)
{
    as_obj *t = text_param(direct ? direct : as_cstr(""));
    if (!t)
        return NULL;
    gaddr s = guest_pstr(t);
    int16_t err = tb_SpeakString(s);
    /* say waits until it has been said */
    for (int i = 0; !err && tb_SpeechBusy() && i < 6000 && !(g_cpu && g_cpu->halted); i++) {
        speech_pump();
        if (events_host_attached())
            usleep(10000);
        else
            machine_pass_tick();
    }
    tb_DisposePtr(s);
    return err ? as_raise(err, t, "Can't speak (error %d).", err) : as_undef();
}

static as_obj *random_number(as_obj *direct, as_obj *params)
{
    double lo = 0, hi = 1, d;
    bool is_int = false, any_int = false, ranged = false, x;
    as_obj *from = param(params, "from"), *to = param(params, "to  ");
    if (direct) {
        direct = as_deref(direct);
        if (!direct || !as_to_number(direct, &hi, &x))
            return direct ? as_raise(asErrCantCoerce, direct, "random number needs a number.") : NULL;
        ranged = true;
        any_int = x;
    }
    if (from) {
        from = as_deref(from);
        if (!from || !as_to_number(from, &lo, &is_int))
            return from ? as_raise(asErrCantCoerce, from, "random number needs a number.") : NULL;
        ranged = true;
        any_int = is_int;
    }
    if (to) {
        to = as_deref(to);
        if (!to || !as_to_number(to, &d, &is_int))
            return to ? as_raise(asErrCantCoerce, to, "random number needs a number.") : NULL;
        hi = d;
        ranged = true;
        any_int = (from ? any_int : true) && is_int;
    }
    double u = (as_random() & 0xFFFFFF) / 16777216.0;
    if (!ranged)
        return as_real(u);
    if (hi < lo) {
        double t = hi;
        hi = lo;
        lo = t;
    }
    if (any_int) {
        int64_t span = (int64_t)hi - (int64_t)lo + 1;
        return as_number((double)((int64_t)lo + (int64_t)(as_random() % (uint64_t)(span > 0 ? span : 1))));
    }
    return as_real(lo + u * (hi - lo));
}

static as_obj *offset_of(as_obj *params)
{
    as_obj *of = text_param(param(params, "psof")), *in = text_param(param(params, "psin"));
    if (!of || !in)
        return as_error_pending() ? NULL : as_raise(-50, NULL, "offset needs of and in.");
    if (!of->n)
        return as_int(0);
    for (uint32_t i = 0; i + of->n <= in->n; i++) {
        uint32_t k = 0;
        while (k < of->n && tolower(in->bytes[i + k]) == tolower(of->bytes[k]))
            k++;
        if (k == of->n)
            return as_int((int32_t)i + 1);
    }
    return as_int(0);
}

static as_obj *count_of(as_obj *direct, as_obj *params)
{
    as_obj *v = direct ? as_deref(direct) : NULL;
    if (!v)
        return direct ? NULL : as_raise(-50, NULL, "count needs something to count.");
    as_obj *each = param(params, "kocl");
    uint32_t cls = each && each->kind == V_CONST ? (uint32_t)each->code : T("cobj");
    if (v->kind == V_TEXT) {
        if (cls == T("cobj") || cls == T("cha ") || cls == T("TEXT") || cls == T("ctxt"))
            return as_int((int32_t)v->n);
        as_obj ref = { .kind = V_REF, .btype = REF_EVERY, .a = v, .b = as_const(cls) };
        as_obj *l = as_deref(&ref);
        return l ? as_int(l->kind == V_LIST ? (int32_t)l->n : 1) : NULL;
    }
    if (v->kind == V_LIST || v->kind == V_RECORD) {
        if (cls == T("cobj") || v->kind == V_RECORD)
            return as_int((int32_t)v->n);
        as_obj ref = { .kind = V_REF, .btype = REF_EVERY, .a = v, .b = as_const(cls) };
        as_obj *l = as_deref(&ref);
        return l ? as_int((int32_t)l->n) : NULL;
    }
    return as_int(1);
}

static as_obj *clipboard_get(void)
{
    gaddr h = tb_NewHandle(0, false, false), off = tb_NewPtr(4, false, true);
    int32_t n = tb_GetScrap(h, (int32_t)T("TEXT"), off);
    as_obj *r;
    if (n < 0) {
        r = as_cstr("");
    } else {
        uint8_t *b = malloc((size_t)n + 1);
        gget(b, rd32(h), (uint32_t)n);
        r = as_text(b, (size_t)n);
        free(b);
    }
    tb_DisposeHandle(h);
    tb_DisposePtr(off);
    return r;
}

static as_obj *clipboard_set(as_obj *direct)
{
    as_obj *t = text_param(direct ? direct : as_cstr(""));
    if (!t)
        return NULL;
    gaddr p = tb_NewPtr(t->n ? (int32_t)t->n : 1, false, false);
    gput(p, t->bytes, t->n);
    tb_ZeroScrap();
    tb_PutScrap((int32_t)t->n, (int32_t)T("TEXT"), p);
    tb_DisposePtr(p);
    return as_undef();
}

static as_obj *current_date(void)
{
    as_obj *d = as_new(V_DATE);
    machine_advance_ticks();
    d->r = (double)rd32(LM_Time);
    return d;
}

static as_obj *not_understood(as_obj *event, as_obj *target)
{
    char e[64], t[128];
    as_show(event, e, sizeof e);
    as_show(target, t, sizeof t);
    return as_raise(asErrNotHandled, target, "%s doesn't understand the %s message.", t, e);
}

/* Commands that would reach outside the guest: refused. */
static bool outside(uint32_t cls, uint32_t id)
{
    static const char *const refused[][2] = {
        { "syso", "exec" }, /* do shell script */
        { "GURL", "GURL" }, /* open location */
        { "aevt", "mvol" }, /* mount volume */
        { "syso", "dosc" },
        { "ascr", "gdut" },
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        if (cls == T(refused[i][0]) && id == T(refused[i][1]))
            return true;
    return false;
}

as_obj *as_command(as_obj *event, as_obj *direct, as_obj *params, as_obj *target)
{
    uint32_t cls = event->ev[0], id = event->ev[1];
    if (outside(cls, id))
        return as_raise(-1708, event, "Blue Box's AppleScript doesn't let scripts reach outside the classic Mac.");
    /* The direct parameter may name an application (quit application "X"). */
    as_obj *app = root_app(direct);
    if (!app && target && target->kind == V_APP)
        app = target;
    if (direct && direct->kind == V_REF && !as_app_rooted(direct) && cls != T("core")) {
        direct = as_deref(direct);
        if (!direct)
            return NULL;
    }
    switch (cls) {
    case 0x7379736F: /* 'syso': Standard Additions */
        switch (id) {
        case 0x646C6F67: /* 'dlog' */
            return display_dialog(direct, params);
        case 0x72616E64: /* 'rand' */
            return random_number(direct, params);
        case 0x62656570: { /* 'beep' */
            double n = 1;
            bool is_int;
            if (direct && !as_to_number(direct, &n, &is_int))
                n = 1;
            for (int i = 0; i < (int)n && i < 10; i++)
                tb_SysBeep(30);
            return as_undef();
        }
        case 0x6F666673: /* 'offs' */
            return offset_of(params);
        case 0x74746F73: /* 'ttos' */
            return say(direct);
        case 0x63757264: /* 'curd' */
            return current_date();
        case 0x73746466: /* 'stdf' */
            return choose_file(params);
        case 0x63686C74: /* 'chlt' */
            return choose_from_list(direct, params);
        case 0x64656C61: { /* 'dela' */
            double n = 0;
            bool is_int;
            if (direct && as_to_number(direct, &n, &is_int) && n > 0)
                machine_skip_time((uint64_t)(n * 1e9));
            return as_undef();
        }
        }
        break;
    case 0x4A6F6E73: /* 'Jons': Jon's Commands, the clipboard */
        if (id == T("gClp"))
            return clipboard_get();
        if (id == T("pClp"))
            return clipboard_set(direct);
        break;
    case 0x67747170: /* 'gtqp' */
        if (id == T("chlt"))
            return choose_from_list(direct, params);
        break;
    case 0x72647772: /* 'rdwr': file commands, on the guest's files */
        if (id == T("read"))
            return read_file(direct, params);
        if (id == T("open")) {
            as_obj *perm = param(params, "perm");
            bool w = false;
            if (perm)
                as_truth(as_deref(perm), &w);
            int16_t refnum;
            int16_t err = open_file(direct ? direct : as_undef(), w, &refnum);
            if (err == 1)
                return NULL;
            if (err)
                return as_raise(err, direct, "Can't open the file for access (error %d).", err);
            return as_int(refnum);
        }
        if (id == T("clos")) {
            double n;
            bool is_int;
            if (!direct || !as_to_number(direct, &n, &is_int))
                return as_raise(-50, direct, "close access needs a file's reference number.");
            int32_t err = pb_call(tb_PBClose, (int16_t)n, 0, 0, NULL);
            return err ? as_raise(err, direct, "Can't close the file (error %d).", err) : as_undef();
        }
        if (id == T("geof")) {
            double n;
            bool is_int;
            int16_t refnum;
            bool opened = false;
            if (direct && as_to_number(direct, &n, &is_int) && is_int)
                refnum = (int16_t)n;
            else {
                int16_t err = open_file(direct ? direct : as_undef(), false, &refnum);
                if (err)
                    return err == 1 ? NULL : as_raise(err, direct, "Can't open the file (error %d).", err);
                opened = true;
            }
            int32_t eof = 0;
            pb_call(tb_PBGetEOF, refnum, 0, 0, &eof);
            if (opened)
                pb_call(tb_PBClose, refnum, 0, 0, NULL);
            return as_int(eof);
        }
        break;
    case 0x666E6472: /* 'fndr' */
        if (id == T("gstl") && (!app || is_finder(app)))
            return gestalt(direct);
        break;
    case 0x636F7265: /* 'core' */
        if (!app && id == T("cnte"))
            return count_of(direct, params);
        if (!app && id == T("getd") && direct)
            return as_deref(direct);
        break;
    case 0x6D697363: /* 'misc' */
        if (!app && id == T("actv"))
            return as_undef();
        break;
    case 0x61657674: /* 'aevt' */
        if (!app && id == T("quit")) {
            as_quit_requested = true;
            return as_undef();
        }
        break;
    }
    if (app) {
        if (is_finder(app))
            return app_error(app, asErrProcNotFound, "");
        return send_to_app(app, cls, id, direct, params);
    }
    LOGF(LOG_UNIMPL, "AppleScript: command %.4s/%.4s isn't one we have\n", (const char *)&(uint32_t){ __builtin_bswap32(cls) },
         (const char *)&(uint32_t){ __builtin_bswap32(id) });
    return not_understood(event, target ? target : as_undef());
}
