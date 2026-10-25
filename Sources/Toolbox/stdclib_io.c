/*
 * StdCLib's descriptors: open, read, write, lseek, close and the rest,
 * through MPW's device switch.
 *
 * As MPW had it: _IntEnv's descriptor table holds an IOEntry per open
 * descriptor (flags, error, device, cookie, count, buffer), and each
 * descriptor's device is an entry of _StdDevs (a name and its faccess,
 * close, read, write and ioctl routines).  open() asks each device's
 * faccess in turn whether the name is its own: the console ("dev:console",
 * CONS), the system devices ("dev:null", "dev:stdin" ..., SYST) and, for
 * everything else, files through the File Manager (FSYS).  A program may
 * put its own device in the switch with _addDevHandler; MPW's SIOW does
 * that for its console window.  The console here writes to the log
 * (stdout: and stderr: lines, --log unimpl) and has nothing to read.
 */
#include <stdio.h>
#include <string.h>

#include "stdclib_internal.h"

enum { NFD = 40 };  /* descriptors */
enum { NDEV = 8 };  /* device switch slots: FSYS, CONS, SYST, then free */
enum { NEXITPROCS = 45 };
/* IOEntry */
enum { IOE_FLAGS = 0, IOE_ERROR = 2, IOE_DEVICE = 4, IOE_COOKIE = 8, IOE_COUNT = 12, IOE_BUFFER = 16, IOE_SIZE = 20 };
/* A device: name, then faccess(name, cmd, arg), close(entry), read(entry),
 * write(entry), ioctl(entry, cmd, arg). */
enum { DEV_SIZE = 24 };
enum { OP_FACCESS, OP_CLOSE, OP_READ, OP_WRITE, OP_IOCTL, NOPS };
/* _IntEnv, packed as the MPW Shell's 68k one: 'SH' 0, argc 2, argv 6,
 * envp 10, exit code 14, the exit procedures 0x16, the descriptor table's
 * size (bytes) 0x1A and address 0x1C, the device switch 0x20. */
enum { IE_ARGC = 2, IE_ARGV = 6, IE_ENVP = 10, IE_EXITPROCS = 0x16, IE_TABSIZE = 0x1A, IE_TABLE = 0x1C,
       IE_DEVS = 0x20, IE_SIZE = 0x40 };
/* A file's cookie (shared by its duplicates): uses, refnum. */
enum { CK_USES = 0, CK_REFNUM = 2, CK_SIZE = 8 };
/* FSp_open and the rest pass an FSSpec as the name, after this mark. */
enum { SPEC_MARK = 0x00187F1B };

static gaddr intenv, table, devs, scratch;
/* Our own devices' routines, by slot and operation. */
typedef uint32_t (*dev_fn)(uint32_t, uint32_t, uint32_t);
static gaddr builtin_tv[3][NOPS];
static dev_fn builtin_fn[3][NOPS];

static void env_init(void);

gaddr sc_intenv(void)
{
    env_init();
    return intenv;
}

gaddr sc_stddevs(void)
{
    env_init();
    return devs;
}

static gaddr entry_of(int32_t fd) { return table + (gaddr)fd * IOE_SIZE; }

/* The descriptor's entry, if it is open (else errno EBADF). */
static gaddr open_entry(int32_t fd)
{
    env_init();
    if (fd < 0 || fd >= NFD || !rd16(entry_of(fd) + IOE_FLAGS)) {
        sc_set_errno(MPW_EBADF);
        return 0;
    }
    return entry_of(fd);
}

static int32_t free_fd(int32_t from)
{
    env_init();
    for (int32_t fd = from < 0 ? 0 : from; fd < NFD; fd++)
        if (!rd16(entry_of(fd) + IOE_FLAGS))
            return fd;
    return -1;
}

/* A device routine: ours directly, a program's through Mixed Mode. */
static uint32_t dev_call(gaddr dev, int op, uint32_t a, uint32_t b, uint32_t c)
{
    gaddr tv = dev ? rd32(dev + 4 + 4 * (gaddr)op) : 0;
    if (!tv)
        return op == OP_FACCESS ? (uint32_t)-1 : MPW_EINVAL;
    for (int k = 0; k < 3; k++)
        if (tv == builtin_tv[k][op])
            return builtin_fn[k][op](a, b, c);
    uint32_t args[3] = { a, b, c };
    return sc_call(tv, args, op == OP_FACCESS || op == OP_IOCTL ? 3 : 1);
}

/* A device's answer: 0, or an errno (MPW marks some with 0x40000000). */
static int dev_errno(uint32_t r) { return (int)(r & 0xFFFF); }

/* faccess through the switch: the last slot first, the file system last
 * of all, since it takes any name.  The device that took it, or 0. */
static int32_t dispatch(gaddr path, uint32_t cmd, gaddr arg, gaddr *taken)
{
    env_init();
    for (int slot = NDEV - 1; slot >= 0; slot--) {
        gaddr dev = devs + (gaddr)slot * DEV_SIZE;
        if (!rd32(dev))
            continue;
        uint32_t r = dev_call(dev, OP_FACCESS, path, cmd, arg);
        if (r == (uint32_t)-1)
            continue;
        if (taken)
            *taken = dev;
        return dev_errno(r);
    }
    return MPW_ENOENT;
}

/* ---------------------------------------------------------------------- */
/* Paths */

