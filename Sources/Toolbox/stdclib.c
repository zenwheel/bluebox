/*
 * StdCLib, MPW's C library for PowerPC programs: strings, memory, the
 * rest of stdlib, time, locale, signals, setjmp, the Pascal string
 * routines, MPW's path routines, the runtime's own calls and the library's
 * data.  The descriptors are in stdclib_io.c, the streams in
 * stdclib_stdio.c.
 *
 * MPW's choices kept: time_t counts seconds since 1904 in local time, as
 * the Mac's clock does, and clock() ticks (60 a second); '\n' is 13 in
 * what the library writes (asctime, perror); signals are bits (SIGABRT 1,
 * SIGINT 2 ...); the only locale is "C".
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "stdclib_internal.h"

static gaddr static_block(gaddr *slot, uint32_t size)
{
    if (!*slot)
        *slot = tb_NewPtr((int32_t)size, true, true);
    return *slot;
}

size_t sc_strlen(gaddr s)
{
    size_t n = 0;
    while (rd8(s + (gaddr)n))
        n++;
    return n;
}

size_t sc_cstr(gaddr s, char *out, size_t outsize)
{
    size_t n = s ? sc_strlen(s) : 0, k = n < outsize - 1 ? n : outsize - 1;
    if (k)
        gget(out, s, (uint32_t)k);
    out[k] = 0;
    return n;
}

/* A guest C string copied to a host string (malloc'd, the caller frees). */
static char *host_string(gaddr s)
{
    size_t n = s ? sc_strlen(s) : 0;
    char *p = malloc(n + 1);
    if (p) {
        if (n)
            gget(p, s, (uint32_t)n);
        p[n] = 0;
    }
    return p;
}

uint32_t sc_call(gaddr tvector, const uint32_t *args, int nargs) { return ppc_call(tvector, args, nargs); }

/* ---------------------------------------------------------------------- */
/* errno and MacOSErr */

static gaddr errno_block, macoserr_block;

gaddr sc_errno_addr(void) { return static_block(&errno_block, 4); }

void sc_set_errno(int e)
{
    if (sc_errno_addr())
        wr32(errno_block, (uint32_t)e);
}

void sc_uerror(int e, int16_t oserr)
{
    if (e)
        sc_set_errno(e);
    if (static_block(&macoserr_block, 4))
        wr16(macoserr_block, (uint16_t)oserr);
}

int sc_errno_for(int16_t err)
{
    switch (err) {
    case 0: return 0;
    case -43: case -35: case -120: case -37: return MPW_ENOENT; /* fnfErr, nsvErr, dirNFErr, bdNamErr */
    case -44: case -46: return MPW_EROFS;                       /* wPrErr, vLckdErr */
    case -45: case -54: case -61: case -47: return MPW_EACCES;  /* fLckdErr, permErr, wrPermErr, fBsyErr */
    case -48: return MPW_EEXIST;                                /* dupFNErr */
    case -42: return MPW_EMFILE;                                /* tmfoErr */
    case -34: return MPW_ENOSPC;                                /* dskFulErr */
    case -108: return MPW_ENOMEM;                               /* memFullErr */
    case -50: return MPW_EINVAL;                                /* paramErr */
    default: return MPW_EIO;
    }
}

static void s_geterrno(ppc_cpu *c) { RETURN(sc_errno_addr()); }
/* _uerror(errno, MacOSErr) */
static void s_uerror(ppc_cpu *c)
{
    sc_uerror((int32_t)R(0), (int16_t)R(1));
    if (!R(0))
        sc_set_errno(0);
    RETURN_VOID();
}

static const char *const messages[] = {
    "No error", "Permission denied", "No such file or directory", "No such resource", "Interrupted system service",
    "I/O error", "No such device or address", "Argument list too long", "Exec format error", "Bad file number",
    "No children", "Resource temporarily unavailable", "Not enough space", "Permission denied", "Bad address",
    "Block device required", "Device busy", "File exists", "Cross-device link", "No such device", "Not a directory",
    "Is a directory", "Invalid argument", "File table overflow", "Too many open files", "Not a character device",
    "Text file busy", "File too large", "No space left on device", "Illegal seek", "Read only file system",
    "Too many links", "Broken pipe", "Math arg out of domain of func", "Math result not representable",
};

const char *sc_strerror_text(int e)
{
    return e >= 0 && e < (int)(sizeof messages / sizeof messages[0]) ? messages[e] : "Unknown error";
}

/* strerror(n): a buffer of the library's. */
static gaddr strerror_buf;
static void s_strerror(ppc_cpu *c)
{
    gaddr b = static_block(&strerror_buf, 64);
    const char *m = sc_strerror_text((int32_t)R(0));
    gput(b, m, (uint32_t)strlen(m) + 1);
    RETURN(b);
}

/* ---------------------------------------------------------------------- */
/* Strings and memory */

static void s_strlen(ppc_cpu *c) { RETURN(sc_strlen(R(0))); }
static void s_strcpy(ppc_cpu *c)
{
    gmemcpy(R(0), R(1), (uint32_t)sc_strlen(R(1)) + 1);
    RETURN(R(0));
}
static void s_strncpy(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    uint32_t n = R(2), i = 0;
    for (; i < n && rd8(s + i); i++)
        wr8(d + i, rd8(s + i));
    for (; i < n; i++)
        wr8(d + i, 0);
    RETURN(d);
}
static void s_strcat(ppc_cpu *c)
{
    gmemcpy(R(0) + (gaddr)sc_strlen(R(0)), R(1), (uint32_t)sc_strlen(R(1)) + 1);
    RETURN(R(0));
}
static void s_strncat(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    size_t at = sc_strlen(d), i = 0;
    for (; i < R(2) && rd8(s + (gaddr)i); i++)
        wr8(d + (gaddr)(at + i), rd8(s + (gaddr)i));
    wr8(d + (gaddr)(at + i), 0);
    RETURN(d);
}
static int gstrncmp(gaddr a, gaddr b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = rd8(a + (gaddr)i), y = rd8(b + (gaddr)i);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}
static void s_strcmp(ppc_cpu *c) { RETURN((int32_t)gstrncmp(R(0), R(1), SIZE_MAX)); }
static void s_strncmp(ppc_cpu *c) { RETURN((int32_t)gstrncmp(R(0), R(1), R(2))); }
static void s_strchr(ppc_cpu *c)
{
    uint8_t ch = (uint8_t)R(1);
    for (gaddr s = R(0);; s++) {
        if (rd8(s) == ch) {
            RETURN(s);
            return;
        }
        if (!rd8(s))
            break;
    }
    RETURN(0);
}
static void s_strrchr(ppc_cpu *c)
{
    gaddr hit = 0;
    uint8_t ch = (uint8_t)R(1);
    for (gaddr s = R(0);; s++) {
        if (rd8(s) == ch)
            hit = s;
        if (!rd8(s))
            break;
    }
    RETURN(hit);
}
static void s_strstr(ppc_cpu *c)
{
    gaddr h = R(0), n = R(1);
    size_t nl = sc_strlen(n), hl = sc_strlen(h);
    for (size_t i = 0; i + nl <= hl; i++)
        if (!memcmp(gptr(h + (gaddr)i), gptr(n), nl)) {
            RETURN(h + (gaddr)i);
            return;
        }
    RETURN(0);
}
/* Whether ch is one of set's characters. */
static bool in_set(uint8_t ch, gaddr set)
{
    for (gaddr p = set; rd8(p); p++)
        if (rd8(p) == ch)
            return true;
    return false;
}
static void s_strpbrk(ppc_cpu *c)
{
    for (gaddr s = R(0); rd8(s); s++)
        if (in_set(rd8(s), R(1))) {
            RETURN(s);
            return;
        }
    RETURN(0);
}
static void s_strspn(ppc_cpu *c)
{
    uint32_t n = 0;
    while (rd8(R(0) + n) && in_set(rd8(R(0) + n), R(1)))
        n++;
    RETURN(n);
}
static void s_strcspn(ppc_cpu *c)
{
    uint32_t n = 0;
    while (rd8(R(0) + n) && !in_set(rd8(R(0) + n), R(1)))
        n++;
    RETURN(n);
}
/* strtok(s, delimiters): where the last call stopped, kept. */
static gaddr strtok_next;
static void s_strtok(ppc_cpu *c)
{
    gaddr s = R(0) ? R(0) : strtok_next;
    if (!s) {
        RETURN(0);
        return;
    }
    while (rd8(s) && in_set(rd8(s), R(1)))
        s++;
    if (!rd8(s)) {
        strtok_next = 0;
        RETURN(0);
        return;
    }
    gaddr start = s;
    while (rd8(s) && !in_set(rd8(s), R(1)))
        s++;
    if (rd8(s)) {
        wr8(s, 0);
        strtok_next = s + 1;
    } else {
        strtok_next = 0;
    }
    RETURN(start);
}
/* The "C" locale's collation is the bytes'. */
static void s_strxfrm(ppc_cpu *c)
{
    size_t n = sc_strlen(R(1));
    if (R(2)) {
        uint32_t k = n < R(2) ? (uint32_t)n + 1 : R(2);
        gmemcpy(R(0), R(1), k);
        if (n >= R(2))
            wr8(R(0) + R(2) - 1, 0);
    }
    RETURN(n);
}
static void s_memcpy(ppc_cpu *c)
{
    if (R(2))
        gmemcpy(R(0), R(1), R(2));
    RETURN(R(0));
}
static void s_memset(ppc_cpu *c)
{
    if (R(2))
        gmemset(R(0), (int)(R(1) & 0xFF), R(2));
    RETURN(R(0));
}
static void s_memcmp(ppc_cpu *c)
{
    int r = R(2) ? memcmp(gptr(R(0)), gptr(R(1)), R(2)) : 0;
    RETURN((int32_t)(r < 0 ? -1 : r > 0));
}
static void s_memchr(ppc_cpu *c)
{
    for (uint32_t i = 0; i < R(2); i++)
        if (rd8(R(0) + i) == (uint8_t)R(1)) {
            RETURN(R(0) + i);
            return;
        }
    RETURN(0);
}
/* memccpy(d, s, c, n): up to and including c; the byte after it in d. */
static void s_memccpy(ppc_cpu *c)
{
    for (uint32_t i = 0; i < R(3); i++) {
        uint8_t b = rd8(R(1) + i);
        wr8(R(0) + i, b);
        if (b == (uint8_t)R(2)) {
            RETURN(R(0) + i + 1);
            return;
        }
    }
    RETURN(0);
}

