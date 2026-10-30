/*
 * QuickDraw 3D's structures as host words (q3_data.h): copied in from
 * the program, out to it again, duplicated and freed by their schemas.
 */
#include <stdlib.h>

#include "q3_data.h"
#include "tb_internal.h"

enum { MAX_ELEMENTS = 1 << 24 };
/* More than a guest buffer can be (a garbage size). */
enum { MAX_BYTES = 1 << 28 };

static int nptrs(const q3schema *s)
{
    int n = 0;
    for (int i = 0; s && i < s->nf; i++)
        if (s->f[i].kind == QF_PTR || s->f[i].kind == QF_BYTES)
            n++;
    return n;
}

q3node *q3d_new(const q3schema *s, uint32_t n)
{
    q3node *nd = calloc(1, sizeof *nd);
    if (!nd)
        return NULL;
    nd->n = n;
    nd->ew = s->words;
    nd->w = calloc((size_t)n * s->words + 1, 4);
    int np = nptrs(s);
    nd->kids = np ? calloc((size_t)n * (size_t)np + 1, sizeof *nd->kids) : NULL;
    if (!nd->w || (np && !nd->kids)) {
        free(nd->w);
        free(nd->kids);
        free(nd);
        return NULL;
    }
    return nd;
}

static q3node *bytes_from_guest(gaddr p, uint32_t n)
{
    if (n > MAX_BYTES)
        return NULL;
    q3node *nd = calloc(1, sizeof *nd);
    if (!nd)
        return NULL;
    nd->bytes = malloc(n ? n : 1);
    if (!nd->bytes) {
        free(nd);
        return NULL;
    }
    nd->nbytes = n;
    if (n)
        gget(nd->bytes, p, n);
    return nd;
}

q3node *q3d_from_guest(gaddr p, const q3schema *s, uint32_t n, const uint32_t *parent)
{
    if (n > MAX_ELEMENTS)
        return NULL;
    q3node *nd = q3d_new(s, n);
    if (!nd)
        return NULL;
    int np = nptrs(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *w = nd->w + (size_t)i * s->words;
        for (uint32_t k = 0; k < s->words; k++)
            w[k] = rd32(p + (i * s->words + k) * 4);
        int pi = 0;
        for (int f = 0; f < s->nf; f++) {
            const q3field *fd = &s->f[f];
            if (fd->kind == QF_OBJ) {
                /* Held as given; an invalid one is dropped. */
                w[fd->off] = q3_get(w[fd->off]) ? q3_ref(w[fd->off]) : 0;
                continue;
            }
            gaddr arr = w[fd->off];
            q3node *kid = NULL;
            if (arr) {
                uint32_t count = fd->count ? fd->count(w, parent) : 0;
                kid = fd->kind == QF_BYTES ? bytes_from_guest(arr, count) : q3d_from_guest(arr, fd->elem, count, w);
            }
            nd->kids[(size_t)i * (size_t)np + (size_t)pi++] = kid;
            w[fd->off] = kid != NULL;
        }
    }
    return nd;
}

void q3d_free(q3node *nd, const q3schema *s)
{
    if (!nd)
        return;
    int np = nptrs(s);
    for (uint32_t i = 0; i < nd->n && s; i++) {
        int pi = 0;
        for (int f = 0; f < s->nf; f++) {
            const q3field *fd = &s->f[f];
            if (fd->kind == QF_OBJ) {
                gaddr o = nd->w[(size_t)i * s->words + fd->off];
                if (o)
                    q3_release(o);
                continue;
            }
            q3node *kid = nd->kids[(size_t)i * (size_t)np + (size_t)pi++];
            q3d_free(kid, fd->kind == QF_BYTES ? NULL : fd->elem);
        }
    }
    free(nd->w);
    free(nd->bytes);
    free(nd->kids);
    free(nd);
}

q3node *q3d_dup(const q3node *nd, const q3schema *s)
{
    if (!nd)
        return NULL;
    if (nd->bytes) {
        q3node *b = calloc(1, sizeof *b);
        if (b) {
            b->bytes = malloc(nd->nbytes ? nd->nbytes : 1);
            b->nbytes = nd->nbytes;
            if (b->bytes)
                memcpy(b->bytes, nd->bytes, nd->nbytes);
        }
        return b;
    }
    q3node *n = q3d_new(s, nd->n);
    if (!n)
        return NULL;
    memcpy(n->w, nd->w, (size_t)nd->n * s->words * 4);
    int np = nptrs(s);
    for (uint32_t i = 0; i < nd->n; i++) {
        int pi = 0;
        for (int f = 0; f < s->nf; f++) {
            const q3field *fd = &s->f[f];
            if (fd->kind == QF_OBJ) {
                gaddr o = n->w[(size_t)i * s->words + fd->off];
                if (o)
                    q3_ref(o);
                continue;
            }
            size_t k = (size_t)i * (size_t)np + (size_t)pi++;
            n->kids[k] = q3d_dup(nd->kids[k], fd->kind == QF_BYTES ? NULL : fd->elem);
        }
    }
    return n;
}