static gaddr pstring_of(const char *s, size_t n)
{
    gaddr p = tb_NewPtr(256, false, true);
    if (p) {
        if (n > 255)
            n = 255;
        wr8(p, (uint8_t)n);
        gput(p + 1, s, (uint32_t)n);
    }
    return p;
}

/* The directory ID of a folder's spec, or 0. */
static int32_t folder_id(gaddr spec)
{
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true), name = tb_NewPtr(64, false, true);
    if (!pb || !name)
        return 0;
    gmemcpy(name, spec + FSSpec_name_off, 64);
    wr32(pb + DirInfo_ioNamePtr_off, name);
    wr16(pb + DirInfo_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
    wr32(pb + DirInfo_ioDrDirID_off, rd32(spec + FSSpec_parID_off));
    int16_t err = tb_PBGetCatInfo(pb, false);
    int32_t id = !err && (rd8(pb + DirInfo_ioFlAttrib_off) & 0x10) ? (int32_t)rd32(pb + DirInfo_ioDrDirID_off) : 0;
    tb_DisposePtr(pb);
    tb_DisposePtr(name);
    return id;
}

int16_t sc_path_spec(int16_t vref, int32_t dirid, gaddr cpath, gaddr spec)
{
    char path[1024];
    size_t n = cpath ? sc_cstr(cpath, path, sizeof path) : 0;
    if (n >= sizeof path)
        return -37; /* bdNamErr */
    if (n <= 255) {
        gaddr p = pstring_of(path, n);
        int16_t err = tb_FSMakeFSSpec(vref, dirid, p, spec);
        tb_DisposePtr(p);
        return err;
    }
    /* Longer than a Str255: a folder at a time. */
    const char *at = path;
    const char *colon = strchr(path, ':');
    if (colon && colon != path) { /* the volume, "Name:" */
        gaddr p = pstring_of(path, (size_t)(colon - path + 1));
        int16_t err = tb_FSMakeFSSpec(0, 0, p, spec);
        tb_DisposePtr(p);
        if (err)
            return err;
        vref = (int16_t)rd16(spec + FSSpec_vRefNum_off);
        dirid = folder_id(spec);
        at = colon + 1;
    } else if (*at == ':') {
        at++;
    }
    for (;;) {
        const char *end = strchr(at, ':');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len > 63)
            return -37;
        gaddr p = pstring_of(at, len);
        int16_t err = tb_FSMakeFSSpec(vref, dirid, p, spec);
        tb_DisposePtr(p);
        if (!end || !end[1])
            return err;
        if (err)
            return err == -43 ? -120 : err; /* dirNFErr */
        vref = (int16_t)rd16(spec + FSSpec_vRefNum_off);
        dirid = folder_id(spec);
        if (!dirid)
            return -120;
        at = end + 1;
    }
}

int16_t sc_spec_path(gaddr spec, gaddr buf, uint32_t bufsize)
{
    char path[2048];
    size_t len = 0;
    int16_t vref = (int16_t)rd16(spec + FSSpec_vRefNum_off);
    uint8_t leaf[64];
    gget(leaf, spec + FSSpec_name_off, 64);
    bool folder = folder_id(spec) != 0 || !leaf[0];
    /* The leaf, then each folder above it to the volume's root. */
    if (leaf[0] && leaf[0] < 64) {
        memcpy(path + sizeof path - leaf[0], leaf + 1, leaf[0]);
        len = leaf[0];
    }
    if (folder && len < sizeof path) {
        memmove(path + sizeof path - len - 1, path + sizeof path - len, len);
        path[sizeof path - 1] = ':';
        len++;
    }
    int32_t dir = (int32_t)rd32(spec + FSSpec_parID_off);
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true), name = tb_NewPtr(64, false, true);
    int16_t err = pb && name ? 0 : -108;
    while (!err && dir != 1 && dir) {
        gmemset(pb, 0, CInfoPBRec_SIZEOF);
        wr8(name, 0);
        wr32(pb + DirInfo_ioNamePtr_off, name);
        wr16(pb + DirInfo_ioVRefNum_off, (uint16_t)vref);
        wr16(pb + DirInfo_ioFDirIndex_off, (uint16_t)-1);
        wr32(pb + DirInfo_ioDrDirID_off, (uint32_t)dir);
        err = tb_PBGetCatInfo(pb, false);
        if (err)
            break;
        uint8_t n = rd8(name);
        if (len + n + 1 > sizeof path) {
            err = -37;
            break;
        }
        path[sizeof path - len - 1] = ':';
        gget(path + sizeof path - len - 1 - n, name + 1, n);
        len += n + 1u;
        dir = (int32_t)rd32(pb + DirInfo_ioDrParID_off);
    }
    if (pb)
        tb_DisposePtr(pb);
    if (name)
        tb_DisposePtr(name);
    if (err)
        return err;
    if (len + 1 > bufsize)
        return -37;
    gput(buf, path + sizeof path - len, (uint32_t)len);
    wr8(buf + (gaddr)len, 0);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* FSYS: files */

