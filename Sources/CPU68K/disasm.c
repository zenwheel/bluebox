/*
 * 68000-68040 disassembler (integer, supervisor, 68030/68040 MMU and cache
 * control, 68881/68882/68040 FPU), in MacsBug's style: uppercase, the
 * mnemonic padded to 10 columns, hex with $, branch targets as absolute
 * addresses.  Memory is read only through rd16/rd32, and all state lives in
 * locals (a mutable static would end up in saved snapshots).
 */
#include "m68k.h"

#include <stdarg.h>
#include <stdio.h>

enum { SZ_B, SZ_W, SZ_L, SZ_S, SZ_D, SZ_X, SZ_P, SZ_Q, SZ_NONE };
static const char sz_ch[] = "BWLSDXPQ";

/* Effective address classes, one bit per mode as the manuals' tables. */
enum {
    M_DN = 1 << 0, M_AN = 1 << 1, M_AI = 1 << 2, M_PI = 1 << 3, M_PD = 1 << 4,
    M_DI = 1 << 5, M_IX = 1 << 6, M_AW = 1 << 7, M_AL = 1 << 8,
    M_PCDI = 1 << 9, M_PCIX = 1 << 10, M_IM = 1 << 11,
    EA_ALL = 0xFFF,
    EA_DATA = EA_ALL & ~M_AN,
    EA_MEM = EA_DATA & ~M_DN,
    EA_CTRL = M_AI | M_DI | M_IX | M_AW | M_AL | M_PCDI | M_PCIX,
    EA_ALT = M_DN | M_AI | M_PI | M_PD | M_DI | M_IX | M_AW | M_AL,
    EA_MALT = EA_ALT & ~M_DN,
    EA_CALT = M_AI | M_DI | M_IX | M_AW | M_AL,
    EA_AALT = EA_ALT | M_AN,
};

typedef struct {
    uint32_t pc0;       /* address of the opcode word */
    uint32_t p;         /* next word to read */
    char mn[64];
    char ops[200];
    int ol;
    bool cmt;           /* a PC-relative target to show as a comment */
    uint32_t cmt_addr;
} D;

static const char *const cc_name[16] = {
    "T", "F", "HI", "LS", "CC", "CS", "NE", "EQ",
    "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE",
};

static const char *const fcc_name[32] = {
    "F", "EQ", "OGT", "OGE", "OLT", "OLE", "OGL", "OR",
    "UN", "UEQ", "UGT", "UGE", "ULT", "ULE", "NE", "T",
    "SF", "SEQ", "GT", "GE", "LT", "LE", "GL", "GLE",
    "NGLE", "NGL", "NLE", "NLT", "NGE", "NGT", "SNE", "ST",
};

static uint16_t next16(D *d)
{
    uint16_t v = rd16(d->p);
    d->p += 2;
    return v;
}

static uint32_t next32(D *d)
{
    uint32_t v = rd32(d->p);
    d->p += 4;
    return v;
}

static void put(D *d, const char *fmt, ...)
{
    va_list ap;
    int room = (int)sizeof d->ops - d->ol;
    if (room <= 1)
        return;
    va_start(ap, fmt);
    int n = vsnprintf(d->ops + d->ol, (size_t)room, fmt, ap);
    va_end(ap);
    if (n > 0)
        d->ol += n < room ? n : room - 1;
}

static void name(D *d, const char *m, int sz)
{
    if (sz < SZ_NONE)
        snprintf(d->mn, sizeof d->mn, "%s.%c", m, sz_ch[sz]);
    else
        snprintf(d->mn, sizeof d->mn, "%s", m);
}

static void name2(D *d, const char *a, const char *b, int sz)
{
    char t[24];
    snprintf(t, sizeof t, "%s%s", a, b);
    name(d, t, sz);
}

static int ea_class(int mode, int reg)
{
    if (mode < 7)
        return 1 << mode;
    return reg <= 4 ? 1 << (7 + reg) : 0;
}

static bool ea_ok(uint16_t op, int mask)
{
    return (ea_class((op >> 3) & 7, op & 7) & mask) != 0;
}

/* Signed hex, as MacsBug shows displacements: $0010, -$0004. */
static void put_s16(D *d, int32_t v)
{
    if (v < 0)
        put(d, "-$%04X", (unsigned)-v);
    else
        put(d, "$%04X", (unsigned)v);
}

static void put_s8(D *d, int32_t v)
{
    if (v < 0)
        put(d, "-$%02X", (unsigned)-v);
    else
        put(d, "$%02X", (unsigned)v);
}

static void put_imm(D *d, int sz)
{
    switch (sz) {
    case SZ_B: put(d, "#$%02X", next16(d) & 0xFF); break;
    case SZ_W: put(d, "#$%04X", next16(d)); break;
    case SZ_L:
    case SZ_S: put(d, "#$%08X", next32(d)); break;
    case SZ_D:
    case SZ_Q: {
        uint32_t hi = next32(d);
        put(d, "#$%08X%08X", hi, next32(d));
        break;
    }
    case SZ_X:
    case SZ_P: {
        uint32_t a = next32(d), b = next32(d);
        put(d, "#$%08X%08X%08X", a, b, next32(d));
        break;
    }
    }
}

static void put_xn(D *d, uint16_t ext)
{
    put(d, "%c%d.%c", ext & 0x8000 ? 'A' : 'D', (ext >> 12) & 7, ext & 0x800 ? 'L' : 'W');
    if (ext & 0x600)
        put(d, "*%d", 1 << ((ext >> 9) & 3));
}

/* A 68020 full-format displacement: word ones signed, long ones as is. */
static void put_disp(D *d, int size)
{
    if (size == 2)
        put_s16(d, (int16_t)next16(d));
    else
        put(d, "$%08X", next32(d));
}

/* Mode 6 or PC with index: the brief word, or the 68020 full format with
 * suppressed base/index and memory indirection. */
