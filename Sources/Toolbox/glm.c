/*
 * OpenGLMemory, the memory manager Apple's OpenGL for Mac OS 9 shipped
 * beside OpenGLLibrary (glm.h): where the OpenGL engine and a program
 * that asks it get their memory, and the fill and copy routines it uses
 * on rectangles of it.
 *
 * The mode (glmSetMode, before the first allocation) picks the heap:
 * the system heap, the application's, Multiprocessing Services' (here the
 * system heap too: tasks run on the one thread), or the program's own
 * page allocation and free routines (GLM_OVERRIDE_MODE, from glmSetFunc).
 * Each block is a block of its own in that heap (a "page" of at least the
 * page size, in override mode); the blocks are listed here so glmRealloc
 * knows their sizes and glmPageFreeAll can free them.  The fill and copy
 * routines are GLM's own unless the program gave its own.  Errors are
 * kept for glmGetError, which clears them.
 */
#include <stdlib.h>
#include <string.h>

#include "hostgl.h"
#include "tb_internal.h"
#include "tb_protos_gen.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { GLM_OVERRIDE_MODE = 1, GLM_SYSTEM_HEAP_MODE = 2, GLM_APPLICATION_HEAP_MODE = 3, GLM_MULTIPROCESSOR_MODE = 4 };
enum {
    GLM_PAGE_ALLOCATION_FUNC_PTR = 1, GLM_PAGE_FREE_FUNC_PTR, GLM_ZERO_FUNC_PTR, GLM_COPY_FUNC_PTR,
    GLM_SET_UBYTE_FUNC_PTR, GLM_SET_USHORT_FUNC_PTR, GLM_SET_UINT_FUNC_PTR, GLM_SET_DOUBLE_FUNC_PTR,
    GLM_FUNC_COUNT
};
enum { GLM_PAGE_SIZE = 1, GLM_NUMBER_PAGES = 2, GLM_CURRENT_MEMORY = 3, GLM_MAXIMUM_MEMORY = 4 };
enum { GLM_NO_ERROR = 0, GLM_INVALID_ENUM = 1, GLM_INVALID_VALUE = 2, GLM_INVALID_OPERATION = 3, GLM_OUT_OF_MEMORY = 4 };

typedef struct {
    gaddr addr;  /* what the program was given */
    gaddr block; /* the heap block (or page) it is in */
    uint32_t size;
    uint32_t mode;
} glm_block;

static glm_block *blocks;
static size_t nblocks, blocks_cap;
static uint32_t mode = GLM_SYSTEM_HEAP_MODE, page_size = 0x10000, error;
static uint32_t current, maximum;
static gaddr funcs[GLM_FUNC_COUNT];

static void set_error(uint32_t e)
{
    if (!error) /* the first since glmGetError, as OpenGL keeps them */
        error = e;
}

static glm_block *find(gaddr addr)
{
    for (size_t i = 0; i < nblocks; i++)
        if (blocks[i].addr == addr)
            return &blocks[i];
    return NULL;
}

/* A block of n bytes or more, aligned to align (a power of two); 0 if
 * the heap hasn't room. */
static gaddr alloc(uint32_t n, uint32_t align, bool clear)
{
    if (nblocks == blocks_cap) {
        size_t cap = blocks_cap ? blocks_cap * 2 : 64;
        glm_block *grown = realloc(blocks, cap * sizeof *grown);
        if (!grown) {
            set_error(GLM_OUT_OF_MEMORY);
            return 0;
        }
        blocks = grown;
        blocks_cap = cap;
    }
    uint32_t want = n + (align > 8 ? align : 0);
    gaddr block = 0;
    if (mode == GLM_OVERRIDE_MODE && funcs[GLM_PAGE_ALLOCATION_FUNC_PTR]) {
        uint32_t page = want > page_size ? want : page_size;
        block = ppc_call(funcs[GLM_PAGE_ALLOCATION_FUNC_PTR], &page, 1);
        if (block && clear)
            gmemset(block, 0, want);
    } else {
        block = tb_NewPtr((int32_t)want, mode != GLM_APPLICATION_HEAP_MODE, clear);
    }
    if (!block) {
        set_error(GLM_OUT_OF_MEMORY);
        return 0;
    }
    gaddr addr = align > 8 ? (block + align - 1) & ~(align - 1) : block;
    blocks[nblocks++] = (glm_block){ addr, block, n, mode };
    current += n;
    if (current > maximum)
        maximum = current;
    return addr;
}

static void release(glm_block *b)
{
    if (b->mode == GLM_OVERRIDE_MODE && funcs[GLM_PAGE_FREE_FUNC_PTR]) {
        uint32_t arg = b->block;
        ppc_call(funcs[GLM_PAGE_FREE_FUNC_PTR], &arg, 1);
    } else if (b->mode != GLM_OVERRIDE_MODE || !funcs[GLM_PAGE_ALLOCATION_FUNC_PTR]) {
        tb_DisposePtr(b->block);
    }
    current -= b->size;
    *b = blocks[--nblocks];
}