static uint32_t fs_open(gaddr name, gaddr entry)
{
    uint16_t flags = rd16(entry + IOE_FLAGS);
    uint32_t oflag = (flags & ~3u) | ((flags & 3u) - 1);
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err;
    if (rd32(name) == SPEC_MARK) {
        gmemcpy(spec, name + 4, FSSpec_SIZEOF);
        err = rd8(spec + FSSpec_name_off) ? 0 : -43;
        if (!err) {
            gaddr fi = tb_NewPtr(FInfo_SIZEOF, false, true);
            err = tb_FSpGetFInfo(spec, fi);
            if (err && folder_id(spec))
                err = 0;
            tb_DisposePtr(fi);
        }
    } else {
        err = sc_path_spec(0, 0, name, spec);
    }
    if (err == -43 && (oflag & MPW_O_CREAT)) {
        err = tb_FSpCreate(spec, 0x4D505320, 0x54455854, 0); /* 'MPS ', 'TEXT' */
    } else if (!err && (oflag & MPW_O_CREAT) && (oflag & MPW_O_EXCL)) {
        err = -48; /* dupFNErr */
    }
    if (!err && !(oflag & (MPW_O_NRESOLVE | MPW_O_ALIAS))) {
        gaddr flags2 = tb_NewPtr(2, false, true);
        err = tb_ResolveAliasFile(spec, 1, flags2, flags2 + 1);
        tb_DisposePtr(flags2);
    }
    if (!err && folder_id(spec))
        err = -1302; /* notAFileErr: EISDIR below */
    int16_t ref = 0;
    if (!err) {
        uint8_t perm = (oflag & 3) == MPW_O_RDWR ? 3 : (oflag & 3) == MPW_O_WRONLY ? 2 : 1;
        gaddr refp = tb_NewPtr(2, false, true);
        err = (oflag & MPW_O_RSRC) ? tb_FSpOpenRF(spec, (int8_t)perm, refp) : tb_FSpOpenDF(spec, (int8_t)perm, refp);
        ref = (int16_t)rd16(refp);
        tb_DisposePtr(refp);
        if (!err && (oflag & MPW_O_TRUNC)) {
            gaddr pb = tb_NewPtr(HFileParam_SIZEOF, false, true);
            wr16(pb + IOParam_ioRefNum_off, (uint16_t)ref);
            wr32(pb + IOParam_ioMisc_off, 0);
            err = tb_PBSetEOF(pb, false);
            if (err)
                tb_PBClose(pb, false);
            tb_DisposePtr(pb);
        }
    }
    tb_DisposePtr(spec);
    if (err) {
        wr16(entry + IOE_ERROR, (uint16_t)err);
        return err == -1302 ? MPW_EISDIR : (uint32_t)sc_errno_for(err);
    }
    gaddr cookie = tb_NewPtr(CK_SIZE, true, true);
    wr16(cookie + CK_USES, 1);
    wr16(cookie + CK_REFNUM, (uint16_t)ref);
    wr32(entry + IOE_COOKIE, cookie);
    return 0;
}

static uint32_t fs_delete(gaddr name)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err = rd32(name) == SPEC_MARK ? (gmemcpy(spec, name + 4, FSSpec_SIZEOF), 0) : sc_path_spec(0, 0, name, spec);
    if (!err)
        err = tb_FSpDelete(spec);
    tb_DisposePtr(spec);
    if (err)
        sc_uerror(0, err);
    return err ? (uint32_t)sc_errno_for(err) : 0;
}

static uint32_t fs_rename(gaddr name, gaddr newname)
{
    gaddr from = tb_NewPtr(FSSpec_SIZEOF, false, true), to = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err = rd32(name) == SPEC_MARK ? (gmemcpy(from, name + 4, FSSpec_SIZEOF), 0) : sc_path_spec(0, 0, name, from);
    if (!err) {
        err = sc_path_spec(0, 0, newname, to);
        if (!err)
            err = -48; /* the new name is taken */
        else if (err == -43)
            err = 0;
    }
    if (!err && rd16(from) != rd16(to))
        err = -1208; /* diffVolErr: EXDEV below */
    if (!err && rd32(from + FSSpec_parID_off) != rd32(to + FSSpec_parID_off)) {
        /* Into another folder first, then the new name. */
        gaddr folder = tb_NewPtr(FSSpec_SIZEOF, false, true), empty = tb_NewPtr(2, false, true);
        err = tb_FSMakeFSSpec((int16_t)rd16(to), (int32_t)rd32(to + FSSpec_parID_off), empty, folder);
        if (!err)
            err = tb_FSpCatMove(from, folder);
        if (!err)
            wr32(from + FSSpec_parID_off, rd32(to + FSSpec_parID_off));
        tb_DisposePtr(folder);
        tb_DisposePtr(empty);
    }
    if (!err && memcmp(gptr(from + FSSpec_name_off), gptr(to + FSSpec_name_off), rd8(to + FSSpec_name_off) + 1u))
        err = tb_FSpRename(from, to + FSSpec_name_off);
    tb_DisposePtr(from);
    tb_DisposePtr(to);
    if (err)
        sc_uerror(0, err);
    return err == -1208 ? MPW_EXDEV : err ? (uint32_t)sc_errno_for(err) : 0;
}

static uint32_t fs_faccess(uint32_t name, uint32_t cmd, uint32_t arg)
{
    if (!name || (!rd8(name) && rd32(name) != SPEC_MARK))
        return MPW_EINVAL;
    switch (cmd) {
    case MPW_F_OPEN: return fs_open(name, arg);
    case MPW_F_DELETE: return fs_delete(name);
    case MPW_F_RENAME: return fs_rename(name, arg);
    default: return MPW_EINVAL;
    }
}

static int16_t fs_refnum(gaddr entry) { return (int16_t)rd16(rd32(entry + IOE_COOKIE) + CK_REFNUM); }

