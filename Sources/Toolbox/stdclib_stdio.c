/*
 * StdCLib's streams: MPW's FILE structs in _iob, and formatted input and
 * output.
 *
 * A program's getc and putc macros (MPW's stdio.h before Universal
 * Interfaces 3) work on the struct itself: getc is
 * (--p->_cnt >= 0 ? *p->_ptr++ : _filbuf(p)) and putc the same with
 * _flsbuf, so the fields here behave as MPW's library kept them.  _cnt is
 * what is left in the buffer; a line-buffered or unbuffered stream keeps
 * _cnt at 0 for writing, so every putc comes to _flsbuf, which buffers up
 * to the end of the line (MPW's '\n', 13).  stdin, stdout and stderr are
 * _iob[0..2], on descriptors 0 to 2.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stdclib_internal.h"

enum { NFILE = 40, BUFSIZE = 1024 };
/* struct FILE (24 bytes) */
enum { F_CNT = 0, F_PTR = 4, F_BASE = 8, F_END = 12, F_SIZE = 16, F_FLAG = 18, F_FILE = 20, FILE_SIZE = 24 };
/* _flag */
enum {
    IOFBF = 0x00, IOREAD = 0x01, IOWRT = 0x02, IONBF = 0x04, IOMYBUF = 0x08, IOEOF = 0x10, IOERR = 0x20,
    IOLBF = 0x40, IORW = 0x80, IOSYNC = 0x100,
};
enum { MPW_EOF = -1 };

static gaddr iob, smallbufs;
static gaddr tmp_name[NFILE]; /* tmpfile's, removed at fclose */
static gaddr tmpnam_buf;
static uint32_t tmp_seq;

static void iob_init(void)
{
    if (iob)
        return;
    /* _iob, then _lastbuf: &_iob[_NFILE], where it is itself. */
    iob = tb_NewPtr(NFILE * FILE_SIZE + 4, true, true);
    wr32(iob + NFILE * FILE_SIZE, iob + NFILE * FILE_SIZE);
    smallbufs = tb_NewPtr(NFILE, true, true);
    wr16(iob + F_FLAG, IOREAD);
    wr16(iob + FILE_SIZE + F_FLAG, IOWRT);
    wr16(iob + FILE_SIZE + F_FILE, 1);
    wr16(iob + 2 * FILE_SIZE + F_FLAG, IOWRT | IOLBF);
    wr16(iob + 2 * FILE_SIZE + F_FILE, 2);
}

gaddr sc_iob(void)
{
    iob_init();
    return iob;
}

gaddr sc_lastbuf(void)
{
    iob_init();
    return iob + NFILE * FILE_SIZE;
}

void sc_stdio_reset(void)
{
    iob = smallbufs = tmpnam_buf = 0;
    memset(tmp_name, 0, sizeof tmp_name);
}

static uint16_t flag(gaddr f) { return rd16(f + F_FLAG); }
static void set_flag(gaddr f, uint16_t v) { wr16(f + F_FLAG, v); }
static int32_t cnt(gaddr f) { return (int32_t)rd32(f + F_CNT); }
static int32_t fd_of(gaddr f) { return (int16_t)rd16(f + F_FILE); }
static int file_index(gaddr f) { return (int)((f - iob) / FILE_SIZE); }
static bool is_open(gaddr f) { return f && (flag(f) & (IOREAD | IOWRT | IORW)); }

/* The errno a call left, put back (a question to a device that says no). */
static int32_t quiet_ioctl(int32_t fd, uint32_t cmd, gaddr arg)
{
    uint32_t e = rd32(sc_errno_addr());
    int32_t r = sc_ioctl(fd, cmd, arg);
    wr32(sc_errno_addr(), e);
    return r;
}

/* _findbuf: a buffer for a stream, line buffered when its device is
 * interactive (the console), a byte of its own when unbuffered. */
static void findbuf(gaddr f)
{
    iob_init();
    uint16_t fl = flag(f);
    gaddr base = 0;
    uint32_t size = 1;
    if (!(fl & IONBF)) {
        if (quiet_ioctl(fd_of(f), MPW_FIOINTERACTIVE, 0) == 0)
            fl |= IOLBF;
        base = tb_NewPtr(BUFSIZE, false, false);
        if (base) {
            size = BUFSIZE;
            fl |= IOMYBUF;
        }
    }
    if (!base) {
        fl = (uint16_t)((fl & ~IOLBF) | IONBF);
        int i = file_index(f);
        base = smallbufs + (gaddr)(i >= 0 && i < NFILE ? i : 0);
    }
    set_flag(f, fl);
    wr32(f + F_BASE, base);
    wr32(f + F_PTR, base);
    wr32(f + F_END, base + size);
    wr16(f + F_SIZE, (uint16_t)size);
    wr32(f + F_CNT, 0);
}

/* _xflsbuf: what is in the buffer written out. */
static int32_t xflsbuf(gaddr f)
{
    gaddr base = rd32(f + F_BASE);
    int32_t n = (int32_t)(rd32(f + F_PTR) - base);
    wr32(f + F_PTR, base);
    wr32(f + F_CNT, (flag(f) & (IOLBF | IONBF)) ? 0 : rd16(f + F_SIZE));
    if (n > 0 && sc_write(fd_of(f), base, (uint32_t)n) != n) {
        set_flag(f, flag(f) | IOERR);
        return MPW_EOF;
    }
    return 0;
}

/* _wrtchk: a stream made ready to write. */
static int32_t wrtchk(gaddr f)
{
    uint16_t fl = flag(f);
    if (!(fl & (IOWRT | IORW)) || ((fl & IOREAD) && !(fl & IORW))) {
        set_flag(f, fl | IOERR);
        sc_set_errno(MPW_EBADF);
        return MPW_EOF;
    }
    if (fl & IOREAD) { /* reading before: back to where the program is */
        if (cnt(f) > 0)
            sc_lseek(fd_of(f), -cnt(f), 1);
        wr32(f + F_CNT, 0);
        wr32(f + F_PTR, rd32(f + F_BASE));
        fl &= (uint16_t)~(IOREAD | IOEOF);
    }
    set_flag(f, fl | IOWRT);
    if (!rd32(f + F_BASE))
        findbuf(f);
    if (rd32(f + F_PTR) == rd32(f + F_BASE) && !(flag(f) & (IONBF | IOLBF)))
        wr32(f + F_CNT, rd16(f + F_SIZE));
    return 0;
}

