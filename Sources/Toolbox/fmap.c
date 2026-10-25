/*
 * File mapping (FileMapping.h, _FileMappingDispatch $AA81): a fork opened
 * for mapping, and views of it in memory.
 *
 * Without virtual memory to page from the file, a view is a block of the
 * system heap read from the fork when it is mapped (or moved to another
 * part of the fork), and written back when it is unmapped or moved, if it
 * was mapped for writing.  A scratch file is kept in memory and goes when
 * it is closed.  Forks are opened through the File Manager, by FSRef
 * (hfsplus.c) or FSSpec.
 */
#include <stdlib.h>
#include <string.h>

#include "legacy.h"
#include "tb_internal.h"

enum {
    vmInvalidBackingFileIDErr = -640, vmMappingPrivilegesErr = -641, vmBusyBackingFileErr = -642,
    vmNoMoreBackingFilesErr = -643, vmInvalidFileViewIDErr = -644, vmFileViewAccessErr = -645,
    vmNoMoreFileViewsErr = -646, vmAddressNotInFileViewErr = -647,
};
enum { kCanReadMappedFile = 1, kCanWriteMappedFile = 2, kNoProcessMappedFile = 0x80000000u };
enum { kIsMappedScratchFile = 1 };
enum { accessRead = 1, accessWrite = 2, accessExecute = 4, accessAll = 7 };
enum { MAX_FILES = 16, MAX_VIEWS = 32, ID_FILE = 0x00B10000, ID_VIEW = 0x00B20000, FSREF_SIZE = 80 };

typedef struct {
    bool used;
    int16_t refnum;     /* the open fork; 0 for a scratch file */
    uint8_t *scratch;   /* a scratch file's bytes */
    uint64_t scratch_size;
    uint32_t privileges;
    gaddr ref;          /* a copy of its FSRef, or 0 */
} mapped_file;

typedef struct {
    bool used;
    int file;
    gaddr base;
    uint32_t length, guard, options, access;
    uint64_t offset;
} file_view;

static mapped_file files[MAX_FILES];
static file_view views[MAX_VIEWS];
static gaddr scratch64; /* 8 bytes for the File Manager's 64-bit arguments */

void fmap_reset(void)
{
    for (int i = 0; i < MAX_FILES; i++)
        free(files[i].scratch);
    memset(files, 0, sizeof files);
    memset(views, 0, sizeof views);
    scratch64 = 0;
}

static gaddr wide(uint64_t v)
{
    if (!scratch64)
        scratch64 = tb_NewPtr(8, true, true);
    wr32(scratch64, (uint32_t)(v >> 32));
    wr32(scratch64 + 4, (uint32_t)v);
    return scratch64;
}

static uint64_t rd64(gaddr p) { return p ? (uint64_t)rd32(p) << 32 | rd32(p + 4) : 0; }

static mapped_file *file_of(uint32_t id)
{
    uint32_t i = id - ID_FILE - 1;
    return (id & 0xFFFF0000u) == ID_FILE && i < MAX_FILES && files[i].used ? &files[i] : NULL;
}

static file_view *view_of(uint32_t id)
{
    uint32_t i = id - ID_VIEW - 1;
    return (id & 0xFFFF0000u) == ID_VIEW && i < MAX_VIEWS && views[i].used ? &views[i] : NULL;
}

static uint64_t file_size(mapped_file *f)
{
    if (!f->refnum)
        return f->scratch_size;
    gaddr p = wide(0);
    return tb_FSGetForkSize(f->refnum, p) == noErr ? rd64(p) : 0;
}

/* A new mapped file for the open fork (or a scratch file). */
static int16_t add_file(int16_t refnum, uint32_t privileges, gaddr ref, gaddr out)
{
    int i = 0;
    while (i < MAX_FILES && files[i].used)
        i++;
    if (i == MAX_FILES) {
        if (refnum)
            tb_FSCloseFork(refnum);
        return memFullErr;
    }
    files[i] = (mapped_file){ true, refnum, NULL, 0, privileges, 0 };
    if (ref) {
        files[i].ref = tb_NewPtr(FSREF_SIZE, true, false);
        if (files[i].ref)
            gmemcpy(files[i].ref, ref, FSREF_SIZE);
    }
    if (out)
        wr32(out, ID_FILE + (uint32_t)i + 1);
    return noErr;
}