static gaddr fs_pb(gaddr entry)
{
    gaddr pb = tb_NewPtr(HFileParam_SIZEOF, false, true);
    if (pb)
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)fs_refnum(entry));
    return pb;
}

static uint32_t fs_close(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)b, (void)c;
    gaddr cookie = rd32(entry + IOE_COOKIE);
    if (!cookie)
        return 0;
    int16_t uses = (int16_t)rd16(cookie + CK_USES) - 1;
    wr16(cookie + CK_USES, (uint16_t)uses);
    if (uses > 0)
        return 0;
    gaddr pb = fs_pb(entry);
    int16_t err = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    tb_DisposePtr(cookie);
    wr32(entry + IOE_COOKIE, 0);
    if (err) {
        wr16(entry + IOE_ERROR, (uint16_t)err);
        return MPW_EIO;
    }
    return 0;
}

static uint32_t fs_transfer(gaddr entry, bool write)
{
    gaddr pb = fs_pb(entry);
    if (write && (rd16(entry + IOE_FLAGS) & MPW_O_APPEND)) {
        wr16(pb + IOParam_ioPosMode_off, 2); /* fsFromLEOF */
        wr32(pb + IOParam_ioPosOffset_off, 0);
        tb_PBSetFPos(pb, false);
    }
    uint32_t want = rd32(entry + IOE_COUNT);
    wr32(pb + IOParam_ioBuffer_off, rd32(entry + IOE_BUFFER));
    wr32(pb + IOParam_ioReqCount_off, want);
    wr16(pb + IOParam_ioPosMode_off, 0); /* fsAtMark */
    int16_t err = want ? (write ? tb_PBWrite(pb, false) : tb_PBRead(pb, false)) : 0;
    uint32_t done = rd32(pb + IOParam_ioActCount_off);
    tb_DisposePtr(pb);
    if (err == -39 && !write) /* eofErr: what there was */
        err = 0;
    if (err) {
        wr16(entry + IOE_ERROR, (uint16_t)err);
        return err == -34 ? MPW_ENOSPC : MPW_EIO;
    }
    wr32(entry + IOE_COUNT, want - done);
    wr32(entry + IOE_BUFFER, rd32(entry + IOE_BUFFER) + done);
    return 0;
}

static uint32_t fs_read(uint32_t entry, uint32_t b, uint32_t c) { (void)b, (void)c; return fs_transfer(entry, false); }
static uint32_t fs_write(uint32_t entry, uint32_t b, uint32_t c) { (void)b, (void)c; return fs_transfer(entry, true); }

static uint32_t fs_ioctl(uint32_t entry, uint32_t cmd, uint32_t arg)
{
    wr16(entry + IOE_ERROR, 0);
    switch (cmd) {
    case MPW_FIOLSEEK: {
        /* arg: whence, offset; the new position back in offset.  A
         * position past the end makes the file that long. */
        int32_t whence = (int32_t)rd32(arg), off = (int32_t)rd32(arg + 4);
        gaddr pb = fs_pb(entry);
        int16_t err = tb_PBGetEOF(pb, false);
        int64_t eof = rd32(pb + IOParam_ioMisc_off);
        if (!err)
            err = tb_PBGetFPos(pb, false);
        int64_t mark = rd32(pb + IOParam_ioPosOffset_off);
        int64_t at = whence == 0 ? off : whence == 1 ? mark + off : whence == 2 ? eof + off : -1;
        if (!err && (at < 0 || at > 0x7FFFFFFF)) {
            tb_DisposePtr(pb);
            return MPW_EINVAL;
        }
        if (!err && at > eof) {
            wr32(pb + IOParam_ioMisc_off, (uint32_t)at);
            err = tb_PBSetEOF(pb, false);
        }
        if (!err) {
            wr16(pb + IOParam_ioPosMode_off, 1); /* fsFromStart */
            wr32(pb + IOParam_ioPosOffset_off, (uint32_t)at);
            err = tb_PBSetFPos(pb, false);
        }
        tb_DisposePtr(pb);
        if (err) {
            wr16(entry + IOE_ERROR, (uint16_t)err);
            return MPW_ESPIPE;
        }
        wr32(arg, 0);
        wr32(arg + 4, (uint32_t)at);
        return 0;
    }
    case MPW_FIODUPFD: {
        gaddr cookie = rd32(entry + IOE_COOKIE);
        if (cookie)
            wr16(cookie + CK_USES, rd16(cookie + CK_USES) + 1);
        return 0;
    }
    case MPW_FIOREFNUM:
        wr16(arg, (uint16_t)fs_refnum(entry));
        return 0;
    case MPW_FIOSETEOF: {
        gaddr pb = fs_pb(entry);
        wr32(pb + IOParam_ioMisc_off, arg);
        int16_t err = tb_PBSetEOF(pb, false);
        tb_DisposePtr(pb);
        if (err)
            wr16(entry + IOE_ERROR, (uint16_t)err);
        return err ? MPW_EINVAL : 0;
    }
    default: /* not interactive; the usual buffer size */
        return MPW_EINVAL;
    }
}

/* ---------------------------------------------------------------------- */
/* CONS: the console, to the log */

static struct {
    char text[512];
    size_t n;
} console[2];

static void console_line(int which)
{
    LOGF(LOG_UNIMPL, "%s: %.*s\n", which ? "stderr" : "stdout", (int)console[which].n, console[which].text);
    console[which].n = 0;
}