static gaddr kid_to_guest(const q3node *kid, const q3field *fd)
{
    if (!kid)
        return 0;
    if (fd->kind == QF_BYTES) {
        gaddr b = q3_galloc(kid->nbytes);
        if (b && kid->nbytes)
            gput(b, kid->bytes, kid->nbytes);
        return b;
    }
    gaddr a = q3_galloc(kid->n * fd->elem->words * 4);
    for (uint32_t i = 0; a && i < kid->n; i++)
        q3d_to_guest(a + i * fd->elem->words * 4, kid, fd->elem, i);
    return a;
}

void q3d_to_guest(gaddr dst, const q3node *nd, const q3schema *s, uint32_t i)
{
    const uint32_t *w = nd->w + (size_t)i * s->words;
    for (uint32_t k = 0; k < s->words; k++)
        wr32(dst + k * 4, w[k]);
    int np = nptrs(s), pi = 0;
    for (int f = 0; f < s->nf; f++) {
        const q3field *fd = &s->f[f];
        if (fd->kind == QF_OBJ) {
            if (w[fd->off])
                q3_ref(w[fd->off]);
            continue;
        }
        wr32(dst + fd->off * 4u, kid_to_guest(nd->kids[(size_t)i * (size_t)np + (size_t)pi++], fd));
    }
}

void q3d_empty_guest(gaddr p, const q3schema *s, uint32_t n, const uint32_t *parent)
{
    if (!p || n > MAX_ELEMENTS)
        return;
    uint32_t *w = malloc(s->words * 4 + 4);
    if (!w)
        return;
    for (uint32_t i = 0; i < n; i++) {
        gaddr e = p + i * s->words * 4;
        for (uint32_t k = 0; k < s->words; k++)
            w[k] = rd32(e + k * 4);
        for (int f = 0; f < s->nf; f++) {
            const q3field *fd = &s->f[f];
            if (fd->kind == QF_OBJ) {
                if (w[fd->off])
                    q3_release(w[fd->off]);
                wr32(e + fd->off * 4u, 0);
                continue;
            }
            gaddr arr = w[fd->off];
            if (!arr)
                continue;
            if (fd->kind == QF_PTR)
                q3d_empty_guest(arr, fd->elem, fd->count ? fd->count(w, parent) : 0, w);
            q3_gfree(arr);
            wr32(e + fd->off * 4u, 0);
        }
    }
    free(w);
}

static int ptr_index(const q3schema *s, int field)
{
    int pi = 0;
    for (int f = 0; f < field; f++)
        if (s->f[f].kind == QF_PTR || s->f[f].kind == QF_BYTES)
            pi++;
    return pi;
}

q3node *q3d_kid(const q3node *nd, const q3schema *s, uint32_t i, int field)
{
    if (!nd || i >= nd->n || !nd->kids)
        return NULL;
    return nd->kids[(size_t)i * (size_t)nptrs(s) + (size_t)ptr_index(s, field)];
}

void q3d_set_kid(q3node *nd, const q3schema *s, uint32_t i, int field, q3node *kid)
{
    if (!nd || i >= nd->n || !nd->kids)
        return;
    size_t k = (size_t)i * (size_t)nptrs(s) + (size_t)ptr_index(s, field);
    const q3field *fd = &s->f[field];
    q3d_free(nd->kids[k], fd->kind == QF_BYTES ? NULL : fd->elem);
    nd->kids[k] = kid;
    nd->w[(size_t)i * s->words + fd->off] = kid != NULL;
}

/* ---------------------------------------------------------------------- */
/* Data objects */

typedef struct {
    uint32_t type;
    const q3schema *s;
} schema_entry;

static schema_entry schemas[128];
static int nschemas;

const q3schema *q3_schema_of(uint32_t type)
{
    /* A registered subclass has its parent's data. */
    for (int depth = 0; type && depth < 32; depth++) {
        for (int i = 0; i < nschemas; i++)
            if (schemas[i].type == type)
                return schemas[i].s;
        q3class *k = q3_class(type);
        type = k ? k->parent : 0;
    }
    return NULL;
}

static void data_dispose(q3obj *o)
{
    q3d_free(o->data, q3_schema_of(o->type));
    o->data = NULL;
}

static bool data_duplicate(q3obj *to, const q3obj *from)
{
    to->data = q3d_dup(from->data, q3_schema_of(from->type));
    return to->data != NULL;
}

void q3_data_class(uint32_t type, const q3schema *s, void (*submit)(q3obj *, q3view *), bool drawable)
{
    bool known = false;
    for (int i = 0; i < nschemas; i++)
        known |= schemas[i].type == type;
    if (!known && nschemas < (int)(sizeof schemas / sizeof schemas[0]))
        schemas[nschemas++] = (schema_entry){ type, s };
    q3_class_ops(type, data_dispose, data_duplicate, submit, drawable);
}

gaddr q3_data_new(uint32_t type, gaddr p)
{
    const q3schema *s = q3_schema_of(type);
    if (!s) {
        q3_error(kQ3ErrorInvalidObjectClass);
        return 0;
    }
    q3node *nd = p ? q3d_from_guest(p, s, 1, NULL) : q3d_new(s, 1);
    q3obj *o = nd ? q3_new(type, nd) : NULL;
    if (!o) {
        q3d_free(nd, s);
        return 0;
    }
    return o->g;
}

q3node *q3_data_of(gaddr g, uint32_t type)
{
    q3obj *o = q3_of(g, type);
    return o && q3_schema_of(o->type) ? o->data : NULL;
}