/* The character classes (ASCII; the rest of MacRoman is none of them). */
#define CTYPE(name, test) \
    static void s_##name(ppc_cpu *c) { int32_t ch = (int32_t)R(0); RETURN(ch >= 0 && ch < 128 && (test) ? 1 : 0); }
CTYPE(isalnum, (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
CTYPE(isalpha, (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
CTYPE(isdigit, ch >= '0' && ch <= '9')
CTYPE(isxdigit, (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
CTYPE(isspace, ch == ' ' || (ch >= 9 && ch <= 13))
CTYPE(isupper, ch >= 'A' && ch <= 'Z')
CTYPE(islower, ch >= 'a' && ch <= 'z')
CTYPE(ispunct, ch > 32 && ch < 127 && !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
CTYPE(isprint, ch >= 32 && ch < 127)
CTYPE(isgraph, ch > 32 && ch < 127)
CTYPE(iscntrl, ch < 32 || ch == 127)
CTYPE(isascii, 1)
#undef CTYPE
static void s_toupper(ppc_cpu *c)
{
    int32_t ch = (int32_t)R(0);
    RETURN(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}
static void s_tolower(ppc_cpu *c)
{
    int32_t ch = (int32_t)R(0);
    RETURN(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
static void s_toascii(ppc_cpu *c) { RETURN(R(0) & 0x7F); }

/* ---------------------------------------------------------------------- */
/* stdlib */

/* malloc and friends: Memory Manager pointers. */
static void s_malloc(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), false, false)); }
static void s_calloc(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)(R(0) * R(1)), false, true)); }
static void s_free(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    RETURN_VOID();
}
static gaddr srealloc(gaddr p, uint32_t n)
{
    if (!p)
        return tb_NewPtr((int32_t)n, false, false);
    if (!n) {
        tb_DisposePtr(p);
        return 0;
    }
    gaddr q = tb_NewPtr((int32_t)n, false, false);
    if (q) {
        uint32_t old = (uint32_t)tb_GetPtrSize(p);
        ppc_copy(q, p, old < n ? old : n);
        tb_DisposePtr(p);
    }
    return q;
}
static void s_realloc(ppc_cpu *c) { RETURN(srealloc(R(0), R(1))); }

/* The AltiVec forms: 16-byte aligned, the block and the size just before. */
static gaddr vec_alloc(uint32_t n, bool clear)
{
    gaddr p = tb_NewPtr((int32_t)(n + 24), false, clear);
    if (!p)
        return 0;
    gaddr a = (p + 8 + 15) & ~15u;
    wr32(a - 4, p);
    wr32(a - 8, n);
    return a;
}
static void s_vec_malloc(ppc_cpu *c) { RETURN(vec_alloc(R(0), false)); }
static void s_vec_calloc(ppc_cpu *c) { RETURN(vec_alloc(R(0) * R(1), true)); }
static void s_vec_free(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(rd32(R(0) - 4));
    RETURN_VOID();
}
static void s_vec_realloc(ppc_cpu *c)
{
    gaddr p = R(0);
    gaddr q = R(1) ? vec_alloc(R(1), false) : 0;
    if (p && q) {
        uint32_t old = rd32(p - 8);
        gmemcpy(q, p, old < R(1) ? old : R(1));
    }
    if (p && (q || !R(1)))
        tb_DisposePtr(rd32(p - 4));
    RETURN(q);
}

static void s_abs(ppc_cpu *c) { RETURN((uint32_t)labs((int32_t)R(0))); }
static void s_llabs(ppc_cpu *c)
{
    int64_t v = (int64_t)((uint64_t)R(0) << 32 | R(1));
    RETURN_64(v < 0 ? -(uint64_t)v : (uint64_t)v);
}
/* div and ldiv(result, numer, denom): the struct through r3. */
static void s_div(ppc_cpu *c)
{
    int32_t n = (int32_t)R(1), d = (int32_t)R(2);
    int32_t q = d && !(d == -1 && n == INT32_MIN) ? n / d : 0, r = d ? n - q * d : n;
    wr32(R(0), (uint32_t)q);
    wr32(R(0) + 4, (uint32_t)r);
    RETURN(R(0));
}
static void s_lldiv(ppc_cpu *c)
{
    int64_t n = (int64_t)((uint64_t)R(1) << 32 | R(2)), d = (int64_t)((uint64_t)R(3) << 32 | R(4));
    int64_t q = d && !(d == -1 && n == INT64_MIN) ? n / d : 0, r = d ? n - q * d : n;
    wr32(R(0), (uint32_t)((uint64_t)q >> 32));
    wr32(R(0) + 4, (uint32_t)q);
    wr32(R(0) + 8, (uint32_t)((uint64_t)r >> 32));
    wr32(R(0) + 12, (uint32_t)r);
    RETURN(R(0));
}

/* The number conversions: the host's, on a copy of the string. */
static char *number_text(gaddr s, char *buf, size_t size)
{
    sc_cstr(s, buf, size);
    return buf;
}
static void s_strtol(ppc_cpu *c)
{
    char buf[128], *end;
    errno = 0;
    long long v = strtoll(number_text(R(0), buf, sizeof buf), &end, (int)R(2));
    if (v > INT32_MAX || v < INT32_MIN || errno == ERANGE) {
        v = v < 0 ? INT32_MIN : INT32_MAX;
        sc_set_errno(MPW_ERANGE);
    }
    if (R(1))
        wr32(R(1), R(0) + (gaddr)(end - buf));
    RETURN((int32_t)v);
}
static void s_strtoul(ppc_cpu *c)
{
    char buf[128], *end;
    errno = 0;
    unsigned long long v = strtoull(number_text(R(0), buf, sizeof buf), &end, (int)R(2));
    bool neg = strchr(buf, '-') && end != buf;
    if ((!neg && v > UINT32_MAX) || (neg && (long long)v < -(long long)UINT32_MAX) || errno == ERANGE) {
        v = UINT32_MAX;
        sc_set_errno(MPW_ERANGE);
    }
    if (R(1))
        wr32(R(1), R(0) + (gaddr)(end - buf));
    RETURN((uint32_t)v);
}
static void s_strtoll(ppc_cpu *c)
{
    char buf[128], *end;
    errno = 0;
    long long v = strtoll(number_text(R(0), buf, sizeof buf), &end, (int)R(2));
    if (errno == ERANGE)
        sc_set_errno(MPW_ERANGE);
    if (R(1))
        wr32(R(1), R(0) + (gaddr)(end - buf));
    RETURN_64(v);
}
static void s_strtoull(ppc_cpu *c)
{
    char buf[128], *end;
    errno = 0;
    unsigned long long v = strtoull(number_text(R(0), buf, sizeof buf), &end, (int)R(2));
    if (errno == ERANGE)
        sc_set_errno(MPW_ERANGE);
    if (R(1))
        wr32(R(1), R(0) + (gaddr)(end - buf));
    RETURN_64(v);
}
static void s_atoi(ppc_cpu *c)
{
    char buf[128];
    RETURN((int32_t)strtol(number_text(R(0), buf, sizeof buf), NULL, 10));
}
static void s_atoll(ppc_cpu *c)
{
    char buf[128];
    RETURN_64(strtoll(number_text(R(0), buf, sizeof buf), NULL, 10));
}
static void s_strtod(ppc_cpu *c)
{
    char buf[512], *end;
    errno = 0;
    double d = strtod(number_text(R(0), buf, sizeof buf), &end);
    if (errno == ERANGE)
        sc_set_errno(MPW_ERANGE);
    if (R(1))
        wr32(R(1), R(0) + (gaddr)(end - buf));
    RETURN_F(d);
}
static void s_atof(ppc_cpu *c)
{
    char buf[512];
    RETURN_F(atof(number_text(R(0), buf, sizeof buf)));
}

/* ecvt and fcvt(value, ndigit, &decpt, &sign): the digits in a buffer of
 * the library's.  The double takes r3 and r4's slots. */
static gaddr cvt_buf;
static gaddr cvt(double v, int n, gaddr decpt, gaddr sign, bool e)
{
    int dp = 0, sg = 0;
    if (n < 0)
        n = 0;
    if (n > 340)
        n = 340;
    const char *digits = e ? ecvt(v, n, &dp, &sg) : fcvt(v, n, &dp, &sg);
    gaddr b = static_block(&cvt_buf, 352);
    size_t len = strlen(digits);
    if (len > 350)
        len = 350;
    gput(b, digits, (uint32_t)len);
    wr8(b + (gaddr)len, 0);
    if (decpt)
        wr32(decpt, (uint32_t)dp);
    if (sign)
        wr32(sign, sg ? 1 : 0);
    return b;
}
static void s_ecvt(ppc_cpu *c) { RETURN(cvt(F(0), (int32_t)R(2), R(3), R(4), true)); }
static void s_fcvt(ppc_cpu *c) { RETURN(cvt(F(0), (int32_t)R(2), R(3), R(4), false)); }
/* _cvt(value, ndigit, &decpt, &sign, eflag) */
static void s_cvt(ppc_cpu *c) { RETURN(cvt(F(0), (int32_t)R(2), R(3), R(4), R(5) != 0)); }

static uint32_t rand_state = 1;
static void s_rand(ppc_cpu *c)
{
    rand_state = rand_state * 1103515245u + 12345u;
    RETURN((rand_state >> 16) & 0x7FFF);
}
static void s_srand(ppc_cpu *c)
{
    rand_state = R(0);
    RETURN_VOID();
}

/* qsort(base, n, size, compare): compare is the program's. */
static void s_qsort(ppc_cpu *c)
{
    gaddr base = R(0), fn = R(3);
    uint32_t n = R(1), size = R(2);
    gaddr tmp = tb_NewPtr((int32_t)size, false, true);
    /* Insertion sort: simple, stable, and calls compare in guest code. */
    for (uint32_t i = 1; i < n && !c->halted && tmp; i++) {
        ppc_copy(tmp, base + i * size, size);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2] = { base + (j - 1) * size, tmp };
            int32_t r = (int32_t)sc_call(fn, args, 2);
            if (r <= 0 || g_cpu->halted)
                break;
            ppc_copy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        ppc_copy(base + j * size, tmp, size);
    }
    if (tmp)
        tb_DisposePtr(tmp);
    RETURN_VOID();
}

/* bsearch(key, base, n, size, compare) */
static void s_bsearch(ppc_cpu *c)
{
    gaddr key = R(0), base = R(1), fn = R(4);
    uint32_t lo = 0, hi = R(2), size = R(3);
    while (lo < hi && !g_cpu->halted) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t args[2] = { key, base + mid * size };
        int32_t r = (int32_t)sc_call(fn, args, 2);
        if (!r) {
            RETURN(base + mid * size);
            return;
        }
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    RETURN(0);
}

/* The environment: none from a shell, what the program sets. */
enum { NENV = 32 };
static struct {
    char name[64];
    gaddr value;
} env[NENV];

static void s_getenv(ppc_cpu *c)
{
    char name[64];
    sc_cstr(R(0), name, sizeof name);
    for (int i = 0; i < NENV; i++)
        if (env[i].value && !strcmp(env[i].name, name)) {
            RETURN(env[i].value);
            return;
        }
    RETURN(0);
}
/* setenv(name, value); putenv is a macro for it. */
static void s_setenv(ppc_cpu *c)
{
    char name[64];
    if (!R(0) || sc_cstr(R(0), name, sizeof name) >= sizeof name || !name[0]) {
        sc_set_errno(MPW_EINVAL);
        RETURN(-1);
        return;
    }
    int slot = -1;
    for (int i = 0; i < NENV; i++)
        if (env[i].value && !strcmp(env[i].name, name))
            slot = i;
    if (slot >= 0) {
        tb_DisposePtr(env[slot].value);
        env[slot].value = 0;
    }
    if (!R(1)) {
        RETURN(0);
        return;
    }
    for (int i = 0; i < NENV && slot < 0; i++)
        if (!env[i].value)
            slot = i;
    uint32_t n = (uint32_t)sc_strlen(R(1)) + 1;
    gaddr v = slot >= 0 ? tb_NewPtr((int32_t)n, true, false) : 0;
    if (!v) {
        sc_set_errno(MPW_ENOMEM);
        RETURN(-1);
        return;
    }
    gmemcpy(v, R(1), n);
    snprintf(env[slot].name, sizeof env[slot].name, "%s", name);
    env[slot].value = v;
    RETURN(0);
}

/* system(command): there is no command processor (system(NULL) is 0). */
static void s_system(ppc_cpu *c)
{
    if (R(0))
        sc_set_errno(MPW_ENOENT);
    RETURN(R(0) ? -1 : 0);
}

/* The multibyte functions: the "C" locale's characters are bytes, and a
 * wchar_t is two of them. */
static void s_mblen(ppc_cpu *c) { RETURN(!R(0) ? 0 : !R(1) ? -1 : rd8(R(0)) ? 1 : 0); }
static void s_mbtowc(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN(0);
        return;
    }
    if (!R(2)) {
        RETURN(-1);
        return;
    }
    uint8_t b = rd8(R(1));
    if (R(0))
        wr16(R(0), b);
    RETURN(b ? 1 : 0);
}
static void s_wctomb(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(0);
        return;
    }
    uint32_t w = R(1) & 0xFFFF;
    if (w > 0xFF) {
        RETURN(-1);
        return;
    }
    wr8(R(0), (uint8_t)w);
    RETURN(1);
}
static void s_mbstowcs(ppc_cpu *c)
{
    uint32_t i = 0;
    for (; i < R(2); i++) {
        uint8_t b = rd8(R(1) + i);
        if (R(0))
            wr16(R(0) + 2 * i, b);
        if (!b)
            break;
    }
    RETURN(i);
}
static void s_wcstombs(ppc_cpu *c)
{
    uint32_t i = 0;
    for (; i < R(2); i++) {
        uint16_t w = rd16(R(1) + 2 * i);
        if (w > 0xFF) {
            RETURN(-1);
            return;
        }
        if (R(0))
            wr8(R(0) + i, (uint8_t)w);
        if (!w)
            break;
    }
    RETURN(i);
}

/* ---------------------------------------------------------------------- */
/* setjmp and longjmp: jmp_buf as setjmp.h has it, LR, CR, SP, TOC, a
 * spare (VRSAVE in the AltiVec form), r13-r31, f14-f31, FPSCR. */

static void s_setjmp(ppc_cpu *c)
{
    gaddr b = R(0);
    wr32(b, c->lr);
    wr32(b + 4, c->cr);
    wr32(b + 8, c->gpr[1]);
    wr32(b + 12, c->gpr[2]);
    wr32(b + 16, 0);
    for (int i = 13; i < 32; i++)
        wr32(b + 20 + 4 * (gaddr)(i - 13), c->gpr[i]);
    for (int i = 14; i < 32; i++) {
        wr32(b + 96 + 8 * (gaddr)(i - 14), (uint32_t)(c->fpr[i] >> 32));
        wr32(b + 100 + 8 * (gaddr)(i - 14), (uint32_t)c->fpr[i]);
    }
    RETURN(0);
}
static void s_longjmp(ppc_cpu *c)
{
    gaddr b = R(0);
    uint32_t v = R(1) ? R(1) : 1;
    c->cr = rd32(b + 4);
    c->gpr[1] = rd32(b + 8);
    c->gpr[2] = rd32(b + 12);
    for (int i = 13; i < 32; i++)
        c->gpr[i] = rd32(b + 20 + 4 * (gaddr)(i - 13));
    for (int i = 14; i < 32; i++)
        c->fpr[i] = (uint64_t)rd32(b + 96 + 8 * (gaddr)(i - 14)) << 32 | rd32(b + 100 + 8 * (gaddr)(i - 14));
    c->gpr[3] = v;
    c->pc = rd32(b);
}

/* ---------------------------------------------------------------------- */
/* Exit */

enum { NEXITPROCS = 45, FIRST_USER_PROC = 5 };
static gaddr exit_status_block;
static bool exiting;

static gaddr exit_procs(void) { return rd32(sc_intenv() + 0x16); }

/* _DoExitProcs(from, to): the exit procedures from slot from down to slot
 * to, each once. */
static void do_exit_procs(int32_t from, int32_t to)
{
    gaddr procs = exit_procs();
    if (from >= NEXITPROCS)
        from = NEXITPROCS - 1;
    for (int32_t i = from; i >= to && i >= 0 && !g_cpu->halted; i--) {
        gaddr tv = rd32(procs + 4 * (gaddr)i);
        if (!tv)
            continue;
        wr32(procs + 4 * (gaddr)i, 0);
        sc_call(tv, NULL, 0);
    }
}

void sc_run_exit_procs(void) { do_exit_procs(NEXITPROCS - 1, 0); }

static void s_DoExitProcs(ppc_cpu *c)
{
    do_exit_procs((int32_t)R(0), (int32_t)R(1));
    RETURN_VOID();
}

static void s_atexit(ppc_cpu *c)
{
    gaddr procs = exit_procs();
    for (int i = FIRST_USER_PROC; i < NEXITPROCS; i++)
        if (!rd32(procs + 4 * (gaddr)i)) {
            wr32(procs + 4 * (gaddr)i, R(0));
            RETURN(0);
            return;
        }
    RETURN(-1);
}

static void halt_program(ppc_cpu *c, int32_t status, const char *how)
{
    if (static_block(&exit_status_block, 4))
        wr32(exit_status_block, (uint32_t)status);
    sc_console_flush();
    rm_update_all();
    cpu_halt(g_cpu, "ExitToShell (%s %d)", how, (int)status);
    ppc_halt(c, "ExitToShell (%s %d)", how, (int)status);
}

/* What StdCLib's termination did when the program ended without exit:
 * the streams flushed and closed. */
void stdclib_terminate(void)
{
    sc_stdio_exit();
    sc_close_all();
}

/* exit(status): the atexit procedures (the last first), then the streams
 * flushed and the files closed. */
static void s_exit(ppc_cpu *c)
{
    int32_t status = (int32_t)R(0);
    if (!exiting) {
        exiting = true;
        sc_run_exit_procs();
        stdclib_terminate();
    }
    halt_program(c, status, "exit");
}
static void s__exit(ppc_cpu *c) { halt_program(c, (int32_t)R(0), "_exit"); }
/* _RTExit(status): exit as MPW's runtime ends a program. */
static void s_RTExit(ppc_cpu *c) { s_exit(c); }

/* ---------------------------------------------------------------------- */
/* Signals: MPW's are bits, SIGABRT 1, SIGINT 2, SIGFPE 4, SIGILL 8, SIGSEGV
 * 16, SIGTERM 32.  __SigEnv holds a handler for each, then the held and
 * pending ones. */

enum { NSIG_MPW = 6, SIG_IGN_MPW = 0, SIG_DFL_MPW = 1, SIG_HOLD_MPW = 3, SIG_RELEASE_MPW = 5 };
enum { SE_HELD = 4 * NSIG_MPW, SE_PENDING = SE_HELD + 4, SE_SIZE = 64 };
static gaddr sigenv;

static gaddr sig_env(void)
{
    if (!sigenv && static_block(&sigenv, SE_SIZE))
        for (int i = 0; i < NSIG_MPW; i++)
            wr32(sigenv + 4 * (gaddr)i, SIG_DFL_MPW);
    return sigenv;
}

static int sig_index(uint32_t sig)
{
    for (int i = 0; i < NSIG_MPW; i++)
        if (sig == 1u << i)
            return i;
    return -1;
}

static int32_t sraise(ppc_cpu *c, uint32_t sig)
{
    int i = sig_index(sig);
    gaddr e = sig_env();
    if (i < 0 || !e) {
        sc_set_errno(MPW_EINVAL);
        return -1;
    }
    if (rd32(e + SE_HELD) & sig) {
        wr32(e + SE_PENDING, rd32(e + SE_PENDING) | sig);
        return 0;
    }
    gaddr h = rd32(e + 4 * (gaddr)i);
    if (h == SIG_IGN_MPW)
        return 0;
    if (h == SIG_DFL_MPW) {
        /* The default: the program ends. */
        LOGF(LOG_UNIMPL, "StdCLib: signal %u, the program ends\n", sig);
        halt_program(c, 3, sig == 1 ? "abort" : "signal");
        return 0;
    }
    wr32(e + 4 * (gaddr)i, SIG_DFL_MPW);
    uint32_t arg = sig;
    sc_call(h, &arg, 1);
    return 0;
}

static void s_raise(ppc_cpu *c)
{
    int32_t r = sraise(c, R(0));
    if (!c->halted)
        RETURN(r);
}

static void s_signal(ppc_cpu *c)
{
    uint32_t sig = R(0);
    gaddr fn = R(1);
    int i = sig_index(sig);
    gaddr e = sig_env();
    if (i < 0 || !e || fn == 0xFFFFFFFF) {
        sc_set_errno(MPW_EINVAL);
        RETURN(-1); /* SIG_ERR */
        return;
    }
    gaddr old = rd32(e + 4 * (gaddr)i);
    if (fn == SIG_HOLD_MPW) {
        wr32(e + SE_HELD, rd32(e + SE_HELD) | sig);
    } else if (fn == SIG_RELEASE_MPW) {
        wr32(e + SE_HELD, rd32(e + SE_HELD) & ~sig);
        if (rd32(e + SE_PENDING) & sig) {
            wr32(e + SE_PENDING, rd32(e + SE_PENDING) & ~sig);
            sraise(c, sig);
            if (c->halted)
                return;
        }
    } else {
        wr32(e + 4 * (gaddr)i, fn);
    }
    RETURN(old);
}

/* abort(): SIGABRT; if its handler comes back, the program ends anyway. */
static void s_abort(ppc_cpu *c)
{
    sraise(c, 1);
    if (!c->halted)
        halt_program(c, 3, "abort");
}

/* ---------------------------------------------------------------------- */
/* Time: time_t is seconds since 1904, local time; struct tm as ANSI's. */

enum { TM_SEC, TM_MIN, TM_HOUR, TM_MDAY, TM_MON, TM_YEAR, TM_WDAY, TM_YDAY, TM_ISDST, TM_FIELDS };
static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                             "August", "September", "October", "November", "December" };

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Days from 1 January 1904 to the date (month 0-11, any day). */
static int64_t days_since_1904(int64_t year, int64_t mon, int64_t mday)
{
    year += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }
    /* Days before the year: from 1 Jan of year 0 (proleptic), less 1904's. */
    int64_t y = year - 1;
    int64_t days = y * 365 + y / 4 - y / 100 + y / 400 - (1903 * 365 + 1903 / 4 - 1903 / 100 + 1903 / 400);
    for (int m = 0; m < mon; m++)
        days += month_days[m] + (m == 1 && leap(year));
    return days + mday - 1;
}