void sc_console_flush(void)
{
    for (int i = 0; i < 2; i++)
        if (console[i].n)
            console_line(i);
}

static bool name_is(gaddr name, const char *want)
{
    char s[32];
    if (!name || sc_cstr(name, s, sizeof s) >= sizeof s)
        return false;
    return !strcasecmp(s, want);
}

static uint32_t co_faccess(uint32_t name, uint32_t cmd, uint32_t arg)
{
    (void)arg;
    if (!name_is(name, "dev:console"))
        return (uint32_t)-1;
    return cmd == MPW_F_OPEN ? 0 : MPW_EINVAL;
}

static uint32_t co_close(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)entry, (void)b, (void)c;
    return 0;
}

/* Nothing to read: the end of the input. */
static uint32_t co_read(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)entry, (void)b, (void)c;
    return 0;
}

static uint32_t co_write(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)b, (void)c;
    int which = entry == entry_of(2) ? 1 : 0;
    uint32_t n = rd32(entry + IOE_COUNT);
    gaddr p = rd32(entry + IOE_BUFFER);
    for (uint32_t i = 0; i < n; i++) {
        char ch = (char)rd8(p + i);
        if (ch == '\r' || ch == '\n') {
            console_line(which);
            continue;
        }
        if (console[which].n == sizeof console[which].text)
            console_line(which);
        console[which].text[console[which].n++] = ch;
    }
    wr32(entry + IOE_COUNT, 0);
    wr32(entry + IOE_BUFFER, p + n);
    return 0;
}

static uint32_t co_ioctl(uint32_t entry, uint32_t cmd, uint32_t arg)
{
    (void)entry, (void)arg;
    return cmd == MPW_FIODUPFD || cmd == MPW_FIOINTERACTIVE ? 0 : MPW_EINVAL;
}

/* ---------------------------------------------------------------------- */
/* SYST: dev:null, and dev:stdin, dev:stdout and dev:stderr (descriptors 0
 * to 2 again) */

static uint32_t sy_faccess(uint32_t name, uint32_t cmd, uint32_t arg)
{
    static const char *const names[] = { "dev:stdin", "dev:stdout", "dev:stderr" };
    int which = -1;
    if (name_is(name, "dev:null"))
        which = 3;
    for (int i = 0; i < 3; i++)
        if (name_is(name, names[i]))
            which = i;
    if (which < 0)
        return (uint32_t)-1;
    if (cmd != MPW_F_OPEN)
        return MPW_EINVAL;
    if ((rd16(arg + IOE_FLAGS) & (MPW_O_CREAT | MPW_O_EXCL)) == (MPW_O_CREAT | MPW_O_EXCL))
        return MPW_EEXIST;
    if (which == 3)
        return 0;
    /* The standard descriptor again, as dup would make it. */
    gaddr from = entry_of(which);
    uint16_t want = rd16(arg + IOE_FLAGS) & 3, have = rd16(from + IOE_FLAGS) & 3;
    if (!have)
        return MPW_EBADF;
    if ((want & have) != want)
        return MPW_EACCES;
    gaddr dev = rd32(from + IOE_DEVICE);
    wr32(arg + IOE_DEVICE, dev);
    wr32(arg + IOE_COOKIE, rd32(from + IOE_COOKIE));
    return dev_call(dev, OP_IOCTL, from, MPW_FIODUPFD, 0);
}

static uint32_t sy_close(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)entry, (void)b, (void)c;
    return 0;
}

static uint32_t sy_read(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)entry, (void)b, (void)c;
    return 0;
}

static uint32_t sy_write(uint32_t entry, uint32_t b, uint32_t c)
{
    (void)b, (void)c;
    wr32(entry + IOE_BUFFER, rd32(entry + IOE_BUFFER) + rd32(entry + IOE_COUNT));
    wr32(entry + IOE_COUNT, 0);
    return 0;
}

static uint32_t sy_ioctl(uint32_t entry, uint32_t cmd, uint32_t arg)
{
    (void)entry;
    if (cmd == MPW_FIOLSEEK) {
        wr32(arg, 0);
        wr32(arg + 4, 0);
        return 0;
    }
    return cmd == MPW_FIODUPFD ? 0 : MPW_EINVAL;
}

/* The device routines as exports (_fsRead and the rest). */
#define DEVICE_EXPORTS(p)                                                                     \
    static void s_##p##FAccess(ppc_cpu *c) { RETURN(p##_faccess(R(0), R(1), R(2))); }         \
    static void s_##p##Close(ppc_cpu *c) { RETURN(p##_close(R(0), 0, 0)); }                   \
    static void s_##p##Read(ppc_cpu *c) { RETURN(p##_read(R(0), 0, 0)); }                     \
    static void s_##p##Write(ppc_cpu *c) { RETURN(p##_write(R(0), 0, 0)); }                   \
    static void s_##p##Ioctl(ppc_cpu *c) { RETURN(p##_ioctl(R(0), R(1), R(2))); }
DEVICE_EXPORTS(fs)
DEVICE_EXPORTS(co)
DEVICE_EXPORTS(sy)

static void s_coExit(ppc_cpu *c) { RETURN_VOID(); }
/* _coOpen(entry): the console needs nothing to open. */
static void s_coOpen(ppc_cpu *c) { RETURN(0); }

/* ---------------------------------------------------------------------- */
/* The environment */