static bool ea_index(D *d, int reg, bool pc)
{
    uint16_t ext = next16(d);
    if (!(ext & 0x100)) {
        put_s8(d, (int8_t)ext);
        put(d, pc ? "(PC," : "(A%d,", reg);
        put_xn(d, ext);
        put(d, ")");
        return true;
    }
    /* The reserved combinations decode as the CPU (cpu68k.c) runs them:
     * BD size 0 is a null displacement, I/IS 4 postindexed with a null
     * outer displacement. */
    int bs = ext & 0x80, is = ext & 0x40, bdsz = (ext >> 4) & 3, iis = ext & 7;
    bool mem = iis != 0, post = !is && (iis & 4);
    int odsz = iis & 3;
    put(d, mem ? "([" : "(");
    bool any = false;
    if (bdsz > 1) {
        put_disp(d, bdsz);
        any = true;
    }
    if (!bs || pc) {
        if (any)
            put(d, ",");
        if (pc)
            put(d, bs ? "ZPC" : "PC");
        else
            put(d, "A%d", reg);
        any = true;
    }
    if (!is && !post) {
        if (any)
            put(d, ",");
        put_xn(d, ext);
        any = true;
    }
    if (!any)
        put(d, "0");
    if (mem) {
        put(d, "]");
        if (post) {
            put(d, ",");
            put_xn(d, ext);
        }
        if (odsz > 1) {
            put(d, ",");
            put_disp(d, odsz);
        }
    }
    put(d, ")");
    return true;
}

/* Prints the operand, reading its extension words.  sz is the immediate
 * size.  False for an encoding no chip accepts. */
static bool ea(D *d, int mode, int reg, int sz)
{
    switch (mode) {
    case 0: put(d, "D%d", reg); return true;
    case 1: put(d, "A%d", reg); return true;
    case 2: put(d, "(A%d)", reg); return true;
    case 3: put(d, "(A%d)+", reg); return true;
    case 4: put(d, "-(A%d)", reg); return true;
    case 5:
        put_s16(d, (int16_t)next16(d));
        put(d, "(A%d)", reg);
        return true;
    case 6: return ea_index(d, reg, false);
    }
    switch (reg) {
    case 0: put(d, "($%04X).W", next16(d)); return true;
    case 1: put(d, "($%08X).L", next32(d)); return true;
    case 2: {
        uint32_t at = d->p;
        int16_t v = (int16_t)next16(d);
        put_s16(d, v);
        put(d, "(PC)");
        d->cmt = true;
        d->cmt_addr = at + (uint32_t)(int32_t)v;
        return true;
    }
    case 3: return ea_index(d, 0, true);
    case 4:
        if (sz >= SZ_NONE)
            return false;
        put_imm(d, sz);
        return true;
    }
    return false;
}

static bool ea_op(D *d, uint16_t op, int sz)
{
    return ea(d, (op >> 3) & 7, op & 7, sz);
}

/* MOVEM's list: bit 0 is D0 and bit 15 A7, reversed for -(An). */
static void reglist(D *d, unsigned mask, bool predec)
{
    if (predec) {
        unsigned r = 0;
        for (int i = 0; i < 16; i++)
            if (mask & (1u << i))
                r |= 1u << (15 - i);
        mask = r;
    }
    if (!mask) {
        put(d, "#$0000");
        return;
    }
    bool first = true;
    for (int g = 0; g < 2; g++) {
        char k = g ? 'A' : 'D';
        for (int i = 0; i < 8; i++) {
            if (!(mask & (1u << (g * 8 + i))))
                continue;
            int j = i;
            while (j < 7 && (mask & (1u << (g * 8 + j + 1))))
                j++;
            put(d, first ? "%c%d" : "/%c%d", k, i);
            if (j > i)
                put(d, "-%c%d", k, j);
            first = false;
            i = j;
        }
    }
}

/* FMOVEM's list as a set: bit n is FPn. */
static void fplist(D *d, unsigned set)
{
    if (!set)
        put(d, "#$00");
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (!(set & (1u << i)))
            continue;
        int j = i;
        while (j < 7 && (set & (1u << (j + 1))))
            j++;
        put(d, first ? "FP%d" : "/FP%d", i);
        if (j > i)
            put(d, "-FP%d", j);
        first = false;
        i = j;
    }
}

static void ctllist(D *d, int list)
{
    static const char *const n[3] = { "FPIAR", "FPSR", "FPCR" };
    bool first = true;
    for (int i = 2; i >= 0; i--) {
        if (list & (1 << i)) {
            put(d, first ? "%s" : "/%s", n[i]);
            first = false;
        }
    }
}

static void target(D *d, uint32_t t)
{
    put(d, "$%08X", t);
}

/* ---- line 0: immediates, bits, MOVEP, MOVES, CAS, CHK2/CMP2 ---- */