/* The local time offset from UTC (seconds) and daylight saving, the
 * host's, at Mac local time t. */
static long host_offset(int64_t t, int *isdst)
{
    time_t u = (time_t)(t - 2082844800LL);
    struct tm lt;
    localtime_r(&u, &lt);
    u -= lt.tm_gmtoff;
    localtime_r(&u, &lt);
    if (isdst)
        *isdst = lt.tm_isdst > 0;
    return lt.tm_gmtoff;
}

static void tm_of(int64_t t, int32_t f[TM_FIELDS])
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400, secs = t - days * 86400;
    f[TM_SEC] = (int32_t)(secs % 60);
    f[TM_MIN] = (int32_t)(secs / 60 % 60);
    f[TM_HOUR] = (int32_t)(secs / 3600);
    f[TM_WDAY] = (int32_t)(((days + 5) % 7 + 7) % 7); /* 1 January 1904 was a Friday */
    int64_t year = 1904;
    for (;;) {
        int64_t len = leap(year) ? 366 : 365;
        if (days < 0) {
            year--;
            days += leap(year) ? 366 : 365;
        } else if (days >= len) {
            days -= len;
            year++;
        } else {
            break;
        }
    }
    f[TM_YEAR] = (int32_t)(year - 1900);
    f[TM_YDAY] = (int32_t)days;
    int m = 0;
    while (days >= month_days[m] + (m == 1 && leap(year))) {
        days -= month_days[m] + (m == 1 && leap(year));
        m++;
    }
    f[TM_MON] = m;
    f[TM_MDAY] = (int32_t)days + 1;
    f[TM_ISDST] = 0;
}