static bool privileges_ok(uint32_t p)
{
    return (p & kCanReadMappedFile) && !(p & ~(uint32_t)(kCanReadMappedFile | kCanWriteMappedFile | kNoProcessMappedFile));
}

static int8_t permission(uint32_t privileges) { return privileges & kCanWriteMappedFile ? 3 : 1; }

int16_t tb_OpenMappedFile(gaddr ref, uint32_t forkNameLength, gaddr forkName, uint32_t privileges, gaddr backingFile)
{
    if (backingFile)
        wr32(backingFile, 0);
    if (!privileges_ok(privileges))
        return vmMappingPrivilegesErr;
    gaddr rn = wide(0);
    int16_t err = tb_FSOpenFork(ref, forkNameLength, forkName, permission(privileges), rn);
    if (err)
        return err;
    return add_file((int16_t)rd16(rn), privileges, ref, backingFile);
}

int16_t tb_FSpOpenMappedFile(gaddr spec, int8_t mapResFork, uint32_t privileges, gaddr backingFile)
{
    if (backingFile)
        wr32(backingFile, 0);
    if (!privileges_ok(privileges))
        return vmMappingPrivilegesErr;
    gaddr rn = wide(0);
    int16_t err = mapResFork ? tb_FSpOpenRF(spec, permission(privileges), rn)
                             : tb_FSpOpenDF(spec, permission(privileges), rn);
    if (err)
        return err;
    gaddr ref = tb_NewPtr(FSREF_SIZE, false, true);
    bool have_ref = ref && tb_FSpMakeFSRef(spec, ref) == noErr;
    err = add_file((int16_t)rd16(rn), privileges, have_ref ? ref : 0, backingFile);
    if (ref)
        tb_DisposePtr(ref);
    return err;
}

int16_t tb_OpenMappedScratchFile(int16_t volume, uint32_t fileSize, uint32_t privileges, gaddr backingFile)
{
    if (backingFile)
        wr32(backingFile, 0);
    if (!privileges_ok(privileges))
        return vmMappingPrivilegesErr;
    uint8_t *bytes = calloc(1, fileSize ? fileSize : 1);
    if (!bytes)
        return memFullErr;
    int16_t err = add_file(0, privileges, 0, backingFile);
    if (err) {
        free(bytes);
        return err;
    }
    mapped_file *f = file_of(rd32(backingFile));
    f->scratch = bytes;
    f->scratch_size = fileSize;
    return noErr;
}

int16_t tb_CloseMappedFile(uint32_t backingFile)
{
    mapped_file *f = file_of(backingFile);
    if (!f)
        return vmInvalidBackingFileIDErr;
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i].used && &files[views[i].file] == f)
            return vmBusyBackingFileErr;
    int16_t err = f->refnum ? tb_FSCloseFork(f->refnum) : noErr;
    free(f->scratch);
    if (f->ref)
        tb_DisposePtr(f->ref);
    memset(f, 0, sizeof *f);
    return err;
}

/* MappedFileInformation: owningProcess, ref, forkName, privileges,
 * currentSize, attributes. */
int16_t tb_GetMappedFileInformation(uint32_t backingFile, uint32_t version, gaddr info)
{
    mapped_file *f = file_of(backingFile);
    if (!f)
        return vmInvalidBackingFileIDErr;
    if (version != 1 || !info)
        return paramErr;
    gmemset(info, 0, 32);
    tb_GetCurrentProcess(info);
    wr32(info + 8, f->ref);
    wr32(info + 16, f->privileges);
    uint64_t size = file_size(f);
    wr32(info + 20, (uint32_t)(size >> 32));
    wr32(info + 24, (uint32_t)size);
    wr32(info + 28, f->refnum ? 0 : kIsMappedScratchFile);
    return noErr;
}