static void install(int slot, uint32_t name, const dev_fn fns[NOPS], void (*const exports[NOPS])(ppc_cpu *),
                    const char *const names[NOPS])
{
    gaddr dev = devs + (gaddr)slot * DEV_SIZE;
    wr32(dev, name);
    for (int op = 0; op < NOPS; op++) {
        builtin_fn[slot][op] = fns[op];
        builtin_tv[slot][op] = cfm_native_tvector(exports[op], names[op]);
        wr32(dev + 4 + 4 * (gaddr)op, builtin_tv[slot][op]);
    }
}

static void env_init(void)
{
    if (intenv)
        return;
    intenv = tb_NewPtr(IE_SIZE, true, true);
    table = tb_NewPtr(NFD * IOE_SIZE, true, true);
    devs = tb_NewPtr(NDEV * DEV_SIZE, true, true);
    scratch = tb_NewPtr(256, true, true);
    gaddr none = tb_NewPtr(8, true, true); /* an empty argv and envp */
    gaddr procs = tb_NewPtr(NEXITPROCS * 4, true, true);
    wr16(intenv, 0x5348); /* 'SH' */
    wr32(intenv + IE_ARGC, 0);
    wr32(intenv + IE_ARGV, none);
    wr32(intenv + IE_ENVP, none);
    wr32(intenv + IE_EXITPROCS, procs);
    wr16(intenv + IE_TABSIZE, NFD * IOE_SIZE);
    wr32(intenv + IE_TABLE, table);
    wr32(intenv + IE_DEVS, devs);

    static const dev_fn fs[NOPS] = { fs_faccess, fs_close, fs_read, fs_write, fs_ioctl };
    static const dev_fn co[NOPS] = { co_faccess, co_close, co_read, co_write, co_ioctl };
    static const dev_fn sy[NOPS] = { sy_faccess, sy_close, sy_read, sy_write, sy_ioctl };
    static void (*const fs_x[NOPS])(ppc_cpu *) = { s_fsFAccess, s_fsClose, s_fsRead, s_fsWrite, s_fsIoctl };
    static void (*const co_x[NOPS])(ppc_cpu *) = { s_coFAccess, s_coClose, s_coRead, s_coWrite, s_coIoctl };
    static void (*const sy_x[NOPS])(ppc_cpu *) = { s_syFAccess, s_syClose, s_syRead, s_syWrite, s_syIoctl };
    static const char *const fs_n[NOPS] = { "_fsFAccess", "_fsClose", "_fsRead", "_fsWrite", "_fsIoctl" };
    static const char *const co_n[NOPS] = { "_coFAccess", "_coClose", "_coRead", "_coWrite", "_coIoctl" };
    static const char *const sy_n[NOPS] = { "_syFAccess", "_syClose", "_syRead", "_syWrite", "_syIoctl" };
    install(0, 0x46535953, fs, fs_x, fs_n); /* 'FSYS' */
    install(1, 0x434F4E53, co, co_x, co_n); /* 'CONS' */
    install(2, 0x53595354, sy, sy_x, sy_n); /* 'SYST' */

    /* Descriptors 0 to 2: the console, to read, write and write. */
    for (int fd = 0; fd < 3; fd++) {
        wr16(entry_of(fd) + IOE_FLAGS, fd ? 2 : 1);
        wr32(entry_of(fd) + IOE_DEVICE, devs + DEV_SIZE);
    }
}

void sc_io_reset(void)
{
    intenv = table = devs = scratch = 0;
    memset(builtin_tv, 0, sizeof builtin_tv);
    memset(console, 0, sizeof console);
}

/* ---------------------------------------------------------------------- */
/* The calls */

int32_t sc_faccess(gaddr path, uint32_t cmd, gaddr arg)
{
    int e = dispatch(path, cmd, arg, NULL);
    if (e) {
        sc_set_errno(e);
        return -1;
    }
    return 0;
}

int32_t sc_open(gaddr path, uint32_t oflag)
{
    int32_t fd = free_fd(0);
    if (fd < 0) {
        sc_set_errno(MPW_EMFILE);
        return -1;
    }
    gaddr entry = entry_of(fd), dev = 0;
    wr16(entry + IOE_FLAGS, (uint16_t)((oflag & ~3u) | ((oflag & 3u) + 1)));
    wr16(entry + IOE_ERROR, 0);
    wr32(entry + IOE_DEVICE, 0);
    wr32(entry + IOE_COOKIE, 0);
    int e = dispatch(path, MPW_F_OPEN, entry, &dev);
    if (e) {
        wr16(entry + IOE_FLAGS, 0);
        sc_uerror(e, (int16_t)rd16(entry + IOE_ERROR));
        return -1;
    }
    if (!rd32(entry + IOE_DEVICE))
        wr32(entry + IOE_DEVICE, dev);
    return fd;
}

int32_t sc_open_spec(gaddr spec, uint32_t oflag)
{
    gaddr name = tb_NewPtr(4 + FSSpec_SIZEOF, false, true);
    if (!name) {
        sc_set_errno(MPW_ENOMEM);
        return -1;
    }
    wr32(name, SPEC_MARK);
    gmemcpy(name + 4, spec, FSSpec_SIZEOF);
    int32_t fd = sc_open(name, oflag);
    tb_DisposePtr(name);
    return fd;
}