static bool line0(D *d, uint16_t op)
{
    static const char *const imm_name[8] = { "ORI", "ANDI", "SUBI", "ADDI", NULL, "EORI", "CMPI", NULL };
    static const char *const bit_name[4] = { "BTST", "BCHG", "BCLR", "BSET" };
    int sz = (op >> 6) & 3;

    switch (op) {
    case 0x003C: case 0x023C: case 0x0A3C:
        name(d, imm_name[op >> 9], SZ_B);
        put_imm(d, SZ_B);
        put(d, ",CCR");
        return true;
    case 0x007C: case 0x027C: case 0x0A7C:
        name(d, imm_name[op >> 9], SZ_W);
        put_imm(d, SZ_W);
        put(d, ",SR");
        return true;
    }
    if (op & 0x100) {
        if ((op & 0x38) == 0x08) {
            int dx = (op >> 9) & 7, ay = op & 7, opm = (op >> 6) & 7;
            name(d, "MOVEP", opm & 1 ? SZ_L : SZ_W);
            int16_t disp = (int16_t)next16(d);
            if (opm & 2) {
                put(d, "D%d,", dx);
                put_s16(d, disp);
                put(d, "(A%d)", ay);
            } else {
                put_s16(d, disp);
                put(d, "(A%d),D%d", ay, dx);
            }
            return true;
        }
        int t = sz;
        if (!ea_ok(op, t == 0 ? EA_DATA : EA_ALT))
            return false;
        name(d, bit_name[t], SZ_NONE);
        put(d, "D%d,", (op >> 9) & 7);
        return ea_op(d, op, SZ_B);
    }
    int kind = (op >> 9) & 7;
    if (kind == 4) {
        int t = sz;
        if (!ea_ok(op, t == 0 ? EA_DATA & ~M_IM : EA_ALT))
            return false;
        name(d, bit_name[t], SZ_NONE);
        put(d, "#$%04X,", next16(d));
        return ea_op(d, op, SZ_NONE);
    }
    if (sz == 3) {
        if (kind <= 2) {
            if (!ea_ok(op, EA_CTRL))
                return false;
            uint16_t ext = next16(d);
            if (ext & 0x7FF)
                return false;
            name(d, ext & 0x800 ? "CHK2" : "CMP2", kind);
            if (!ea_op(d, op, SZ_NONE))
                return false;
            put(d, ",%c%d", ext & 0x8000 ? 'A' : 'D', (ext >> 12) & 7);
            return true;
        }
        if (kind >= 5) {
            int csz = kind - 5;
            if ((op & 0x3F) == 0x3C && csz) {
                uint16_t e1 = next16(d), e2 = next16(d);
                if ((e1 & 0x0E38) || (e2 & 0x0E38))
                    return false;
                name(d, "CAS2", csz);
                put(d, "D%d:D%d,D%d:D%d,(%c%d):(%c%d)", e1 & 7, e2 & 7, (e1 >> 6) & 7, (e2 >> 6) & 7,
                    e1 & 0x8000 ? 'A' : 'D', (e1 >> 12) & 7, e2 & 0x8000 ? 'A' : 'D', (e2 >> 12) & 7);
                return true;
            }
            if (!ea_ok(op, EA_MALT))
                return false;
            uint16_t ext = next16(d);
            if (ext & 0xFE38)
                return false;
            name(d, "CAS", csz);
            put(d, "D%d,D%d,", ext & 7, (ext >> 6) & 7);
            return ea_op(d, op, SZ_NONE);
        }
        return false;   /* CALLM/RTM: 68020 only */
    }
    if (kind == 7) {
        if (!ea_ok(op, EA_MALT))
            return false;
        uint16_t ext = next16(d);
        if (ext & 0x7FF)
            return false;
        name(d, "MOVES", sz);
        char r = ext & 0x8000 ? 'A' : 'D';
        int rn = (ext >> 12) & 7;
        if (ext & 0x800) {
            put(d, "%c%d,", r, rn);
            return ea_op(d, op, SZ_NONE);
        }
        if (!ea_op(d, op, SZ_NONE))
            return false;
        put(d, ",%c%d", r, rn);
        return true;
    }
    if (!ea_ok(op, kind == 6 ? EA_DATA & ~M_IM : EA_ALT))
        return false;
    name(d, imm_name[kind], sz);
    put_imm(d, sz);
    put(d, ",");
    return ea_op(d, op, SZ_NONE);
}

/* ---- lines 1-3: MOVE, MOVEA ---- */

static bool line_move(D *d, uint16_t op)
{
    static const int msz[4] = { -1, SZ_B, SZ_L, SZ_W };
    int sz = msz[op >> 12];
    int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    if (!ea_ok(op, sz == SZ_B ? EA_DATA : EA_ALL))
        return false;
    if (dmode == 1) {
        if (sz == SZ_B)
            return false;
        name(d, "MOVEA", sz);
    } else {
        if (!(ea_class(dmode, dreg) & EA_ALT))
            return false;
        name(d, "MOVE", sz);
    }
    if (!ea_op(d, op, sz))
        return false;
    put(d, ",");
    return ea(d, dmode, dreg, SZ_NONE);
}

/* ---- line 4: miscellaneous ---- */

static const char *movec_name(int c)
{
    switch (c) {
    case 0x000: return "SFC";
    case 0x001: return "DFC";
    case 0x002: return "CACR";
    case 0x003: return "TC";
    case 0x004: return "ITT0";
    case 0x005: return "ITT1";
    case 0x006: return "DTT0";
    case 0x007: return "DTT1";
    case 0x800: return "USP";
    case 0x801: return "VBR";
    case 0x802: return "CAAR";
    case 0x803: return "MSP";
    case 0x804: return "ISP";
    case 0x805: return "MMUSR";
    case 0x806: return "URP";
    case 0x807: return "SRP";
    }
    return NULL;
}