static void tm_put(gaddr tm, const int32_t f[TM_FIELDS])
{
    for (int i = 0; i < TM_FIELDS; i++)
        wr32(tm + 4 * (gaddr)i, (uint32_t)f[i]);
}

static void tm_get(gaddr tm, int32_t f[TM_FIELDS])
{
    for (int i = 0; i < TM_FIELDS; i++)
        f[i] = (int32_t)rd32(tm + 4 * (gaddr)i);
}

static gaddr tm_buf, asctime_buf;

static void s_time(ppc_cpu *c)
{
    uint32_t t = rd32(LM_Time);
    if (R(0))
        wr32(R(0), t);
    RETURN(t);
}
static void s_clock(ppc_cpu *c) { RETURN(rd32(LM_Ticks)); }
static void s_difftime(ppc_cpu *c) { RETURN_F((double)R(0) - (double)R(1)); }

static void s_localtime(ppc_cpu *c)
{
    int32_t f[TM_FIELDS];
    uint32_t t = rd32(R(0));
    tm_of(t, f);
    host_offset(t, &f[TM_ISDST]);
    gaddr tm = static_block(&tm_buf, 4 * TM_FIELDS);
    tm_put(tm, f);
    RETURN(tm);
}

static void s_gmtime(ppc_cpu *c)
{
    int32_t f[TM_FIELDS];
    uint32_t t = rd32(R(0));
    tm_of((int64_t)t - host_offset(t, NULL), f);
    gaddr tm = static_block(&tm_buf, 4 * TM_FIELDS);
    tm_put(tm, f);
    RETURN(tm);
}

/* mktime(tm): its fields brought into range (and the weekday and day of
 * the year set); the time, or -1 outside 1904 to 2040. */
static void s_mktime(ppc_cpu *c)
{
    int32_t f[TM_FIELDS];
    tm_get(R(0), f);
    int64_t t = days_since_1904(1900 + (int64_t)f[TM_YEAR], f[TM_MON], f[TM_MDAY]) * 86400 +
                (int64_t)f[TM_HOUR] * 3600 + (int64_t)f[TM_MIN] * 60 + f[TM_SEC];
    if (t < 0 || t > 0xFFFFFFFFLL) {
        RETURN(-1);
        return;
    }
    int32_t isdst = f[TM_ISDST];
    tm_of(t, f);
    if (isdst < 0)
        host_offset(t, &isdst);
    f[TM_ISDST] = isdst;
    tm_put(R(0), f);
    RETURN((uint32_t)t);
}

static gaddr sasctime(gaddr tm)
{
    int32_t f[TM_FIELDS];
    tm_get(tm, f);
    char s[64];
    snprintf(s, sizeof s, "%.3s %.3s%3d %.2d:%.2d:%.2d %d%c", day_names[((f[TM_WDAY] % 7) + 7) % 7],
             month_names[((f[TM_MON] % 12) + 12) % 12], f[TM_MDAY], f[TM_HOUR], f[TM_MIN], f[TM_SEC],
             1900 + f[TM_YEAR], MPW_NEWLINE);
    gaddr b = static_block(&asctime_buf, 64);
    gput(b, s, (uint32_t)strlen(s) + 1);
    return b;
}

static void s_asctime(ppc_cpu *c) { RETURN(sasctime(R(0))); }
static void s_ctime(ppc_cpu *c)
{
    s_localtime(c);
    RETURN(sasctime(c->gpr[3]));
}

/* strftime(s, max, format, tm), in the "C" locale. */
static void s_strftime(ppc_cpu *c)
{
    int32_t f[TM_FIELDS];
    tm_get(R(3), f);
    char out[1024];
    size_t n = 0;
    const char *wd = day_names[((f[TM_WDAY] % 7) + 7) % 7], *mn = month_names[((f[TM_MON] % 12) + 12) % 12];
    int hour12 = f[TM_HOUR] % 12 ? f[TM_HOUR] % 12 : 12;
    for (gaddr p = R(2); rd8(p) && n < sizeof out - 64; p++) {
        char ch = (char)rd8(p);
        if (ch != '%') {
            out[n++] = ch;
            continue;
        }
        char k = (char)rd8(++p);
        if (!k)
            break;
        size_t room = sizeof out - n;
        int w = 0;
        switch (k) {
        case 'a': w = snprintf(out + n, room, "%.3s", wd); break;
        case 'A': w = snprintf(out + n, room, "%s", wd); break;
        case 'b': w = snprintf(out + n, room, "%.3s", mn); break;
        case 'B': w = snprintf(out + n, room, "%s", mn); break;
        case 'c':
            w = snprintf(out + n, room, "%.3s %.3s %02d %02d:%02d:%02d %d", wd, mn, f[TM_MDAY], f[TM_HOUR], f[TM_MIN],
                         f[TM_SEC], 1900 + f[TM_YEAR]);
            break;
        case 'd': w = snprintf(out + n, room, "%02d", f[TM_MDAY]); break;
        case 'H': w = snprintf(out + n, room, "%02d", f[TM_HOUR]); break;
        case 'I': w = snprintf(out + n, room, "%02d", hour12); break;
        case 'j': w = snprintf(out + n, room, "%03d", f[TM_YDAY] + 1); break;
        case 'm': w = snprintf(out + n, room, "%02d", f[TM_MON] + 1); break;
        case 'M': w = snprintf(out + n, room, "%02d", f[TM_MIN]); break;
        case 'p': w = snprintf(out + n, room, "%s", f[TM_HOUR] < 12 ? "AM" : "PM"); break;
        case 'S': w = snprintf(out + n, room, "%02d", f[TM_SEC]); break;
        case 'U': w = snprintf(out + n, room, "%02d", (f[TM_YDAY] + 7 - f[TM_WDAY]) / 7); break;
        case 'W': w = snprintf(out + n, room, "%02d", (f[TM_YDAY] + 7 - (f[TM_WDAY] + 6) % 7) / 7); break;
        case 'w': w = snprintf(out + n, room, "%d", f[TM_WDAY]); break;
        case 'x': w = snprintf(out + n, room, "%02d/%02d/%02d", f[TM_MON] + 1, f[TM_MDAY], (1900 + f[TM_YEAR]) % 100); break;
        case 'X': w = snprintf(out + n, room, "%02d:%02d:%02d", f[TM_HOUR], f[TM_MIN], f[TM_SEC]); break;
        case 'y': w = snprintf(out + n, room, "%02d", (1900 + f[TM_YEAR]) % 100); break;
        case 'Y': w = snprintf(out + n, room, "%d", 1900 + f[TM_YEAR]); break;
        case 'Z': break; /* no zone name */
        case '%': out[n] = '%', w = 1; break;
        default: w = snprintf(out + n, room, "%%%c", k); break;
        }
        n += (size_t)(w > 0 ? w : 0);
    }
    if (n + 1 > R(1)) {
        RETURN(0);
        return;
    }
    gput(R(0), out, (uint32_t)n);
    wr8(R(0) + (gaddr)n, 0);
    RETURN(n);
}