/* The one after *backingFile (the first after 0). */
int16_t tb_GetNextMappedFile(gaddr backingFile)
{
    if (!backingFile)
        return paramErr;
    uint32_t id = rd32(backingFile);
    uint32_t from = id ? id - ID_FILE : 0;
    if (id && !file_of(id))
        return vmInvalidBackingFileIDErr;
    for (uint32_t i = from; i < MAX_FILES; i++)
        if (files[i].used) {
            wr32(backingFile, ID_FILE + i + 1);
            return noErr;
        }
    return vmNoMoreBackingFilesErr;
}

int16_t tb_SetMappedFileSize(uint32_t backingFile, uint16_t positionMode, gaddr positionOffset)
{
    mapped_file *f = file_of(backingFile);
    if (!f)
        return vmInvalidBackingFileIDErr;
    if (!(f->privileges & kCanWriteMappedFile))
        return vmMappingPrivilegesErr;
    if (f->refnum)
        return tb_FSSetForkSize(f->refnum, positionMode, positionOffset);
    int64_t off = (int64_t)rd64(positionOffset), size;
    switch (positionMode & 3) {
    case 1: size = off; break;                              /* fsFromStart */
    case 2: size = (int64_t)f->scratch_size + off; break;    /* fsFromLEOF */
    default: return paramErr;
    }
    if (size < 0)
        return -40; /* posErr */
    uint8_t *p = realloc(f->scratch, size ? (size_t)size : 1);
    if (!p)
        return memFullErr;
    if ((uint64_t)size > f->scratch_size)
        memset(p + f->scratch_size, 0, (size_t)((uint64_t)size - f->scratch_size));
    f->scratch = p;
    f->scratch_size = (uint64_t)size;
    return noErr;
}

int16_t tb_GetFileViewAccessOptions(gaddr access, gaddr options)
{
    if (access)
        wr32(access, accessAll);
    if (options)
        wr32(options, 0);
    return noErr;
}

/* A view's bytes from the fork (past its end, zeros), or back to it (as
 * far as its end). */
static int16_t transfer(file_view *v, bool write)
{
    mapped_file *f = &files[v->file];
    uint64_t size = file_size(f), n = v->offset >= size ? 0 : size - v->offset;
    if (n > v->length)
        n = v->length;
    if (!write && n < v->length)
        gmemset(v->base + (uint32_t)n, 0, v->length - (uint32_t)n);
    if (!n)
        return noErr;
    if (!f->refnum) {
        if (write)
            gget(f->scratch + v->offset, v->base, (uint32_t)n);
        else
            gput(v->base, f->scratch + v->offset, (uint32_t)n);
        return noErr;
    }
    gaddr off = tb_NewPtr(12, false, true);
    if (!off)
        return memFullErr;
    wr32(off, (uint32_t)(v->offset >> 32));
    wr32(off + 4, (uint32_t)v->offset);
    int16_t err = write ? tb_FSWriteFork(f->refnum, 1, off, (uint32_t)n, v->base, off + 8)
                        : tb_FSReadFork(f->refnum, 1, off, (uint32_t)n, v->base, off + 8);
    tb_DisposePtr(off);
    return err == -39 ? noErr : err; /* eofErr: the rest is zeros */
}