static bool line4(D *d, uint16_t op)
{
    int sz = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7, rx = (op >> 9) & 7;

    if (op & 0x100) {
        switch ((op >> 6) & 7) {
        case 7:
            if ((op & 0xFFF8) == 0x49C0) {
                name(d, "EXTB", SZ_L);
                put(d, "D%d", reg);
                return true;
            }
            if (!ea_ok(op, EA_CTRL))
                return false;
            name(d, "LEA", SZ_NONE);
            if (!ea_op(d, op, SZ_NONE))
                return false;
            put(d, ",A%d", rx);
            return true;
        case 6:
        case 4:
            if (!ea_ok(op, EA_DATA))
                return false;
            name(d, "CHK", (op & 0x80) ? SZ_W : SZ_L);
            if (!ea_op(d, op, (op & 0x80) ? SZ_W : SZ_L))
                return false;
            put(d, ",D%d", rx);
            return true;
        }
        return false;
    }

    switch ((op >> 8) & 0xF) {
    case 0x0: case 0x2: case 0x4: case 0x6: {
        int k = (op >> 9) & 3;
        if (sz == 3) {
            bool to = k >= 2;
            if (!ea_ok(op, to ? EA_DATA : EA_ALT))
                return false;
            name(d, "MOVE", SZ_W);
            const char *r = (k == 0 || k == 3) ? "SR" : "CCR";
            if (to) {
                if (!ea_op(d, op, SZ_W))
                    return false;
                put(d, ",%s", r);
                return true;
            }
            put(d, "%s,", r);
            return ea_op(d, op, SZ_NONE);
        }
        static const char *const n[4] = { "NEGX", "CLR", "NEG", "NOT" };
        if (!ea_ok(op, EA_ALT))
            return false;
        name(d, n[k], sz);
        return ea_op(d, op, SZ_NONE);
    }
    case 0x8:
        switch (sz) {
        case 0:
            if (mode == 1) {
                name(d, "LINK", SZ_L);
                int32_t v = (int32_t)next32(d);
                if (v < 0)
                    put(d, "A%d,#-$%08X", reg, 0u - (uint32_t)v);
                else
                    put(d, "A%d,#$%08X", reg, (uint32_t)v);
                return true;
            }
            if (!ea_ok(op, EA_ALT))
                return false;
            name(d, "NBCD", SZ_B);
            return ea_op(d, op, SZ_NONE);
        case 1:
            if (mode == 0) {
                name(d, "SWAP", SZ_NONE);
                put(d, "D%d", reg);
                return true;
            }
            if (mode == 1) {
                name(d, "BKPT", SZ_NONE);
                put(d, "#$%d", reg);
                return true;
            }
            if (!ea_ok(op, EA_CTRL))
                return false;
            name(d, "PEA", SZ_NONE);
            return ea_op(d, op, SZ_NONE);
        default:
            if (mode == 0) {
                name(d, "EXT", sz == 2 ? SZ_W : SZ_L);
                put(d, "D%d", reg);
                return true;
            }
            if (!ea_ok(op, EA_CALT | M_PD))
                return false;
            name(d, "MOVEM", sz == 2 ? SZ_W : SZ_L);
            reglist(d, next16(d), mode == 4);
            put(d, ",");
            return ea_op(d, op, SZ_NONE);
        }
    case 0xA:
        if (op == 0x4AFC) {
            name(d, "ILLEGAL", SZ_NONE);
            return true;
        }
        if (sz == 3) {
            if (!ea_ok(op, EA_ALT))
                return false;
            name(d, "TAS", SZ_B);
            return ea_op(d, op, SZ_NONE);
        }
        if (!ea_ok(op, sz == SZ_B ? EA_DATA : EA_ALL))
            return false;
        name(d, "TST", sz);
        return ea_op(d, op, sz);
    case 0xC:
        if (sz < 2) {
            if (!ea_ok(op, EA_DATA))
                return false;
            uint16_t ext = next16(d);
            if (ext & 0x83F8)
                return false;
            int dl = (ext >> 12) & 7, dh = ext & 7;
            bool sgn = ext & 0x800, quad = ext & 0x400;
            if (sz == 0)
                name(d, sgn ? "MULS" : "MULU", SZ_L);
            else if (!quad && dl != dh)
                name(d, sgn ? "DIVSL" : "DIVUL", SZ_L);
            else
                name(d, sgn ? "DIVS" : "DIVU", SZ_L);
            if (!ea_op(d, op, SZ_L))
                return false;
            if (quad || (sz == 1 && dl != dh))
                put(d, ",D%d:D%d", dh, dl);
            else
                put(d, ",D%d", dl);
            return true;
        }
        if (!ea_ok(op, EA_CTRL | M_PI))
            return false;
        name(d, "MOVEM", sz == 2 ? SZ_W : SZ_L);
        {
            uint16_t mask = next16(d);
            if (!ea_op(d, op, SZ_NONE))
                return false;
            put(d, ",");
            reglist(d, mask, false);
        }
        return true;
    case 0xE:
        if (sz == 1) {
            switch (mode) {
            case 0: case 1:
                name(d, "TRAP", SZ_NONE);
                put(d, "#$%X", op & 15);
                return true;
            case 2:
                name(d, "LINK", SZ_NONE);
                put(d, "A%d,#", reg);
                put_s16(d, (int16_t)next16(d));
                return true;
            case 3:
                name(d, "UNLK", SZ_NONE);
                put(d, "A%d", reg);
                return true;
            case 4:
                name(d, "MOVE", SZ_L);
                put(d, "A%d,USP", reg);
                return true;
            case 5:
                name(d, "MOVE", SZ_L);
                put(d, "USP,A%d", reg);
                return true;
            case 6:
                switch (reg) {
                case 0: name(d, "RESET", SZ_NONE); return true;
                case 1: name(d, "NOP", SZ_NONE); return true;
                case 2:
                    name(d, "STOP", SZ_NONE);
                    put_imm(d, SZ_W);
                    return true;
                case 3: name(d, "RTE", SZ_NONE); return true;
                case 4:
                    name(d, "RTD", SZ_NONE);
                    put(d, "#");
                    put_s16(d, (int16_t)next16(d));
                    return true;
                case 5: name(d, "RTS", SZ_NONE); return true;
                case 6: name(d, "TRAPV", SZ_NONE); return true;
                case 7: name(d, "RTR", SZ_NONE); return true;
                }
                return false;
            case 7: {
                if (reg != 2 && reg != 3)
                    return false;
                uint16_t ext = next16(d);
                const char *c = movec_name(ext & 0xFFF);
                char num[8];
                if (!c) {
                    snprintf(num, sizeof num, "$%03X", ext & 0xFFF);
                    c = num;
                }
                name(d, "MOVEC", SZ_NONE);
                char r = ext & 0x8000 ? 'A' : 'D';
                int rn = (ext >> 12) & 7;
                if (reg == 2)
                    put(d, "%s,%c%d", c, r, rn);
                else
                    put(d, "%c%d,%s", r, rn, c);
                return true;
            }
            }
            return false;
        }
        if (sz >= 2) {
            if (!ea_ok(op, EA_CTRL))
                return false;
            name(d, sz == 2 ? "JSR" : "JMP", SZ_NONE);
            return ea_op(d, op, SZ_NONE);
        }
        return false;
    }
    return false;
}

/* ---- line 5: ADDQ, SUBQ, Scc, DBcc, TRAPcc ---- */