static void free_addr(gaddr addr)
{
    if (!addr)
        return;
    glm_block *b = find(addr);
    if (!b) {
        set_error(GLM_INVALID_VALUE);
        return;
    }
    release(b);
}

/* glmSetMode(GLenum mode): only before anything is allocated. */
static void glm_SetMode(ppc_cpu *c)
{
    uint32_t m = R(0);
    if (m < GLM_OVERRIDE_MODE || m > GLM_MULTIPROCESSOR_MODE)
        set_error(GLM_INVALID_ENUM);
    else if (nblocks)
        set_error(GLM_INVALID_OPERATION);
    else
        mode = m;
    RETURN(0);
}

/* glmSetFunc(GLenum type, GLMfunctions func): the union is one pointer. */
static void glm_SetFunc(ppc_cpu *c)
{
    uint32_t type = R(0);
    if (type < GLM_PAGE_ALLOCATION_FUNC_PTR || type >= GLM_FUNC_COUNT)
        set_error(GLM_INVALID_ENUM);
    else if ((type == GLM_PAGE_ALLOCATION_FUNC_PTR || type == GLM_PAGE_FREE_FUNC_PTR) && nblocks)
        set_error(GLM_INVALID_OPERATION); /* pages already handed out by the old ones */
    else
        funcs[type] = R(1);
    RETURN(0);
}

/* glmSetInteger(GLenum param, GLint value): the page size, a power of two. */
static void glm_SetInteger(ppc_cpu *c)
{
    uint32_t v = R(1);
    if (R(0) != GLM_PAGE_SIZE)
        set_error(GLM_INVALID_ENUM);
    else if (!v || (v & (v - 1)) || v > 0x1000000)
        set_error(GLM_INVALID_VALUE);
    else if (nblocks)
        set_error(GLM_INVALID_OPERATION);
    else
        page_size = v;
    RETURN(0);
}

/* glmGetInteger(GLenum param) */
static void glm_GetInteger(ppc_cpu *c)
{
    uint32_t v = 0;
    switch (R(0)) {
    case GLM_PAGE_SIZE: v = page_size; break;
    case GLM_NUMBER_PAGES:
        for (size_t i = 0; i < nblocks; i++)
            v += (blocks[i].size + page_size - 1) / page_size;
        break;
    case GLM_CURRENT_MEMORY: v = current; break;
    case GLM_MAXIMUM_MEMORY: v = maximum; break;
    default: set_error(GLM_INVALID_ENUM); break;
    }
    RETURN(v);
}

/* glmPageFreeAll(void): everything GLM handed out, at once. */
static void glm_PageFreeAll(ppc_cpu *c)
{
    while (nblocks)
        release(&blocks[nblocks - 1]);
    RETURN(0);
}

static void glm_Malloc(ppc_cpu *c) { RETURN((int32_t)R(0) <= 0 ? 0 : alloc(R(0), 8, false)); }
static void glm_DMAMalloc(ppc_cpu *c) { RETURN((int32_t)R(0) <= 0 ? 0 : alloc(R(0), 32, false)); }
/* AltiVec vectors: 16-byte aligned. */
static void glm_VecAlloc(ppc_cpu *c) { RETURN((int32_t)R(0) <= 0 ? 0 : alloc(R(0), 16, false)); }

/* glmCalloc(GLsizei nmemb, GLsizei size) */
static void glm_Calloc(ppc_cpu *c)
{
    uint64_t n = (uint64_t)R(0) * R(1);
    if ((int32_t)R(0) < 0 || (int32_t)R(1) < 0 || n > 0x7FFFFFFF) {
        set_error(GLM_INVALID_VALUE);
        RETURN(0);
        return;
    }
    RETURN(n ? alloc((uint32_t)n, 8, true) : 0);
}

/* glmRealloc(GLvoid *ptr, GLsizei size): as realloc, the contents kept
 * up to the smaller size; a nil ptr allocates, a size of 0 frees. */
static void glm_Realloc(ppc_cpu *c)
{
    gaddr p = R(0);
    uint32_t n = R(1);
    if (!p) {
        RETURN((int32_t)n <= 0 ? 0 : alloc(n, 8, false));
        return;
    }
    glm_block *b = find(p);
    if (!b) {
        set_error(GLM_INVALID_VALUE);
        RETURN(0);
        return;
    }
    if ((int32_t)n <= 0) {
        release(b);
        RETURN(0);
        return;
    }
    uint32_t old = b->size;
    gaddr q = alloc(n, 8, false);
    if (!q) {
        RETURN(0); /* the old block stays, as realloc leaves it */
        return;
    }
    gmemcpy(q, p, old < n ? old : n);
    free_addr(p); /* found again: alloc may have moved the list */
    RETURN(q);
}

static void glm_Free(ppc_cpu *c)
{
    free_addr(R(0));
    RETURN(0);
}

/* The rectangle routines: width elements in a row, height rows, and skip
 * elements from the end of one row to the start of the next. */
static bool override_fill(ppc_cpu *c, int type, int nargs)
{
    if (!funcs[type])
        return false;
    uint32_t args[6];
    for (int i = 0; i < nargs; i++)
        args[i] = R(i);
    ppc_call(funcs[type], args, nargs);
    return true;
}

