/*
 * QuickDraw 3D's data structures kept on the host (q3_data.c).  Nearly
 * every structure a program hands QuickDraw 3D is 32-bit words (floats,
 * longs, enums, object references and pointers), so an object's data is
 * kept as those words in host order, laid out as the program's structure
 * is, with each pointer's array kept beside it the same way.  A schema
 * says which words are object references (a reference is held for each)
 * and which are pointers to arrays (how many elements, of what).
 *
 * From that come _New and _SetData (from the program's structure),
 * _GetData (back out, arrays made in guest memory and references given),
 * _EmptyData (those freed), duplication, disposal, and the accessors of
 * single fields.
 */
#ifndef Q3_DATA_H
#define Q3_DATA_H

#include "q3_obj.h"

typedef struct q3schema q3schema;

enum { QF_OBJ = 1, QF_PTR, QF_BYTES };

typedef struct {
    uint16_t off, kind;
    /* QF_PTR: elements; QF_BYTES: bytes.  self is the structure holding
     * the pointer, parent the one holding that (NULL at the top). */
    uint32_t (*count)(const uint32_t *self, const uint32_t *parent);
    const q3schema *elem;
} q3field;

struct q3schema {
    uint32_t words;
    int nf;
    const q3field *f;
};

typedef struct q3node q3node;
struct q3node {
    uint32_t n, ew;  /* elements, words each */
    uint32_t *w;
    uint8_t *bytes;  /* a QF_BYTES array */
    uint32_t nbytes;
    q3node **kids;   /* n * the schema's pointer fields, in field order */
};

q3node *q3d_from_guest(gaddr p, const q3schema *s, uint32_t n, const uint32_t *parent);
q3node *q3d_new(const q3schema *s, uint32_t n); /* zeroed */
void q3d_free(q3node *nd, const q3schema *s);
q3node *q3d_dup(const q3node *nd, const q3schema *s);
/* Element i's words at dst, its arrays made in guest memory and its
 * references new ones (GetData). */
void q3d_to_guest(gaddr dst, const q3node *nd, const q3schema *s, uint32_t i);
/* What GetData made in the structure at p freed (EmptyData). */
void q3d_empty_guest(gaddr p, const q3schema *s, uint32_t n, const uint32_t *parent);
/* The array behind pointer field `field` (its index in s->f) of element i. */
q3node *q3d_kid(const q3node *nd, const q3schema *s, uint32_t i, int field);
/* Replace that array (taking nd2, which may be NULL). */
void q3d_set_kid(q3node *nd, const q3schema *s, uint32_t i, int field, q3node *nd2);

static inline float q3d_f(const uint32_t *w, int i)
{
    float f;
    memcpy(&f, &w[i], 4);
    return f;
}
static inline void q3d_setf(uint32_t *w, int i, float f) { memcpy(&w[i], &f, 4); }

/* ---- Data objects: an object whose data is a q3node of its class's
 * schema (q3_data_class). ---- */
void q3_data_class(uint32_t type, const q3schema *s, void (*submit)(q3obj *, q3view *), bool drawable);
const q3schema *q3_schema_of(uint32_t type);
/* A new object from the program's structure at p (0: zeroed). */
gaddr q3_data_new(uint32_t type, gaddr p);
/* The object's words (NULL, with an error, if it isn't one of type). */
q3node *q3_data_of(gaddr g, uint32_t type);

/* Native functions for the accessors, one per export. */
#define Q3_GETW(fn, TYPE_, off, n)                                                                                    \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d && Q3R(1))                                                                                             \
            for (int i_ = 0; i_ < (n); i_++)                                                                         \
                wr32(Q3R(1) + 4 * (uint32_t)i_, d->w[(off) + i_]);                                                   \
        Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
#define Q3_SETW(fn, TYPE_, off, n)                                                                                    \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d && Q3R(1)) {                                                                                           \
            for (int i_ = 0; i_ < (n); i_++)                                                                         \
                d->w[(off) + i_] = rd32(Q3R(1) + 4 * (uint32_t)i_);                                                  \
            q3_edited(q3_get(Q3R(0)));                                                                               \
        }                                                                                                            \
        Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
/* A word by value (an enum, a long, a Boolean). */
#define Q3_SETV(fn, TYPE_, off)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d) {                                                                                                     \
            d->w[off] = Q3R(1);                                                                                      \
            q3_edited(q3_get(Q3R(0)));                                                                               \
        }                                                                                                            \
        Q3RETURN(d ? kQ3Success : kQ3Failure);                                                                       \
    }
/* A float by value (in f1). */
#define Q3_SETF(fn, TYPE_, off)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d) {                                                                                                     \
            q3d_setf(d->w, off, q3_farg(c, 0));                                                                      \
            q3_edited(q3_get(Q3R(0)));                                                                               \
        }                                                                                                            \
        Q3RETURN(d ? kQ3Success : kQ3Failure);                                                                       \
    }
/* An object reference: got as a new reference, set as one held. */
#define Q3_GETO(fn, TYPE_, off)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d && Q3R(1))                                                                                             \
            wr32(Q3R(1), d->w[off] ? q3_ref(d->w[off]) : 0);                                                         \
        Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
#define Q3_SETO(fn, TYPE_, off)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d) {                                                                                                     \
            q3_assign(&d->w[off], Q3R(1));                                                                           \
            q3_edited(q3_get(Q3R(0)));                                                                               \
        }                                                                                                            \
        Q3RETURN(d ? kQ3Success : kQ3Failure);                                                                       \
    }
/* The whole structure: _GetData and _SetData. */
#define Q3_GETDATA(fn, TYPE_)                                                                                         \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3node *d = q3_data_of(Q3R(0), TYPE_);                                                                        \
        if (d && Q3R(1))                                                                                             \
            q3d_to_guest(Q3R(1), d, q3_schema_of(TYPE_), 0);                                                          \
        Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);                                                             \
    }
#define Q3_SETDATA(fn, TYPE_)                                                                                         \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        q3obj *o_ = q3_of(Q3R(0), TYPE_);                                                                             \
        q3node *n_ = o_ && Q3R(1) ? q3d_from_guest(Q3R(1), q3_schema_of(o_->type), 1, NULL) : NULL;                  \
        if (n_) {                                                                                                    \
            q3d_free(o_->data, q3_schema_of(o_->type));                                                              \
            o_->data = n_;                                                                                           \
            q3_edited(o_);                                                                                           \
        }                                                                                                            \
        Q3RETURN(n_ ? kQ3Success : kQ3Failure);                                                                      \
    }
#define Q3_NEWDATA(fn, TYPE_)                                                                                         \
    static void fn(ppc_cpu *c) { Q3RETURN(q3_data_new(TYPE_, Q3R(0))); }
#define Q3_EMPTYDATA(fn, TYPE_)                                                                                       \
    static void fn(ppc_cpu *c)                                                                                       \
    {                                                                                                                \
        if (Q3R(0))                                                                                                  \
            q3d_empty_guest(Q3R(0), q3_schema_of(TYPE_), 1, NULL);                                                    \
        Q3RETURN(Q3R(0) ? kQ3Success : kQ3Failure);                                                                  \
    }

#endif