static bool line5(D *d, uint16_t op)
{
    int sz = (op >> 6) & 3, cc = (op >> 8) & 15;
    if (sz == 3) {
        if ((op & 0x38) == 0x08) {
            name2(d, "DB", cc == 1 ? "RA" : cc_name[cc], SZ_NONE);
            put(d, "D%d,", op & 7);
            uint32_t at = d->p;
            target(d, at + (uint32_t)(int32_t)(int16_t)next16(d));
            return true;
        }
        int r = op & 0x3F;
        if (r == 0x3A || r == 0x3B) {
            name2(d, "TRAP", cc_name[cc], r == 0x3A ? SZ_W : SZ_L);
            put_imm(d, r == 0x3A ? SZ_W : SZ_L);
            return true;
        }
        if (r == 0x3C) {
            name2(d, "TRAP", cc_name[cc], SZ_NONE);
            return true;
        }
        if (!ea_ok(op, EA_ALT))
            return false;
        name2(d, "S", cc_name[cc], SZ_NONE);
        return ea_op(d, op, SZ_NONE);
    }
    if (!ea_ok(op, sz == SZ_B ? EA_ALT : EA_AALT))
        return false;
    int q = (op >> 9) & 7;
    name(d, op & 0x100 ? "SUBQ" : "ADDQ", sz);
    put(d, "#$%X,", q ? q : 8);
    return ea_op(d, op, SZ_NONE);
}

/* ---- line 6: Bcc, BRA, BSR ---- */

static bool line6(D *d, uint16_t op)
{
    int cc = (op >> 8) & 15;
    const char *n = cc == 0 ? "BRA" : cc == 1 ? "BSR" : NULL;
    uint32_t base = d->pc0 + 2;
    int32_t disp = (int8_t)op;
    int sz = SZ_S;
    if (disp == 0) {
        disp = (int16_t)next16(d);
        sz = SZ_W;
    } else if (disp == -1) {
        disp = (int32_t)next32(d);
        sz = SZ_L;
    }
    if (n)
        name(d, n, sz);
    else
        name2(d, "B", cc_name[cc], sz);
    target(d, base + (uint32_t)disp);
    return true;
}

/* ---- lines 8, 9, B, C, D: arithmetic and logic ---- */

static bool line_arith(D *d, uint16_t op)
{
    int line = op >> 12, rx = (op >> 9) & 7, opm = (op >> 6) & 7, ry = op & 7;
    int sz = opm & 3;

    if (opm == 3 || opm == 7) {
        if (line == 0x8 || line == 0xC) {
            if (!ea_ok(op, EA_DATA))
                return false;
            static const char *const n[2][2] = { { "DIVU", "DIVS" }, { "MULU", "MULS" } };
            name(d, n[line == 0xC][opm == 7], SZ_W);
            if (!ea_op(d, op, SZ_W))
                return false;
            put(d, ",D%d", rx);
            return true;
        }
        int asz = opm == 3 ? SZ_W : SZ_L;
        name(d, line == 0x9 ? "SUBA" : line == 0xD ? "ADDA" : "CMPA", asz);
        if (!ea_op(d, op, asz))
            return false;
        put(d, ",A%d", rx);
        return true;
    }

    static const char *const basic[16] = {
        [0x8] = "OR", [0x9] = "SUB", [0xB] = "CMP", [0xC] = "AND", [0xD] = "ADD",
    };
    if (opm < 3) {
        if (!ea_ok(op, (sz == SZ_B || line == 0x8 || line == 0xC) ? EA_DATA : EA_ALL))
            return false;
        name(d, basic[line], sz);
        if (!ea_op(d, op, sz))
            return false;
        put(d, ",D%d", rx);
        return true;
    }

    /* opmode 4-6: register to memory, or the two-register forms */
    if (line == 0xB) {
        if ((op & 0x38) == 0x08) {
            name(d, "CMPM", sz);
            put(d, "(A%d)+,(A%d)+", ry, rx);
            return true;
        }
        if (!ea_ok(op, EA_ALT))
            return false;
        name(d, "EOR", sz);
        put(d, "D%d,", rx);
        return ea_op(d, op, SZ_NONE);
    }
    if ((op & 0x30) == 0) {
        bool mem = op & 8;
        if (line == 0xC && opm != 4) {
            int k = op & 0x1F8;
            if (k == 0x140)
                put(d, "D%d,D%d", rx, ry);
            else if (k == 0x148)
                put(d, "A%d,A%d", rx, ry);
            else if (k == 0x188)
                put(d, "D%d,A%d", rx, ry);
            else
                return false;
            name(d, "EXG", SZ_NONE);
            return true;
        }
        if (line == 0x8 && opm != 4) {
            name(d, opm == 5 ? "PACK" : "UNPK", SZ_NONE);
            if (mem)
                put(d, "-(A%d),-(A%d),", ry, rx);
            else
                put(d, "D%d,D%d,", ry, rx);
            put_imm(d, SZ_W);
            return true;
        }
        if (line == 0x8 || line == 0xC)
            name(d, line == 0x8 ? "SBCD" : "ABCD", SZ_NONE);
        else
            name(d, line == 0x9 ? "SUBX" : "ADDX", sz);
        if (mem)
            put(d, "-(A%d),-(A%d)", ry, rx);
        else
            put(d, "D%d,D%d", ry, rx);
        return true;
    }
    if (!ea_ok(op, EA_MALT))
        return false;
    name(d, basic[line], sz);
    put(d, "D%d,", rx);
    return ea_op(d, op, SZ_NONE);
}

/* ---- line 7: MOVEQ and Blue Box's NATIVE ---- */

static bool line7(D *d, uint16_t op)
{
    if (op == CPU_NATIVE_OP) {
        name(d, "NATIVE", SZ_NONE);
        put(d, "$%04X", next16(d));
        return true;
    }
    if (op & 0x100)
        return false;
    name(d, "MOVEQ", SZ_NONE);
    put(d, "#$%02X,D%d", op & 0xFF, (op >> 9) & 7);
    return true;
}

/* ---- line E: shifts, rotates, bit fields ---- */