int32_t sc_fflush(gaddr f);

/* _filbuf: the buffer filled; its first byte, or EOF. */
static int32_t filbuf(gaddr f)
{
    iob_init();
    uint16_t fl = flag(f);
    if (!(fl & (IOREAD | IORW))) {
        wr32(f + F_CNT, 0);
        return MPW_EOF;
    }
    if ((fl & (IORW | IOWRT)) == (IORW | IOWRT)) { /* writing before */
        if (xflsbuf(f))
            return MPW_EOF;
        wr32(f + F_CNT, 0);
        fl &= (uint16_t)~IOWRT;
    }
    fl |= IOREAD;
    set_flag(f, fl);
    if (fl & IOEOF) {
        wr32(f + F_CNT, 0);
        return MPW_EOF;
    }
    if (!rd32(f + F_BASE))
        findbuf(f);
    /* Output that should be out before input is read: streams marked
     * _IOSYNC, and line-buffered ones before an interactive read (so a
     * prompt shows). */
    bool interactive = flag(f) & (IOLBF | IONBF);
    for (gaddr g = iob; g < iob + NFILE * FILE_SIZE; g += FILE_SIZE) {
        uint16_t gf = flag(g);
        if (g != f && (gf & IOWRT) && ((gf & IOSYNC) || (interactive && (gf & IOLBF))) && rd32(g + F_BASE) &&
            xflsbuf(g))
            return MPW_EOF;
    }
    gaddr base = rd32(f + F_BASE);
    wr32(f + F_PTR, base);
    int32_t n = sc_read(fd_of(f), base, (flag(f) & IONBF) ? 1 : rd16(f + F_SIZE));
    wr32(f + F_CNT, (uint32_t)(n - 1));
    if (n > 0) {
        wr32(f + F_PTR, base + 1);
        return rd8(base);
    }
    fl = flag(f);
    if (n == 0) {
        fl |= IOEOF;
        if (fl & IORW)
            fl &= (uint16_t)~IOREAD;
    } else {
        fl |= IOERR;
    }
    set_flag(f, fl);
    wr32(f + F_CNT, 0);
    return MPW_EOF;
}

/* _flsbuf(c, f): c written when the buffer had no room for it (or the
 * stream buffers by line or not at all). */
static int32_t flsbuf(uint8_t ch, gaddr f)
{
    iob_init();
    uint16_t fl = flag(f);
    if ((fl & (IOLBF | IOEOF | IOWRT)) == (IOLBF | IOWRT)) {
        gaddr p = rd32(f + F_PTR);
        if (p < rd32(f + F_END)) {
            wr8(p, ch);
            wr32(f + F_PTR, p + 1);
            if (ch == MPW_NEWLINE)
                return xflsbuf(f) == MPW_EOF ? MPW_EOF : ch;
            return ch;
        }
    } else if ((fl & (IONBF | IOEOF | IOWRT)) == (IONBF | IOWRT)) {
        gaddr one = rd32(f + F_BASE);
        wr8(one, ch);
        if (sc_write(fd_of(f), one, 1) == 1)
            return ch;
        set_flag(f, flag(f) | IOERR);
        return MPW_EOF;
    }
    gaddr base = rd32(f + F_BASE);
    if ((fl & (IOEOF | IOWRT)) != IOWRT || !base ||
        (rd32(f + F_PTR) == base && cnt(f) == 0 && !(fl & (IOLBF | IONBF)))) {
        if (wrtchk(f))
            return MPW_EOF;
    }
    if (xflsbuf(f) != MPW_EOF) {
        int32_t n = cnt(f) - 1;
        wr32(f + F_CNT, (uint32_t)n);
        if (n >= 0) {
            gaddr p = rd32(f + F_PTR);
            wr8(p, ch);
            wr32(f + F_PTR, p + 1);
        } else {
            wr32(f + F_CNT, 0);
            flsbuf(ch, f);
        }
    }
    return (flag(f) & IOERR) ? MPW_EOF : ch;
}

/* The macros, as programs have them. */
static int32_t sgetc(gaddr f)
{
    int32_t n = cnt(f) - 1;
    wr32(f + F_CNT, (uint32_t)n);
    if (n >= 0) {
        gaddr p = rd32(f + F_PTR);
        wr32(f + F_PTR, p + 1);
        return rd8(p);
    }
    return filbuf(f);
}

static int32_t sputc(uint8_t ch, gaddr f)
{
    int32_t n = cnt(f) - 1;
    wr32(f + F_CNT, (uint32_t)n);
    if (n >= 0) {
        gaddr p = rd32(f + F_PTR);
        wr8(p, ch);
        wr32(f + F_PTR, p + 1);
        return ch;
    }
    return flsbuf(ch, f);
}

static int32_t sungetc(int32_t ch, gaddr f)
{
    if (ch == MPW_EOF || !is_open(f))
        return MPW_EOF;
    if ((flag(f) & (IORW | IOWRT)) == (IORW | IOWRT)) {
        if (xflsbuf(f))
            return MPW_EOF;
        set_flag(f, flag(f) & (uint16_t)~IOWRT);
    } else if (flag(f) & IOWRT) {
        return MPW_EOF;
    }
    set_flag(f, flag(f) | IOREAD);
    if (!rd32(f + F_BASE))
        findbuf(f);
    gaddr base = rd32(f + F_BASE), p = rd32(f + F_PTR);
    if (cnt(f) < 0)
        wr32(f + F_CNT, 0);
    if (p == base) {
        if (cnt(f) > 0)
            return MPW_EOF; /* no room before what is there */
        p++;
    }
    wr32(f + F_PTR, --p);
    wr8(p, (uint8_t)ch);
    wr32(f + F_CNT, (uint32_t)(cnt(f) + 1));
    set_flag(f, flag(f) & (uint16_t)~IOEOF);
    return (uint8_t)ch;
}

