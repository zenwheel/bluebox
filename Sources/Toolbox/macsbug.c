/*
 * MacsBug: a debugger for the program, with MacsBug's commands, for its
 * 68k and PowerPC code.
 *
 * The debugger stops the machine where it is, on the machine's own thread:
 * before an instruction (a breakpoint, a step done, the host's break: the
 * CPUs' trace hooks, set only while one of those is wanted, which also
 * keeps the JITs out of the way), or at a call (a trap break, the
 * program's own Debugger or DebugStr).  Stopped, it runs commands until
 * one goes on, the machine's clock held still as a suspension holds it.
 * The commands come from a script (machine_debug_script: --debugger-script,
 * the tests) or a console (machine_debug_attach, machine_debug_post: the
 * host's window); each is a line of text in and text out
 * (machine_debug_command), so the window is a thin layer over this.
 *
 * Breakpoints are kept here, by address, not written into the program's
 * code.  Traps are native routines, so a step over one (t, s) runs the
 * whole call, shown with its arguments and result as the generator
 * describes them (trap_sig_args); so do the trap records (atr) and traces
 * (att).  The stack crawl (sc) follows A6 links in 68k code and the back
 * chain in PowerPC code, crossing between them where Mixed Mode did.
 *
 * Everything here is the host's (BB_HOST): a snapshot carries none of it.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "machine.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "trap.h"

BB_HOST bool g_macsbug_traps;

void ev_wake(void);

/* ---------------------------------------------------------------------- */
/* Output */

BB_HOST static machine_debug_fn console_fn;
BB_HOST static void *console_ctx;
BB_HOST static bool console_interactive;
BB_HOST static char *kept;        /* output with no console, for machine_debug_take_output */
BB_HOST static size_t kept_len, kept_cap;
BB_HOST static char *capture;     /* machine_debug_command's copy */
BB_HOST static size_t capture_len, capture_cap;

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap * 2 : 4096;
        while (c < *len + n + 1)
            c *= 2;
        char *b = bb_host_realloc(*buf, c);
        if (!b)
            return;
        *buf = b;
        *cap = c;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void emit(const char *s)
{
    size_t n = strlen(s);
    if (capture)
        append(&capture, &capture_len, &capture_cap, s, n);
    if (console_fn)
        console_fn(console_ctx, MACHINE_DEBUG_OUTPUT, s);
    else if (kept_len < (64u << 20)) /* no one taking it: no more than this */
        append(&kept, &kept_len, &kept_cap, s, n);
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(buf);
}

char *machine_debug_take_output(void)
{
    char *s = kept ? kept : bb_host_calloc(1, 1);
    kept = NULL;
    kept_len = kept_cap = 0;
    return s;
}

/* ---------------------------------------------------------------------- */
/* The state */

enum { MAX_BREAKS = 64, MAX_TRAPS = 32, RECORDS = 256, PENDING = 32, MAX_SWITCHES = 32 };

typedef struct {
    gaddr addr;
    uint32_t every;   /* stop at every nth time (0: each time) */
    uint32_t hits;
    char cond[96];    /* stop only when this is true, or "" */
    char cmds[160];   /* commands run when it stops */
    bool temp;        /* gt's, mr's: gone once reached */
    gaddr sp;         /* a temporary one's: only with the stack back at least this high */
} breakpoint;

enum { TW_BREAK = 1, TW_TRACE = 2, TW_HEAP = 4 };
typedef struct {
    int kind;         /* TW_BREAK, TW_TRACE or TW_HEAP */
    char name[64];    /* a routine's name, or "" */
    uint16_t lo, hi;  /* trap words (normalised) */
    char cond[96];
    uint32_t every, hits;
} trap_watch;

typedef struct {
    uint64_t icount;
    bool ppc, done, patched;
    gaddr from;
    char text[200];   /* the call and its arguments */
    char result[64];
} call_record;

typedef struct {
    bool to_ppc;      /* into PowerPC code (from 68k), or into 68k code (from PowerPC) */
    int thread;
    gaddr callee_sp;  /* where the callee's frames end */
    uint32_t pc68, a6, a7;
    uint32_t pc, lr, r1;
} mode_switch;

BB_HOST static breakpoint breaks[MAX_BREAKS];
BB_HOST static int nbreaks;
BB_HOST static trap_watch watches[MAX_TRAPS];
BB_HOST static int nwatches;
BB_HOST static bool recording;
BB_HOST static call_record records[RECORDS];
BB_HOST static unsigned nrecords, record_next;
BB_HOST static mode_switch switches[MAX_SWITCHES];
BB_HOST static int nswitches;
BB_HOST static bool dx_off;      /* dx off: the program's Debugger calls are passed by */

/* Stepping: steps still to take; over (t) or into (s); a step taken
 * finishes at the next instruction (step_next), or at the instruction
 * after a call (over_pc, with the stack back at over_sp). */
BB_HOST static uint32_t step_left;
BB_HOST static bool step_over, step_next, over_wait, over_ppc;
BB_HOST static gaddr over_pc, over_sp;
/* A step's calls, shown when it stops. */
BB_HOST static call_record step_calls[16];
BB_HOST static unsigned nstep_calls, step_calls_dropped;
/* Step spy (ss): a checksum of [spy_lo, spy_hi) watched. */
BB_HOST static bool spy_on;
BB_HOST static gaddr spy_lo, spy_hi;
BB_HOST static uint32_t spy_sum;
/* The instruction the machine goes on from is not broken on again. */
BB_HOST static bool skip_armed, skip_ppc;
BB_HOST static gaddr skip_pc;

BB_HOST static atomic_bool break_wanted;
BB_HOST static bool stopped;
BB_HOST static bool stop_ppc;      /* the processor stopped */
BB_HOST static char stop_why[256];
BB_HOST static bool resume;        /* a command went on */
BB_HOST static gaddr dot;          /* the address last shown: "." */
BB_HOST static gaddr list_next;    /* where il/dm go on from with no address */
BB_HOST static int list_kind;      /* 'i' or 'm' */
BB_HOST static gaddr heap_zone;    /* hx's choice (0: the application zone) */

/* The script's lines, and the console's commands. */
BB_HOST static char *script;
BB_HOST static size_t script_len, script_cap, script_at;
BB_HOST static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
BB_HOST static pthread_cond_t post_cond = PTHREAD_COND_INITIALIZER;
BB_HOST static char *posted;
BB_HOST static size_t posted_len, posted_cap;

/* The trace hooks they were before ours. */
BB_HOST static void (*prev68)(cpu68k *);
BB_HOST static void (*prevppc)(ppc_cpu *);

static void stop(bool ppc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void run_line(const char *line);

/* ---------------------------------------------------------------------- */
/* The processors */

static bool ppc_running(void) { return g_ppc_mode && !mm_level_68k(); }
/* The processor commands look at: the one stopped, else the one running. */
static bool cur_ppc(void) { return stopped ? stop_ppc : ppc_running(); }

static uint32_t cur_pc(void) { return cur_ppc() ? g_ppc.pc : g_cpu->pc; }
static uint32_t cur_sp(void) { return cur_ppc() ? g_ppc.gpr[1] : g_cpu->a[7]; }

/* Whether a is in a PowerPC fragment's code (else 68k code is assumed). */
static bool ppc_code_at(gaddr a)
{
    char f[8], s[8];
    uint32_t so, fo;
    return g_ppc_mode && cfm_describe(a, f, sizeof f, s, 0, &so, &fo);
}

/* ---------------------------------------------------------------------- */
/* Names: routines in 68k code (MacsBug's symbols after each routine's
 * RTS, RTD or JMP (A0)), in PowerPC code (the traceback table after each
 * routine, or the fragment's exports), traps, low memory. */

static bool symbol_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
           ch == '%' || ch == '.' || ch == ' ' || ch == ':' || ch == '~' || ch == '$';
}

/* A MacsBug symbol at a: its name in out and its length in bytes. */
static uint32_t symbol_at(gaddr a, char *out, size_t n)
{
    uint8_t b = rd8(a);
    unsigned len, start;
    if (b > 0x80 && b < 0xA0) {
        len = b - 0x80u;
        start = 1;
    } else if (b == 0x80) {
        len = rd8(a + 1);
        start = 2;
    } else if (b & 0x80 && symbol_char(b & 0x7F) && (b & 0x7F) != ' ') {
        len = 8;
        start = 0;
    } else {
        return 0;
    }
    if (len < 2 || len + 1 > n)
        return 0;
    for (unsigned i = 0; i < len; i++) {
        uint8_t ch = rd8(a + start + i);
        if (start == 0)
            ch &= 0x7F;
        if (!symbol_char(ch))
            return 0;
        out[i] = (char)ch;
    }
    unsigned l = len;
    while (l && out[l - 1] == ' ')
        l--;
    out[l] = 0;
    return l ? start + len : 0;
}

/* The routine around a in 68k code: name, start; searched forward from a
 * for the end of its routine, and back for the end of the one before. */
static bool name_68k(gaddr a, char *name, size_t n, gaddr *start)
{
    a = mem_strip(a) & ~1u;
    if (a < 0x1000 || a >= 0xFFF00000u)
        return false;
    gaddr end = 0;
    for (gaddr p = a; p < a + 0x8000; p += 2) {
        uint16_t w = rd16(p);
        uint32_t at = w == 0x4E75 || w == 0x4ED0 ? p + 2 : w == 0x4E74 ? p + 4 : 0;
        if (at && symbol_at(at, name, n)) {
            end = p;
            break;
        }
    }
    if (!end)
        return false;
    /* Back to the routine's start: after the symbol before it (and its
     * constants' length word), or a LINK A6 nearer. */
    gaddr s = a;
    char other[80];
    for (gaddr p = a; p > a - 0x8000 && p >= 0x1000; p -= 2) {
        uint16_t w = rd16(p - 2);
        if (w == 0x4E56 && p - 2 <= a) {
            s = p - 2;
            break;
        }
        uint32_t len = 0;
        for (gaddr q = p - 2; q > p - 72 && q >= 0x1000; q--)
            if ((len = symbol_at(q, other, sizeof other)) && q + len <= p && q + len + 2 > p) {
                uint16_t prev = rd16((q - 2) & ~1u);
                if (prev == 0x4E75 || prev == 0x4ED0 || rd16((q - 4) & ~1u) == 0x4E74)
                    break;
                len = 0;
            } else {
                len = 0;
            }
        if (len) {
            s = p;
            if (rd16(s) == 0) /* the constants' length */
                s += 2;
            break;
        }
    }
    *start = s;
    return true;
}

/* A PowerPC routine's traceback table (the word of zeros after its last
 * instruction): its name and its start, from the offset it gives. */
static bool name_ppc_traceback(gaddr a, char *name, size_t n, gaddr *start)
{
    a &= ~3u;
    for (gaddr p = a; p < a + 0x10000; p += 4) {
        if (rd32(p) != 0)
            continue;
        uint8_t lang = rd8(p + 5), f1 = rd8(p + 6), f2 = rd8(p + 7);
        uint8_t fixed = rd8(p + 10), fl = rd8(p + 11);
        if (rd8(p + 4) != 0 || lang > 15)
            return false;
        gaddr q = p + 12;
        if (fixed || (fl >> 1))
            q += 4; /* parminfo */
        uint32_t tboff = 0;
        if (f1 & 0x20) {
            tboff = rd32(q);
            q += 4;
        }
        if (f2 & 0x80)
            q += 4; /* hand_mask */
        if (f1 & 0x08)
            q += 4 + 4 * rd32(q); /* ctl_info */
        if (!(f2 & 0x40))
            return false;
        unsigned len = rd16(q);
        if (!len || len >= n || len > 255)
            return false;
        for (unsigned i = 0; i < len; i++) {
            uint8_t ch = rd8(q + 2 + i);
            if (ch < 0x20 || ch >= 0x7F)
                return false;
            name[i] = (char)ch;
        }
        name[len] = 0;
        *start = tboff && tboff <= p ? p - tboff : 0;
        return true;
    }
    return false;
}

/* "Name+$off" for an address, or "" when nothing names it. */
static void describe(gaddr a, char *out_, size_t n)
{
    char name[96];
    gaddr start = 0;
    out_[0] = 0;
    if (ppc_code_at(a)) {
        char frag[64], sym[96];
        uint32_t so = 0, fo = 0;
        if (name_ppc_traceback(a, name, sizeof name, &start) && start && start <= a)
            snprintf(out_, n, "%s+$%04X", name, a - start);
        else if (cfm_describe(a, frag, sizeof frag, sym, sizeof sym, &so, &fo))
            snprintf(out_, n, sym[0] ? "%s+$%04X" : "%s+$%05X", sym[0] ? sym : frag, sym[0] ? so : fo);
        return;
    }
    int ni = native_index_for(a & ~1u);
    if (ni < 0 && (a & 1) == 0)
        ni = native_index_for(a - 2) >= 0 && rd16(a - 4) == CPU_NATIVE_OP ? native_index_for(a - 4) : -1;
    if (ni >= 0 && native_name_at(ni)) {
        snprintf(out_, n, "%s (native)", native_name_at(ni));
        return;
    }
    if (name_68k(a, name, sizeof name, &start) && start <= a)
        snprintf(out_, n, "%s+$%04X", name, a - start);
}

/* A trap's name for the disassembler: the routine a dispatcher's
 * selector picks when the instruction is the one stopped at. */
static const char *aline_name(uint16_t op)
{
    const char *n = trap_name(op);
    return n && n[0] != '_' ? n : NULL;
}

/* Low-memory globals, case aside. */
static bool lowmem_named(const char *s, uint32_t *v)
{
    for (unsigned i = 0; i < lowmem_count; i++)
        if (!strcasecmp(lowmem_table[i].name, s)) {
            *v = lowmem_table[i].addr;
            return true;
        }
    return false;
}