static bool lineE(D *d, uint16_t op)
{
    static const char *const sh[4] = { "AS", "LS", "ROX", "RO" };
    if ((op & 0xC0) == 0xC0) {
        if (op & 0x800) {
            static const char *const bf[8] = {
                "BFTST", "BFEXTU", "BFCHG", "BFEXTS", "BFCLR", "BFFFO", "BFSET", "BFINS",
            };
            int t = (op >> 8) & 7;
            bool writes = t == 2 || t == 4 || t == 6 || t == 7;
            if (!ea_ok(op, M_DN | (writes ? EA_CALT : EA_CTRL)))
                return false;
            uint16_t ext = next16(d);
            if (ext & 0x8000)
                return false;
            int dn = (ext >> 12) & 7;
            name(d, bf[t], SZ_NONE);
            if (t == 7)
                put(d, "D%d,", dn);
            if (!ea_op(d, op, SZ_NONE))
                return false;
            if (ext & 0x800)
                put(d, "{D%d:", (ext >> 6) & 7);
            else
                put(d, "{%d:", (ext >> 6) & 31);
            if (ext & 0x20)
                put(d, "D%d}", ext & 7);
            else
                put(d, "%d}", (ext & 31) ? ext & 31 : 32);
            if (t == 1 || t == 3 || t == 5)
                put(d, ",D%d", dn);
            return true;
        }
        if (!ea_ok(op, EA_MALT))
            return false;
        name2(d, sh[(op >> 9) & 3], op & 0x100 ? "L" : "R", SZ_W);
        return ea_op(d, op, SZ_NONE);
    }
    int sz = (op >> 6) & 3, c = (op >> 9) & 7;
    name2(d, sh[(op >> 3) & 3], op & 0x100 ? "L" : "R", sz);
    if (op & 0x20)
        put(d, "D%d,D%d", c, op & 7);
    else
        put(d, "#$%X,D%d", c ? c : 8, op & 7);
    return true;
}

/* ---- line F: FPU (coprocessor 1), 68030 PMMU (0), 68040 extras ---- */

static const char *const fop_name[0x80] = {
    [0x00] = "FMOVE", [0x01] = "FINT", [0x02] = "FSINH", [0x03] = "FINTRZ",
    [0x04] = "FSQRT", [0x06] = "FLOGNP1", [0x08] = "FETOXM1", [0x09] = "FTANH",
    [0x0A] = "FATAN", [0x0C] = "FASIN", [0x0D] = "FATANH", [0x0E] = "FSIN",
    [0x0F] = "FTAN", [0x10] = "FETOX", [0x11] = "FTWOTOX", [0x12] = "FTENTOX",
    [0x14] = "FLOGN", [0x15] = "FLOG10", [0x16] = "FLOG2", [0x18] = "FABS",
    [0x19] = "FCOSH", [0x1A] = "FNEG", [0x1C] = "FACOS", [0x1D] = "FCOS",
    [0x1E] = "FGETEXP", [0x1F] = "FGETMAN", [0x20] = "FDIV", [0x21] = "FMOD",
    [0x22] = "FADD", [0x23] = "FMUL", [0x24] = "FSGLDIV", [0x25] = "FREM",
    [0x26] = "FSCALE", [0x27] = "FSGLMUL", [0x28] = "FSUB",
    [0x30] = "FSINCOS", [0x31] = "FSINCOS", [0x32] = "FSINCOS", [0x33] = "FSINCOS",
    [0x34] = "FSINCOS", [0x35] = "FSINCOS", [0x36] = "FSINCOS", [0x37] = "FSINCOS",
    [0x38] = "FCMP", [0x3A] = "FTST",
    /* 68040 single- and double-rounded forms */
    [0x40] = "FSMOVE", [0x41] = "FSSQRT", [0x44] = "FDMOVE", [0x45] = "FDSQRT",
    [0x58] = "FSABS", [0x5A] = "FSNEG", [0x5C] = "FDABS", [0x5E] = "FDNEG",
    [0x60] = "FSDIV", [0x62] = "FSADD", [0x63] = "FSMUL", [0x64] = "FDDIV",
    [0x66] = "FDADD", [0x67] = "FDMUL", [0x68] = "FSSUB", [0x6C] = "FDSUB",
};

/* Format field to size: L S X P W D B (7 is the FMOVECR/dynamic-k slot). */
static const int ffmt_sz[8] = { SZ_L, SZ_S, SZ_X, SZ_P, SZ_W, SZ_D, SZ_B, SZ_P };

static void fop_dst(D *d, int opm, int dst, uint16_t ext)
{
    if (opm == 0x3A)
        return;                         /* FTST has no destination */
    if ((opm & 0x78) == 0x30)
        put(d, ",FP%d:FP%d", ext & 7, dst);
    else
        put(d, ",FP%d", dst);
}

static bool fpu_general(D *d, uint16_t op)
{
    uint16_t ext = next16(d);
    int type = ext >> 13, fmt = (ext >> 10) & 7, dst = (ext >> 7) & 7;
    int opm = ext & 0x7F;

    switch (type) {
    case 0:
        if (!fop_name[opm])     /* the CPU ignores the EA field here */
            return false;
        name(d, fop_name[opm], SZ_X);
        put(d, "FP%d", fmt);
        fop_dst(d, opm, dst, ext);
        return true;
    case 2: {
        if (fmt == 7) {
            if (op & 0x3F)
                return false;
            name(d, "FMOVECR", SZ_X);
            put(d, "#$%02X,FP%d", opm, dst);
            return true;
        }
        int sz = ffmt_sz[fmt];
        bool dn_ok = sz == SZ_L || sz == SZ_S || sz == SZ_W || sz == SZ_B;
        if (!fop_name[opm] || !ea_ok(op, dn_ok ? EA_DATA : EA_MEM))
            return false;
        name(d, fop_name[opm], sz);
        if (!ea_op(d, op, sz))
            return false;
        fop_dst(d, opm, dst, ext);
        return true;
    }
    case 3: {
        int sz = ffmt_sz[fmt];
        bool dn_ok = sz == SZ_L || sz == SZ_S || sz == SZ_W || sz == SZ_B;
        if (!ea_ok(op, dn_ok ? EA_ALT : EA_MALT))
            return false;
        if (fmt != 3 && fmt != 7 && opm)
            return false;
        name(d, "FMOVE", sz);
        put(d, "FP%d,", dst);
        if (!ea_op(d, op, SZ_NONE))
            return false;
        if (fmt == 3) {
            int k = opm & 0x40 ? opm - 0x80 : opm;
            put(d, "{#%d}", k);
        } else if (fmt == 7) {
            if (opm & 0xF)
                return false;
            put(d, "{D%d}", (opm >> 4) & 7);
        }
        return true;
    }
    case 4:
    case 5: {
        int list = fmt, n = (list & 1) + ((list >> 1) & 1) + ((list >> 2) & 1);
        if (!list)
            return false;
        int mask = type == 4 ? EA_ALL : EA_ALT | M_AN;
        if (n > 1)
            mask &= ~(M_DN | M_AN);
        else if (list != 1)
            mask &= ~M_AN;
        if (!ea_ok(op, mask))
            return false;
        name(d, n > 1 ? "FMOVEM" : "FMOVE", SZ_L);
        if (type == 4) {
            if ((op & 0x3F) == 0x3C) {
                for (int i = 0; i < n; i++)
                    put(d, i ? ",#$%08X" : "#$%08X", next32(d));
            } else if (!ea_op(d, op, SZ_NONE)) {
                return false;
            }
            put(d, ",");
            ctllist(d, list);
            return true;
        }
        ctllist(d, list);
        put(d, ",");
        return ea_op(d, op, SZ_NONE);
    }
    case 6:
    case 7: {
        bool to_mem = type == 7;
        bool postinc = ext & 0x1000, dyn = ext & 0x800;
        if (!ea_ok(op, to_mem ? EA_CALT | M_PD : EA_CTRL | M_PI))
            return false;
        name(d, "FMOVEM", SZ_X);
        if (!to_mem) {
            if (!ea_op(d, op, SZ_NONE))
                return false;
            put(d, ",");
        }
        if (dyn) {
            put(d, "D%d", (ext >> 4) & 7);
        } else {
            unsigned set = 0;
            for (int i = 0; i < 8; i++)
                if (ext & (1u << i))
                    set |= 1u << (postinc ? 7 - i : i);
            fplist(d, set);
        }
        if (!to_mem)
            return true;
        put(d, ",");
        return ea_op(d, op, SZ_NONE);
    }
    }
    return false;
}