/* ---------------------------------------------------------------------- */
/* Locale: "C" only.  _CategoryLoc has a locale code per category
 * (collate, ctype, monetary, numeric, time). */

static gaddr loc_block, category_block, locale_name, locale_composite;

static gaddr lconv(void)
{
    if (!loc_block && static_block(&loc_block, 64)) {
        /* Ten strings, decimal_point "." and the rest "", then eight
         * chars of CHAR_MAX. */
        gaddr strings = tb_NewPtr(4, true, true);
        wr8(strings, '.');
        for (int i = 0; i < 10; i++)
            wr32(loc_block + 4 * (gaddr)i, i ? strings + 1 : strings);
        for (int i = 0; i < 8; i++)
            wr8(loc_block + 40 + (gaddr)i, 0x7F);
    }
    return loc_block;
}

static gaddr categories(void)
{
    if (!category_block && static_block(&category_block, 8))
        gput(category_block, "CCCCC", 5);
    return category_block;
}

static void s_localeconv(ppc_cpu *c) { RETURN(lconv()); }

/* ConvertTheString(name): a locale's code, 'C' ("" is the native locale,
 * which is "C" too); 0 for one there isn't. */
static int locale_code(gaddr name)
{
    char s[32];
    sc_cstr(name, s, sizeof s);
    return !strcmp(s, "C") || !s[0] ? 'C' : 0;
}
static void s_ConvertTheString(ppc_cpu *c) { RETURN(locale_code(R(0))); }

/* MakeTheLocaleString(): "#c#c#c#c#c", the categories' codes. */
static gaddr make_locale_string(void)
{
    gaddr s = static_block(&locale_composite, 12), cat = categories();
    for (int i = 0; i < 5; i++) {
        wr8(s + 2 * (gaddr)i, '#');
        wr8(s + 2 * (gaddr)i + 1, rd8(cat + (gaddr)i));
    }
    wr8(s + 10, 0);
    return s;
}
static void s_MakeTheLocaleString(ppc_cpu *c) { RETURN(make_locale_string()); }

/* ParseTheLocaleString(s, codes): a string MakeTheLocaleString made, its
 * five codes; 0, or -1 if it isn't one. */
static int parse_locale_string(gaddr s, gaddr codes)
{
    if (sc_strlen(s) != 10)
        return -1;
    for (int i = 0; i < 5; i++)
        if (rd8(s + 2 * (gaddr)i) != '#' || rd8(s + 2 * (gaddr)i + 1) != 'C')
            return -1;
    for (int i = 0; i < 5 && codes; i++)
        wr8(codes + (gaddr)i, 'C');
    return 0;
}
static void s_ParseTheLocaleString(ppc_cpu *c) { RETURN(parse_locale_string(R(0), R(1))); }

static void s_setlocale(ppc_cpu *c)
{
    int32_t cat = (int32_t)R(0);
    gaddr name = static_block(&locale_name, 4);
    wr8(name, 'C');
    if (cat < 1 || cat > 6 || (R(1) && !locale_code(R(1)) && parse_locale_string(R(1), 0))) {
        RETURN(0);
        return;
    }
    RETURN(name);
}

/* ---------------------------------------------------------------------- */
/* Pascal strings (PLStringFuncs.h) */

static int pcompare(gaddr a, gaddr b, int n)
{
    int la = rd8(a), lb = rd8(b);
    for (int i = 1; i <= la && i <= lb && i <= n; i++) {
        int x = rd8(a + (gaddr)i), y = rd8(b + (gaddr)i);
        if (x != y)
            return x - y;
    }
    if (la > n)
        la = n;
    if (lb > n)
        lb = n;
    return la - lb;
}
static void s_PLstrcmp(ppc_cpu *c) { RETURN((int16_t)pcompare(R(0), R(1), 255)); }
static void s_PLstrncmp(ppc_cpu *c) { RETURN((int16_t)pcompare(R(0), R(1), (int16_t)R(2))); }
static void s_PLstrcpy(ppc_cpu *c)
{
    gmemcpy(R(0), R(1), rd8(R(1)) + 1u);
    RETURN(R(0));
}
static void s_PLstrncpy(ppc_cpu *c)
{
    int n = rd8(R(1)), max = (int16_t)R(2);
    if (n > max)
        n = max < 0 ? 0 : max;
    gmemcpy(R(0) + 1, R(1) + 1, (uint32_t)n);
    wr8(R(0), (uint8_t)n);
    RETURN(R(0));
}
static void pcat(gaddr d, gaddr s, int max)
{
    int ld = rd8(d), n = rd8(s);
    if (n > max)
        n = max < 0 ? 0 : max;
    if (ld + n > 255)
        n = 255 - ld;
    gmemcpy(d + 1 + (gaddr)ld, s + 1, (uint32_t)n);
    wr8(d, (uint8_t)(ld + n));
}
static void s_PLstrcat(ppc_cpu *c)
{
    pcat(R(0), R(1), 255);
    RETURN(R(0));
}
static void s_PLstrncat(ppc_cpu *c)
{
    pcat(R(0), R(1), (int16_t)R(2));
    RETURN(R(0));
}
static void s_PLstrchr(ppc_cpu *c)
{
    gaddr s = R(0);
    for (int i = 1; i <= rd8(s); i++)
        if (rd8(s + (gaddr)i) == (uint8_t)R(1)) {
            RETURN(s + (gaddr)i);
            return;
        }
    RETURN(0);
}
static void s_PLstrrchr(ppc_cpu *c)
{
    gaddr s = R(0);
    for (int i = rd8(s); i >= 1; i--)
        if (rd8(s + (gaddr)i) == (uint8_t)R(1)) {
            RETURN(s + (gaddr)i);
            return;
        }
    RETURN(0);
}
static bool pin_set(uint8_t ch, gaddr set)
{
    for (int i = 1; i <= rd8(set); i++)
        if (rd8(set + (gaddr)i) == ch)
            return true;
    return false;
}
static void s_PLstrpbrk(ppc_cpu *c)
{
    for (int i = 1; i <= rd8(R(0)); i++)
        if (pin_set(rd8(R(0) + (gaddr)i), R(1))) {
            RETURN(R(0) + (gaddr)i);
            return;
        }
    RETURN(0);
}
static void s_PLstrspn(ppc_cpu *c)
{
    int n = 0;
    while (n < rd8(R(0)) && pin_set(rd8(R(0) + 1 + (gaddr)n), R(1)))
        n++;
    RETURN(n);
}
/* Where str2's characters are in str1: their position (1 on), or 0. */
static int ppos(gaddr a, gaddr b)
{
    int la = rd8(a), lb = rd8(b);
    for (int i = 1; i + lb - 1 <= la; i++)
        if (!memcmp(gptr(a + (gaddr)i), gptr(b + 1), (size_t)lb))
            return i;
    return 0;
}
static void s_PLstrstr(ppc_cpu *c)
{
    int at = ppos(R(0), R(1));
    RETURN(at ? R(0) + (gaddr)at : 0);
}
static void s_PLstrlen(ppc_cpu *c) { RETURN(rd8(R(0))); }
static void s_PLpos(ppc_cpu *c) { RETURN(ppos(R(0), R(1))); }

/* _c2pstrcpy(dst, src): a C string to a Pascal one (255 at most). */
static void s__c2pstrcpy(ppc_cpu *c)
{
    size_t n = sc_strlen(R(1));
    if (n > 255)
        n = 255;
    gmemcpy(R(0) + 1, R(1), (uint32_t)n);
    wr8(R(0), (uint8_t)n);
    RETURN(R(0));
}
/* __SmartC2Pstr(s): c2pstr in place, cut to 255. */
static void s_SmartC2Pstr(ppc_cpu *c)
{
    size_t n = sc_strlen(R(0));
    if (n > 255)
        n = 255;
    gmemcpy(R(0) + 1, R(0), (uint32_t)n);
    wr8(R(0), (uint8_t)n);
    RETURN(R(0));
}

/* ---------------------------------------------------------------------- */
/* MPW's path routines (IntEnv.h, FSpio.h) */

static bool spec_is_folder(gaddr spec)
{
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true);
    wr32(pb + DirInfo_ioNamePtr_off, spec + FSSpec_name_off);
    wr16(pb + DirInfo_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
    wr32(pb + DirInfo_ioDrDirID_off, rd32(spec + FSSpec_parID_off));
    bool folder = !tb_PBGetCatInfo(pb, false) && (rd8(pb + DirInfo_ioFlAttrib_off) & 0x10);
    tb_DisposePtr(pb);
    return folder;
}

static bool spec_is_alias(gaddr spec)
{
    gaddr fi = tb_NewPtr(FInfo_SIZEOF, false, true);
    bool alias = !tb_FSpGetFInfo(spec, fi) && (rd16(fi + FInfo_fdFlags_off) & 0x8000);
    tb_DisposePtr(fi);
    return alias;
}

static void put_bool(gaddr p, bool v)
{
    if (p)
        wr8(p, v ? 1 : 0);
}

/* The spec's leaf alias followed when asked; isFolder, hadAlias and
 * leafIsAlias as MPW's routines give them. */
static int16_t finish_spec(int16_t err, gaddr spec, bool resolve_leaf, gaddr is_folder, gaddr had_alias,
                           gaddr leaf_is_alias)
{
    bool alias = false, aliased = false;
    if (!err) {
        alias = spec_is_alias(spec);
        if (alias && resolve_leaf) {
            gaddr flags = tb_NewPtr(2, false, true);
            err = tb_ResolveAliasFile(spec, 1, flags, flags + 1);
            aliased = !err && rd8(flags + 1);
            tb_DisposePtr(flags);
        }
    }
    put_bool(is_folder, !err && spec_is_folder(spec));
    put_bool(had_alias, aliased);
    put_bool(leaf_is_alias, alias);
    return err;
}