uint32_t sc_fwrite_host(const void *data, uint32_t n, gaddr f)
{
    const uint8_t *p = data;
    if (!is_open(f)) {
        sc_set_errno(MPW_EBADF);
        return 0;
    }
    uint32_t done = 0;
    while (done < n) {
        int32_t room = cnt(f);
        if (room > 0) { /* straight into the buffer */
            uint32_t k = n - done < (uint32_t)room ? n - done : (uint32_t)room;
            gaddr at = rd32(f + F_PTR);
            gput(at, p + done, k);
            wr32(f + F_PTR, at + k);
            wr32(f + F_CNT, (uint32_t)(room - (int32_t)k));
            done += k;
            continue;
        }
        if (sputc(p[done], f) == MPW_EOF)
            break;
        done++;
    }
    return done;
}

static uint32_t fwrite_guest(gaddr src, uint32_t n, gaddr f)
{
    uint32_t done = 0;
    while (done < n) {
        uint8_t chunk[4096];
        uint32_t k = n - done < sizeof chunk ? n - done : (uint32_t)sizeof chunk;
        gget(chunk, src + done, k);
        uint32_t w = sc_fwrite_host(chunk, k, f);
        done += w;
        if (w < k)
            break;
    }
    return done;
}

int32_t sc_fflush(gaddr f)
{
    iob_init();
    if (!f) {
        int32_t r = 0;
        for (gaddr g = iob; g < iob + NFILE * FILE_SIZE; g += FILE_SIZE)
            if (is_open(g) && sc_fflush(g))
                r = MPW_EOF;
        return r;
    }
    if (!is_open(f))
        return MPW_EOF;
    uint16_t fl = flag(f);
    if ((fl & IOWRT) && rd32(f + F_BASE))
        return xflsbuf(f);
    if ((fl & IOREAD) && rd32(f + F_BASE)) {
        /* Input not yet taken given back to the file. */
        if (cnt(f) > 0 && fd_of(f) > 2)
            sc_lseek(fd_of(f), -cnt(f), 1);
        wr32(f + F_CNT, 0);
        wr32(f + F_PTR, rd32(f + F_BASE));
        if (fl & IORW)
            set_flag(f, fl & (uint16_t)~IOREAD);
    }
    return 0;
}

static int32_t sfclose(gaddr f)
{
    if (!is_open(f))
        return MPW_EOF;
    int32_t r = sc_fflush(f);
    if (sc_close(fd_of(f)) < 0)
        r = MPW_EOF;
    if ((flag(f) & IOMYBUF) && rd32(f + F_BASE))
        tb_DisposePtr(rd32(f + F_BASE));
    gmemset(f, 0, FILE_SIZE);
    int i = file_index(f);
    if (i >= 0 && i < NFILE && tmp_name[i]) {
        sc_faccess(tmp_name[i], MPW_F_DELETE, 0);
        tb_DisposePtr(tmp_name[i]);
        tmp_name[i] = 0;
    }
    return r;
}

void sc_stdio_exit(void)
{
    if (!iob)
        return;
    for (gaddr g = iob; g < iob + NFILE * FILE_SIZE; g += FILE_SIZE)
        if (is_open(g))
            sfclose(g);
}

/* _findiop: a free FILE, or 0. */
static gaddr findiop(void)
{
    iob_init();
    for (gaddr g = iob; g < iob + NFILE * FILE_SIZE; g += FILE_SIZE)
        if (!is_open(g))
            return g;
    return 0;
}

/* fopen's mode: open()'s flags and the stream's; false if it isn't one. */
static bool parse_mode(gaddr mode, uint32_t *oflag, uint16_t *fl)
{
    char m[8];
    if (!mode)
        return false;
    sc_cstr(mode, m, sizeof m);
    bool plus = strchr(m + 1, '+') != NULL;
    switch (m[0]) {
    case 'r': *oflag = MPW_O_RDONLY; *fl = IOREAD; break;
    case 'w': *oflag = MPW_O_WRONLY | MPW_O_CREAT | MPW_O_TRUNC; *fl = IOWRT; break;
    case 'a': *oflag = MPW_O_WRONLY | MPW_O_CREAT | MPW_O_APPEND; *fl = IOWRT; break;
    default: return false;
    }
    if (plus) {
        *oflag = (*oflag & ~3u) | MPW_O_RDWR;
        *fl = IORW;
    }
    return true;
}

static gaddr open_stream(gaddr f, int32_t fd, uint16_t fl)
{
    gmemset(f, 0, FILE_SIZE);
    wr16(f + F_FILE, (uint16_t)fd);
    set_flag(f, fl);
    return f;
}

/* fopen, freopen and their FSSpec forms: name is a C path, or an FSSpec
 * when spec. */
static gaddr sfopen(gaddr name, bool spec, gaddr mode, gaddr f)
{
    uint32_t oflag;
    uint16_t fl;
    if (f)
        sfclose(f);
    if (!parse_mode(mode, &oflag, &fl)) {
        sc_set_errno(MPW_EINVAL);
        return 0;
    }
    if (!f && !(f = findiop())) {
        sc_set_errno(MPW_ENFILE);
        return 0;
    }
    int32_t fd = spec ? sc_open_spec(name, oflag) : sc_open(name, oflag);
    if (fd < 0)
        return 0;
    return open_stream(f, fd, fl);
}

static int32_t sfseek(gaddr f, int32_t off, int32_t whence)
{
    if (!is_open(f) || whence < 0 || whence > 2) {
        sc_set_errno(MPW_EINVAL);
        return -1;
    }
    uint16_t fl = flag(f);
    if (fl & IOWRT) {
        if (rd32(f + F_BASE) && xflsbuf(f))
            return -1;
    } else if ((fl & IOREAD) && whence == 1 && cnt(f) > 0) {
        off -= cnt(f);
    }
    wr32(f + F_CNT, 0);
    wr32(f + F_PTR, rd32(f + F_BASE));
    fl &= (uint16_t)~IOEOF;
    if (fl & IORW)
        fl &= (uint16_t)~(IOREAD | IOWRT);
    set_flag(f, fl);
    return sc_lseek(fd_of(f), off, whence) < 0 ? -1 : 0;
}