static bool fpu(D *d, uint16_t op)
{
    switch ((op >> 6) & 7) {
    case 0:
        return fpu_general(d, op);
    case 1: {
        uint16_t c = next16(d);
        if (c & 0xFFE0)
            return false;
        if ((op & 0x38) == 0x08) {
            name2(d, "FDB", fcc_name[c], SZ_NONE);
            put(d, "D%d,", op & 7);
            uint32_t at = d->p;
            target(d, at + (uint32_t)(int32_t)(int16_t)next16(d));
            return true;
        }
        int r = op & 0x3F;
        if (r == 0x3A || r == 0x3B) {
            name2(d, "FTRAP", fcc_name[c], r == 0x3A ? SZ_W : SZ_L);
            put_imm(d, r == 0x3A ? SZ_W : SZ_L);
            return true;
        }
        if (r == 0x3C) {
            name2(d, "FTRAP", fcc_name[c], SZ_NONE);
            return true;
        }
        if (!ea_ok(op, EA_ALT))
            return false;
        name2(d, "FS", fcc_name[c], SZ_NONE);
        return ea_op(d, op, SZ_NONE);
    }
    case 2:
    case 3: {
        int c = op & 0x3F;
        if (c & 0x20)
            return false;
        uint32_t base = d->pc0 + 2;
        int32_t disp;
        bool l = op & 0x40;
        if (l)
            disp = (int32_t)next32(d);
        else
            disp = (int16_t)next16(d);
        if (op == 0xF280 && disp == 0) {
            name(d, "FNOP", SZ_NONE);
            return true;
        }
        name2(d, "FB", fcc_name[c], l ? SZ_L : SZ_W);
        target(d, base + (uint32_t)disp);
        return true;
    }
    case 4:
        if (!ea_ok(op, EA_CALT | M_PD))
            return false;
        name(d, "FSAVE", SZ_NONE);
        return ea_op(d, op, SZ_NONE);
    case 5:
        if (!ea_ok(op, EA_CTRL | M_PI))
            return false;
        name(d, "FRESTORE", SZ_NONE);
        return ea_op(d, op, SZ_NONE);
    }
    return false;
}

/* PMMU function code operand: SFC, DFC, Dn or #n. */
static bool put_fc(D *d, int fc)
{
    if (fc == 0)
        put(d, "SFC");
    else if (fc == 1)
        put(d, "DFC");
    else if ((fc & 0x18) == 0x08)
        put(d, "D%d", fc & 7);
    else if ((fc & 0x18) == 0x10)
        put(d, "#$%X", fc & 7);
    else
        return false;
    return true;
}

static bool pmmu030(D *d, uint16_t op)
{
    if (op & 0x1C0)
        return false;
    uint16_t ext = next16(d);
    int top = ext >> 13;

    if (top == 1) {
        if (ext == 0x2400) {
            if (op & 0x3F)
                return false;
            name(d, "PFLUSHA", SZ_NONE);
            return true;
        }
        if ((ext & 0xFDE0) == 0x2000) {
            if (!ea_ok(op, EA_CALT))
                return false;
            name(d, ext & 0x200 ? "PLOADR" : "PLOADW", SZ_NONE);
            if (!put_fc(d, ext & 0x1F))
                return false;
            put(d, ",");
            return ea_op(d, op, SZ_NONE);
        }
        int mode = (ext >> 10) & 7;
        if ((mode != 4 && mode != 6) || (ext & 0x300))
            return false;
        if (mode == 4 ? (op & 0x3F) != 0 : !ea_ok(op, EA_CALT))
            return false;
        name(d, "PFLUSH", SZ_NONE);
        if (!put_fc(d, ext & 0x1F))
            return false;
        put(d, ",#$%X", (ext >> 5) & 7);
        if (mode == 6) {
            put(d, ",");
            return ea_op(d, op, SZ_NONE);
        }
        return true;
    }
    if (top == 4) {
        if (!ea_ok(op, EA_CALT))
            return false;
        int level = (ext >> 10) & 7;
        name(d, ext & 0x200 ? "PTESTR" : "PTESTW", SZ_NONE);
        if (!put_fc(d, ext & 0x1F))
            return false;
        put(d, ",");
        if (!ea_op(d, op, SZ_NONE))
            return false;
        put(d, ",#$%X", level);
        if (ext & 0x100)
            put(d, ",A%d", (ext >> 5) & 7);
        else if ((ext >> 5) & 7)
            return false;
        return true;
    }
    if (top == 0 || top == 2 || top == 3) {
        if (ext & 0xFF)
            return false;
        int preg = (ext >> 10) & 7;
        bool rd = ext & 0x200, fd = ext & 0x100;
        const char *r;
        int sz;
        if (top == 0 && preg == 2) { r = "TT0"; sz = SZ_L; }
        else if (top == 0 && preg == 3) { r = "TT1"; sz = SZ_L; }
        else if (top == 2 && preg == 0) { r = "TC"; sz = SZ_L; }
        else if (top == 2 && preg == 2) { r = "SRP"; sz = SZ_Q; }
        else if (top == 2 && preg == 3) { r = "CRP"; sz = SZ_Q; }
        else if (top == 3 && preg == 0 && !fd) { r = "MMUSR"; sz = SZ_W; }
        else return false;
        int mask = (rd ? EA_ALT : EA_ALL) & ~M_AN;
        if (sz == SZ_Q)
            mask &= ~M_DN;
        if (!ea_ok(op, mask))
            return false;
        name(d, fd ? "PMOVEFD" : "PMOVE", sz);
        if (rd) {
            put(d, "%s,", r);
            return ea_op(d, op, SZ_NONE);
        }
        if (!ea_op(d, op, sz))
            return false;
        put(d, ",%s", r);
        return true;
    }
    return false;
}