static int16_t pascal_spec(int16_t vref, int32_t dir, gaddr ppath, gaddr spec)
{
    return tb_FSMakeFSSpec(vref, dir, ppath, spec);
}

/* MakeResolvedFSSpec(vRefNum, dirID, name, *spec, *isFolder, *hadAlias,
 * *leafIsAlias) */
static void s_MakeResolvedFSSpec(ppc_cpu *c)
{
    int16_t err = pascal_spec((int16_t)R(0), (int32_t)R(1), R(2), R(3));
    RETURN((int32_t)finish_spec(err, R(3), true, R(4), R(5), R(6)));
}
/* MakeResolvedFSSpec_Long(vRefNum, dirID, path, nameBuffer, *spec, ...) */
static void s_MakeResolvedFSSpec_Long(ppc_cpu *c)
{
    int16_t err = sc_path_spec((int16_t)R(0), (int32_t)R(1), R(2), R(4));
    if (R(3))
        gmemcpy(R(3), R(4) + FSSpec_name_off, 64);
    RETURN((int32_t)finish_spec(err, R(4), true, R(5), R(6), R(7)));
}
/* ResolveFolderAliases(vRefNum, dirID, path, resolveLeaf, *spec, ...) */
static void s_ResolveFolderAliases(ppc_cpu *c)
{
    int16_t err = pascal_spec((int16_t)R(0), (int32_t)R(1), R(2), R(4));
    RETURN((int32_t)finish_spec(err, R(4), (R(3) & 0xFF) != 0, R(5), R(6), R(7)));
}
/* ResolveFolderAliases_Long(vRefNum, dirID, path, nameBuffer, resolveLeaf,
 * *spec, *isFolder, *hadAlias, *leafIsAlias) */
static void s_ResolveFolderAliases_Long(ppc_cpu *c)
{
    int16_t err = sc_path_spec((int16_t)R(0), (int32_t)R(1), R(2), R(5));
    if (R(3))
        gmemcpy(R(3), R(5) + FSSpec_name_off, 64);
    RETURN((int32_t)finish_spec(err, R(5), (R(4) & 0xFF) != 0, R(6), R(7), sc_arg(c, 8)));
}
/* _ResolveFileAlias(vRefNum, dirID, name, known, resolveLeaf, *spec,
 * *isFolder, *hadAlias, *leafIsAlias) */
static void s_ResolveFileAlias(ppc_cpu *c)
{
    int16_t err = pascal_spec((int16_t)R(0), (int32_t)R(1), R(2), R(5));
    RETURN((int32_t)finish_spec(err, R(5), (R(4) & 0xFF) != 0, R(6), R(7), sc_arg(c, 8)));
}
/* _GetAliasInfo(spec, resolve, *isFolder, *isAlias) */
static void s_GetAliasInfo(ppc_cpu *c)
{
    bool alias = spec_is_alias(R(0));
    int16_t err = finish_spec(0, R(0), (R(1) & 0xFF) != 0, R(2), 0, 0);
    put_bool(R(3), alias);
    RETURN((int32_t)err);
}

/* A path made from a spec into buf: the spec's error, or the path's. */
static int16_t path_result(int16_t err, gaddr spec, gaddr buf, uint32_t size)
{
    if (err && err != -43)
        return err;
    int16_t perr = sc_spec_path(spec, buf, size);
    return perr ? perr : err;
}

/* MakeResolvedPath(vRefNum, dirID, path, resolveLeaf, buffer, *isFolder,
 * *hadAlias, *leafIsAlias) and _Long (C path, the buffer's size last). */
static void make_resolved_path(ppc_cpu *c, bool cpath, uint32_t size)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err = cpath ? sc_path_spec((int16_t)R(0), (int32_t)R(1), R(2), spec)
                        : pascal_spec((int16_t)R(0), (int32_t)R(1), R(2), spec);
    err = finish_spec(err, spec, (R(3) & 0xFF) != 0, R(5), R(6), R(7));
    err = path_result(err, spec, R(4), size);
    tb_DisposePtr(spec);
    RETURN((int32_t)err);
}
static void s_MakeResolvedPath(ppc_cpu *c) { make_resolved_path(c, false, 256); }
static void s_MakeResolvedPath_Long(ppc_cpu *c) { make_resolved_path(c, true, sc_arg(c, 8)); }

/* ResolvePath(raw, resolved, *isFolder, *hadAlias): a path with its alias
 * followed, in full.  IEResolvePath takes the raw path as a Pascal string. */
static void resolve_path(ppc_cpu *c, bool pascal, uint32_t size)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err = pascal ? pascal_spec(0, 0, R(0), spec) : sc_path_spec(0, 0, R(0), spec);
    err = finish_spec(err, spec, true, R(2), R(3), 0);
    err = path_result(err, spec, R(1), size);
    tb_DisposePtr(spec);
    RETURN((int32_t)err);
}
static void s_ResolvePath(ppc_cpu *c) { resolve_path(c, false, 256); }
static void s_IEResolvePath(ppc_cpu *c) { resolve_path(c, true, 256); }
static void s_ResolvePath_Long(ppc_cpu *c) { resolve_path(c, false, R(4)); }

/* _FSSpec2Path(spec, path) and FSSpec2Path_Long(spec, path, size): the
 * FSSpec comes by value, in the first 18 argument words. */
static gaddr spec_by_value(ppc_cpu *c)
{
    gaddr spec = tb_NewPtr(72, false, true);
    for (unsigned i = 0; i < 18; i++) {
        uint32_t w = sc_arg(c, i);
        if (4 * i + 4 <= 72)
            wr32(spec + 4 * i, w);
    }
    return spec;
}
static void s_FSSpec2Path(ppc_cpu *c)
{
    gaddr spec = spec_by_value(c);
    int16_t err = sc_spec_path(spec, sc_arg(c, 18), 256);
    tb_DisposePtr(spec);
    RETURN((int32_t)err);
}
static void s_FSSpec2Path_Long(ppc_cpu *c)
{
    gaddr spec = spec_by_value(c);
    int16_t err = sc_spec_path(spec, sc_arg(c, 18), sc_arg(c, 19));
    tb_DisposePtr(spec);
    RETURN((int32_t)err);
}
/* FSMakeFSSpec_Long(path, nameBuffer, vRefNum, dirID, *spec) */
static void s_FSMakeFSSpec_Long(ppc_cpu *c)
{
    int16_t err = sc_path_spec((int16_t)R(2), (int32_t)R(3), R(0), R(4));
    if (R(1) && (!err || err == -43))
        gmemcpy(R(1), R(4) + FSSpec_name_off, 64);
    RETURN((int32_t)err);
}

/* The FSSpec forms of open and the rest: the spec passed as the name, the
 * way MPW's library passed it to its devices. */
static gaddr spec_name(gaddr spec)
{
    gaddr name = tb_NewPtr(4 + FSSpec_SIZEOF, false, true);
    if (name) {
        wr32(name, 0x00187F1B);
        gmemcpy(name + 4, spec, FSSpec_SIZEOF);
    }
    return name;
}
static int32_t spec_faccess(gaddr spec, uint32_t cmd, gaddr arg)
{
    gaddr name = spec_name(spec);
    int32_t r = name ? sc_faccess(name, cmd, arg) : -1;
    if (name)
        tb_DisposePtr(name);
    return r;
}
static void s_FSp_open(ppc_cpu *c) { RETURN(sc_open_spec(R(0), R(1))); }
static void s_FSp_creat(ppc_cpu *c) { RETURN(sc_open_spec(R(0), MPW_O_WRONLY | MPW_O_CREAT | MPW_O_TRUNC)); }
static void s_FSp_remove(ppc_cpu *c) { RETURN(spec_faccess(R(0), MPW_F_DELETE, 0)); }
static void s_FSp_rename(ppc_cpu *c) { RETURN(spec_faccess(R(0), MPW_F_RENAME, R(1))); }
static void s_FSp_faccess(ppc_cpu *c) { RETURN(spec_faccess(R(0), R(1), R(2))); }

static void set_file_info(gaddr spec, uint32_t creator, uint32_t type)
{
    gaddr fi = tb_NewPtr(FInfo_SIZEOF, false, true);
    if (!tb_FSpGetFInfo(spec, fi)) {
        wr32(fi + FInfo_fdCreator_off, creator);
        wr32(fi + FInfo_fdType_off, type);
        tb_FSpSetFInfo(spec, fi);
    }
    tb_DisposePtr(fi);
}
static void s_FSp_fsetfileinfo(ppc_cpu *c)
{
    set_file_info(R(0), R(1), R(2));
    RETURN_VOID();
}
/* fsetfileinfo(path, creator, type) */
static void s_fsetfileinfo(ppc_cpu *c)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    if (!sc_path_spec(0, 0, R(0), spec))
        set_file_info(spec, R(1), R(2));
    tb_DisposePtr(spec);
    RETURN_VOID();
}

/* getpid(): the process's own number, from its serial number. */
static uint32_t spid(void)
{
    gaddr psn = tb_NewPtr(8, false, true);
    uint32_t pid = 1;
    if (psn && !tb_GetCurrentProcess(psn) && rd32(psn + 4))
        pid = rd32(psn + 4);
    if (psn)
        tb_DisposePtr(psn);
    return pid;
}
static void s_getpid(ppc_cpu *c) { RETURN(spid()); }

/* getIDstring(s): s's characters, from the end, made the process's
 * number in base 41 (mktemp's letters). */
static void id_string(gaddr s, size_t n)
{
    static const char digits[] = "0123456789ABCDEFGHiJKLMNoPQRSTUVWXYZ@!%$_";
    uint32_t v = spid();
    for (size_t i = n; i > 0; i--) {
        wr8(s + (gaddr)i - 1, (uint8_t)digits[v % 41]);
        v /= 41;
    }
}
static void s_getIDstring(ppc_cpu *c)
{
    id_string(R(0), sc_strlen(R(0)));
    RETURN_VOID();
}

/* mktemp(template): its trailing X's made a name no file has (or the
 * string emptied when it can't). */