static int32_t sftell(gaddr f)
{
    if (!is_open(f)) {
        sc_set_errno(MPW_EBADF);
        return -1;
    }
    int32_t pos = sc_lseek(fd_of(f), 0, 1);
    if (pos < 0)
        return -1;
    if ((flag(f) & IOREAD) && cnt(f) > 0)
        pos -= cnt(f);
    else if ((flag(f) & IOWRT) && rd32(f + F_BASE))
        pos += (int32_t)(rd32(f + F_PTR) - rd32(f + F_BASE));
    return pos;
}

/* setvbuf(f, buf, mode, size), before the first read or write. */
static int32_t ssetvbuf(gaddr f, gaddr buf, int32_t mode, uint32_t size)
{
    if (!is_open(f) || (mode != IOFBF && mode != IOLBF && mode != IONBF))
        return -1;
    if ((flag(f) & IOMYBUF) && rd32(f + F_BASE))
        tb_DisposePtr(rd32(f + F_BASE));
    uint16_t fl = flag(f) & (uint16_t)~(IOLBF | IONBF | IOMYBUF);
    wr32(f + F_BASE, 0);
    wr32(f + F_PTR, 0);
    wr32(f + F_CNT, 0);
    if (mode == IONBF) {
        set_flag(f, fl | IONBF);
        findbuf(f);
        return 0;
    }
    if (!size)
        size = BUFSIZE;
    if (size > 0xFFFF)
        size = 0xFFFF;
    if (!buf) {
        buf = tb_NewPtr((int32_t)size, false, false);
        if (!buf)
            return -1;
        fl |= IOMYBUF;
    }
    set_flag(f, fl | (uint16_t)mode);
    wr32(f + F_BASE, buf);
    wr32(f + F_PTR, buf);
    wr32(f + F_END, buf + size);
    wr16(f + F_SIZE, (uint16_t)size);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Formatted output */

/* The arguments: words of a call from first on, or a va_list's. */
typedef struct {
    const ppc_cpu *c;
    gaddr va;
    unsigned w;
} args;

static uint32_t next_word(args *a)
{
    unsigned n = a->w++;
    return a->va ? rd32(a->va + 4 * n) : sc_arg(a->c, n);
}

static uint64_t next_dword(args *a)
{
    uint64_t hi = next_word(a);
    return hi << 32 | next_word(a);
}

static double next_double(args *a)
{
    uint64_t bits = next_dword(a);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

typedef struct {
    char *p;
    size_t n, cap;
} out_buf;

static void emit(out_buf *o, const char *s, size_t n)
{
    if (o->n + n + 1 > o->cap) {
        size_t cap = (o->n + n + 1) * 2;
        char *q = realloc(o->p, cap);
        if (!q)
            return;
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

/* The format with its arguments, into o. */
static void format(gaddr fmt, args *a, out_buf *o)
{
    emit(o, "", 0);
    for (gaddr p = fmt; rd8(p); p++) {
        char ch = (char)rd8(p);
        if (ch != '%') {
            emit(o, &ch, 1);
            continue;
        }
        p++;
        char flags[8] = "";
        size_t nf = 0;
        while (rd8(p) && strchr("-+ #0", (char)rd8(p)) && nf < 6)
            flags[nf++] = (char)rd8(p++);
        int width = -1, prec = -1;
        if (rd8(p) == '*') {
            width = (int32_t)next_word(a);
            if (width < 0) {
                flags[nf++] = '-';
                width = -width;
            }
            p++;
        } else {
            while (rd8(p) >= '0' && rd8(p) <= '9')
                width = (width < 0 ? 0 : width * 10) + (rd8(p++) - '0');
        }
        if (rd8(p) == '.') {
            p++;
            prec = 0;
            if (rd8(p) == '*') {
                prec = (int32_t)next_word(a);
                p++;
            } else {
                while (rd8(p) >= '0' && rd8(p) <= '9')
                    prec = prec * 10 + (rd8(p++) - '0');
            }
        }
        int size = 0; /* -2 hh, -1 h, 1 l, 2 ll, 3 L */
        for (;;) {
            char m = (char)rd8(p);
            if (m == 'h')
                size = size == -1 ? -2 : -1;
            else if (m == 'l')
                size = size == 1 ? 2 : 1;
            else if (m == 'q')
                size = 2;
            else if (m == 'L')
                size = 3;
            else if (m != 'j' && m != 'z' && m != 't')
                break;
            p++;
        }
        char conv = (char)rd8(p);
        if (!conv)
            break;
        char spec[48];
        int sl = snprintf(spec, sizeof spec, "%%%s", flags);
        if (width >= 0)
            sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", width);
        if (prec >= 0)
            sl += snprintf(spec + sl, sizeof spec - (size_t)sl, ".%d", prec);
        char piece[512];
        char *big = NULL;
        piece[0] = 0;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (size == 2)
                v = (long long)next_dword(a);
            else
                v = (int32_t)next_word(a);
            if (size == -1)
                v = (int16_t)v;
            else if (size == -2)
                v = (int8_t)v;
            snprintf(spec + sl, sizeof spec - (size_t)sl, "ll%c", conv);
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = size == 2 ? next_dword(a) : next_word(a);
            if (size == -1)
                v &= 0xFFFF;
            else if (size == -2)
                v &= 0xFF;
            snprintf(spec + sl, sizeof spec - (size_t)sl, "ll%c", conv);
            snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 'c': {
            snprintf(spec + sl, sizeof spec - (size_t)sl, "c");
            snprintf(piece, sizeof piece, spec, (int)(uint8_t)next_word(a));
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "%08X", next_word(a));
            break;
        case 's': case 'P': {
            gaddr s = next_word(a);
            size_t n = 0, cap = prec >= 0 ? (size_t)prec : (size_t)-1;
            const char *src = "(null)";
            char *str = NULL;
            if (s && conv == 'P') { /* a Pascal string */
                n = rd8(s);
                str = malloc(n + 1);
                if (str)
                    gget(str, s + 1, (uint32_t)n);
            } else if (s) {
                while (n < cap && rd8(s + (gaddr)n))
                    n++;
                str = malloc(n + 1);
                if (str)
                    gget(str, s, (uint32_t)n);
            }
            if (str) {
                str[n] = 0;
                src = str;
            }
            snprintf(spec + sl, sizeof spec - (size_t)sl, "s");
            int len = snprintf(NULL, 0, spec, src);
            big = malloc((size_t)len + 1);
            if (big)
                snprintf(big, (size_t)len + 1, spec, src);
            free(str);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = next_double(a);
            if (size == 3) /* MrC's long double: a double-double */
                d += next_double(a);
            snprintf(spec + sl, sizeof spec - (size_t)sl, "%c", conv);
            int len = snprintf(NULL, 0, spec, d);
            big = malloc((size_t)len + 1);
            if (big)
                snprintf(big, (size_t)len + 1, spec, d);
            break;
        }
        case 'n': {
            gaddr dst = next_word(a);
            if (!dst)
                break;
            if (size == -1)
                wr16(dst, (uint16_t)o->n);
            else if (size == -2)
                wr8(dst, (uint8_t)o->n);
            else if (size == 2) {
                wr32(dst, 0);
                wr32(dst + 4, (uint32_t)o->n);
            } else
                wr32(dst, (uint32_t)o->n);
            break;
        }
        case '%':
            strcpy(piece, "%");
            break;
        default:
            emit(o, "%", 1);
            emit(o, &conv, 1);
            break;
        }
        if (big) {
            emit(o, big, strlen(big));
            free(big);
        } else {
            emit(o, piece, strlen(piece));
        }
    }
}

/* Formatted output to a stream: the count, or -1. */
static int32_t print_to(gaddr f, gaddr fmt, args *a)
{
    out_buf o = { 0 };
    format(fmt, a, &o);
    uint32_t w = o.p ? sc_fwrite_host(o.p, (uint32_t)o.n, f) : 0;
    int32_t r = w == o.n ? (int32_t)o.n : -1;
    free(o.p);
    return r;
}

static int32_t print_to_string(gaddr s, gaddr fmt, args *a)
{
    out_buf o = { 0 };
    format(fmt, a, &o);
    if (o.p)
        gput(s, o.p, (uint32_t)o.n + 1);
    int32_t n = (int32_t)o.n;
    free(o.p);
    return n;
}

/* ---------------------------------------------------------------------- */
/* Formatted input */

typedef struct {
    gaddr f;  /* a stream, or */
    gaddr s;  /* a string */
    long count;
} source;

static int src_get(source *s)
{
    int ch;
    if (s->f)
        ch = sgetc(s->f);
    else
        ch = rd8(s->s) ? rd8(s->s++) : MPW_EOF;
    if (ch != MPW_EOF)
        s->count++;
    return ch;
}

static void src_unget(source *s, int ch)
{
    if (ch == MPW_EOF)
        return;
    s->count--;
    if (s->f)
        sungetc(ch, s->f);
    else
        s->s--;
}

static bool is_space(int ch) { return ch == ' ' || (ch >= 9 && ch <= 13); }

static int digit_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 10;
    return 99;
}

/* The scan: the count of conversions stored, or EOF when the input ended
 * before the first. */
static int32_t scan(source *in, gaddr fmt, args *a)
{
    int32_t stored = 0;
    bool any = false;
    for (gaddr p = fmt; rd8(p); p++) {
        int fc = rd8(p);
        if (is_space(fc)) {
            int ch;
            while (is_space(ch = src_get(in)))
                ;
            src_unget(in, ch);
            continue;
        }
        if (fc != '%' || rd8(p + 1) == '%') {
            if (fc == '%')
                p++;
            int ch = src_get(in);
            if (ch != fc) {
                src_unget(in, ch);
                if (ch == MPW_EOF && !any)
                    return MPW_EOF;
                break;
            }
            continue;
        }
        p++;
        bool skip = false;
        if (rd8(p) == '*')
            skip = true, p++;
        int width = 0;
        while (rd8(p) >= '0' && rd8(p) <= '9')
            width = width * 10 + (rd8(p++) - '0');
        int size = 0;
        for (;;) {
            char m = (char)rd8(p);
            if (m == 'h')
                size = size == -1 ? -2 : -1;
            else if (m == 'l')
                size = size == 1 ? 2 : 1;
            else if (m == 'L')
                size = 3;
            else if (m == 'q')
                size = 2;
            else
                break;
            p++;
        }
        char conv = (char)rd8(p);
        if (!conv)
            break;
        if (conv == 'n') {
            if (!skip) {
                gaddr dst = next_word(a);
                if (size == -1)
                    wr16(dst, (uint16_t)in->count);
                else
                    wr32(dst, (uint32_t)in->count);
            }
            continue;
        }
        int ch;
        if (conv != 'c' && conv != '[') {
            while (is_space(ch = src_get(in)))
                ;
            src_unget(in, ch);
        }
        if (conv == 'c' || conv == 's' || conv == '[') {
            bool set[256] = { false };
            if (conv == '[') {
                p++;
                bool negate = rd8(p) == '^';
                if (negate)
                    p++;
                bool first = true;
                while (rd8(p) && (rd8(p) != ']' || first)) {
                    int lo = rd8(p);
                    if (rd8(p + 1) == '-' && rd8(p + 2) && rd8(p + 2) != ']') {
                        for (int k = lo; k <= rd8(p + 2); k++)
                            set[k] = true;
                        p += 2;
                    } else {
                        set[lo] = true;
                    }
                    p++;
                    first = false;
                }
                if (negate)
                    for (int k = 0; k < 256; k++)
                        set[k] = !set[k];
                if (!rd8(p))
                    p--;
            }
            int max = width ? width : conv == 'c' ? 1 : 0x7FFFFFFF;
            gaddr dst = skip ? 0 : next_word(a);
            int n = 0;
            while (n < max) {
                ch = src_get(in);
                if (ch == MPW_EOF)
                    break;
                if ((conv == 's' && is_space(ch)) || (conv == '[' && !set[ch])) {
                    src_unget(in, ch);
                    break;
                }
                if (dst)
                    wr8(dst + (gaddr)n, (uint8_t)ch);
                n++;
            }
            if (!n || (conv == 'c' && n < max)) {
                if (ch == MPW_EOF && !any)
                    return MPW_EOF;
                if (!n)
                    break;
            }
            any = true;
            if (dst && conv != 'c')
                wr8(dst + (gaddr)n, 0);
            if (dst)
                stored++;
            continue;
        }
        /* Numbers: the characters that can be one, then the conversion. */
        char buf[400];
        int n = 0, max = width && width < (int)sizeof buf - 1 ? width : (int)sizeof buf - 1;
        bool floating = strchr("eEfgGaA", conv) != NULL;
        int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : conv == 'i' ? 0 : 10;
        ch = src_get(in);
        if ((ch == '+' || ch == '-') && n < max) {
            buf[n++] = (char)ch;
            ch = src_get(in);
        }
        if (floating) {
            bool dot = false, exp = false;
            int digits = 0;
            while (n < max && ch != MPW_EOF) {
                if (ch >= '0' && ch <= '9') {
                    digits++;
                } else if (ch == '.' && !dot && !exp) {
                    dot = true;
                } else if ((ch == 'e' || ch == 'E') && digits && !exp) {
                    exp = true;
                    buf[n++] = (char)ch;
                    ch = src_get(in);
                    if ((ch == '+' || ch == '-') && n < max) {
                        buf[n++] = (char)ch;
                        ch = src_get(in);
                    }
                    continue;
                } else {
                    break;
                }
                buf[n++] = (char)ch;
                ch = src_get(in);
            }
        } else {
            if (ch == '0' && n < max && (base == 0 || base == 16)) {
                buf[n++] = (char)ch;
                ch = src_get(in);
                if ((ch == 'x' || ch == 'X') && n < max) {
                    buf[n++] = (char)ch;
                    ch = src_get(in);
                    base = 16;
                } else if (base == 0) {
                    base = 8;
                }
            }
            if (base == 0)
                base = 10;
            while (n < max && ch != MPW_EOF && digit_value(ch) < base) {
                buf[n++] = (char)ch;
                ch = src_get(in);
            }
        }
        src_unget(in, ch);
        buf[n] = 0;
        bool digits = false;
        for (int k = 0; k < n; k++)
            if (buf[k] >= '0' && buf[k] <= '9')
                digits = true;
            else if (base == 16 && digit_value(buf[k]) < 16 && buf[k] != 'x' && buf[k] != 'X')
                digits = true;
        if (!digits) {
            if (ch == MPW_EOF && !any)
                return MPW_EOF;
            break;
        }
        any = true;
        if (skip)
            continue;
        gaddr dst = next_word(a);
        if (floating) {
            double d = strtod(buf, NULL);
            if (size == 1) {
                sc_wr_double(dst, d);
            } else if (size == 3) {
                sc_wr_double(dst, d);
                sc_wr_double(dst + 8, 0);
            } else {
                float fv = (float)d;
                uint32_t bits;
                memcpy(&bits, &fv, 4);
                wr32(dst, bits);
            }
        } else {
            unsigned long long v = conv == 'd' || conv == 'i' ? (unsigned long long)strtoll(buf, NULL, base)
                                                            : strtoull(buf, NULL, base);
            if (size == -1)
                wr16(dst, (uint16_t)v);
            else if (size == -2)
                wr8(dst, (uint8_t)v);
            else if (size == 2) {
                wr32(dst, (uint32_t)(v >> 32));
                wr32(dst + 4, (uint32_t)v);
            } else
                wr32(dst, (uint32_t)v);
        }
        stored++;
    }
    return stored;
}

/* ---------------------------------------------------------------------- */
/* Exports */

static gaddr stdin_f(void) { return sc_iob(); }
static gaddr stdout_f(void) { return sc_iob() + FILE_SIZE; }
static gaddr stderr_f(void) { return sc_iob() + 2 * FILE_SIZE; }

static void s_filbuf(ppc_cpu *c) { RETURN(filbuf(R(0))); }
static void s_flsbuf(ppc_cpu *c) { RETURN(flsbuf((uint8_t)R(0), R(1))); }
static void s_findbuf(ppc_cpu *c)
{
    findbuf(R(0));
    RETURN(0);
}
static void s_findiop(ppc_cpu *c) { RETURN(findiop()); }
static void s_wrtchk(ppc_cpu *c) { RETURN(wrtchk(R(0))); }
static void s_xflsbuf(ppc_cpu *c) { RETURN(xflsbuf(R(0))); }
/* _bufsync(f): _cnt no more than what is left in the buffer. */
static void s_bufsync(ppc_cpu *c)
{
    gaddr f = R(0);
    int32_t left = (int32_t)(rd32(f + F_END) - rd32(f + F_PTR));
    if (cnt(f) > left)
        wr32(f + F_CNT, (uint32_t)(left > 0 ? left : 0));
    RETURN_VOID();
}

static void s_fopen(ppc_cpu *c) { RETURN(sfopen(R(0), false, R(1), 0)); }
static void s_freopen(ppc_cpu *c) { RETURN(sfopen(R(0), false, R(1), R(2))); }
static void s_FSp_fopen(ppc_cpu *c) { RETURN(sfopen(R(0), true, R(1), 0)); }
static void s_FSp_freopen(ppc_cpu *c) { RETURN(sfopen(R(0), true, R(1), R(2))); }
static void s_fdopen(ppc_cpu *c)
{
    uint32_t oflag;
    uint16_t fl;
    gaddr f = 0;
    if (!parse_mode(R(1), &oflag, &fl))
        sc_set_errno(MPW_EINVAL);
    else if (!(f = findiop()))
        sc_set_errno(MPW_ENFILE);
    else
        open_stream(f, (int32_t)R(0), fl);
    RETURN(f);
}
static void s_fclose(ppc_cpu *c) { RETURN(sfclose(R(0))); }
static void s_fflush(ppc_cpu *c) { RETURN(sc_fflush(R(0))); }

static void s_fread(ppc_cpu *c)
{
    gaddr dst = R(0), f = R(3);
    uint32_t size = R(1), total = R(1) * R(2), done = 0;
    if (!size || !is_open(f)) {
        RETURN(0);
        return;
    }
    while (done < total) {
        int32_t have = cnt(f);
        if (have > 0) {
            uint32_t k = total - done < (uint32_t)have ? total - done : (uint32_t)have;
            gaddr at = rd32(f + F_PTR);
            gmemcpy(dst + done, at, k);
            wr32(f + F_PTR, at + k);
            wr32(f + F_CNT, (uint32_t)(have - (int32_t)k));
            done += k;
            continue;
        }
        int32_t ch = filbuf(f);
        if (ch == MPW_EOF)
            break;
        wr8(dst + done++, (uint8_t)ch);
    }
    RETURN(done / size);
}

static void s_fwrite(ppc_cpu *c)
{
    uint32_t size = R(1);
    RETURN(size ? fwrite_guest(R(0), size * R(2), R(3)) / size : 0);
}

static void s_fgetc(ppc_cpu *c) { RETURN(is_open(R(0)) ? sgetc(R(0)) : MPW_EOF); }
static void s_getchar(ppc_cpu *c) { RETURN(sgetc(stdin_f())); }
static void s_fputc(ppc_cpu *c) { RETURN(is_open(R(1)) ? sputc((uint8_t)R(0), R(1)) : MPW_EOF); }
static void s_putchar(ppc_cpu *c) { RETURN(sputc((uint8_t)R(0), stdout_f())); }
static void s_ungetc(ppc_cpu *c) { RETURN(sungetc((int32_t)R(0), R(1))); }

static void s_fgets(ppc_cpu *c)
{
    gaddr s = R(0), f = R(2);
    int32_t n = (int32_t)R(1), i = 0;
    if (n <= 0 || !is_open(f)) {
        RETURN(0);
        return;
    }
    while (i < n - 1) {
        int32_t ch = sgetc(f);
        if (ch == MPW_EOF)
            break;
        wr8(s + (gaddr)i++, (uint8_t)ch);
        if (ch == MPW_NEWLINE)
            break;
    }
    if (!i || (flag(f) & IOERR)) {
        RETURN(0);
        return;
    }
    wr8(s + (gaddr)i, 0);
    RETURN(s);
}

static void s_gets(ppc_cpu *c)
{
    gaddr s = R(0), f = stdin_f();
    uint32_t i = 0;
    int32_t ch;
    while ((ch = sgetc(f)) != MPW_EOF && ch != MPW_NEWLINE)
        wr8(s + i++, (uint8_t)ch);
    if ((ch == MPW_EOF && !i) || (flag(f) & IOERR)) {
        RETURN(0);
        return;
    }
    wr8(s + i, 0);
    RETURN(s);
}

static void s_fputs(ppc_cpu *c)
{
    uint32_t n = (uint32_t)sc_strlen(R(0));
    RETURN(is_open(R(1)) && fwrite_guest(R(0), n, R(1)) == n ? 0 : MPW_EOF);
}

static void s_puts(ppc_cpu *c)
{
    gaddr f = stdout_f();
    uint32_t n = (uint32_t)sc_strlen(R(0));
    bool ok = fwrite_guest(R(0), n, f) == n && sputc(MPW_NEWLINE, f) != MPW_EOF;
    RETURN(ok ? 0 : MPW_EOF);
}

/* getw and putw: a word, high byte first. */
static void s_getw(ppc_cpu *c)
{
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        int32_t ch = sgetc(R(0));
        if (ch == MPW_EOF) {
            RETURN(MPW_EOF);
            return;
        }
        w = w << 8 | (uint32_t)ch;
    }
    RETURN(w);
}

static void s_putw(ppc_cpu *c)
{
    uint8_t b[4] = { (uint8_t)(R(0) >> 24), (uint8_t)(R(0) >> 16), (uint8_t)(R(0) >> 8), (uint8_t)R(0) };
    RETURN(sc_fwrite_host(b, 4, R(1)) == 4 ? 0 : MPW_EOF);
}

static void s_fseek(ppc_cpu *c) { RETURN(sfseek(R(0), (int32_t)R(1), (int32_t)R(2))); }
static void s_ftell(ppc_cpu *c) { RETURN(sftell(R(0))); }
static void s_rewind(ppc_cpu *c)
{
    sfseek(R(0), 0, 0);
    set_flag(R(0), flag(R(0)) & (uint16_t)~IOERR);
    RETURN_VOID();
}
static void s_fgetpos(ppc_cpu *c)
{
    int32_t pos = sftell(R(0));
    if (pos >= 0)
        wr32(R(1), (uint32_t)pos);
    RETURN(pos < 0 ? -1 : 0);
}
static void s_fsetpos(ppc_cpu *c) { RETURN(sfseek(R(0), (int32_t)rd32(R(1)), 0)); }

static void s_clearerr(ppc_cpu *c)
{
    set_flag(R(0), flag(R(0)) & (uint16_t)~(IOERR | IOEOF));
    RETURN_VOID();
}
static void s_feof(ppc_cpu *c) { RETURN(flag(R(0)) & IOEOF); }
static void s_ferror(ppc_cpu *c) { RETURN(flag(R(0)) & IOERR); }
static void s_fileno(ppc_cpu *c) { RETURN(fd_of(R(0))); }
/* One task here: the locks are always free. */
static void s_flockfile(ppc_cpu *c) { RETURN_VOID(); }
static void s_ftrylockfile(ppc_cpu *c) { RETURN(0); }

static void s_setvbuf(ppc_cpu *c) { RETURN(ssetvbuf(R(0), R(1), (int32_t)R(2), R(3))); }
static void s_setbuf(ppc_cpu *c)
{
    if (R(1))
        ssetvbuf(R(0), R(1), IOFBF, BUFSIZE);
    else
        ssetvbuf(R(0), 0, IONBF, 0);
    RETURN_VOID();
}

/* tmpnam(s): "tmp." and letters, a name no file has, in the default
 * directory; in s, or a buffer of the library's. */
static gaddr stmpnam(gaddr s)
{
    if (!s) {
        if (!tmpnam_buf)
            tmpnam_buf = tb_NewPtr(16, true, true);
        s = tmpnam_buf;
    }
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    for (int tries = 0; tries < 17576; tries++) {
        uint32_t n = tmp_seq++;
        char name[16];
        snprintf(name, sizeof name, "tmp.%c%c%c%c%06u", 'a' + (int)(n % 26), 'a' + (int)(n / 26 % 26),
                 'a' + (int)(n / 676 % 26), 'a' + (int)(rd32(LM_Ticks) % 26), (unsigned)(rd32(LM_Time) % 1000000));
        gput(s, name, (uint32_t)strlen(name) + 1);
        if (sc_path_spec(0, 0, s, spec) == -43)
            break;
    }
    tb_DisposePtr(spec);
    return s;
}

static void s_tmpnam(ppc_cpu *c) { RETURN(stmpnam(R(0))); }

/* tmpfile(): a new file for update, removed when it is closed. */
static void s_tmpfile(ppc_cpu *c)
{
    gaddr name = tb_NewPtr(16, true, true), mode = tb_NewPtr(4, false, true);
    stmpnam(name);
    gput(mode, "w+", 3);
    gaddr f = sfopen(name, false, mode, 0);
    tb_DisposePtr(mode);
    if (f)
        tmp_name[file_index(f)] = name;
    else
        tb_DisposePtr(name);
    RETURN(f);
}

static void s_remove(ppc_cpu *c) { RETURN(sc_faccess(R(0), MPW_F_DELETE, 0)); }
static void s_rename(ppc_cpu *c) { RETURN(sc_faccess(R(0), MPW_F_RENAME, R(1))); }

/* perror(s): "s: " and errno's message on stderr. */
static void s_perror(ppc_cpu *c)
{
    char line[300];
    char s[200] = "";
    if (R(0))
        sc_cstr(R(0), s, sizeof s);
    int n = snprintf(line, sizeof line, "%s%s%s%c", s, *s ? ": " : "", sc_strerror_text((int32_t)rd32(sc_errno_addr())),
                     MPW_NEWLINE);
    sc_fwrite_host(line, (uint32_t)n, stderr_f());
    RETURN_VOID();
}

static void s_printf(ppc_cpu *c)
{
    args a = { c, 0, 1 };
    RETURN(print_to(stdout_f(), R(0), &a));
}
static void s_fprintf(ppc_cpu *c)
{
    args a = { c, 0, 2 };
    RETURN(print_to(R(0), R(1), &a));
}
static void s_sprintf(ppc_cpu *c)
{
    args a = { c, 0, 2 };
    RETURN(print_to_string(R(0), R(1), &a));
}
static void s_vprintf(ppc_cpu *c)
{
    args a = { c, R(1), 0 };
    RETURN(print_to(stdout_f(), R(0), &a));
}
static void s_vfprintf(ppc_cpu *c)
{
    args a = { c, R(2), 0 };
    RETURN(print_to(R(0), R(1), &a));
}
static void s_vsprintf(ppc_cpu *c)
{
    args a = { c, R(2), 0 };
    RETURN(print_to_string(R(0), R(1), &a));
}
/* _doprnt(format, va_list, f): vfprintf's work. */
static void s_doprnt(ppc_cpu *c)
{
    args a = { c, R(1), 0 };
    RETURN(print_to(R(2), R(0), &a));
}

static void s_scanf(ppc_cpu *c)
{
    source in = { stdin_f(), 0, 0 };
    args a = { c, 0, 1 };
    RETURN(scan(&in, R(0), &a));
}
static void s_fscanf(ppc_cpu *c)
{
    source in = { R(0), 0, 0 };
    args a = { c, 0, 2 };
    RETURN(is_open(R(0)) ? scan(&in, R(1), &a) : MPW_EOF);
}
static void s_sscanf(ppc_cpu *c)
{
    source in = { 0, R(0), 0 };
    args a = { c, 0, 2 };
    RETURN(scan(&in, R(1), &a));
}
/* _doscan(f, format, va_list) */
static void s_doscan(ppc_cpu *c)
{
    source in = { R(0), 0, 0 };
    args a = { c, R(2), 0 };
    RETURN(scan(&in, R(1), &a));
}

const ppc_export sc_stdio_exports[] = {
    { "_filbuf", s_filbuf }, { "_flsbuf", s_flsbuf }, { "_findbuf", s_findbuf }, { "_findiop", s_findiop },
    { "_wrtchk", s_wrtchk }, { "_xflsbuf", s_xflsbuf }, { "_bufsync", s_bufsync },
    { "fopen", s_fopen }, { "freopen", s_freopen }, { "FSp_fopen", s_FSp_fopen }, { "FSp_freopen", s_FSp_freopen },
    { "fdopen", s_fdopen }, { "__fdopen", s_fdopen }, { "fclose", s_fclose }, { "__fclose", s_fclose },
    { "fflush", s_fflush }, { "__fflush", s_fflush }, { "fread", s_fread }, { "fwrite", s_fwrite },
    { "__fwrite", s_fwrite }, { "fgetc", s_fgetc }, { "getc", s_fgetc }, { "getc_unlocked", s_fgetc },
    { "getchar", s_getchar }, { "getchar_unlocked", s_getchar }, { "fputc", s_fputc }, { "putc", s_fputc },
    { "putc_unlocked", s_fputc }, { "putchar", s_putchar }, { "putchar_unlocked", s_putchar },
    { "ungetc", s_ungetc }, { "__ungetc", s_ungetc }, { "fgets", s_fgets }, { "gets", s_gets },
    { "fputs", s_fputs }, { "puts", s_puts }, { "getw", s_getw }, { "putw", s_putw },
    { "fseek", s_fseek }, { "__fseek", s_fseek }, { "ftell", s_ftell }, { "__ftell", s_ftell },
    { "rewind", s_rewind }, { "fgetpos", s_fgetpos }, { "fsetpos", s_fsetpos },
    { "clearerr", s_clearerr }, { "__clearerr", s_clearerr }, { "feof", s_feof }, { "__feof", s_feof },
    { "ferror", s_ferror }, { "__ferror", s_ferror }, { "fileno", s_fileno },
    { "flockfile", s_flockfile }, { "funlockfile", s_flockfile }, { "ftrylockfile", s_ftrylockfile },
    { "setvbuf", s_setvbuf }, { "setbuf", s_setbuf }, { "tmpnam", s_tmpnam }, { "tmpfile", s_tmpfile },
    { "remove", s_remove }, { "rename", s_rename }, { "perror", s_perror },
    { "printf", s_printf }, { "fprintf", s_fprintf }, { "sprintf", s_sprintf }, { "vprintf", s_vprintf },
    { "vfprintf", s_vfprintf }, { "vsprintf", s_vsprintf }, { "_doprnt", s_doprnt },
    { "scanf", s_scanf }, { "fscanf", s_fscanf }, { "sscanf", s_sscanf }, { "_doscan", s_doscan },
};
const unsigned sc_stdio_export_count = sizeof sc_stdio_exports / sizeof sc_stdio_exports[0];