static bool lineF(D *d, uint16_t op)
{
    int cp = (op >> 9) & 7;
    if (cp == 1)
        return fpu(d, op);
    if (cp == 0)
        return pmmu030(d, op);
    if ((op & 0xFF00) == 0xF400) {
        static const char *const caches[4] = { "NC", "DC", "IC", "BC" };
        static const char *const scope[4] = { NULL, "L", "P", "A" };
        int s = (op >> 3) & 3;
        if (!s)
            return false;
        name2(d, op & 0x20 ? "CPUSH" : "CINV", scope[s], SZ_NONE);
        put(d, "%s", caches[(op >> 6) & 3]);
        if (s != 3)
            put(d, ",(A%d)", op & 7);
        else if (op & 7)
            return false;
        return true;
    }
    if ((op & 0xFFE0) == 0xF500) {
        static const char *const n[4] = { "PFLUSHN", "PFLUSH", "PFLUSHAN", "PFLUSHA" };
        int m = (op >> 3) & 3;
        name(d, n[m], SZ_NONE);
        if (m < 2)
            put(d, "(A%d)", op & 7);
        else if (op & 7)
            return false;
        return true;
    }
    if ((op & 0xFFD8) == 0xF548) {
        name(d, op & 0x20 ? "PTESTR" : "PTESTW", SZ_NONE);
        put(d, "(A%d)", op & 7);
        return true;
    }
    if ((op & 0xFFE0) == 0xF600) {
        int ay = op & 7;
        uint32_t a = next32(d);
        name(d, "MOVE16", SZ_NONE);
        switch ((op >> 3) & 3) {
        case 0: put(d, "(A%d)+,($%08X).L", ay, a); break;
        case 1: put(d, "($%08X).L,(A%d)+", a, ay); break;
        case 2: put(d, "(A%d),($%08X).L", ay, a); break;
        case 3: put(d, "($%08X).L,(A%d)", a, ay); break;
        }
        return true;
    }
    if ((op & 0xFFF8) == 0xF620) {
        uint16_t ext = next16(d);
        if ((ext & 0x8FFF) != 0x8000)
            return false;
        name(d, "MOVE16", SZ_NONE);
        put(d, "(A%d)+,(A%d)+", op & 7, (ext >> 12) & 7);
        return true;
    }
    return false;
}

/* ---- A-line traps ---- */

static void aline(D *d, uint16_t op, const char *(*aline_name)(uint16_t op))
{
    const char *n = aline_name ? aline_name(op) : NULL;
    if (!n) {
        snprintf(d->mn, sizeof d->mn, "_%04X", op);
        return;
    }
    const char *f1 = "", *f2 = "";
    if (op & 0x800) {
        if (op & 0x400)
            f1 = ",AutoPop";
    } else {
        if (op & 0x400)
            f1 = ",Sys";
        if (op & 0x200)
            f2 = ",Clear";
    }
    snprintf(d->mn, sizeof d->mn, "_%s%s%s", n, f1, f2);
}

int cpu_disasm_named(uint32_t addr, char *buf, int bufsize, const char *(*aline_name)(uint16_t op))
{
    D d;
    memset(&d, 0, sizeof d);
    d.pc0 = addr;
    d.p = addr + 2;
    uint16_t op = rd16(addr);
    bool ok;

    switch (op >> 12) {
    case 0x0: ok = line0(&d, op); break;
    case 0x1: case 0x2: case 0x3: ok = line_move(&d, op); break;
    case 0x4: ok = line4(&d, op); break;
    case 0x5: ok = line5(&d, op); break;
    case 0x6: ok = line6(&d, op); break;
    case 0x7: ok = line7(&d, op); break;
    case 0xA: aline(&d, op, aline_name); ok = true; break;
    case 0xE: ok = lineE(&d, op); break;
    case 0xF: ok = lineF(&d, op); break;
    default: ok = line_arith(&d, op); break;
    }

    if (bufsize <= 0)
        return ok ? (int)(d.p - addr) : 2;
    if (!ok) {
        snprintf(buf, (size_t)bufsize, "DC.W      $%04X", op);
        return 2;
    }
    if (!d.ol)
        snprintf(buf, (size_t)bufsize, "%s", d.mn);
    else if (d.cmt)
        snprintf(buf, (size_t)bufsize, "%-9s %s   ; $%08X", d.mn, d.ops, d.cmt_addr);
    else
        snprintf(buf, (size_t)bufsize, "%-9s %s", d.mn, d.ops);
    return (int)(d.p - addr);
}

int cpu_disasm(uint32_t addr, char *buf, int bufsize)
{
    return cpu_disasm_named(addr, buf, bufsize, NULL);
}