static void s_mktemp(ppc_cpu *c)
{
    gaddr t = R(0);
    size_t n = sc_strlen(t), x = n;
    while (x > 0 && rd8(t + (gaddr)x - 1) == 'X')
        x--;
    if (x < n)
        id_string(t + (gaddr)x, n - x);
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    bool found = sc_path_spec(0, 0, t, spec) == -43;
    for (int letter = 'a'; !found && x < n && letter <= 'z'; letter++) {
        wr8(t + (gaddr)x, (uint8_t)letter);
        found = sc_path_spec(0, 0, t, spec) == -43;
    }
    tb_DisposePtr(spec);
    if (!found)
        wr8(t, 0);
    RETURN(t);
}

/* binhex(n, buf, len): n in hexadecimal at the end of len characters
 * of buf, zeros before it (no terminating zero). */
static void s_binhex(ppc_cpu *c)
{
    uint32_t v = R(0);
    int32_t len = (int32_t)R(2);
    if (len > 0)
        gmemset(R(1), '0', (uint32_t)len);
    for (int32_t i = len - 1; i >= 0 && v; i--, v >>= 4)
        wr8(R(1) + (gaddr)i, (uint8_t)"0123456789ABCDEF"[v & 15]);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* The runtime */

/* _RTInit(block, &argc, &argv, &envp, flags): the arguments a program
 * has, none (there is no MPW Shell). */
static void s_RTInit(ppc_cpu *c)
{
    gaddr ie = sc_intenv();
    if (R(1))
        wr32(R(1), (uint32_t)((uint32_t)rd16(ie + 2) << 16 | rd16(ie + 4)));
    if (R(2))
        wr32(R(2), (uint32_t)rd16(ie + 6) << 16 | rd16(ie + 8));
    if (R(3))
        wr32(R(3), (uint32_t)rd16(ie + 10) << 16 | rd16(ie + 12));
    RETURN(0);
}

/* __RestoreInitialCFragWorld(save) keeps the current zone and A5 in save
 * and goes back to those the program started with; __RevertCFragWorld
 * (save) restores what was kept. */
static gaddr initial_zone;
static uint32_t initial_a5;
static void s_RestoreInitialCFragWorld(ppc_cpu *c)
{
    if (!initial_zone) {
        initial_zone = tb_GetZone();
        initial_a5 = g_cpu->a[5];
    }
    wr32(R(0), tb_GetZone());
    wr32(R(0) + 4, g_cpu->a[5]);
    wr32(R(0) + 8, 0);
    tb_SetZone(initial_zone);
    g_cpu->a[5] = initial_a5;
    RETURN_VOID();
}
static void s_RevertCFragWorld(ppc_cpu *c)
{
    if (rd32(R(0)))
        tb_SetZone(rd32(R(0)));
    g_cpu->a[5] = rd32(R(0) + 4);
    RETURN_VOID();
}

/* __GetTrapType(trap): ToolTrap (1) or OSTrap (0). */
static void s_GetTrapType(ppc_cpu *c) { RETURN((R(0) & 0x0800) ? 1 : 0); }

static uint32_t trap_address(uint16_t trap, bool toolbox)
{
    ppc_trap_begin();
    ppc_trap_set(0, trap);
    ppc_trap((uint16_t)(0xA346 | (toolbox ? 0x0400 : 0)));
    uint32_t r = ppc_trap_get(8);
    ppc_trap_end();
    return r;
}
/* __NumToolboxTraps(): 0x400, or 0x200 on a Mac whose table repeats
 * after 0x200 entries (InitGraf's two numbers the same routine). */
static void s_NumToolboxTraps(ppc_cpu *c)
{
    RETURN(trap_address(0xA86E, true) == trap_address(0xAA6E, true) ? 0x200 : 0x400);
}
/* TrapAvailable(trap): its address isn't _Unimplemented's. */
static void s_TrapAvailable(ppc_cpu *c)
{
    uint16_t t = (uint16_t)R(0);
    RETURN(trap_address(t, (t & 0x0800) != 0) != trap_address(0xA89F, true));
}

/* _Bogus(): MPW's stand-in for a routine it didn't have. */
static void s_Bogus(ppc_cpu *c)
{
    LOGF(LOG_UNIMPL, "StdCLib: _Bogus called\n");
    RETURN(0);
}

/* The library's own allocator calls (standalone code): the Memory
 * Manager's. */
static void s_SA_NewPtr(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), false, false)); }
static void s_SA_DisposePtr(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    RETURN((int16_t)rd16(LM_MemErr));
}
static void s_SA_GetPtrSize(ppc_cpu *c) { RETURN(tb_GetPtrSize(R(0))); }
static void s_SA_SetPtrSize(ppc_cpu *c)
{
    tb_SetPtrSize(R(0), (int32_t)R(1));
    RETURN(rd16(LM_MemErr) ? 0 : R(0));
}
static void s_SA_FreeMem(ppc_cpu *c) { RETURN(tb_FreeMem(false)); }
/* _SA_GetPID(): (network << 8) | node, 0 without AppleTalk. */
static void s_SA_GetPID(ppc_cpu *c) { RETURN(0); }

/* malloc's own bookkeeping: there are no pools to check or empty. */
static void s_nothing(ppc_cpu *c) { RETURN_VOID(); }

/* __assertprint(...): MPW's assert, failing: the message to the log. */
static void s_assertprint(ppc_cpu *c)
{
    char msg[256] = "";
    size_t n = 0;
    for (int i = 0; i < 3 && n < sizeof msg - 1; i++) {
        uint32_t v = R(i);
        if (v >= 0x1000 && v < 0x40000000) { /* a string */
            for (uint32_t k = 0; k < 120 && rd8(v + k) && n < sizeof msg - 2; k++)
                msg[n++] = (char)rd8(v + k);
            msg[n++] = ' ';
        } else {
            n += (size_t)snprintf(msg + n, sizeof msg - n, "%d ", (int32_t)v);
        }
    }
    msg[n < sizeof msg ? n : sizeof msg - 1] = 0;
    LOGF(LOG_UNIMPL, "assertion failed: %s\n", msg);
    RETURN_VOID();
}

/* ---------------------------------------------------------------------- */
/* Data */

/* The character classes, MPW style: upper 1, lower 2, digit 4, space 8,
 * punctuation 16, control 32, blank 64, hex digit 128.  __p_CType points
 * at the entry for 0, with one for EOF before it. */
static gaddr ctype_table(void)
{
    gaddr t = tb_NewPtr(258, true, true);
    for (int ch = 0; ch < 128; ch++) {
        uint8_t k = 0;
        if (ch >= 'A' && ch <= 'Z')
            k |= 1;
        if (ch >= 'a' && ch <= 'z')
            k |= 2;
        if (ch >= '0' && ch <= '9')
            k |= 4 | 128;
        if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))
            k |= 128;
        if (ch == ' ' || (ch >= 9 && ch <= 13))
            k |= 8;
        if (ch == ' ')
            k |= 64;
        if (ch < 32 || ch == 127)
            k |= 32;
        if (ch > 32 && ch < 127 && !(k & 7))
            k |= 16;
        wr8(t + 1 + (gaddr)ch, k);
    }
    return t + 1;
}

static gaddr new_double(double d)
{
    gaddr p = tb_NewPtr(16, true, true);
    if (p)
        sc_wr_double(p, d);
    return p;
}

static gaddr new_float(float f)
{
    gaddr p = tb_NewPtr(8, true, true);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    if (p)
        wr32(p, bits);
    return p;
}

/* MoneyData, NumericData, TimeData and _PublicTimeInfo: a locale code,
 * then the category's information (lconv, and the day and month names,
 * 16 bytes each). */
static gaddr locale_data(bool time_names)
{
    gaddr p = tb_NewPtr(8, true, true);
    wr8(p, 'C');
    if (time_names) {
        gaddr names = tb_NewPtr(19 * 16, true, true);
        for (int i = 0; i < 7; i++)
            gput(names + 16 * (gaddr)i, day_names[i], (uint32_t)strlen(day_names[i]));
        for (int i = 0; i < 12; i++)
            gput(names + 16 * (gaddr)(7 + i), month_names[i], (uint32_t)strlen(month_names[i]));
        wr32(p + 4, names);
    } else {
        wr32(p + 4, lconv());
    }
    return p;
}

static const char *const data_names[] = {
    "errno", "MacOSErr", "_iob", "_lastbuf", "_IntEnv", "_StdDevs", "__p_CType", "_DBL_EPSILON", "_DBL_MIN",
    "_DBL_MAX", "_FLT_EPSILON", "_FLT_MIN", "_FLT_MAX", "_LDBL_EPSILON", "_LDBL_MIN", "_LDBL_MAX", "StandAlone",
    "_exit_status", "__target_for_exit", "__NubAt3", "__C_phase", "__SigEnv", "NoMoreDebugStr", "__loc",
    "_CategoryLoc", "_PublicTimeInfo", "MoneyData", "NumericData", "TimeData", "__yd",
};

bool stdclib_data_known(const char *name)
{
    for (unsigned i = 0; i < sizeof data_names / sizeof data_names[0]; i++)
        if (!strcmp(name, data_names[i]))
            return true;
    return false;
}

