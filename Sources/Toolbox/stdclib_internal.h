/*
 * StdCLib, MPW's C library for PowerPC programs: what its parts share
 * (stdclib.c, stdclib_io.c, stdclib_stdio.c).  Not a public API.
 *
 * MPW's library keeps its state where programs can see it: errno, the FILE
 * structs in _iob that the getc and putc macros work on, the descriptor
 * table and the device switch (_StdDevs) in _IntEnv.  Those are in guest
 * memory here too, laid out as MPW laid them out.
 */
#ifndef STDCLIB_INTERNAL_H
#define STDCLIB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define F(n) ppc_fpr(c, 1 + (n))
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define RETURN_F(v) do { ppc_set_fpr(c, 1, (v)); c->pc = c->lr; } while (0)
#define RETURN_VOID() do { c->pc = c->lr; } while (0)
/* A long long result: high word in r3, low in r4. */
#define RETURN_64(v) do { uint64_t v_ = (uint64_t)(v); c->gpr[3] = (uint32_t)(v_ >> 32); c->gpr[4] = (uint32_t)v_; c->pc = c->lr; } while (0)

/* MPW's errno values (errno.h): Unix's numbers. */
enum {
    MPW_EPERM = 1, MPW_ENOENT = 2, MPW_EIO = 5, MPW_EBADF = 9, MPW_ENOMEM = 12, MPW_EACCES = 13,
    MPW_EEXIST = 17, MPW_EXDEV = 18, MPW_ENOTDIR = 20, MPW_EISDIR = 21, MPW_EINVAL = 22, MPW_ENFILE = 23,
    MPW_EMFILE = 24, MPW_ENOTTY = 25, MPW_EFBIG = 27, MPW_ENOSPC = 28, MPW_ESPIPE = 29, MPW_EROFS = 30,
    MPW_EDOM = 33, MPW_ERANGE = 34,
};

/* open()'s flags (fcntl.h). */
enum {
    MPW_O_RDONLY = 0x0, MPW_O_WRONLY = 0x1, MPW_O_RDWR = 0x2, MPW_O_APPEND = 0x8, MPW_O_RSRC = 0x10,
    MPW_O_ALIAS = 0x20, MPW_O_CREAT = 0x100, MPW_O_TRUNC = 0x200, MPW_O_EXCL = 0x400, MPW_O_BINARY = 0x800,
    MPW_O_NRESOLVE = 0x4000,
};

/* faccess() and ioctl() requests (fcntl.h, ioctl.h). */
enum {
    MPW_F_OPEN = 0x6400, MPW_F_DELETE = 0x6401, MPW_F_RENAME = 0x6402,
    MPW_FIOLSEEK = 0x6600, MPW_FIODUPFD = 0x6601, MPW_FIOINTERACTIVE = 0x6602, MPW_FIOBUFSIZE = 0x6603,
    MPW_FIOFNAME = 0x6604, MPW_FIOREFNUM = 0x6605, MPW_FIOSETEOF = 0x6606,
};

/* MPW's '\n' is a carriage return (its C compilers make it 13). */
enum { MPW_NEWLINE = 13 };

/* Argument word n of a call (GPR slots, then the parameter area). */
static inline uint32_t sc_arg(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}

static inline double sc_rd_double(gaddr p)
{
    uint64_t bits = (uint64_t)rd32(p) << 32 | rd32(p + 4);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static inline void sc_wr_double(gaddr p, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    wr32(p, (uint32_t)(bits >> 32));
    wr32(p + 4, (uint32_t)bits);
}

/* stdclib.c */
size_t sc_strlen(gaddr s);
/* A guest C string copied to the host (cut to fit), its full length. */
size_t sc_cstr(gaddr s, char *out, size_t outsize);
/* errno and MacOSErr: the library's last error, and the Mac OS error
 * behind it. */
gaddr sc_errno_addr(void);
void sc_set_errno(int e);
void sc_uerror(int e, int16_t oserr);
int sc_errno_for(int16_t oserr);
/* A program's PowerPC routine called from the library. */
uint32_t sc_call(gaddr tvector, const uint32_t *args, int nargs);
/* exit()'s work (stdclib.c): the atexit procedures, then the files. */
void sc_run_exit_procs(void);
/* errno's message (strerror, perror). */
const char *sc_strerror_text(int e);

/* stdclib_io.c: descriptors, through the device switch */
gaddr sc_intenv(void);
gaddr sc_stddevs(void);
int32_t sc_open(gaddr path, uint32_t oflag);
int32_t sc_open_spec(gaddr spec, uint32_t oflag);
int32_t sc_read(int32_t fd, gaddr buf, uint32_t n);
int32_t sc_write(int32_t fd, gaddr buf, uint32_t n);
int32_t sc_lseek(int32_t fd, int32_t off, int32_t whence);
int32_t sc_close(int32_t fd);
int32_t sc_ioctl(int32_t fd, uint32_t cmd, gaddr arg);
int32_t sc_faccess(gaddr path, uint32_t cmd, gaddr arg);
void sc_close_all(void);
/* The console's partial lines to the log. */
void sc_console_flush(void);
/* A C path (any length, relative to the default directory) as an FSSpec;
 * fnfErr leaves a good spec for a file that isn't there. */
int16_t sc_path_spec(int16_t vref, int32_t dirid, gaddr cpath, gaddr spec);
/* An FSSpec's full path as a C string; the error, or bufsize too small. */
int16_t sc_spec_path(gaddr spec, gaddr buf, uint32_t bufsize);
void sc_io_reset(void);
extern const ppc_export sc_io_exports[];
extern const unsigned sc_io_export_count;

/* stdclib_stdio.c: FILE streams */
gaddr sc_iob(void);
gaddr sc_lastbuf(void);
int32_t sc_fflush(gaddr f);
/* Every stream flushed and closed, as at exit. */
void sc_stdio_exit(void);
/* Host bytes written to a stream; the count written. */
uint32_t sc_fwrite_host(const void *p, uint32_t n, gaddr f);
void sc_stdio_reset(void);
extern const ppc_export sc_stdio_exports[];
extern const unsigned sc_stdio_export_count;

#endif