/* glmZero(GLubyte *buffer, GLsizei width, GLsizei height, GLsizei skip) */
static void glm_Zero(ppc_cpu *c)
{
    if (!override_fill(c, GLM_ZERO_FUNC_PTR, 4)) {
        gaddr p = R(0);
        int32_t w = (int32_t)R(1), h = (int32_t)R(2), skip = (int32_t)R(3);
        for (int32_t y = 0; w > 0 && y < h; y++, p += (uint32_t)(w + skip))
            gmemset(p, 0, (uint32_t)w);
    }
    RETURN(0);
}

/* glmCopy(const GLubyte *src, GLubyte *dst, GLsizei width, GLsizei height,
 * GLsizei src_skip, GLsizei dst_skip) */
static void glm_Copy(ppc_cpu *c)
{
    if (!override_fill(c, GLM_COPY_FUNC_PTR, 6)) {
        gaddr s = R(0), d = R(1);
        int32_t w = (int32_t)R(2), h = (int32_t)R(3), ss = (int32_t)R(4), ds = (int32_t)R(5);
        for (int32_t y = 0; w > 0 && y < h; y++, s += (uint32_t)(w + ss), d += (uint32_t)(w + ds))
            gmemcpy(d, s, (uint32_t)w);
    }
    RETURN(0);
}

/* glmSetUByte, glmSetUShort, glmSetUInt (buffer, width, height, skip,
 * value): elements of 1, 2 and 4 bytes. */
static void set_elements(ppc_cpu *c, int type, int size)
{
    if (!override_fill(c, type, 5)) {
        gaddr p = R(0);
        int32_t w = (int32_t)R(1), h = (int32_t)R(2), skip = (int32_t)R(3);
        uint32_t v = R(4);
        for (int32_t y = 0; w > 0 && y < h; y++, p += (uint32_t)(skip * size))
            for (int32_t x = 0; x < w; x++, p += (uint32_t)size) {
                if (size == 1)
                    wr8(p, (uint8_t)v);
                else if (size == 2)
                    wr16(p, (uint16_t)v);
                else
                    wr32(p, v);
            }
    }
    RETURN(0);
}
static void glm_SetUByte(ppc_cpu *c) { set_elements(c, GLM_SET_UBYTE_FUNC_PTR, 1); }
static void glm_SetUShort(ppc_cpu *c) { set_elements(c, GLM_SET_USHORT_FUNC_PTR, 2); }
static void glm_SetUInt(ppc_cpu *c) { set_elements(c, GLM_SET_UINT_FUNC_PTR, 4); }

/* glmSetDouble(GLdouble *buffer, GLsizei width, GLsizei height, GLsizei
 * skip, GLdouble value): the value comes in f1 (and stays there for the
 * program's own routine). */
static void glm_SetDouble(ppc_cpu *c)
{
    if (!override_fill(c, GLM_SET_DOUBLE_FUNC_PTR, 4)) {
        gaddr p = R(0);
        int32_t w = (int32_t)R(1), h = (int32_t)R(2), skip = (int32_t)R(3);
        uint64_t v = c->fpr[1];
        for (int32_t y = 0; w > 0 && y < h; y++, p += (uint32_t)skip * 8)
            for (int32_t x = 0; x < w; x++, p += 8) {
                wr32(p, (uint32_t)(v >> 32));
                wr32(p + 4, (uint32_t)v);
            }
    }
    RETURN(0);
}

static void glm_GetError(ppc_cpu *c)
{
    uint32_t e = error;
    error = GLM_NO_ERROR;
    RETURN(e);
}

/* Guest memory is gone (a new program): so are the blocks. */
void glm_reset(void)
{
    free(blocks);
    blocks = NULL;
    nblocks = blocks_cap = 0;
    mode = GLM_SYSTEM_HEAP_MODE;
    page_size = 0x10000;
    error = current = maximum = 0;
    memset(funcs, 0, sizeof funcs);
}

const ppc_export glm_exports[] = {
    { "glmSetMode", glm_SetMode },
    { "glmSetFunc", glm_SetFunc },
    { "glmSetInteger", glm_SetInteger },
    { "glmGetInteger", glm_GetInteger },
    { "glmPageFreeAll", glm_PageFreeAll },
    { "glmMalloc", glm_Malloc },
    { "glmCalloc", glm_Calloc },
    { "glmRealloc", glm_Realloc },
    { "glmFree", glm_Free },
    { "glmVecAlloc", glm_VecAlloc },
    { "glmVecFree", glm_Free },
    { "glmDMAMalloc", glm_DMAMalloc },
    { "glmDMAFree", glm_Free },
    { "glmZero", glm_Zero },
    { "glmCopy", glm_Copy },
    { "glmSetUByte", glm_SetUByte },
    { "glmSetUShort", glm_SetUShort },
    { "glmSetUInt", glm_SetUInt },
    { "glmSetDouble", glm_SetDouble },
    { "glmGetError", glm_GetError },
};
const unsigned glm_export_count = sizeof glm_exports / sizeof glm_exports[0];