/* A data export's address (the importer keeps it: cfm.c asks once). */
gaddr stdclib_data_export(const char *name)
{
    if (!strcmp(name, "errno"))
        return sc_errno_addr();
    if (!strcmp(name, "MacOSErr"))
        return static_block(&macoserr_block, 4);
    if (!strcmp(name, "_iob"))
        return sc_iob();
    if (!strcmp(name, "_lastbuf"))
        return sc_lastbuf();
    if (!strcmp(name, "_IntEnv"))
        return sc_intenv();
    if (!strcmp(name, "_StdDevs"))
        return sc_stddevs();
    if (!strcmp(name, "__p_CType")) {
        gaddr p = tb_NewPtr(4, true, true);
        wr32(p, ctype_table());
        return p;
    }
    if (!strcmp(name, "_DBL_EPSILON"))
        return new_double(2.220446049250313e-16);
    if (!strcmp(name, "_DBL_MIN"))
        return new_double(2.2250738585072014e-308);
    if (!strcmp(name, "_DBL_MAX"))
        return new_double(1.7976931348623157e308);
    if (!strcmp(name, "_FLT_EPSILON"))
        return new_float(1.1920929e-7f);
    if (!strcmp(name, "_FLT_MIN"))
        return new_float(1.17549435e-38f);
    if (!strcmp(name, "_FLT_MAX"))
        return new_float(3.40282347e38f);
    /* long double is MrC's double-double: the double's limits, a zero
     * low half. */
    if (!strcmp(name, "_LDBL_EPSILON"))
        return new_double(2.220446049250313e-16);
    if (!strcmp(name, "_LDBL_MIN"))
        return new_double(2.2250738585072014e-308);
    if (!strcmp(name, "_LDBL_MAX"))
        return new_double(1.7976931348623157e308);
    if (!strcmp(name, "StandAlone")) { /* an application, not an MPW tool */
        gaddr p = tb_NewPtr(4, true, true);
        wr32(p, 1);
        return p;
    }
    if (!strcmp(name, "_exit_status"))
        return static_block(&exit_status_block, 4);
    if (!strcmp(name, "__SigEnv"))
        return sig_env();
    if (!strcmp(name, "__loc"))
        return lconv();
    if (!strcmp(name, "_CategoryLoc"))
        return categories();
    if (!strcmp(name, "_PublicTimeInfo") || !strcmp(name, "TimeData"))
        return locale_data(true);
    if (!strcmp(name, "MoneyData") || !strcmp(name, "NumericData"))
        return locale_data(false);
    if (!strcmp(name, "__yd")) { /* the days before each month, and the year's */
        gaddr p = tb_NewPtr(13 * 4, true, true);
        int days = 0;
        for (int m = 0; m <= 12; m++) {
            wr32(p + 4 * (gaddr)m, (uint32_t)days);
            if (m < 12)
                days += month_days[m];
        }
        return p;
    }
    if (stdclib_data_known(name)) /* __target_for_exit (a jmp_buf) and flags */
        return tb_NewPtr(1024, true, true);
    return 0;
}

void stdclib_reset(void)
{
    errno_block = macoserr_block = strerror_buf = cvt_buf = 0;
    strtok_next = 0;
    rand_state = 1;
    for (int i = 0; i < NENV; i++)
        env[i].value = 0;
    exit_status_block = 0;
    exiting = false;
    sigenv = 0;
    tm_buf = asctime_buf = 0;
    loc_block = category_block = locale_name = locale_composite = 0;
    initial_zone = 0;
    initial_a5 = 0;
    sc_io_reset();
    sc_stdio_reset();
}

static const ppc_export sc_exports[] = {
    /* errno */
    { "__geterrno", s_geterrno }, { "_uerror", s_uerror }, { "strerror", s_strerror },
    /* strings and memory */
    { "strlen", s_strlen }, { "__lib_Cstrlen", s_strlen }, { "strcpy", s_strcpy }, { "strncpy", s_strncpy },
    { "strcat", s_strcat }, { "strncat", s_strncat }, { "strcmp", s_strcmp }, { "strncmp", s_strncmp },
    { "strcoll", s_strcmp }, { "strxfrm", s_strxfrm }, { "strchr", s_strchr }, { "strrchr", s_strrchr },
    { "strstr", s_strstr }, { "strpbrk", s_strpbrk }, { "strspn", s_strspn }, { "strcspn", s_strcspn },
    { "strtok", s_strtok }, { "memcpy", s_memcpy }, { "memmove", s_memcpy }, { "_memcpy", s_memcpy },
    { "_rmemcpy", s_memcpy }, { "memset", s_memset }, { "memcmp", s_memcmp }, { "memchr", s_memchr },
    { "_memchr", s_memchr }, { "memccpy", s_memccpy },
    /* ctype */
    { "isalnum", s_isalnum }, { "isalpha", s_isalpha }, { "isdigit", s_isdigit }, { "isxdigit", s_isxdigit },
    { "isspace", s_isspace }, { "isupper", s_isupper }, { "islower", s_islower }, { "ispunct", s_ispunct },
    { "isprint", s_isprint }, { "isgraph", s_isgraph }, { "iscntrl", s_iscntrl }, { "isascii", s_isascii },
    { "toupper", s_toupper }, { "tolower", s_tolower }, { "toascii", s_toascii },
    /* stdlib */
    { "malloc", s_malloc }, { "calloc", s_calloc }, { "free", s_free }, { "realloc", s_realloc },
    { "vec_malloc", s_vec_malloc }, { "vec_calloc", s_vec_calloc }, { "vec_free", s_vec_free },
    { "vec_realloc", s_vec_realloc }, { "abs", s_abs }, { "labs", s_abs }, { "llabs", s_llabs },
    { "div", s_div }, { "ldiv", s_div }, { "lldiv", s_lldiv }, { "atoi", s_atoi }, { "atol", s_atoi },
    { "atoll", s_atoll }, { "atof", s_atof }, { "strtol", s_strtol }, { "strtoul", s_strtoul },
    { "strtoll", s_strtoll }, { "strtoull", s_strtoull }, { "strtod", s_strtod }, { "ecvt", s_ecvt },
    { "fcvt", s_fcvt }, { "_cvt", s_cvt }, { "rand", s_rand }, { "srand", s_srand }, { "qsort", s_qsort },
    { "bsearch", s_bsearch }, { "getenv", s_getenv }, { "setenv", s_setenv }, { "system", s_system },
    { "mblen", s_mblen }, { "mbtowc", s_mbtowc }, { "wctomb", s_wctomb }, { "mbstowcs", s_mbstowcs },
    { "wcstombs", s_wcstombs }, { "atexit", s_atexit }, { "exit", s_exit }, { "_exit", s__exit },
    { "abort", s_abort }, { "__abort", s_abort }, { "__abort__Fv", s_abort }, { "_DoExitProcs", s_DoExitProcs },
    /* setjmp */
    { "setjmp", s_setjmp }, { "__setjmp", s_setjmp }, { "__vec_setjmp", s_setjmp }, { "longjmp", s_longjmp },
    { "__vec_longjmp", s_longjmp },
    /* signals */
    { "signal", s_signal }, { "raise", s_raise },
    /* time */
    { "time", s_time }, { "clock", s_clock }, { "difftime", s_difftime }, { "mktime", s_mktime },
    { "localtime", s_localtime }, { "gmtime", s_gmtime }, { "asctime", s_asctime }, { "ctime", s_ctime },
    { "strftime", s_strftime },
    /* locale */
    { "setlocale", s_setlocale }, { "localeconv", s_localeconv }, { "ConvertTheString", s_ConvertTheString },
    { "MakeTheLocaleString", s_MakeTheLocaleString }, { "ParseTheLocaleString", s_ParseTheLocaleString },
    /* Pascal strings */
    { "PLstrcmp", s_PLstrcmp }, { "PLstrncmp", s_PLstrncmp }, { "PLstrcpy", s_PLstrcpy },
    { "PLstrncpy", s_PLstrncpy }, { "PLstrcat", s_PLstrcat }, { "PLstrncat", s_PLstrncat },
    { "PLstrchr", s_PLstrchr }, { "PLstrrchr", s_PLstrrchr }, { "PLstrpbrk", s_PLstrpbrk },
    { "PLstrspn", s_PLstrspn }, { "PLstrstr", s_PLstrstr }, { "PLstrlen", s_PLstrlen }, { "PLpos", s_PLpos },
    { "_c2pstrcpy", s__c2pstrcpy }, { "__SmartC2Pstr", s_SmartC2Pstr },
    /* paths */
    { "MakeResolvedFSSpec", s_MakeResolvedFSSpec }, { "MakeResolvedFSSpec_Long", s_MakeResolvedFSSpec_Long },
    { "ResolveFolderAliases", s_ResolveFolderAliases }, { "ResolveFolderAliases_Long", s_ResolveFolderAliases_Long },
    { "_ResolveFileAlias", s_ResolveFileAlias }, { "_GetAliasInfo", s_GetAliasInfo },
    { "MakeResolvedPath", s_MakeResolvedPath }, { "MakeResolvedPath_Long", s_MakeResolvedPath_Long },
    { "ResolvePath", s_ResolvePath }, { "ResolvePath_Long", s_ResolvePath_Long }, { "IEResolvePath", s_IEResolvePath },
    { "_FSSpec2Path", s_FSSpec2Path }, { "FSSpec2Path_Long", s_FSSpec2Path_Long },
    { "FSMakeFSSpec_Long", s_FSMakeFSSpec_Long }, { "FSp_open", s_FSp_open }, { "FSp_creat", s_FSp_creat },
    { "FSp_remove", s_FSp_remove }, { "FSp_unlink", s_FSp_remove }, { "FSp_rename", s_FSp_rename },
    { "FSp_faccess", s_FSp_faccess }, { "FSp_fsetfileinfo", s_FSp_fsetfileinfo },
    { "fsetfileinfo", s_fsetfileinfo }, { "mktemp", s_mktemp }, { "getIDstring", s_getIDstring },
    { "binhex", s_binhex }, { "getpid", s_getpid },
    /* the runtime */
    { "_RTInit", s_RTInit }, { "_RTExit", s_RTExit }, { "__RestoreInitialCFragWorld", s_RestoreInitialCFragWorld },
    { "__RevertCFragWorld", s_RevertCFragWorld }, { "__GetTrapType", s_GetTrapType },
    { "__NumToolboxTraps", s_NumToolboxTraps }, { "TrapAvailable", s_TrapAvailable }, { "_Bogus", s_Bogus },
    { "_badPtr", s_nothing }, { "_SA_NewPtr", s_SA_NewPtr }, { "_SA_DisposePtr", s_SA_DisposePtr },
    { "_SA_DeletePtr", s_SA_DisposePtr }, { "_SA_GetPtrSize", s_SA_GetPtrSize }, { "_SA_SetPtrSize", s_SA_SetPtrSize },
    { "_SA_FreeMem", s_SA_FreeMem }, { "_SA_GetPID", s_SA_GetPID }, { "__DebugMallocHeap", s_nothing },
    { "__DeleteEmptyBuckets", s_nothing }, { "__mallocStats", s_nothing }, { "__assertprint", s_assertprint },
};

/* StdCLib's routine by name, or NULL. */
void (*stdclib_export_fn(const char *name))(ppc_cpu *)
{
    static const struct {
        const ppc_export *table;
        const unsigned *count;
    } tables[] = { { sc_exports, NULL }, { sc_io_exports, &sc_io_export_count }, { sc_stdio_exports, &sc_stdio_export_count } };
    for (unsigned t = 0; t < 3; t++) {
        unsigned n = tables[t].count ? *tables[t].count : sizeof sc_exports / sizeof sc_exports[0];
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    }
    return NULL;
}