static int32_t transfer(int32_t fd, gaddr buf, uint32_t n, bool write)
{
    gaddr entry = open_entry(fd);
    if (!entry)
        return -1;
    if (!(rd16(entry + IOE_FLAGS) & (write ? 2 : 1))) {
        sc_set_errno(MPW_EBADF);
        return -1;
    }
    wr32(entry + IOE_COUNT, n);
    wr32(entry + IOE_BUFFER, buf);
    wr16(entry + IOE_ERROR, 0);
    uint32_t r = dev_call(rd32(entry + IOE_DEVICE), write ? OP_WRITE : OP_READ, entry, 0, 0);
    if (r) {
        sc_uerror(dev_errno(r), (int16_t)rd16(entry + IOE_ERROR));
        return -1;
    }
    return (int32_t)(n - rd32(entry + IOE_COUNT));
}

int32_t sc_read(int32_t fd, gaddr buf, uint32_t n) { return transfer(fd, buf, n, false); }
int32_t sc_write(int32_t fd, gaddr buf, uint32_t n) { return transfer(fd, buf, n, true); }

int32_t sc_ioctl(int32_t fd, uint32_t cmd, gaddr arg)
{
    gaddr entry = open_entry(fd);
    if (!entry)
        return -1;
    if (cmd == MPW_FIODUPFD) {
        /* A new descriptor, the lowest free from arg on, for the same. */
        int32_t to = free_fd((int32_t)arg);
        if (to < 0) {
            sc_set_errno(MPW_EMFILE);
            return -1;
        }
        uint32_t r = dev_call(rd32(entry + IOE_DEVICE), OP_IOCTL, entry, MPW_FIODUPFD, 0);
        if (r) {
            sc_uerror(dev_errno(r), (int16_t)rd16(entry + IOE_ERROR));
            return -1;
        }
        gmemcpy(entry_of(to), entry, IOE_SIZE);
        return to;
    }
    wr16(entry + IOE_ERROR, 0);
    uint32_t r = dev_call(rd32(entry + IOE_DEVICE), OP_IOCTL, entry, cmd, arg);
    if (r) {
        sc_uerror(dev_errno(r), (int16_t)rd16(entry + IOE_ERROR));
        return -1;
    }
    return 0;
}

int32_t sc_lseek(int32_t fd, int32_t off, int32_t whence)
{
    env_init();
    wr32(scratch, (uint32_t)whence);
    wr32(scratch + 4, (uint32_t)off);
    if (sc_ioctl(fd, MPW_FIOLSEEK, scratch) < 0)
        return -1;
    return (int32_t)rd32(scratch + 4);
}

int32_t sc_close(int32_t fd)
{
    gaddr entry = open_entry(fd);
    if (!entry)
        return -1;
    wr16(entry + IOE_ERROR, 0);
    uint32_t r = dev_call(rd32(entry + IOE_DEVICE), OP_CLOSE, entry, 0, 0);
    wr16(entry + IOE_FLAGS, 0);
    if (r) {
        sc_uerror(dev_errno(r), (int16_t)rd16(entry + IOE_ERROR));
        return -1;
    }
    return 0;
}

void sc_close_all(void)
{
    if (!intenv)
        return;
    for (int32_t fd = 0; fd < NFD; fd++)
        if (rd16(entry_of(fd) + IOE_FLAGS))
            sc_close(fd);
    sc_console_flush();
}

/* ---------------------------------------------------------------------- */
/* Exports */

static void s_open(ppc_cpu *c) { RETURN(sc_open(R(0), R(1))); }
static void s_creat(ppc_cpu *c) { RETURN(sc_open(R(0), MPW_O_WRONLY | MPW_O_CREAT | MPW_O_TRUNC)); }
static void s_read(ppc_cpu *c) { RETURN(sc_read((int32_t)R(0), R(1), R(2))); }
static void s_write(ppc_cpu *c) { RETURN(sc_write((int32_t)R(0), R(1), R(2))); }
static void s_lseek(ppc_cpu *c) { RETURN(sc_lseek((int32_t)R(0), (int32_t)R(1), (int32_t)R(2))); }
static void s_close(ppc_cpu *c) { RETURN(sc_close((int32_t)R(0))); }
static void s_ioctl(ppc_cpu *c) { RETURN(sc_ioctl((int32_t)R(0), R(1), R(2))); }
/* fcntl(fd, cmd, arg): F_DUPFD (0) only. */
static void s_fcntl(ppc_cpu *c)
{
    if (R(1) != 0) {
        sc_set_errno(MPW_EINVAL);
        RETURN(-1);
        return;
    }
    RETURN(sc_ioctl((int32_t)R(0), MPW_FIODUPFD, R(2)));
}
static void s_dup(ppc_cpu *c) { RETURN(sc_ioctl((int32_t)R(0), MPW_FIODUPFD, 0)); }
static void s_faccess(ppc_cpu *c) { RETURN(sc_faccess(R(0), R(1), R(2))); }
static void s_unlink(ppc_cpu *c) { RETURN(sc_faccess(R(0), MPW_F_DELETE, 0)); }

/* access(path, mode): whether it is there (and, for writing, not locked). */
static void s_access(ppc_cpu *c)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    int16_t err = sc_path_spec(0, 0, R(0), spec);
    if (!err && (R(1) & 2) && !folder_id(spec)) {
        gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF, false, true);
        wr32(pb + HFileInfo_ioNamePtr_off, spec + FSSpec_name_off);
        wr16(pb + HFileInfo_ioVRefNum_off, rd16(spec + FSSpec_vRefNum_off));
        wr32(pb + HFileInfo_ioDirID_off, rd32(spec + FSSpec_parID_off));
        err = tb_PBGetCatInfo(pb, false);
        if (!err && (rd8(pb + HFileInfo_ioFlAttrib_off) & 1))
            err = -45; /* fLckdErr */
        tb_DisposePtr(pb);
    }
    tb_DisposePtr(spec);
    if (err) {
        sc_uerror(sc_errno_for(err), err);
        RETURN(-1);
    } else {
        RETURN(0);
    }
}