int16_t tb_MapFileView(uint32_t backingFile, gaddr backingBase, uint32_t backingLength, uint32_t access,
                       uint32_t guardLength, uint32_t options, gaddr viewBase, gaddr viewLength, gaddr theView)
{
    mapped_file *f = file_of(backingFile);
    if (!f)
        return vmInvalidBackingFileIDErr;
    if ((access & accessWrite) && !(f->privileges & kCanWriteMappedFile))
        return vmFileViewAccessErr;
    if (access & ~(uint32_t)accessAll)
        return vmFileViewAccessErr;
    uint64_t base = rd64(backingBase), size = file_size(f);
    if (backingLength == 0xFFFFFFFFu) /* kMapEntireFork */
        backingLength = base >= size ? 0 : (uint32_t)(size - base);
    if (!backingLength)
        return paramErr;
    int i = 0;
    while (i < MAX_VIEWS && views[i].used)
        i++;
    if (i == MAX_VIEWS)
        return memFullErr;
    gaddr mem = tb_NewPtr((int32_t)backingLength, true, true);
    if (!mem)
        return memFullErr;
    views[i] = (file_view){ true, (int)(f - files), mem, backingLength, guardLength, options, access, base };
    int16_t err = transfer(&views[i], false);
    if (err) {
        tb_DisposePtr(mem);
        views[i].used = false;
        return err;
    }
    if (viewBase)
        wr32(viewBase, mem);
    if (viewLength)
        wr32(viewLength, backingLength);
    if (theView)
        wr32(theView, ID_VIEW + (uint32_t)i + 1);
    return noErr;
}

int16_t tb_UnmapFileView(uint32_t theView)
{
    file_view *v = view_of(theView);
    if (!v)
        return vmInvalidFileViewIDErr;
    int16_t err = v->access & accessWrite ? transfer(v, true) : noErr;
    tb_DisposePtr(v->base);
    v->used = false;
    return err;
}

/* FileViewInformation: owningProcess, viewBase, viewLength, backingFile,
 * backingBase, access, guardLength, options. */
int16_t tb_GetFileViewInformation(uint32_t theView, uint32_t version, gaddr info)
{
    file_view *v = view_of(theView);
    if (!v)
        return vmInvalidFileViewIDErr;
    if (version != 1 || !info)
        return paramErr;
    gmemset(info, 0, 40);
    tb_GetCurrentProcess(info);
    wr32(info + 8, v->base);
    wr32(info + 12, v->length);
    wr32(info + 16, ID_FILE + (uint32_t)v->file + 1);
    wr32(info + 20, (uint32_t)(v->offset >> 32));
    wr32(info + 24, (uint32_t)v->offset);
    wr32(info + 28, v->access);
    wr32(info + 32, v->guard);
    wr32(info + 36, v->options);
    return noErr;
}

int16_t tb_GetFileViewFromAddress(gaddr address, gaddr theView)
{
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i].used && address >= views[i].base && address - views[i].base < views[i].length) {
            if (theView)
                wr32(theView, ID_VIEW + (uint32_t)i + 1);
            return noErr;
        }
    return vmAddressNotInFileViewErr;
}

int16_t tb_GetNextFileView(gaddr theView)
{
    if (!theView)
        return paramErr;
    uint32_t id = rd32(theView);
    if (id && !view_of(id))
        return vmInvalidFileViewIDErr;
    for (uint32_t i = id ? id - ID_VIEW : 0; i < MAX_VIEWS; i++)
        if (views[i].used) {
            wr32(theView, ID_VIEW + i + 1);
            return noErr;
        }
    return vmNoMoreFileViewsErr;
}

int16_t tb_SetFileViewAccess(uint32_t theView, uint32_t access)
{
    file_view *v = view_of(theView);
    if (!v)
        return vmInvalidFileViewIDErr;
    if ((access & ~(uint32_t)accessAll) ||
        ((access & accessWrite) && !(files[v->file].privileges & kCanWriteMappedFile)))
        return vmFileViewAccessErr;
    /* what was written so far goes back before writing is taken away */
    int16_t err = (v->access & accessWrite) && !(access & accessWrite) ? transfer(v, true) : noErr;
    v->access = access;
    return err;
}

int16_t tb_SetFileViewBackingBase(uint32_t theView, gaddr newBackingBase)
{
    file_view *v = view_of(theView);
    if (!v)
        return vmInvalidFileViewIDErr;
    int16_t err = v->access & accessWrite ? transfer(v, true) : noErr;
    if (err)
        return err;
    v->offset = rd64(newBackingBase);
    return transfer(v, false);
}