/* A trap by name: its trap word (dispatched routines: the dispatcher's). */
static bool trap_named(const char *s, uint16_t *tw)
{
    if (s[0] == '_')
        s++;
    for (unsigned i = 0; i < 256; i++)
        if (os_trap_names[i] && !strcasecmp(os_trap_names[i], s)) {
            *tw = (uint16_t)(0xA000 | i);
            return true;
        }
    for (unsigned i = 0; i < 1024; i++)
        if (tb_trap_names[i] && !strcasecmp(tb_trap_names[i], s)) {
            *tw = (uint16_t)(0xA800 | i);
            return true;
        }
    for (unsigned i = 0; i < dispatch_sig_count; i++)
        if (!strcasecmp(dispatch_sigs[i].name, s)) {
            *tw = dispatch_sigs[i].trap;
            return true;
        }
    return false;
}

static gaddr trap_address(uint16_t tw)
{
    return tw & 0x0800 ? rd32(TB_TRAP_TABLE + (tw & 0x3FFu) * 4) : rd32(OS_TRAP_TABLE + (tw & 0xFFu) * 4);
}

/* A 68k routine named name in the program's code: each CODE resource
 * loaded is searched for the symbol. */
static bool code_symbol(const char *name, gaddr *addr)
{
    gaddr zones[2] = { rd32(LM_ApplZone), rd32(LM_SysZone) };
    char sym[96];
    for (int z = 0; z < 2; z++) {
        if (!zones[z] || (z && zones[z] == zones[0]))
            continue;
        gaddr b = 0;
        mm_block blk;
        while (mm_zone_block(zones[z], &b, &blk)) {
            if (blk.damaged)
                break;
            if (blk.use != 2)
                continue;
            uint32_t type;
            int16_t id, ref;
            if (!rsrc_info_of(blk.mp, &type, &id, sym, 0, &ref) || type != 0x434F4445) /* 'CODE' */
                continue;
            for (gaddr p = blk.data; p + 4 < blk.data + blk.lsize; p += 2) {
                uint16_t w = rd16(p);
                uint32_t at = w == 0x4E75 || w == 0x4ED0 ? p + 2 : w == 0x4E74 ? p + 4 : 0;
                if (at && symbol_at(at, sym, sizeof sym) && !strcasecmp(sym, name)) {
                    gaddr start;
                    if (name_68k(p, sym, sizeof sym, &start)) {
                        *addr = start;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/* Registers */

typedef struct {
    const char *name;
    int ppc;   /* 0: 68k, 1: PowerPC, 2: whichever is current */
    int kind;  /* 'd', 'a', 'r' (GPR), or a special */
    int n;
} reg_name;

static const reg_name special_regs[] = {
    { "pc", 2, 'p', 0 }, { "sp", 2, 's', 0 }, { "sr", 0, 'S', 0 },  { "usp", 0, 'u', 0 }, { "isp", 0, 'i', 0 },
    { "lr", 1, 'l', 0 }, { "ctr", 1, 'c', 0 }, { "cr", 1, 'C', 0 }, { "xer", 1, 'x', 0 }, { "fpscr", 1, 'f', 0 },
    { "vbr", 0, 'v', 0 }, { "ccr", 0, 'K', 0 }, { "pc68", 0, 'P', 0 }, { "pcppc", 1, 'Q', 0 },
};

static bool reg_lookup(const char *s, reg_name *r)
{
    size_t n = strlen(s);
    if (n == 2 && (tolower((unsigned char)s[0]) == 'd' || tolower((unsigned char)s[0]) == 'a') && s[1] >= '0' &&
        s[1] <= '7') {
        *r = (reg_name){ s, 0, tolower((unsigned char)s[0]), s[1] - '0' };
        return true;
    }
    if ((n == 2 || n == 3) && tolower((unsigned char)s[0]) == 'r' && isdigit((unsigned char)s[1]) &&
        (n == 2 || isdigit((unsigned char)s[2]))) {
        int k = atoi(s + 1);
        if (k < 32) {
            *r = (reg_name){ s, 1, 'r', k };
            return true;
        }
    }
    for (size_t i = 0; i < sizeof special_regs / sizeof special_regs[0]; i++)
        if (!strcasecmp(special_regs[i].name, s)) {
            *r = special_regs[i];
            return true;
        }
    return false;
}

static uint32_t *reg_slot(const reg_name *r)
{
    cpu68k *c = g_cpu;
    bool ppc = r->ppc == 2 ? cur_ppc() : r->ppc == 1;
    switch (r->kind) {
    case 'd': return &c->d[r->n];
    case 'a': return &c->a[r->n];
    case 'r': return &g_ppc.gpr[r->n];
    case 'p': return ppc ? &g_ppc.pc : &c->pc;
    case 's': return ppc ? &g_ppc.gpr[1] : &c->a[7];
    case 'u': return &c->usp;
    case 'i': return &c->isp;
    case 'l': return &g_ppc.lr;
    case 'c': return &g_ppc.ctr;
    case 'C': return &g_ppc.cr;
    case 'x': return &g_ppc.xer;
    case 'f': return &g_ppc.fpscr;
    case 'v': return &c->vbr;
    case 'P': return &c->pc;
    case 'Q': return &g_ppc.pc;
    }
    return NULL;
}

static uint32_t reg_get(const reg_name *r)
{
    if (r->kind == 'S')
        return g_cpu->sr;
    if (r->kind == 'K')
        return g_cpu->sr & 0xFF;
    uint32_t *p = reg_slot(r);
    return p ? *p : 0;
}

static void reg_set(const reg_name *r, uint32_t v)
{
    if (r->kind == 'S') {
        cpu_set_sr(g_cpu, (uint16_t)v);
        return;
    }
    if (r->kind == 'K') {
        g_cpu->sr = (uint16_t)((g_cpu->sr & 0xFF00) | (v & 0xFF));
        return;
    }
    uint32_t *p = reg_slot(r);
    if (p)
        *p = v;
}

/* ---------------------------------------------------------------------- */
/* Expressions: numbers in hex unless #decimal, registers, @ (the long at)
 * and ^ after (the same), 'TYPE', ".", names (low memory, traps, routines),
 * + - * / % & | ~ ! and the comparisons for conditions. */

typedef struct {
    const char *s;
    const char *err;
    bool relational; /* a comparison was made: a condition, not a count */
} parser;

static void skip_ws(parser *p)
{
    while (*p->s == ' ' || *p->s == '\t')
        p->s++;
}

static uint32_t parse_expr(parser *p);

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '%'; }

static bool lookup_name(const char *w, uint32_t *v)
{
    reg_name r;
    if (reg_lookup(w, &r)) {
        *v = reg_get(&r);
        return true;
    }
    if (lowmem_named(w, v))
        return true;
    uint16_t tw;
    if (trap_named(w, &tw)) {
        *v = trap_address(tw);
        return true;
    }
    if (g_ppc_mode && cfm_symbol_code(w, v))
        return true;
    return code_symbol(w, v);
}

static uint32_t parse_primary(parser *p)
{
    skip_ws(p);
    const char *s = p->s;
    if (*s == '(') {
        p->s++;
        uint32_t v = parse_expr(p);
        skip_ws(p);
        if (*p->s == ')')
            p->s++;
        else if (!p->err)
            p->err = "missing )";
        return v;
    }
    if (*s == '\'') {
        uint32_t v = 0;
        s++;
        while (*s && *s != '\'')
            v = v << 8 | (uint8_t)*s++;
        if (*s == '\'')
            s++;
        p->s = s;
        return v;
    }
    if (*s == '#') {
        p->s = s + 1;
        if (!isdigit((unsigned char)*p->s)) {
            p->err = "a number after #";
            return 0;
        }
        return (uint32_t)strtoul(p->s, (char **)&p->s, 10);
    }
    if (*s == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        p->s = s + (*s == '$' ? 1 : 2);
        if (!isxdigit((unsigned char)*p->s)) {
            p->err = "a hex number";
            return 0;
        }
        return (uint32_t)strtoul(p->s, (char **)&p->s, 16);
    }
    if (*s == '.' && !word_char(s[1])) {
        p->s = s + 1;
        return dot;
    }
    if (word_char(*s) || *s == '.') {
        char w[96];
        size_t n = 0;
        while ((word_char(*s) || *s == '.') && n < sizeof w - 1)
            w[n++] = *s++;
        w[n] = 0;
        p->s = s;
        bool hex = true;
        for (size_t i = 0; i < n; i++)
            hex = hex && isxdigit((unsigned char)w[i]);
        reg_name r;
        if (reg_lookup(w, &r))
            return reg_get(&r);
        if (hex)
            return (uint32_t)strtoul(w, NULL, 16);
        uint32_t v;
        if (lookup_name(w, &v))
            return v;
        p->err = "unknown name";
        return 0;
    }
    p->err = *s ? "can't read the expression" : "an expression is missing";
    return 0;
}

static uint32_t parse_unary(parser *p)
{
    skip_ws(p);
    uint32_t v;
    switch (*p->s) {
    case '-': p->s++; v = (uint32_t)-parse_unary(p); break;
    case '~': p->s++; v = ~parse_unary(p); break;
    case '!': p->s++; v = !parse_unary(p); break;
    case '@': p->s++; v = rd32(mem_strip(parse_unary(p))); break;
    case '+': p->s++; v = parse_unary(p); break;
    default: v = parse_primary(p); break;
    }
    skip_ws(p);
    while (*p->s == '^') {
        p->s++;
        v = rd32(mem_strip(v));
        skip_ws(p);
    }
    return v;
}

static uint32_t parse_term(parser *p)
{
    uint32_t v = parse_unary(p);
    for (;;) {
        skip_ws(p);
        char op = *p->s;
        if (op != '*' && op != '/' && op != '%' && op != '&')
            return v;
        p->s++;
        uint32_t w = parse_unary(p);
        if (op == '*')
            v *= w;
        else if (op == '&')
            v &= w;
        else if (!w)
            p->err = "division by zero";
        else
            v = op == '/' ? v / w : v % w;
    }
}

static uint32_t parse_sum(parser *p)
{
    uint32_t v = parse_term(p);
    for (;;) {
        skip_ws(p);
        char op = *p->s;
        if (op != '+' && op != '-' && op != '|')
            return v;
        p->s++;
        uint32_t w = parse_term(p);
        v = op == '+' ? v + w : op == '-' ? v - w : v | w;
    }
}

static uint32_t parse_expr(parser *p)
{
    uint32_t v = parse_sum(p);
    skip_ws(p);
    const char *s = p->s;
    int op = 0;
    if (s[0] == '=' && s[1] == '=')
        op = 'e', p->s += 2;
    else if (s[0] == '=')
        op = 'e', p->s += 1;
    else if (s[0] == '!' && s[1] == '=')
        op = 'n', p->s += 2;
    else if (s[0] == '<' && s[1] == '>')
        op = 'n', p->s += 2;
    else if (s[0] == '<' && s[1] == '=')
        op = 'l', p->s += 2;
    else if (s[0] == '>' && s[1] == '=')
        op = 'g', p->s += 2;
    else if (s[0] == '<')
        op = '<', p->s += 1;
    else if (s[0] == '>')
        op = '>', p->s += 1;
    if (!op)
        return v;
    p->relational = true;
    uint32_t w = parse_sum(p);
    switch (op) {
    case 'e': return v == w;
    case 'n': return v != w;
    case 'l': return v <= w;
    case 'g': return v >= w;
    case '<': return v < w;
    default: return v > w;
    }
}

/* One expression from *s (moved past it); false with a message if bad. */
static bool eval(const char **s, uint32_t *v, bool *relational)
{
    parser p = { *s, NULL, false };
    *v = parse_expr(&p);
    if (p.err) {
        out("  %s: %.40s\n", p.err, *s);
        return false;
    }
    *s = p.s;
    if (relational)
        *relational = p.relational;
    return true;
}

static bool eval_str(const char *s, uint32_t *v)
{
    const char *t = s;
    return eval(&t, v, NULL);
}

/* The arguments of a command line: words separated by spaces or commas,
 * an expression's own spaces allowed inside parentheses or quotes. */
static int split_args(const char *s, char args[][160], int max)
{
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == ',')
            s++;
        if (!*s || n == max)
            return n;
        size_t k = 0;
        int depth = 0;
        char q = 0;
        while (*s && (q || depth || (*s != ' ' && *s != '\t' && *s != ','))) {
            if (q && *s == q)
                q = 0;
            else if (!q && (*s == '\'' || *s == '"'))
                q = *s;
            else if (!q && *s == '(')
                depth++;
            else if (!q && *s == ')' && depth)
                depth--;
            if (k < 159)
                args[n][k++] = *s;
            s++;
        }
        args[n][k] = 0;
        /* "a + b": an operator joins the words around it. */
        while (*s == ' ') {
            const char *t = s;
            while (*t == ' ')
                t++;
            bool joins = strchr("+-*/&|=<>!", *t) && *t && k && !strchr("+-*/&|=<>!", args[n][k - 1]);
            bool ends_op = k && strchr("+-*/&|=<>", args[n][k - 1]);
            if (!joins && !ends_op)
                break;
            while (*t && *t != ' ' && *t != ',' && k < 159)
                args[n][k++] = *t++;
            args[n][k] = 0;
            s = t;
        }
        n++;
    }
}

/* ---------------------------------------------------------------------- */
/* Arguments and results, as the generator describes them */

typedef struct {
    const char *name;
    const char *args;
} named_args;

BB_HOST static named_args *by_name;
BB_HOST static unsigned nby_name;

static int cmp_named(const void *a, const void *b)
{
    return strcmp(((const named_args *)a)->name, ((const named_args *)b)->name);
}

/* A routine's description by name (a PowerPC call's). */
static const char *args_for(const char *name)
{
    if (!by_name) {
        by_name = bb_host_malloc((trap_sig_count + dispatch_sig_count) * sizeof *by_name);
        if (!by_name)
            return NULL;
        for (unsigned i = 0; i < trap_sig_count; i++)
            by_name[nby_name++] = (named_args){ trap_sigs[i].name, trap_sig_args[i] };
        for (unsigned i = 0; i < dispatch_sig_count; i++)
            by_name[nby_name++] = (named_args){ dispatch_sigs[i].name, dispatch_sig_args[i] };
        qsort(by_name, nby_name, sizeof *by_name, cmp_named);
    }
    named_args key = { name, NULL };
    const named_args *f = bsearch(&key, by_name, nby_name, sizeof *by_name, cmp_named);
    return f ? f->args : NULL;
}

/* A value shown by its kind. */
static void show_value(char kind, unsigned size, uint32_t v, char *o, size_t n)
{
    if (size == 1)
        v &= 0xFF;
    else if (size == 2)
        v &= 0xFFFF;
    int32_t sv = size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
    switch (kind) {
    case 'i':
    case 'e': snprintf(o, n, "%d", sv); return;
    case 'b': snprintf(o, n, "%s", v ? "true" : "false"); return;
    case 't': {
        char t[5];
        for (int i = 0; i < 4; i++) {
            uint8_t c = (uint8_t)(v >> (24 - 8 * i));
            t[i] = c >= 0x20 && c < 0x7F ? (char)c : '.';
        }
        t[4] = 0;
        snprintf(o, n, "'%s'", t);
        return;
    }
    case 'P': snprintf(o, n, "(%d,%d)", (int16_t)(v >> 16), (int16_t)v); return;
    case 's':
    case 'c': {
        gaddr a = mem_strip(v);
        char s[48];
        size_t k = 0;
        if (a >= 0x100 && v != 0xFFFFFFFFu) {
            unsigned len = kind == 's' ? rd8(a) : 255;
            for (unsigned i = 0; i < len && k < 40; i++) {
                uint8_t ch = rd8(a + (kind == 's') + i);
                if (kind == 'c' && !ch)
                    break;
                s[k++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '.';
            }
            s[k] = 0;
            snprintf(o, n, "$%08X \"%s\"%s", v, s, kind == 's' && len > 40 ? "..." : "");
        } else {
            snprintf(o, n, "$%08X", v);
        }
        return;
    }
    case 'r': {
        gaddr a = mem_strip(v);
        if (a >= 0x100)
            snprintf(o, n, "$%08X {%d,%d,%d,%d}", v, (int16_t)rd16(a), (int16_t)rd16(a + 2), (int16_t)rd16(a + 4),
                     (int16_t)rd16(a + 6));
        else
            snprintf(o, n, "$%08X", v);
        return;
    }
    default:
        snprintf(o, n, size == 1 ? "$%02X" : size == 2 ? "$%04X" : "$%08X", v);
        return;
    }
}

/* One argument of a description: name, kind, size, register (or NULL). */
typedef struct {
    char name[40];
    char kind;
    unsigned size;
    char reg[24];
} arg_desc;

static int parse_desc(const char *d, char *conv, arg_desc *ret, arg_desc *args, int max)
{
    *conv = d[0];
    memset(ret, 0, sizeof *ret);
    const char *r = strchr(d, '|');
    if (!r)
        return 0;
    r++;
    const char *a = strchr(r, '|');
    if (!a)
        return 0;
    if (a > r) {
        ret->kind = r[0];
        ret->size = (unsigned)atoi(r + 1);
        const char *at = memchr(r, '@', (size_t)(a - r));
        if (at)
            snprintf(ret->reg, sizeof ret->reg, "%.*s", (int)(a - at - 1), at + 1);
    }
    a++;
    int n = 0;
    while (*a && n < max) {
        const char *e = strchr(a, ',');
        size_t len = e ? (size_t)(e - a) : strlen(a);
        char item[80];
        snprintf(item, sizeof item, "%.*s", (int)len, a);
        char *eq = strchr(item, '=');
        if (eq) {
            *eq = 0;
            arg_desc *x = &args[n++];
            memset(x, 0, sizeof *x);
            snprintf(x->name, sizeof x->name, "%s", item);
            x->kind = eq[1];
            x->size = (unsigned)atoi(eq + 2);
            char *at = strchr(eq + 1, '@');
            if (at)
                snprintf(x->reg, sizeof x->reg, "%s", at + 1);
        }
        a += len + (e ? 1 : 0);
    }
    return n;
}

static uint32_t reg68(const char *reg, const cpu68k *c, uint16_t tw, bool *skip)
{
    *skip = false;
    if (!strncmp(reg, "Out<", 4)) {
        *skip = true;
        return 0;
    }
    if (!strncmp(reg, "InOut<", 6))
        reg += 6;
    if (!strncmp(reg, "TrapBit<", 8)) {
        const char *b = reg + 8;
        uint32_t bit = !strncmp(b, "SYSBIT", 6) ? 0x400 : !strncmp(b, "CLRBIT", 6) ? 0x200 : (uint32_t)strtoul(b, NULL, 0);
        return (tw & bit) != 0;
    }
    if (!strncmp(reg, "D0LowWord", 9))
        return c->d[0] & 0xFFFF;
    if (!strncmp(reg, "D0HighWord", 10))
        return c->d[0] >> 16;
    if ((reg[0] == 'D' || reg[0] == 'A') && reg[1] >= '0' && reg[1] <= '7')
        return reg[0] == 'D' ? c->d[reg[1] - '0'] : c->a[reg[1] - '0'];
    *skip = true;
    return 0;
}

/* "Name(arg, arg)" for a call about to be made.  68k: the CPU at the trap
 * (sel_bytes of selector on the stack, base the stack's arguments' end);
 * PowerPC: r3 on and the caller's parameter area. */
static void format_call(const char *name, const char *desc, bool ppc, uint16_t tw, unsigned sel_bytes, char *o,
                        size_t n)
{
    size_t k = (size_t)snprintf(o, n, "%s", name);
    if (!desc) {
        if (k < n && !ppc)
            snprintf(o + k, n - k, " (%04X)", tw);
        return;
    }
    char conv;
    arg_desc ret, args[24];
    int na = parse_desc(desc, &conv, &ret, args, 24);
    const cpu68k *c = g_cpu;
    /* Pascal: the last argument on top; the arguments' offsets from it. */
    gaddr base = c->a[7] + sel_bytes + ((tw & 0x0C00) == 0x0C00 ? 4 : 0);
    uint32_t below = 0;
    for (int i = 0; i < na; i++)
        below += args[i].size <= 2 ? 2 : 4;
    snprintf(o + k, n - k, "(");
    k = strlen(o);
    int ppc_arg = 0;
    for (int i = 0; i < na && k < n; i++) {
        uint32_t v = 0;
        bool skip = false;
        if (ppc) {
            /* PowerPC's exports have no trap bits: NewPtrSys is its own. */
            if (!strncmp(args[i].reg, "TrapBit<", 8))
                continue;
            uint32_t j = (uint32_t)ppc_arg++;
            v = j < 8 ? g_ppc.gpr[3 + j] : rd32(g_ppc.gpr[1] + 24 + 4 * j);
        } else if (conv == 'P') {
            unsigned slot = args[i].size <= 2 ? 2 : 4;
            below -= slot;
            gaddr at = base + below;
            v = slot == 4 ? rd32(at) : args[i].size == 1 ? rd8(at) : rd16(at);
        } else if (conv == 'C') {
            v = rd32(base + 4 * (uint32_t)i);
        } else {
            v = reg68(args[i].reg, c, tw, &skip);
        }
        if (skip)
            continue;
        char val[96];
        show_value(args[i].kind, args[i].size, v, val, sizeof val);
        snprintf(o + k, n - k, "%s%s=%s", o[k - 1] == '(' ? "" : ", ", args[i].name, val);
        k = strlen(o);
    }
    if (k < n)
        snprintf(o + k, n - k, ")");
}

/* The result of a call just made, "" if none. */
static void format_result(const char *desc, bool ppc, char *o, size_t n)
{
    o[0] = 0;
    if (!desc)
        return;
    char conv;
    arg_desc ret, args[24];
    int na = parse_desc(desc, &conv, &ret, args, 24);
    const cpu68k *c = g_cpu;
    size_t k = 0;
    if (ret.kind) {
        uint32_t v;
        if (ppc)
            v = g_ppc.gpr[3];
        else if (!strcmp(ret.reg, "D0Minus1Boolean"))
            v = c->d[0] == 0;
        else if (ret.reg[0] == 'A' || ret.reg[0] == 'D')
            v = ret.reg[0] == 'D' ? c->d[ret.reg[1] - '0'] : c->a[ret.reg[1] - '0'];
        else if (conv == 'C')
            v = c->d[0];
        else
            v = ret.size == 1 ? rd8(c->a[7]) : ret.size == 2 ? rd16(c->a[7]) : rd32(c->a[7]);
        show_value(ret.kind, ret.size, v, o, n);
        k = strlen(o);
    }
    /* Results through registers (Out<A0> ...). */
    for (int i = 0; i < na && !ppc && k < n; i++) {
        const char *r = args[i].reg;
        if (strncmp(r, "Out<", 4) && strncmp(r, "InOut<", 6))
            continue;
        r = strchr(r, '<') + 1;
        uint32_t v = r[0] == 'D' ? c->d[r[1] - '0'] : c->a[r[1] - '0'];
        char val[64];
        show_value(args[i].kind, args[i].size, v, val, sizeof val);
        snprintf(o + k, n - k, "%s%s=%s", k ? ", " : "", args[i].name, val);
        k = strlen(o);
    }
}

/* ---------------------------------------------------------------------- */
/* The hooks: before each instruction, while wanted */

static uint32_t checksum(gaddr lo, gaddr hi)
{
    uint32_t s = 0;
    for (gaddr a = lo; a < hi; a++)
        s = (s << 1 | s >> 31) ^ rd8(a);
    return s;
}

static bool hooks_wanted(void) { return nbreaks || step_left || atomic_load(&break_wanted) || spy_on; }

static void hook68(cpu68k *c);
static void hookppc(ppc_cpu *c);

void macsbug_retrace(void)
{
    bool want = hooks_wanted();
    cpu68k *c = g_cpu;
    if (c) {
        if (c->trace != hook68) {
            prev68 = c->trace;
            if (want)
                c->trace = hook68;
        } else if (!want) {
            c->trace = prev68;
        }
    }
    if (g_ppc.trace != hookppc) {
        prevppc = g_ppc.trace;
        if (want)
            g_ppc.trace = hookppc;
    } else if (!want) {
        g_ppc.trace = prevppc;
    }
}

static void update_traps(void);

/* Whether the instruction at pc is a call to be stepped over: JSR, BSR
 * or a trap (68k); bl, bctrl, blrl (PowerPC). */
static uint32_t call_length(bool ppc, gaddr pc)
{
    if (ppc) {
        uint32_t op = rd32(pc);
        unsigned primary = op >> 26;
        if ((primary == 18 || primary == 16) && (op & 1))
            return 4;
        if (primary == 19 && (op & 1) && (((op >> 1) & 0x3FF) == 16 || ((op >> 1) & 0x3FF) == 528))
            return 4;
        return 0;
    }
    uint16_t op = rd16(pc);
    if ((op & 0xF000) == 0xA000)
        return 2;
    if ((op & 0xFF00) == 0x6100) { /* BSR */
        uint8_t d = op & 0xFF;
        return d == 0 ? 4 : d == 0xFF ? 6 : 2;
    }
    if ((op & 0xFFC0) == 0x4E80) { /* JSR <ea>: its length from the mode */
        char buf[96];
        return (uint32_t)cpu_disasm(pc, buf, sizeof buf);
    }
    return 0;
}

/* A step about to be taken at the instruction stopped at (or reached). */
static void step_begin(bool ppc, gaddr pc, gaddr sp)
{
    uint32_t len = step_over ? call_length(ppc, pc) : 0;
    over_wait = len != 0;
    step_next = !over_wait;
    if (over_wait) {
        over_pc = pc + len;
        over_sp = sp;
        over_ppc = ppc;
    }
}

/* The checks before an instruction: true to stop (why said). */
static bool check(bool ppc, gaddr pc, gaddr sp, char *why, size_t n)
{
    bool skip = skip_armed && skip_ppc == ppc && skip_pc == pc;
    skip_armed = false;
    if (atomic_exchange(&break_wanted, false)) {
        snprintf(why, n, "User break");
        return true;
    }
    if (step_left) {
        bool done = false;
        if (step_next)
            done = true;
        else if (over_wait && over_ppc == ppc && pc == over_pc && sp >= over_sp)
            done = true;
        if (done) {
            if (--step_left == 0) {
                step_next = over_wait = false;
                snprintf(why, n, "Step");
                return true;
            }
            step_begin(ppc, pc, sp);
        }
    }
    if (spy_on) {
        uint32_t s = checksum(spy_lo, spy_hi);
        if (s != spy_sum) {
            spy_sum = s;
            snprintf(why, n, "Step spy: memory from $%08X to $%08X changed", spy_lo, spy_hi);
            return true;
        }
    }
    if (skip)
        return false;
    for (int i = 0; i < nbreaks; i++) {
        breakpoint *b = &breaks[i];
        if (b->addr != pc || (b->temp && sp < b->sp))
            continue;
        if (b->cond[0]) {
            uint32_t v;
            if (!eval_str(b->cond, &v) || !v)
                continue;
        }
        b->hits++;
        if (b->every > 1 && b->hits % b->every)
            continue;
        if (b->temp) {
            snprintf(why, n, b->cmds[0] ? "%s" : "Temporary breakpoint", b->cmds);
            breaks[i] = breaks[--nbreaks];
        } else {
            snprintf(why, n, "Breakpoint at $%08X%s%s", pc, b->every > 1 ? " (count reached)" : "",
                     b->cond[0] ? " (condition true)" : "");
        }
        return true;
    }
    return false;
}

/* A breakpoint's own commands, run when it stops there. */
static const char *break_cmds(gaddr pc)
{
    for (int i = 0; i < nbreaks; i++)
        if (breaks[i].addr == pc && !breaks[i].temp && breaks[i].cmds[0])
            return breaks[i].cmds;
    return NULL;
}

static void hook68(cpu68k *c)
{
    if (prev68)
        prev68(c);
    char why[256];
    if (check(false, c->pc, c->a[7], why, sizeof why))
        stop(false, "%s", why);
}

static void hookppc(ppc_cpu *c)
{
    if (prevppc)
        prevppc(c);
    char why[256];
    if (check(true, c->pc, c->gpr[1], why, sizeof why))
        stop(true, "%s", why);
}

void macsbug_poll(void)
{
    if (atomic_load(&break_wanted) && (!g_cpu || g_cpu->trace != hook68))
        macsbug_retrace();
}

void machine_debug_break(void)
{
    atomic_store(&break_wanted, true);
    ev_wake(); /* a program asleep in WaitNextEvent wakes to be stopped */
}

bool machine_debug_stopped(void) { return stopped; }

/* ---------------------------------------------------------------------- */
/* Traps: breaks, traces, records, a step's calls */

static uint16_t norm_trap(uint16_t tw) { return tw & 0x0800 ? (uint16_t)(0xA800 | (tw & 0x3FF)) : (uint16_t)(0xA000 | (tw & 0xFF)); }

static bool watch_matches(const trap_watch *w, uint16_t tw, const char *routine, const char *dispatcher)
{
    if (w->name[0])
        return (routine && !strcasecmp(w->name, routine)) || (dispatcher && !strcasecmp(w->name, dispatcher));
    if (!tw)
        return w->lo == 0xA000 && w->hi == 0xAFFF; /* a PowerPC call: only "every trap" */
    uint16_t t = norm_trap(tw);
    return t >= w->lo && t <= w->hi;
}

/* Pending calls, their results to come. */
typedef struct {
    const char *desc;
    bool ppc;
    int record;   /* index in records, or -1 */
    int step;     /* index in step_calls, or -1 */
} pending_call;

BB_HOST static pending_call pending[PENDING];
BB_HOST static int npending;
BB_HOST static int step_depth; /* the trap depth a step began at */

static bool step_reporting(void) { return step_left != 0; }

static void update_traps(void)
{
    g_macsbug_traps = nwatches || recording || step_reporting() || debugger_stops();
}

/* A call about to be made: true to stop at it (why said). */
static bool call_enter(bool ppc, uint16_t tw, const char *routine, const char *dispatcher, const char *desc,
                       unsigned sel_bytes, gaddr from, char *why, size_t wn)
{
    char text[200];
    text[0] = 0;
    bool want_text = recording || step_reporting();
    bool stop_here = false;
    for (int i = 0; i < nwatches; i++) {
        trap_watch *w = &watches[i];
        if (!watch_matches(w, tw, routine, dispatcher))
            continue;
        if (w->cond[0]) {
            uint32_t v;
            if (!eval_str(w->cond, &v) || !v)
                continue;
        }
        if (!text[0])
            format_call(routine ? routine : "?", desc, ppc, tw, sel_bytes, text, sizeof text);
        if (w->kind == TW_TRACE) {
            out("%c %08X  %s\n", ppc ? 'P' : 'A', from, text);
        } else if (w->kind == TW_HEAP) {
            gaddr zs[2] = { rd32(LM_SysZone), rd32(LM_ApplZone) };
            for (int z = 0; z < 2; z++)
                if (zs[z] && !mm_check_zone(zs[z])) {
                    snprintf(why, wn, "Heap check failed before %s (zone $%08X; see the log)", routine ? routine : "?",
                             zs[z]);
                    stop_here = true;
                }
        } else {
            w->hits++;
            if (w->every <= 1 || w->hits % w->every == 0) {
                snprintf(why, wn, "%s break at %s", ppc ? "PowerPC call" : "A-trap", text);
                stop_here = true;
            }
        }
    }
    if (npending < PENDING) {
        pending_call *p = &pending[npending++];
        p->desc = desc;
        p->ppc = ppc;
        p->record = p->step = -1;
        if (want_text && !text[0])
            format_call(routine ? routine : "?", desc, ppc, tw, sel_bytes, text, sizeof text);
        if (recording) {
            call_record *r = &records[record_next];
            p->record = (int)record_next;
            record_next = (record_next + 1) % RECORDS;
            if (nrecords < RECORDS)
                nrecords++;
            memset(r, 0, sizeof *r);
            r->icount = g_cpu->icount;
            r->ppc = ppc;
            r->from = from;
            snprintf(r->text, sizeof r->text, "%s", text);
        }
        if (step_reporting() && npending - 1 == step_depth) {
            if (nstep_calls < sizeof step_calls / sizeof step_calls[0]) {
                call_record *r = &step_calls[nstep_calls];
                p->step = (int)nstep_calls++;
                memset(r, 0, sizeof *r);
                r->ppc = ppc;
                r->from = from;
                snprintf(r->text, sizeof r->text, "%s", text);
            } else {
                step_calls_dropped++;
            }
        }
    }
    return stop_here;
}

static void call_done(bool ppc)
{
    if (!npending)
        return;
    pending_call *p = &pending[--npending];
    if (p->record < 0 && p->step < 0)
        return;
    char res[64];
    format_result(p->desc, ppc, res, sizeof res);
    if (p->record >= 0) {
        records[p->record].done = true;
        snprintf(records[p->record].result, sizeof records[p->record].result, "%s", res);
    }
    if (p->step >= 0) {
        step_calls[p->step].done = true;
        snprintf(step_calls[p->step].result, sizeof step_calls[p->step].result, "%s", res);
    }
}

/* Whether a 68k trap is one of the program's own debugger calls MacsBug
 * takes: kind and message. */
static bool debugger_trap(cpu68k *c, uint16_t tw, const char **kind, char *msg, size_t n, bool *has_msg)
{
    *has_msg = false;
    uint16_t t = norm_trap(tw);
    if (t == 0xA9FF) {
        *kind = "Debugger";
        return true;
    }
    if (t == 0xABFF) {
        *kind = "DebugStr";
        pstr_to_c(rd32(c->a[7]), msg, n);
        *has_msg = true;
        return true;
    }
    if (t == 0xA9C9) {
        int16_t code = (int16_t)c->d[0];
        if (code == -490) {
            *kind = "SysBreak";
            return true;
        }
        if (code == -491) {
            *kind = "SysBreakStr";
            pstr_to_c(rd32(c->a[7]), msg, n);
            *has_msg = true;
            return true;
        }
        if (code == -492) {
            *kind = "SysBreakFunc";
            snprintf(msg, n, "routine %08X", rd32(c->a[7]));
            *has_msg = true;
            return true;
        }
    }
    return false;
}

/* Stopped at a call and stepped from there: the call is the step's, its
 * result shown when the step ends. */
static void adopt_call(const char *routine, const char *desc, bool ppc, uint16_t tw, unsigned sel_bytes, gaddr from)
{
    if (!step_left || !npending)
        return;
    pending_call *p = &pending[npending - 1];
    step_depth = npending - 1;
    if (p->step >= 0 || nstep_calls >= sizeof step_calls / sizeof step_calls[0])
        return;
    call_record *r = &step_calls[nstep_calls];
    p->step = (int)nstep_calls++;
    memset(r, 0, sizeof *r);
    r->ppc = ppc;
    r->from = from;
    format_call(routine, desc, ppc, tw, sel_bytes, r->text, sizeof r->text);
}

bool macsbug_trap(cpu68k *c, uint16_t tw)
{
    gaddr at = c->op_pc;
    if (debugger_stops() && !dx_off) {
        const char *kind;
        char msg[256];
        bool has_msg;
        if (debugger_trap(c, tw, &kind, msg, sizeof msg, &has_msg)) {
            /* MacsBug takes the trap: the machine goes on after it, a
             * string's argument popped. */
            if (has_msg)
                c->a[7] += 4;
            debugger_count(kind, has_msg ? msg : NULL);
            if (has_msg)
                stop(false, "%s: %s", kind, msg);
            else
                stop(false, "%s", kind);
            return true;
        }
    }
    const char *desc;
    unsigned sel_bytes;
    const char *routine = trap_routine(c, tw, &desc, &sel_bytes);
    const char *dispatcher = trap_name(tw);
    char why[256];
    bool native = native_index_for(trap_address(tw)) >= 0;
    if (!call_enter(false, tw, routine ? routine : dispatcher, dispatcher, desc, sel_bytes, at, why, sizeof why))
        goto made;
    /* Stopped at the trap, its PC on it; it is made if the PC is still
     * there when the machine goes on. */
    c->pc = at;
    stop(false, "%s", why);
    if (c->pc != at) {
        if (npending)
            npending--;
        return true;
    }
    adopt_call(routine ? routine : dispatcher, desc, false, tw, sel_bytes, at);
    c->pc = at + 2;
made:
    if (!native && npending) {
        /* A patch of the program's: its result is not ours to see. */
        pending_call *p = &pending[npending - 1];
        if (p->record >= 0)
            records[p->record].patched = true;
        if (p->step >= 0)
            step_calls[p->step].patched = true;
        npending--;
    }
    return false;
}

void macsbug_trap_done(cpu68k *c, uint16_t tw)
{
    (void)tw;
    if (native_index_for(trap_address(tw)) < 0)
        return; /* went to the program's patch: noted at the trap */
    call_done(false);
}

bool macsbug_native(ppc_cpu *c, const char *name, gaddr at)
{
    char why[256];
    const char *desc = args_for(name);
    if (!call_enter(true, 0, name, NULL, desc, 0, c->lr - 4, why, sizeof why))
        return false;
    c->pc = at;
    stop(true, "%s", why);
    if (c->pc != at) {
        if (npending)
            npending--;
        return true;
    }
    adopt_call(name, desc, true, 0, 0, c->lr - 4);
    c->pc = at + 4;
    return false;
}

void macsbug_native_done(ppc_cpu *c)
{
    (void)c;
    call_done(true);
}

void macsbug_debugger_call(bool ppc, const char *kind, const char *msg)
{
    if (dx_off)
        return;
    if (msg)
        stop(ppc, "%s: %s", kind, msg);
    else
        stop(ppc, "%s", kind);
}

/* ---------------------------------------------------------------------- */
/* Mixed Mode switches, for the stack crawl */

void macsbug_switch_push(bool to_ppc, gaddr callee_sp)
{
    if (nswitches >= MAX_SWITCHES) {
        nswitches++;
        return;
    }
    mode_switch *s = &switches[nswitches++];
    s->to_ppc = to_ppc;
    s->thread = thread_current_index();
    s->callee_sp = callee_sp;
    s->pc68 = g_cpu->pc;
    s->a6 = g_cpu->a[6];
    s->a7 = g_cpu->a[7];
    s->pc = g_ppc.pc;
    s->lr = g_ppc.lr;
    s->r1 = g_ppc.gpr[1];
}

void macsbug_switch_pop(void)
{
    if (nswitches)
        nswitches--;
}

/* ---------------------------------------------------------------------- */
/* Displays */

static void show_68k_regs(void)
{
    cpu68k *c = g_cpu;
    for (int r = 0; r < 8; r += 4)
        out("  D%d = %08X  D%d = %08X  D%d = %08X  D%d = %08X\n", r, c->d[r], r + 1, c->d[r + 1], r + 2, c->d[r + 2],
            r + 3, c->d[r + 3]);
    for (int r = 0; r < 8; r += 4)
        out("  A%d = %08X  A%d = %08X  A%d = %08X  A%d = %08X\n", r, c->a[r], r + 1, c->a[r + 1], r + 2, c->a[r + 2],
            r + 3, c->a[r + 3]);
    uint16_t sr = c->sr;
    out("  PC = %08X  SR = %04X  %c%c%c%c%c  %s  int %d%s\n", c->pc, sr, sr & SR_X ? 'X' : 'x', sr & SR_N ? 'N' : 'n',
        sr & SR_Z ? 'Z' : 'z', sr & SR_V ? 'V' : 'v', sr & SR_C ? 'C' : 'c', sr & SR_S ? "Supervisor" : "User",
        (sr >> 8) & 7, sr & SR_T ? "  Trace" : "");
}

static void show_ppc_regs(void)
{
    ppc_cpu *c = &g_ppc;
    out("  PC = %08X  LR = %08X  CTR = %08X  CR = %08X  XER = %08X\n", c->pc, c->lr, c->ctr, c->cr, c->xer);
    for (int r = 0; r < 32; r += 4)
        out("  R%-2d= %08X  R%-2d= %08X  R%-2d= %08X  R%-2d= %08X\n", r, c->gpr[r], r + 1, c->gpr[r + 1], r + 2,
            c->gpr[r + 2], r + 3, c->gpr[r + 3]);
}

static void show_regs(bool both)
{
    if (cur_ppc()) {
        out(" PowerPC registers\n");
        show_ppc_regs();
        if (both) {
            out(" 68k registers\n");
            show_68k_regs();
        }
    } else {
        if (both && g_ppc_mode) {
            out(" 68k registers\n");
            show_68k_regs();
            out(" PowerPC registers\n");
            show_ppc_regs();
        } else {
            show_68k_regs();
        }
    }
}

static void show_float_regs(void)
{
    if (cur_ppc() || g_ppc_mode) {
        out(" PowerPC floating point  FPSCR = %08X\n", g_ppc.fpscr);
        for (int r = 0; r < 32; r += 2)
            out("  F%-2d = %-22.15g  F%-2d = %-22.15g\n", r, ppc_fpr(&g_ppc, r), r + 1, ppc_fpr(&g_ppc, r + 1));
    }
    if (!cur_ppc()) {
        cpu68k *c = g_cpu;
        out(" 68k floating point  FPCR = %08X  FPSR = %08X  FPIAR = %08X\n", c->fpcr, c->fpsr, c->fpiar);
        for (int r = 0; r < 8; r += 2)
            out("  FP%d = %-22.15g  FP%d = %-22.15g\n", r, c->fp[r], r + 1, c->fp[r + 1]);
    }
}

/* One instruction's line; its length. */
static uint32_t list_one(bool ppc, gaddr a, bool mark)
{
    char text[128], hex[40];
    uint32_t len;
    if (ppc) {
        a &= ~3u;
        uint32_t op = rd32(a);
        ppc_disasm(a, op, text, sizeof text);
        if ((op >> 26) == 1) { /* our NATIVE word: the routine it is */
            size_t k = strlen(text);
            snprintf(text + k, sizeof text - k, "  ; %s", cfm_native_name(op & 0x03FFFFFFu));
        }
        snprintf(hex, sizeof hex, "%08X", op);
        len = 4;
    } else {
        len = (uint32_t)cpu_disasm_named(a, text, sizeof text, aline_name);
        if (len < 2 || len > 22)
            len = 2;
        size_t k = 0;
        hex[0] = 0;
        for (uint32_t i = 0; i < len && k + 6 < sizeof hex; i += 2)
            k += (size_t)snprintf(hex + k, sizeof hex - k, "%s%04X", i ? " " : "", rd16(a + i));
    }
    bool bp = false;
    for (int i = 0; i < nbreaks && !bp; i++)
        bp = breaks[i].addr == a && !breaks[i].temp;
    out(" %c%c%08X  %-40s | %s\n", mark ? '*' : ' ', bp ? '>' : ' ', a, text, hex);
    return len;
}

/* il, ip, id: n instructions from a, a routine's name above its first. */
static void list(bool ppc, gaddr a, int n)
{
    char last[128] = "", name[128];
    gaddr pc = cur_pc();
    for (int i = 0; i < n; i++) {
        describe(a, name, sizeof name);
        char *plus = strrchr(name, '+');
        size_t base = plus ? (size_t)(plus - name) : strlen(name);
        if (name[0] && (strncmp(name, last, base) || last[base] != 0)) {
            snprintf(last, sizeof last, "%.*s", (int)base, name);
            out(" %s\n", name);
        } else if (!name[0] && last[0]) {
            last[0] = 0;
        }
        a += list_one(ppc, a, a == pc);
    }
    list_next = a;
    list_kind = 'i';
}

/* The 68k instruction n before a, as near as code can say: tried from
 * each of the 10 words before a, the one whose instructions end at a. */
static gaddr back_68k(gaddr a, int n)
{
    gaddr best = a - 2 * (uint32_t)n;
    for (gaddr s = a - 20 * (uint32_t)n; s < a; s += 2) {
        gaddr p = s;
        int k = 0;
        char buf[96];
        while (p < a) {
            p += (uint32_t)cpu_disasm(p, buf, sizeof buf);
            k++;
        }
        if (p == a && k >= n) {
            gaddr q = s;
            for (int i = 0; i < k - n; i++)
                q += (uint32_t)cpu_disasm(q, buf, sizeof buf);
            return q;
        }
    }
    return best;
}

/* dm: memory as hex and characters, 16 bytes a line. */
static void dump(gaddr a, uint32_t n, int unit)
{
    char name[128];
    describe(a, name, sizeof name);
    if (name[0])
        out(" Displaying memory from %08X (%s)\n", a, name);
    else
        out(" Displaying memory from %08X\n", a);
    for (uint32_t off = 0; off < n; off += 16) {
        char line[160];
        size_t k = (size_t)snprintf(line, sizeof line, "  %08X  ", a + off);
        for (uint32_t i = 0; i < 16; i += (uint32_t)unit) {
            if (off + i < n) {
                if (unit == 1)
                    k += (size_t)snprintf(line + k, sizeof line - k, "%02X", rd8(a + off + i));
                else if (unit == 2)
                    k += (size_t)snprintf(line + k, sizeof line - k, "%04X", rd16(a + off + i));
                else
                    k += (size_t)snprintf(line + k, sizeof line - k, "%08X", rd32(a + off + i));
            } else {
                k += (size_t)snprintf(line + k, sizeof line - k, "%*s", 2 * unit, "");
            }
            if (unit > 1 || i % 2 == 1)
                line[k++] = ' ';
        }
        line[k++] = ' ';
        for (uint32_t i = 0; i < 16 && off + i < n; i++) {
            uint8_t ch = rd8(a + off + i);
            line[k++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '.';
        }
        line[k++] = '\n';
        line[k] = 0;
        emit(line);
    }
    dot = a;
    list_next = a + n;
    list_kind = 'm';
}

/* dm's templates: a few records MacsBug knew by name. */
static bool dump_template(gaddr a, const char *t)
{
    if (!strcasecmp(t, "pstring")) {
        char s[256];
        pstr_to_c(a, s, sizeof s);
        out("  %08X  \"%s\" (%u bytes)\n", a, s, rd8(a));
    } else if (!strcasecmp(t, "cstring")) {
        char s[256];
        size_t k = 0;
        while (k < sizeof s - 1 && rd8(a + (gaddr)k))
            s[k] = (char)rd8(a + (gaddr)k), k++;
        s[k] = 0;
        out("  %08X  \"%s\"\n", a, s);
    } else if (!strcasecmp(t, "rect")) {
        out("  Rect at %08X  top %d  left %d  bottom %d  right %d\n", a, (int16_t)rd16(a), (int16_t)rd16(a + 2),
            (int16_t)rd16(a + 4), (int16_t)rd16(a + 6));
    } else if (!strcasecmp(t, "point")) {
        out("  Point at %08X  v %d  h %d\n", a, (int16_t)rd16(a), (int16_t)rd16(a + 2));
    } else if (!strcasecmp(t, "handle")) {
        out("  Handle %08X -> master pointer %08X\n", a, rd32(a));
    } else {
        return false;
    }
    dot = a;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Heaps */

static gaddr current_zone(void)
{
    if (heap_zone)
        return heap_zone;
    gaddr z = rd32(LM_ApplZone);
    return z ? z : rd32(LM_SysZone);
}

static const char *zone_name(gaddr z)
{
    if (z == rd32(LM_SysZone))
        return "System";
    if (z == rd32(LM_ApplZone))
        return "Application";
    return "Other";
}

static void heap_zones(void)
{
    gaddr zs[3] = { rd32(LM_SysZone), rd32(LM_ApplZone), rd32(LM_TheZone) };
    out(" Heap zones\n");
    for (int i = 0; i < 3; i++) {
        if (!zs[i] || (i == 1 && zs[1] == zs[0]) || (i == 2 && (zs[2] == zs[0] || zs[2] == zs[1])))
            continue;
        out("  %08X  %-11s  to %08X  %s\n", zs[i], i == 2 ? "Current" : zone_name(zs[i]), mm_zone_limit(zs[i]),
            zs[i] == rd32(LM_TheZone) ? "(TheZone)" : "");
    }
}

/* hd's filter: F free, N nonrelocatable, R relocatable, L locked, P
 * purgeable, RS resources, or a resource type 'TYPE'. */
static bool block_shown(const mm_block *b, const char *filter, uint32_t type, bool is_rsrc)
{
    if (!filter[0])
        return true;
    if (!strcasecmp(filter, "F"))
        return b->use == 0;
    if (!strcasecmp(filter, "N"))
        return b->use == 1;
    if (!strcasecmp(filter, "R"))
        return b->use == 2;
    if (!strcasecmp(filter, "L"))
        return b->use == 2 && (b->state & 0x80);
    if (!strcasecmp(filter, "P"))
        return b->use == 2 && (b->state & 0x40);
    if (!strcasecmp(filter, "RS"))
        return is_rsrc;
    if (filter[0] == '\'' && strlen(filter) >= 5) {
        uint32_t t = (uint32_t)(uint8_t)filter[1] << 24 | (uint32_t)(uint8_t)filter[2] << 16 |
                     (uint32_t)(uint8_t)filter[3] << 8 | (uint8_t)filter[4];
        return is_rsrc && t == type;
    }
    return true;
}

static void heap_display(const char *filter)
{
    gaddr z = current_zone();
    if (!z) {
        out("  No heap zone\n");
        return;
    }
    out(" Displaying the %s heap at %08X\n", zone_name(z), z);
    out("    Start    Length     Tag  Mstr Ptr  Lock Prg  Type    ID    File  Name\n");
    gaddr b = 0;
    mm_block blk;
    unsigned shown = 0;
    uint32_t bytes = 0;
    while (mm_zone_block(z, &b, &blk)) {
        if (blk.damaged) {
            out("  ** Damaged block at %08X (size %u): the zone's walk ends here\n", blk.addr, blk.psize);
            break;
        }
        uint32_t type = 0;
        int16_t id = 0, ref = 0;
        char name[64] = "";
        bool rs = blk.use == 2 && rsrc_info_of(blk.mp, &type, &id, name, sizeof name, &ref);
        if (!block_shown(&blk, filter, type, rs))
            continue;
        shown++;
        bytes += blk.lsize;
        char tag = blk.use == 0 ? 'F' : blk.use == 1 ? 'N' : 'R';
        char line[200];
        size_t k = (size_t)snprintf(line, sizeof line, "   %08X  %08X+%02X  %c   ", blk.data, blk.lsize, blk.sizec, tag);
        if (blk.use == 2)
            k += (size_t)snprintf(line + k, sizeof line - k, "%08X  %c    %c", blk.mp, blk.state & 0x80 ? 'L' : ' ',
                                  blk.state & 0x40 ? 'P' : ' ');
        else
            k += (size_t)snprintf(line + k, sizeof line - k, "%15s", "");
        if (rs) {
            char t[5];
            for (int i = 0; i < 4; i++) {
                uint8_t ch = (uint8_t)(type >> (24 - 8 * i));
                t[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '.';
            }
            t[4] = 0;
            k += (size_t)snprintf(line + k, sizeof line - k, "    '%s'  %-5d %04X  %s", t, id, (uint16_t)ref, name);
        }
        snprintf(line + k, sizeof line - k, "\n");
        emit(line);
    }
    out("  %u blocks shown, %u bytes in them\n", shown, bytes);
}

static void heap_totals(void)
{
    gaddr z = current_zone();
    if (!z) {
        out("  No heap zone\n");
        return;
    }
    uint32_t n[3] = { 0 }, sz[3] = { 0 }, locked = 0, purgeable = 0, largest_free = 0;
    gaddr b = 0;
    mm_block blk;
    bool damaged = false;
    while (mm_zone_block(z, &b, &blk)) {
        if (blk.damaged) {
            damaged = true;
            break;
        }
        n[blk.use % 3]++;
        sz[blk.use % 3] += blk.psize;
        if (blk.use == 0 && blk.psize > largest_free)
            largest_free = blk.psize;
        if (blk.use == 2 && (blk.state & 0x80))
            locked++;
        if (blk.use == 2 && (blk.state & 0x40))
            purgeable++;
    }
    out(" Totals for the %s heap at %08X (to %08X)\n", zone_name(z), z, mm_zone_limit(z));
    out("  Free blocks            %6u  %10u bytes  (largest %u)\n", n[0], sz[0], largest_free);
    out("  Nonrelocatable blocks  %6u  %10u bytes\n", n[1], sz[1]);
    out("  Relocatable blocks     %6u  %10u bytes  (%u locked, %u purgeable)\n", n[2], sz[2], locked, purgeable);
    if (damaged)
        out("  ** The zone is damaged: these are its blocks up to the damage\n");
}

/* hc: the zone's blocks tile it, each relocatable block's master pointer
 * points back at it. */
static void heap_check(void)
{
    gaddr zs[2] = { rd32(LM_SysZone), rd32(LM_ApplZone) };
    for (int i = 0; i < 2; i++) {
        gaddr z = zs[i];
        if (!z || (i == 1 && z == zs[0]))
            continue;
        gaddr b = 0, prev = 0;
        mm_block blk;
        bool ok = true;
        while (mm_zone_block(z, &b, &blk)) {
            if (blk.damaged) {
                out("  The %s heap at %08X is damaged: block %08X has size %u (the block before is %08X)\n",
                    zone_name(z), z, blk.addr, blk.psize, prev);
                ok = false;
                break;
            }
            if (blk.use == 2) {
                uint32_t mp = mem_strip(rd32(blk.mp));
                if (mp != blk.data && !(blk.state & 0x80 && mp > blk.data && mp <= blk.addr + blk.psize)) {
                    out("  The %s heap at %08X: block %08X's master pointer %08X holds %08X\n", zone_name(z), z,
                        blk.addr, blk.mp, rd32(blk.mp));
                    ok = false;
                }
            }
            prev = blk.addr;
        }
        if (ok && b != mm_zone_limit(z)) {
            out("  The %s heap at %08X: its blocks end at %08X, not at bkLim %08X\n", zone_name(z), z, b,
                mm_zone_limit(z));
            ok = false;
        }
        if (ok)
            out("  The %s heap at %08X checks out\n", zone_name(z), z);
    }
}

/* wh: what an address is (a block, a routine, a trap, low memory), or
 * where a trap or a name is. */
static void where(const char *arg)
{
    uint16_t tw;
    if (arg[0] && !isxdigit((unsigned char)arg[0]) && arg[0] != '$' && arg[0] != '#' && arg[0] != '@' &&
        arg[0] != '(' && trap_named(arg, &tw)) {
        gaddr a = trap_address(tw);
        out("  Trap %s ($%04X) is at %08X%s\n", trap_name(tw), tw, a,
            native_index_for(a) >= 0 ? " (Blue Box's own)" : " (patched by the program)");
        return;
    }
    uint32_t a;
    if (!arg[0])
        a = cur_pc();
    else if (!eval_str(arg, &a))
        return;
    if (a >= 0xA000 && a <= 0xAFFF && arg[0] && !strchr(arg, '@')) {
        out("  Trap $%04X is %s, at %08X\n", (unsigned)a, trap_name((uint16_t)a), trap_address((uint16_t)a));
        return;
    }
    char name[128];
    describe(a, name, sizeof name);
    bool said = false;
    if (name[0]) {
        out("  Address %08X is in %s\n", a, name);
        said = true;
    }
    for (unsigned i = 0; i < lowmem_count; i++)
        if (a >= lowmem_table[i].addr && a < lowmem_table[i].addr + lowmem_table[i].size) {
            out("  Address %08X is in low memory: %s+$%X\n", a, lowmem_table[i].name, a - lowmem_table[i].addr);
            said = true;
            break;
        }
    gaddr zs[2] = { rd32(LM_SysZone), rd32(LM_ApplZone) };
    for (int i = 0; i < 2 && !said; i++) {
        gaddr z = zs[i];
        if (!z || (i == 1 && z == zs[0]) || a < z || a >= mm_zone_limit(z))
            continue;
        gaddr b = 0;
        mm_block blk;
        while (mm_zone_block(z, &b, &blk)) {
            if (blk.damaged)
                break;
            if (a < blk.addr || a >= blk.addr + blk.psize)
                continue;
            const char *use = blk.use == 0 ? "free" : blk.use == 1 ? "nonrelocatable" : "relocatable";
            out("  Address %08X is in the %s heap at %08X, in the %s block at %08X (+$%X of its %u bytes)\n", a,
                zone_name(z), z, use, blk.data, a - blk.data, blk.lsize);
            uint32_t type;
            int16_t id, ref;
            char rn[64];
            if (blk.use == 2) {
                out("  Its handle (master pointer) is %08X%s%s\n", blk.mp, blk.state & 0x80 ? ", locked" : "",
                    blk.state & 0x40 ? ", purgeable" : "");
                if (rsrc_info_of(blk.mp, &type, &id, rn, sizeof rn, &ref))
                    out("  It is resource '%c%c%c%c' %d%s%s%s from file %d\n", (char)(type >> 24), (char)(type >> 16),
                        (char)(type >> 8), (char)type, id, rn[0] ? " \"" : "", rn, rn[0] ? "\"" : "", ref);
            }
            said = true;
            break;
        }
    }
    if (!said && a >= GA_ROM && a < GA_ROM + GA_ROM_SIZE) {
        out("  Address %08X is in Blue Box's ROM (its natives' stubs)\n", a);
        said = true;
    }
    if (!said)
        out("  Address %08X is not in a heap, a routine or low memory\n", a);
    dot = a;
}

/* ---------------------------------------------------------------------- */
/* The stack crawl */

static bool switch_here(const mode_switch *s) { return s->thread == thread_current_index(); }

/* 68k frames by A6 links (from a6) up to limit (0: no limit), or every
 * long on the stack that looks like a return address (sc7). */
static void crawl_68k(gaddr pc, gaddr a6, gaddr a7, gaddr limit, bool sc7, int *count)
{
    char name[128];
    describe(pc, name, sizeof name);
    out("  68k      PC %08X  %s\n", pc, name);
    if (sc7) {
        gaddr top = limit ? limit : rd32(LM_CurStackBase);
        if (!top || top < a7 || top - a7 > 0x100000)
            top = a7 + 0x4000;
        for (gaddr p = a7 & ~1u; p < top && *count < 200; p += 2) {
            uint32_t r = mem_strip(rd32(p));
            if (r < 0x1000 || r & 1 || r >= 0xF0000000u)
                continue;
            /* After a JSR or BSR: one of the forms ends at r. */
            uint16_t w2 = rd16(r - 2), w4 = rd16(r - 4), w6 = rd16(r - 6);
            bool call = (w2 & 0xFFF8) == 0x4E90 || (w2 & 0xFF00) == 0x6100 || (w4 & 0xFFF8) == 0x4EA8 ||
                        (w4 & 0xFFF8) == 0x4EB0 || w4 == 0x4EB8 || w4 == 0x4EBA || w4 == 0x4EBB ||
                        w4 == 0x6100 || w6 == 0x4EB9 || w6 == 0x61FF;
            if (!call)
                continue;
            describe(r, name, sizeof name);
            out("  %08X  return %08X  %s\n", p, r, name);
            (*count)++;
        }
        return;
    }
    gaddr f = mem_strip(a6);
    while (f && !(f & 1) && f >= (mem_strip(a7) & ~1u) && (!limit || f < limit) && *count < 200) {
        gaddr ret = mem_strip(rd32(f + 4));
        gaddr next = mem_strip(rd32(f));
        if (!ret)
            break;
        describe(ret, name, sizeof name);
        out("  %08X  return %08X  %s\n", f, ret, name);
        (*count)++;
        if (next <= f)
            break;
        f = next;
    }
}

/* PowerPC frames by the back chain from r1 (the first caller from LR),
 * up to limit. */
static void crawl_ppc(gaddr pc, gaddr lr, gaddr r1, gaddr limit, bool lr_first, int *count)
{
    char name[128];
    describe(pc, name, sizeof name);
    out("  PowerPC  PC %08X  %s\n", pc, name);
    if (lr_first && lr) {
        describe(lr, name, sizeof name);
        out("  LR        %08X  %s\n", lr, name);
    }
    gaddr f = r1;
    gaddr last = lr;
    while (f && !(f & 3) && (!limit || f < limit) && *count < 200) {
        gaddr back = rd32(f);
        if (!back || back <= f || (back & 3))
            break;
        gaddr ret = rd32(back + 8);
        if (ret && ret != last) {
            describe(ret, name, sizeof name);
            out("  %08X  return %08X  %s\n", back, ret, name);
            (*count)++;
        }
        last = 0;
        f = back;
    }
}

static void stack_crawl(bool sc7)
{
    int count = 0;
    bool ppc = cur_ppc();
    out(" Calling chain%s, innermost first\n", sc7 ? " (68k: every return address on the stack)" : "");
    /* The live registers, then each Mixed Mode switch's caller, innermost
     * first, each processor's walk ending where its callee began. */
    gaddr pc = ppc ? g_ppc.pc : g_cpu->pc, lr = g_ppc.lr, r1 = g_ppc.gpr[1];
    gaddr a6 = g_cpu->a[6], a7 = g_cpu->a[7];
    bool first = true;
    int k = nswitches < MAX_SWITCHES ? nswitches : MAX_SWITCHES;
    for (int i = k - 1; i >= -1; i--) {
        const mode_switch *s = NULL;
        if (i >= 0) {
            if (!switch_here(&switches[i]) || switches[i].to_ppc != ppc)
                continue;
            s = &switches[i];
        }
        gaddr limit = s ? s->callee_sp : 0;
        if (ppc)
            crawl_ppc(pc, lr, r1, limit, first, &count);
        else
            crawl_68k(pc, a6, a7, limit, sc7, &count);
        if (!s)
            break;
        out("  -- Mixed Mode: called from %s code --\n", s->to_ppc ? "68k" : "PowerPC");
        ppc = !s->to_ppc;
        if (ppc) {
            pc = s->lr; /* the call's return: the native it made is ours */
            lr = 0;
            r1 = s->r1;
        } else {
            pc = s->pc68;
            a6 = s->a6;
            a7 = s->a7;
        }
        first = false;
    }
    if (!count)
        out("  (no frames found)\n");
}

/* ---------------------------------------------------------------------- */
/* Breakpoints and trap watches: the commands' side */

static void list_breaks(void)
{
    int shown = 0;
    for (int i = 0; i < nbreaks; i++) {
        breakpoint *b = &breaks[i];
        if (b->temp)
            continue;
        char name[128];
        describe(b->addr, name, sizeof name);
        out("  %08X  %-30s hits %u%s%s%s%s%s\n", b->addr, name, b->hits, b->every > 1 ? "  every " : "",
            b->every > 1 ? "" : "", b->cond[0] ? "  if " : "", b->cond, b->cmds[0] ? "  then" : "");
        if (b->every > 1)
            out("            (stops every %u times)\n", b->every);
        if (b->cmds[0])
            out("            commands: %s\n", b->cmds);
        shown++;
    }
    if (!shown)
        out("  No breakpoints\n");
}

/* A count, a condition, or 'commands' after an address. */
static void break_options(char args[][160], int from, int n, uint32_t *every, char *cond, size_t cn, char *cmds,
                          size_t mn)
{
    for (int i = from; i < n; i++) {
        const char *a = args[i];
        size_t len = strlen(a);
        if ((a[0] == '\'' || a[0] == '"') && len >= 2 && a[len - 1] == a[0] && !(a[0] == '\'' && len == 6)) {
            snprintf(cmds, mn, "%.*s", (int)(len - 2), a + 1);
            continue;
        }
        const char *s = a;
        uint32_t v;
        bool rel = false;
        if (!eval(&s, &v, &rel))
            continue;
        if (rel)
            snprintf(cond, cn, "%s", a);
        else
            *every = v;
    }
}

static void add_break(gaddr a, uint32_t every, const char *cond, const char *cmds)
{
    for (int i = 0; i < nbreaks; i++)
        if (breaks[i].addr == a && !breaks[i].temp) {
            breaks[i].every = every;
            snprintf(breaks[i].cond, sizeof breaks[i].cond, "%s", cond);
            snprintf(breaks[i].cmds, sizeof breaks[i].cmds, "%s", cmds);
            out("  Breakpoint at %08X changed\n", a);
            return;
        }
    if (nbreaks == MAX_BREAKS) {
        out("  No room for another breakpoint (%d)\n", MAX_BREAKS);
        return;
    }
    breakpoint *b = &breaks[nbreaks++];
    memset(b, 0, sizeof *b);
    b->addr = a;
    b->every = every;
    snprintf(b->cond, sizeof b->cond, "%s", cond);
    snprintf(b->cmds, sizeof b->cmds, "%s", cmds);
    char name[128];
    describe(a, name, sizeof name);
    out("  Breakpoint set at %08X%s%s\n", a, name[0] ? "  " : "", name);
}

static void add_temp(gaddr a, gaddr sp, const char *why)
{
    if (nbreaks == MAX_BREAKS)
        return;
    breakpoint *b = &breaks[nbreaks++];
    memset(b, 0, sizeof *b);
    b->addr = a;
    b->temp = true;
    b->sp = sp;
    snprintf(b->cmds, sizeof b->cmds, "%s", why);
}

/* atb, att, athc's traps: none (every one), names, trap words or a range. */
static void add_watches(int kind, char args[][160], int n)
{
    int added = 0;
    uint32_t every = 0;
    char cond[96] = "";
    int i = 1;
    if (n == 1) {
        if (nwatches < MAX_TRAPS)
            watches[nwatches++] = (trap_watch){ kind, "", 0xA000, 0xAFFF, "", 0, 0 };
        added++;
    }
    while (i < n) {
        const char *a = args[i];
        trap_watch w = { kind, "", 0, 0, "", 0, 0 };
        uint32_t lo, hi;
        uint16_t tw;
        bool is_num = (isxdigit((unsigned char)a[0]) || a[0] == '$') && eval_str(a, &lo) && lo >= 0xA000 &&
                      lo <= 0xAFFF;
        if (is_num) {
            w.lo = w.hi = norm_trap((uint16_t)lo);
            if (i + 1 < n && eval_str(args[i + 1], &hi) && hi >= 0xA000 && hi <= 0xAFFF && hi > lo) {
                w.hi = norm_trap((uint16_t)hi);
                i++;
            }
        } else if (trap_named(a, &tw) || args_for(a[0] == '_' ? a + 1 : a)) {
            snprintf(w.name, sizeof w.name, "%s", a[0] == '_' ? a + 1 : a);
        } else {
            /* A count or condition for the traps before it. */
            const char *s = a;
            uint32_t v;
            bool rel = false;
            if (eval(&s, &v, &rel)) {
                if (rel)
                    snprintf(cond, sizeof cond, "%s", a);
                else
                    every = v;
            }
            i++;
            continue;
        }
        if (nwatches < MAX_TRAPS) {
            watches[nwatches++] = w;
            added++;
        } else {
            out("  No room for another trap watch (%d)\n", MAX_TRAPS);
        }
        i++;
    }
    for (int k = nwatches - added; k < nwatches; k++) {
        watches[k].every = every;
        snprintf(watches[k].cond, sizeof watches[k].cond, "%s", cond);
        const trap_watch *w = &watches[k];
        const char *what = kind == TW_BREAK ? "A-trap break" : kind == TW_TRACE ? "A-trap trace" : "A-trap heap check";
        if (w->name[0])
            out("  %s at %s\n", what, w->name);
        else if (w->lo == 0xA000 && w->hi == 0xAFFF)
            out("  %s at every trap\n", what);
        else
            out("  %s at %04X%s%.0X\n", what, w->lo, w->hi != w->lo ? " to " : "", w->hi != w->lo ? w->hi : 0);
    }
    update_traps();
}

static void list_watches(void)
{
    if (!nwatches)
        out("  No A-trap breaks, traces or heap checks\n");
    for (int i = 0; i < nwatches; i++) {
        const trap_watch *w = &watches[i];
        const char *what = w->kind == TW_BREAK ? "break" : w->kind == TW_TRACE ? "trace" : "heap check";
        char at[80];
        if (w->name[0])
            snprintf(at, sizeof at, "%s", w->name);
        else if (w->lo == 0xA000 && w->hi == 0xAFFF)
            snprintf(at, sizeof at, "every trap");
        else if (w->lo == w->hi)
            snprintf(at, sizeof at, "%04X (%s)", w->lo, trap_name(w->lo));
        else
            snprintf(at, sizeof at, "%04X to %04X", w->lo, w->hi);
        out("  A-trap %-10s %s%s%s  hits %u\n", what, at, w->cond[0] ? "  if " : "", w->cond, w->hits);
    }
    out("  A-trap recording is %s (%u calls kept)\n", recording ? "on" : "off", nrecords);
}

static void clear_watches(char args[][160], int n)
{
    if (n == 1) {
        nwatches = 0;
        out("  All A-trap breaks, traces and heap checks cleared\n");
        update_traps();
        return;
    }
    for (int i = 1; i < n; i++) {
        const char *a = args[i][0] == '_' ? args[i] + 1 : args[i];
        uint32_t v = 0;
        bool num = (isxdigit((unsigned char)a[0]) || a[0] == '$') && eval_str(a, &v) && v >= 0xA000 && v <= 0xAFFF;
        int removed = 0;
        for (int k = 0; k < nwatches;) {
            bool hit = num ? norm_trap((uint16_t)v) >= watches[k].lo && norm_trap((uint16_t)v) <= watches[k].hi &&
                                 !watches[k].name[0]
                           : !strcasecmp(watches[k].name, a);
            if (hit) {
                watches[k] = watches[--nwatches];
                removed++;
            } else {
                k++;
            }
        }
        out("  %s: %d cleared\n", args[i], removed);
    }
    update_traps();
}

static void playback(void)
{
    if (!nrecords) {
        out("  No calls recorded%s\n", recording ? "" : " (atr turns recording on)");
        return;
    }
    out(" The last %u calls, oldest first\n", nrecords);
    unsigned first = (record_next + RECORDS - nrecords) % RECORDS;
    for (unsigned i = 0; i < nrecords; i++) {
        const call_record *r = &records[(first + i) % RECORDS];
        out("  %c %08X  %s%s%s%s\n", r->ppc ? 'P' : 'A', r->from, r->text,
            r->done && r->result[0] ? " = " : r->patched ? "  (patched)" : "", r->done ? r->result : "",
            !r->done && !r->patched ? "  (not returned)" : "");
    }
}

/* ---------------------------------------------------------------------- */
/* Stopping */

static void step_report(void)
{
    for (unsigned i = 0; i < nstep_calls; i++) {
        const call_record *r = &step_calls[i];
        out("  %s%s%s%s\n", r->text, r->done && r->result[0] ? " = " : "", r->done ? r->result : "",
            r->patched ? "  (the program's patch of it ran)" : !r->done ? "  (not returned yet)" : "");
    }
    if (step_calls_dropped)
        out("  ... and %u more calls\n", step_calls_dropped);
    nstep_calls = step_calls_dropped = 0;
}

/* Where the machine stopped: the reason, then the instruction. */
static void stop_banner(void)
{
    gaddr pc = cur_pc();
    char name[128];
    describe(pc, name, sizeof name);
    out("%s\n", stop_why);
    out("  %s at %08X%s%s\n", stop_ppc ? "PowerPC" : "68k", pc, name[0] ? "  " : "", name);
    list_one(stop_ppc, pc, true);
}

static char *next_line(char *buf, size_t n)
{
    if (script && script_at < script_len) {
        const char *s = script + script_at;
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        snprintf(buf, n, "%.*s", (int)(len < n - 1 ? len : n - 1), s);
        script_at += len + (e ? 1 : 0);
        return buf;
    }
    if (!console_fn || !console_interactive)
        return NULL;
    pthread_mutex_lock(&post_lock);
    while (console_fn && console_interactive && !posted_len)
        pthread_cond_wait(&post_cond, &post_lock);
    char *r = NULL;
    if (posted_len) {
        const char *e = memchr(posted, '\n', posted_len);
        size_t len = e ? (size_t)(e - posted) : posted_len;
        snprintf(buf, n, "%.*s", (int)(len < n - 1 ? len : n - 1), posted);
        size_t used = len + (e ? 1 : 0);
        memmove(posted, posted + used, posted_len - used);
        posted_len -= used;
        r = buf;
    }
    pthread_mutex_unlock(&post_lock);
    return r;
}

BB_HOST static int stop_depth;

static void stop(bool ppc, const char *fmt, ...)
{
    if (stop_depth) /* a command's own call into the program */
        return;
    stop_depth++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stop_why, sizeof stop_why, fmt, ap);
    va_end(ap);
    stopped = true;
    stop_ppc = ppc;
    resume = false;
    step_left = 0;
    step_next = over_wait = false;
    machine_clock_hold(true);
    gaddr pc = cur_pc();
    dot = pc;
    list_next = pc;
    list_kind = 'i';
    if (console_fn)
        console_fn(console_ctx, MACHINE_DEBUG_STOP, stop_why);
    step_report();
    stop_banner();
    const char *cmds = break_cmds(pc);
    if (cmds) {
        char copy[160];
        snprintf(copy, sizeof copy, "%s", cmds);
        run_line(copy);
    }
    char line[512];
    while (!resume) {
        if (!next_line(line, sizeof line)) {
            if (console_fn && console_interactive)
                continue;
            out("  (no more commands: g)\n");
            run_line("g");
            break;
        }
        out("> %s\n", line);
        run_line(line);
        if (g_cpu->halted)
            break;
    }
    stopped = false;
    machine_clock_hold(false);
    /* The instruction it goes on from is not broken on again. */
    skip_armed = true;
    skip_ppc = stop_ppc;
    skip_pc = skip_ppc ? g_ppc.pc : g_cpu->pc;
    step_depth = npending;
    if (step_left)
        step_begin(ppc, skip_pc, ppc ? g_ppc.gpr[1] : g_cpu->a[7]);
    update_traps();
    macsbug_retrace();
    if (console_fn)
        console_fn(console_ctx, MACHINE_DEBUG_GO, "");
    stop_depth--;
}

/* ---------------------------------------------------------------------- */
/* Going on */

static void going_on(void)
{
    if (step_left) {
        step_depth = npending;
        nstep_calls = step_calls_dropped = 0;
        if (!stopped)
            step_begin(cur_ppc(), cur_pc(), cur_sp());
    }
    update_traps();
    macsbug_retrace();
}

static void go(void)
{
    resume = true;
    if (!stopped)
        going_on();
}

static void exit_to_shell(void)
{
    if (cur_ppc()) {
        void (*fn)(ppc_cpu *) = cfm_export_fn("ExitToShell");
        gaddr tv = fn ? cfm_native_tvector(fn, "ExitToShell") : 0;
        if (tv) {
            g_ppc.pc = rd32(tv);
            go();
            return;
        }
    } else {
        gaddr stub = mm_new_ptr_in(rd32(LM_SysZone), 2, false);
        if (stub) {
            wr16(stub, 0xA9F4); /* _ExitToShell: the program's cleanup runs as for its own */
            g_cpu->pc = stub;
            go();
            return;
        }
    }
    cpu_halt(g_cpu, "ExitToShell (the debugger's es)");
    go();
}

/* mr: the routine running now returns. */
static void magic_return(const char *arg)
{
    gaddr target, sp;
    if (cur_ppc()) {
        /* LR until the routine has saved it (mflr r0 before the PC), then
         * the saved LR in the caller's frame. */
        bool saved = false;
        for (gaddr p = g_ppc.pc - 4; p > g_ppc.pc - 256; p -= 4) {
            uint32_t op = rd32(p);
            if (op == 0x4E800020 || op == 0)
                break;
            if (op == 0x7C0802A6) {
                saved = true;
                break;
            }
        }
        gaddr back = rd32(g_ppc.gpr[1]);
        target = saved && back ? rd32(back + 8) : g_ppc.lr;
        sp = g_ppc.gpr[1] + 1;
    } else {
        cpu68k *c = g_cpu;
        uint32_t off;
        if (arg[0] && eval_str(arg, &off)) {
            target = rd32(c->a[7] + off);
            sp = c->a[7] + off + 4;
        } else if (c->a[6] >= c->a[7] && c->a[6] - c->a[7] < 0x100000 && !(c->a[6] & 1)) {
            target = rd32(c->a[6] + 4);
            sp = c->a[6] + 8;
        } else {
            target = rd32(c->a[7]);
            sp = c->a[7] + 4;
        }
    }
    char name[128];
    describe(target, name, sizeof name);
    out("  Magic return to %08X%s%s\n", target, name[0] ? "  " : "", name);
    add_temp(target, sp, "Magic return");
    go();
}

/* ---------------------------------------------------------------------- */
/* Commands */

static const char *const help_text[] = {
    "Flow",
    "  G [addr]           go (from addr)",
    "  GT addr            go till addr",
    "  T [n] / SO [n]     step n instructions, over calls and traps (shown with their arguments)",
    "  S [n]              step n instructions, into calls",
    "  MR [offset]        magic return: go till the routine returns",
    "  SS addr [addr]     step spy: stop when memory there changes (SS alone: off)",
    "  ES                 exit to shell: the program quits",
    "Breakpoints",
    "  BR addr [n|cond] ['cmds']  break at addr (every nth time, or when cond holds; run cmds)",
    "  BRC [addr]         clear one, or all",
    "  BRD                list them",
    "A-traps (68k traps and PowerPC Toolbox calls)",
    "  ATB [trap...] [n|cond]  break at traps: names, numbers or a range; none: every one",
    "  ATT [trap...]      trace them as they are called",
    "  ATHC [trap...]     check the heaps before each",
    "  ATR [ON|OFF]       record every call and its result",
    "  ATP                play the record back",
    "  ATC [trap...]      clear trap breaks, traces and heap checks",
    "  ATD                list them",
    "  DX [ON|OFF]        the program's Debugger and DebugStr calls stop here, or not",
    "Memory",
    "  DM [addr [n|template]]  display memory (templates: pstring cstring rect point handle)",
    "  DB / DW / DL addr  display a byte, word, long",
    "  DP [addr]          display a page (128 bytes)",
    "  SM addr value...   set memory (values' sizes from their digits; 'text' too)",
    "  SB / SW / SL addr value...  set bytes, words, longs",
    "  F addr n value     find value (or 'text') in n bytes from addr",
    "Code",
    "  IL [addr [n]]      disassemble from addr (68k or PowerPC, as the code there is)",
    "  IP [addr]          disassemble around addr",
    "  ID [addr]          disassemble one instruction",
    "  SC / SC6           stack crawl by A6 links and the back chain, across Mixed Mode",
    "  SC7                stack crawl from every return address on the 68k stack",
    "  WH [addr|trap]     what is at addr (heap block, routine, low memory), or where a trap is",
    "Registers",
    "  RD / TD            the registers (TD: both processors)",
    "  TF                 the floating point registers",
    "  reg = expr         set a register: D0-D7 A0-A7 PC SP SR, R0-R31 LR CTR CR XER",
    "Heaps",
    "  HZ                 the heap zones",
    "  HX [zone]          the zone the heap commands show",
    "  HD [F|N|R|L|P|RS|'TYPE']  the zone's blocks",
    "  HT                 the zone's totals",
    "  HC                 check the heaps",
    "Other",
    "  HOW                why the machine stopped",
    "  EV expr / expr     an expression's value: hex unless #decimal, @addr or addr^ the long there,",
    "                     registers, low-memory names, trap and routine names, 'TYPE', '.' (the last address)",
    "  HELP               this list",
};

static void help(void)
{
    for (size_t i = 0; i < sizeof help_text / sizeof help_text[0]; i++)
        out("%s\n", help_text[i]);
}

static void show_value_line(uint32_t v)
{
    char t[5];
    for (int i = 0; i < 4; i++) {
        uint8_t ch = (uint8_t)(v >> (24 - 8 * i));
        t[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '.';
    }
    t[4] = 0;
    out("  $%08X  #%u  #%d  '%s'\n", v, v, (int32_t)v, t);
}

/* sm's values: a string's bytes, or a number in as many bytes as its
 * digits need (any other expression: a long). */
static void set_memory(gaddr a, char args[][160], int from, int n, int unit)
{
    gaddr p = a;
    for (int i = from; i < n; i++) {
        const char *v = args[i];
        size_t len = strlen(v);
        if ((v[0] == '\'' || v[0] == '"') && len >= 2 && v[len - 1] == v[0]) {
            for (size_t k = 1; k + 1 < len; k++)
                wr8(p++, (uint8_t)v[k]);
            continue;
        }
        uint32_t x;
        if (!eval_str(v, &x))
            return;
        int size = unit;
        if (!size) {
            const char *d = v[0] == '$' ? v + 1 : v;
            size_t digits = strspn(d, "0123456789abcdefABCDEF");
            size = digits == strlen(d) && digits <= 2 ? 1 : digits == strlen(d) && digits <= 4 ? 2 : 4;
        }
        if (size == 1)
            wr8(p, (uint8_t)x);
        else if (size == 2)
            wr16(p, (uint16_t)x);
        else
            wr32(p, x);
        p += (uint32_t)size;
    }
    cpu_icache_flush(g_cpu, a, p - a);
    if (g_ppc_mode)
        ppc_jit_flush();
    out("  Memory set at %08X (%u bytes)\n", a, p - a);
    dump(a, p - a < 16 ? 16 : p - a, 1);
}

static void find(char args[][160], int n, int unit)
{
    uint32_t a, len;
    if (n < 4 || !eval_str(args[1], &a) || !eval_str(args[2], &len)) {
        out("  F addr n value\n");
        return;
    }
    uint8_t pat[160];
    size_t plen = 0;
    const char *v = args[3];
    size_t vl = strlen(v);
    if ((v[0] == '\'' || v[0] == '"') && vl >= 2 && v[vl - 1] == v[0] && !(unit && vl == 6)) {
        for (size_t k = 1; k + 1 < vl; k++)
            pat[plen++] = (uint8_t)v[k];
    } else {
        uint32_t x;
        if (!eval_str(v, &x))
            return;
        int size = unit;
        if (!size) {
            const char *d = v[0] == '$' ? v + 1 : v;
            size_t digits = strspn(d, "0123456789abcdefABCDEF");
            size = digits == strlen(d) && digits <= 2 ? 1 : digits == strlen(d) && digits <= 4 ? 2 : 4;
        }
        for (int k = size - 1; k >= 0; k--)
            pat[plen++] = (uint8_t)(x >> (8 * k));
    }
    if (!plen)
        return;
    for (gaddr p = a; p + (gaddr)plen <= a + len; p++) {
        size_t k = 0;
        while (k < plen && rd8(p + (gaddr)k) == pat[k])
            k++;
        if (k == plen) {
            out("  Found at %08X\n", p);
            dump(p, 16, 1);
            return;
        }
    }
    out("  Not found in %08X to %08X\n", a, a + len);
}

/* Each command: name, args (args[0] the name, lower case). */
static void command(char args[][160], int n, const char *rest)
{
    const char *c = args[0];
    uint32_t v;
#define IS(x) (!strcmp(c, x))
    if (IS("g")) {
        if (n > 1 && eval_str(rest, &v)) {
            if (cur_ppc())
                g_ppc.pc = v;
            else
                g_cpu->pc = v;
        }
        go();
    } else if (IS("gt")) {
        if (n < 2 || !eval_str(rest, &v)) {
            out("  GT addr\n");
            return;
        }
        add_temp(v, 0, "Go-till address reached");
        go();
    } else if (IS("t") || IS("so") || IS("s")) {
        uint32_t k = 1;
        if (n > 1 && !eval_str(rest, &k))
            return;
        step_left = k ? k : 1;
        step_over = !IS("s");
        go();
    } else if (IS("mr")) {
        magic_return(rest);
    } else if (IS("ss")) {
        if (n < 2) {
            spy_on = false;
            out("  Step spy off\n");
            macsbug_retrace();
            return;
        }
        uint32_t lo, hi;
        if (!eval_str(args[1], &lo))
            return;
        hi = lo + 4;
        if (n > 2 && !eval_str(args[2], &hi))
            return;
        if (hi <= lo)
            hi = lo + 4;
        spy_lo = lo;
        spy_hi = hi;
        spy_sum = checksum(lo, hi);
        spy_on = true;
        out("  Step spy on %08X to %08X: the machine stops when it changes\n", lo, hi);
        macsbug_retrace();
    } else if (IS("es")) {
        exit_to_shell();
    } else if (IS("br")) {
        if (n < 2) {
            list_breaks();
            return;
        }
        if (!eval_str(args[1], &v))
            return;
        uint32_t every = 0;
        char cond[96] = "", cmds[160] = "";
        break_options(args, 2, n, &every, cond, sizeof cond, cmds, sizeof cmds);
        add_break(v, every, cond, cmds);
        macsbug_retrace();
    } else if (IS("brc")) {
        if (n < 2) {
            int k = 0;
            for (int i = 0; i < nbreaks;)
                if (!breaks[i].temp) {
                    breaks[i] = breaks[--nbreaks];
                    k++;
                } else {
                    i++;
                }
            out("  All breakpoints cleared (%d)\n", k);
        } else if (eval_str(rest, &v)) {
            bool found = false;
            for (int i = 0; i < nbreaks; i++)
                if (breaks[i].addr == v && !breaks[i].temp) {
                    breaks[i] = breaks[--nbreaks];
                    found = true;
                    break;
                }
            out(found ? "  Breakpoint at %08X cleared\n" : "  No breakpoint at %08X\n", v);
        }
        macsbug_retrace();
    } else if (IS("brd")) {
        list_breaks();
    } else if (IS("atb") || IS("att") || IS("athc")) {
        add_watches(IS("atb") ? TW_BREAK : IS("att") ? TW_TRACE : TW_HEAP, args, n);
    } else if (IS("atc")) {
        clear_watches(args, n);
    } else if (IS("atd")) {
        list_watches();
    } else if (IS("atr")) {
        recording = n > 1 ? strcasecmp(args[1], "off") != 0 : !recording;
        if (recording && n > 1 && !strcasecmp(args[1], "on"))
            nrecords = record_next = 0;
        out("  A-trap recording %s\n", recording ? "on" : "off");
        update_traps();
    } else if (IS("atp")) {
        playback();
    } else if (IS("dx")) {
        dx_off = n > 1 ? !strcasecmp(args[1], "off") : !dx_off;
        out("  The program's Debugger calls %s\n", dx_off ? "are passed by (DX off)" : "stop here (DX on)");
    } else if (IS("dm") || IS("dp") || IS("db") || IS("dw") || IS("dl")) {
        gaddr a = list_kind == 'm' ? list_next : dot;
        if (n > 1 && !eval_str(args[1], &a))
            return;
        if (IS("db") || IS("dw") || IS("dl")) {
            uint32_t x = IS("db") ? rd8(a) : IS("dw") ? rd16(a) : rd32(a);
            out("  %08X  ", a);
            show_value_line(x);
            dot = a;
            return;
        }
        uint32_t len = IS("dp") ? 128 : 16;
        if (n > 2 && IS("dm")) {
            if (dump_template(a, args[2]))
                return;
            if (!eval_str(args[2], &len))
                return;
        }
        dump(a, len ? len : 16, 1);
    } else if (IS("sm") || IS("sb") || IS("sw") || IS("sl")) {
        if (n < 3 || !eval_str(args[1], &v)) {
            out("  %s addr value...\n", c);
            return;
        }
        set_memory(v, args, 2, n, IS("sb") ? 1 : IS("sw") ? 2 : IS("sl") ? 4 : 0);
    } else if (IS("f") || IS("fb") || IS("fw") || IS("fl")) {
        find(args, n, IS("fb") ? 1 : IS("fw") ? 2 : IS("fl") ? 4 : 0);
    } else if (IS("il") || IS("ip") || IS("id")) {
        gaddr a = list_kind == 'i' ? list_next : cur_pc();
        uint32_t count = IS("id") ? 1 : 16;
        if (n > 1 && !eval_str(args[1], &a))
            return;
        if (n > 2 && !eval_str(args[2], &count))
            return;
        /* A fragment's code is PowerPC; otherwise the processor stopped's. */
        bool ppc = ppc_code_at(a) || cur_ppc();
        if (IS("ip"))
            a = ppc ? a - 32 : back_68k(a, 8);
        list(ppc, a, (int)(count ? count : 1));
        dot = a;
    } else if (IS("rd") || IS("td")) {
        show_regs(IS("td"));
    } else if (IS("tf")) {
        show_float_regs();
    } else if (IS("sc") || IS("sc6") || IS("sc7")) {
        stack_crawl(IS("sc7"));
    } else if (IS("hz")) {
        heap_zones();
    } else if (IS("hx")) {
        if (n > 1 && eval_str(rest, &v))
            heap_zone = v;
        else if (n == 1)
            heap_zone = heap_zone == rd32(LM_SysZone) || (!heap_zone && !rd32(LM_ApplZone)) ? rd32(LM_ApplZone)
                                                                                             : rd32(LM_SysZone);
        out("  The heap commands show the %s heap at %08X\n", zone_name(current_zone()), current_zone());
    } else if (IS("hd")) {
        heap_display(n > 1 ? args[1] : "");
    } else if (IS("ht")) {
        heap_totals();
    } else if (IS("hc")) {
        heap_check();
    } else if (IS("wh")) {
        where(rest);
    } else if (IS("how")) {
        if (stopped)
            out("%s\n", stop_why);
        else
            out("  The machine is not stopped in the debugger\n");
    } else if (IS("help") || IS("?")) {
        help();
    } else if (IS("ev")) {
        if (eval_str(rest, &v))
            show_value_line(v);
    }
#undef IS
}

/* One command: "reg = value", or a command and its arguments. */
static void run_one(const char *cmd)
{
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (!*cmd)
        return;
    /* reg = expr, reg := expr */
    char reg[16];
    size_t k = 0;
    while (k < sizeof reg - 1 && isalnum((unsigned char)cmd[k]))
        reg[k] = cmd[k], k++;
    reg[k] = 0;
    const char *r = cmd + k;
    while (*r == ' ')
        r++;
    reg_name rn;
    if (k && (r[0] == '=' || (r[0] == ':' && r[1] == '=')) && r[1] != '=' && reg_lookup(reg, &rn)) {
        r += r[0] == ':' ? 2 : 1;
        uint32_t v;
        if (eval_str(r, &v)) {
            reg_set(&rn, v);
            out("  %s = %08X\n", reg, reg_get(&rn));
        }
        return;
    }
    char args[16][160];
    int n = split_args(cmd, args, 16);
    if (!n)
        return;
    for (char *p = args[0]; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    const char *rest = cmd;
    while (*rest && *rest != ' ' && *rest != '\t')
        rest++;
    while (*rest == ' ' || *rest == '\t')
        rest++;
    static const char *const known[] = { "g",  "gt", "t",  "so", "s",  "mr",  "ss",  "es",  "br",  "brc", "brd",
                                         "atb", "att", "athc", "atc", "atd", "atr", "atp", "dx",  "dm",  "dp",  "db",
                                         "dw", "dl", "sm", "sb", "sw", "sl", "f",  "fb",  "fw",  "fl",  "il",  "ip",
                                         "id", "rd", "td", "tf", "sc", "sc6", "sc7", "hz",  "hx",  "hd",  "ht",  "hc",
                                         "wh", "how", "help", "?", "ev" };
    bool is_cmd = false;
    for (size_t i = 0; i < sizeof known / sizeof known[0] && !is_cmd; i++)
        is_cmd = !strcmp(args[0], known[i]);
    if (!is_cmd) {
        /* An expression on its own: its value. */
        uint32_t v;
        if (eval_str(cmd, &v))
            show_value_line(v);
        return;
    }
    command(args, n, rest);
}

/* A line: commands separated by ";" outside quotes, until one goes on. */
static void run_line(const char *line)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", line);
    char *s = buf;
    while (*s && !resume) {
        char *e = s;
        char q = 0;
        while (*e && (q || *e != ';')) {
            if (q && *e == q)
                q = 0;
            else if (!q && (*e == '\'' || *e == '"'))
                q = *e;
            e++;
        }
        bool last = !*e;
        *e = 0;
        run_one(s);
        if (last)
            break;
        s = e + 1;
    }
}

/* ---------------------------------------------------------------------- */
/* The host's side */

void machine_debug_command(const char *line, char *o, size_t n)
{
    char *saved = capture;
    size_t saved_len = capture_len, saved_cap = capture_cap;
    capture = bb_host_calloc(1, 1);
    capture_len = 0;
    capture_cap = 1;
    if (!stopped)
        resume = false;
    run_line(line);
    if (!stopped)
        resume = false;
    if (n) {
        snprintf(o, n, "%s", capture ? capture : "");
    }
    bb_host_free(capture);
    capture = saved;
    capture_len = saved_len;
    capture_cap = saved_cap;
}

void machine_debug_script(const char *text)
{
    if (script_at && script_at >= script_len)
        script_len = script_at = 0;
    /* Commands separated by ";" are lines of their own: those after one
     * that goes on wait for the next stop.  A quoted ";" (a breakpoint's
     * commands) stays. */
    size_t from = script_len;
    append(&script, &script_len, &script_cap, text, strlen(text));
    char q = 0;
    for (size_t i = from; i < script_len; i++) {
        char ch = script[i];
        if (q && ch == q)
            q = 0;
        else if (!q && (ch == '\'' || ch == '"'))
            q = ch;
        else if (!q && ch == ';')
            script[i] = '\n';
        else if (ch == '\n')
            q = 0;
    }
    if (script_len && script[script_len - 1] != '\n')
        append(&script, &script_len, &script_cap, "\n", 1);
    update_traps();
}

bool macsbug_console(void) { return console_fn || (script && script_at < script_len); }

void machine_debug_attach(machine_debug_fn fn, void *ctx, bool interactive)
{
    pthread_mutex_lock(&post_lock);
    console_fn = fn;
    console_ctx = ctx;
    console_interactive = fn && interactive;
    pthread_cond_broadcast(&post_cond);
    pthread_mutex_unlock(&post_lock);
    update_traps();
}

void machine_debug_post(const char *line)
{
    pthread_mutex_lock(&post_lock);
    append(&posted, &posted_len, &posted_cap, line, strlen(line));
    append(&posted, &posted_len, &posted_cap, "\n", 1);
    pthread_cond_broadcast(&post_cond);
    pthread_mutex_unlock(&post_lock);
}

void machine_debug_reset(void)
{
    nbreaks = nwatches = 0;
    recording = false;
    nrecords = record_next = 0;
    step_left = 0;
    step_next = over_wait = false;
    spy_on = false;
    dx_off = false;
    skip_armed = false;
    atomic_store(&break_wanted, false);
    npending = 0;
    nstep_calls = step_calls_dropped = 0;
    heap_zone = 0;
    script_len = script_at = 0;
    bb_host_free(kept);
    kept = NULL;
    kept_len = kept_cap = 0;
    update_traps();
    macsbug_retrace();
}

void machine_debug_registers(char *o, size_t n)
{
    char *saved = capture;
    size_t saved_len = capture_len, saved_cap = capture_cap;
    capture = bb_host_calloc(1, 1);
    capture_len = 0;
    capture_cap = 1;
    machine_debug_fn fn = console_fn;
    console_fn = NULL; /* the panel's, not the console's */
    char *k = kept;
    size_t kl = kept_len, kc = kept_cap;
    kept = NULL;
    kept_len = kept_cap = 0;
    show_regs(true);
    bb_host_free(kept);
    kept = k;
    kept_len = kl;
    kept_cap = kc;
    console_fn = fn;
    snprintf(o, n, "%s", capture ? capture : "");
    bb_host_free(capture);
    capture = saved;
    capture_len = saved_len;
    capture_cap = saved_cap;
}