/* _getIOPort(&fd): the descriptor's IOEntry; -1 asks for a free one, and
 * its number comes back in fd. */
static void s_getIOPort(ppc_cpu *c)
{
    int32_t fd = (int32_t)rd32(R(0));
    if (fd == -1) {
        fd = free_fd(0);
        if (fd < 0) {
            sc_set_errno(MPW_EMFILE);
            RETURN(0);
            return;
        }
        wr32(R(0), (uint32_t)fd);
        RETURN(entry_of(fd));
        return;
    }
    RETURN(open_entry(fd));
}

/* _faccess(&device, name, cmd, arg): faccess through the switch, the
 * device that answered back in device. */
static void s__faccess(ppc_cpu *c)
{
    gaddr dev = 0;
    int e = dispatch(R(1), R(2), R(3), &dev);
    if (R(0))
        wr32(R(0), dev);
    if (e)
        sc_set_errno(e);
    RETURN(e ? -1 : 0);
}

/* _addDevHandler(slot, name, faccess, close, read, write, ioctl): a
 * device in the switch (slot 1 is the console, as MPW's SIOW uses it). */
static void s_addDevHandler(ppc_cpu *c)
{
    int32_t slot = (int32_t)R(0);
    if (slot < 0 || slot >= NDEV) {
        RETURN(-1);
        return;
    }
    env_init();
    gaddr dev = devs + (gaddr)slot * DEV_SIZE;
    for (int i = 0; i < 6; i++)
        wr32(dev + 4 * (gaddr)i, R(1 + i));
    RETURN(0);
}

/* _getDevHandler(slot, &name, &faccess, &close, &read, &write, &ioctl) */
static void s_getDevHandler(ppc_cpu *c)
{
    int32_t slot = (int32_t)R(0);
    if (slot < 0 || slot >= NDEV) {
        RETURN(-1);
        return;
    }
    env_init();
    gaddr dev = devs + (gaddr)slot * DEV_SIZE;
    for (int i = 0; i < 6; i++)
        if (R(1 + i))
            wr32(R(1 + i), rd32(dev + 4 * (gaddr)i));
    RETURN(0);
}

/* _coreIOExit(): every descriptor closed. */
static void s_coreIOExit(ppc_cpu *c)
{
    sc_close_all();
    RETURN_VOID();
}

/* __growFileTable(table, size): a block at least that big (the same one
 * when it already is, or grown); 0 if there is no room. */
static void s_growFileTable(ppc_cpu *c)
{
    gaddr p = R(0);
    int32_t want = (int32_t)R(1);
    if (!p) {
        RETURN(tb_NewPtr(want, false, true));
        return;
    }
    int32_t have = tb_GetPtrSize(p);
    if (want <= have) {
        RETURN(p);
        return;
    }
    tb_SetPtrSize(p, want);
    if (!rd16(LM_MemErr)) {
        gmemset(p + (gaddr)have, 0, (uint32_t)(want - have));
        RETURN(p);
        return;
    }
    gaddr q = tb_NewPtr(want, false, true);
    if (q) {
        gmemcpy(q, p, (uint32_t)have);
        tb_DisposePtr(p);
    }
    RETURN(q);
}

const ppc_export sc_io_exports[] = {
    { "open", s_open }, { "__open", s_open }, { "creat", s_creat }, { "read", s_read }, { "__read", s_read },
    { "write", s_write }, { "__write", s_write }, { "lseek", s_lseek }, { "__lseek", s_lseek },
    { "close", s_close }, { "__close", s_close }, { "ioctl", s_ioctl }, { "__ioctl", s_ioctl },
    { "fcntl", s_fcntl }, { "__fcntl", s_fcntl }, { "dup", s_dup }, { "__dup", s_dup },
    { "faccess", s_faccess }, { "_faccess", s__faccess }, { "unlink", s_unlink }, { "access", s_access },
    { "_getIOPort", s_getIOPort }, { "_addDevHandler", s_addDevHandler }, { "_getDevHandler", s_getDevHandler },
    { "_coreIOExit", s_coreIOExit }, { "__growFileTable", s_growFileTable },
    { "_fsFAccess", s_fsFAccess }, { "_fsClose", s_fsClose }, { "_fsRead", s_fsRead }, { "_fsWrite", s_fsWrite },
    { "_fsIoctl", s_fsIoctl }, { "_coFAccess", s_coFAccess }, { "_coClose", s_coClose }, { "_coRead", s_coRead },
    { "_coWrite", s_coWrite }, { "_coIoctl", s_coIoctl }, { "_coExit", s_coExit }, { "_coOpen", s_coOpen },
    { "_syFAccess", s_syFAccess }, { "_syClose", s_syClose }, { "_syRead", s_syRead }, { "_syWrite", s_syWrite },
    { "_syIoctl", s_syIoctl },
};
const unsigned sc_io_export_count = sizeof sc_io_exports / sizeof sc_io_exports[0];
