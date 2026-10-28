/*
 * QT atom containers (QuickTime 2.1 and later) and user data.
 *
 * An atom container is a handle holding a tree of atoms: a 12-byte header
 * (10 reserved bytes and a lock count), then the root atom 'sean', ID 1.
 * An atom's header is its size, type and ID, 2 reserved bytes, the count of
 * its children and 4 reserved bytes; a leaf holds its data after the
 * header instead of children.  A QTAtom is an atom's offset in the handle,
 * so it lasts until the container changes, as QuickTime's did; 0
 * (kParentAtomIsContainer) is the root.  Sprite samples, wired actions,
 * media properties, effect parameters and component settings are all
 * kept this way, and the calls work on the handle's bytes in place.
 *
 * User data is items of types (each its own bytes), kept with movies,
 * tracks and media and saved as their 'udta' atoms; a text item ('©nam'
 * and the like) is a run of entries, each a length, a language and text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qt_atoms.h"
#include "tb_internal.h"

enum {
    userDataItemNotFound = -2026,
    cannotFindAtomErr = -2101, notLeafAtomErr = -2102, atomsNotOfSameTypeErr = -2103,
    atomIndexInvalidErr = -2104, duplicateAtomTypeAndIDErr = -2105, invalidAtomErr = -2106,
    invalidAtomContainerErr = -2107, invalidAtomTypeErr = -2108, cannotBeLeafAtomErr = -2109,
};

#define SEAN 0x7365616Eu /* 'sean' */

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* ---- Reading a container's bytes ---- */

static bool valid_root(const uint8_t *c, size_t len)
{
    return len >= QTA_HEADER + QTA_ATOM && get32(c + QTA_HEADER + 4) == SEAN && get32(c + QTA_HEADER) >= QTA_ATOM &&
           QTA_HEADER + (size_t)get32(c + QTA_HEADER) <= len;
}

uint32_t qta_root(const uint8_t *c, size_t len) { return valid_root(c, len) ? QTA_HEADER : 0; }

static uint32_t atom_size(const uint8_t *c, uint32_t a) { return get32(c + a); }
static uint16_t child_count(const uint8_t *c, uint32_t a) { return get16(c + a + 14); }
uint32_t qta_type(const uint8_t *c, uint32_t a) { return get32(c + a + 4); }
uint32_t qta_id(const uint8_t *c, uint32_t a) { return get32(c + a + 8); }

/* The atom a parent argument names (0: the root). */
static uint32_t parent_atom(uint32_t parent) { return parent ? parent : QTA_HEADER; }

/* The first child of a, or 0; and the one after child (within a). */
static uint32_t first_child(const uint8_t *c, uint32_t a)
{
    return child_count(c, a) && atom_size(c, a) > QTA_ATOM ? a + QTA_ATOM : 0;
}

static uint32_t next_child(const uint8_t *c, uint32_t a, uint32_t child)
{
    uint32_t n = atom_size(c, child), end = a + atom_size(c, a);
    if (n < QTA_ATOM || child + n + QTA_ATOM > end)
        return 0;
    return child + n;
}

/* Is atom a somewhere in the tree under from (from included)?  The chain
 * from the root to it, when asked for. */
static bool locate(const uint8_t *c, uint32_t from, uint32_t a, uint32_t *chain, int *depth, int max)
{
    if (*depth >= max)
        return false;
    chain[(*depth)++] = from;
    if (from == a)
        return true;
    for (uint32_t k = first_child(c, from); k; k = next_child(c, from, k))
        if (k <= a && a < k + atom_size(c, k) && locate(c, k, a, chain, depth, max))
            return true;
    (*depth)--;
    return false;
}

static bool is_atom(const uint8_t *c, size_t len, uint32_t a)
{
    if (!valid_root(c, len) || a < QTA_HEADER || a + QTA_ATOM > len)
        return false;
    uint32_t chain[64];
    int depth = 0;
    return locate(c, QTA_HEADER, a, chain, &depth, 64);
}

uint32_t qta_find(const uint8_t *c, size_t len, uint32_t parent, uint32_t type, uint32_t id)
{
    uint32_t p = parent_atom(parent);
    if (!is_atom(c, len, p))
        return 0;
    for (uint32_t k = first_child(c, p); k; k = next_child(c, p, k))
        if (qta_type(c, k) == type && qta_id(c, k) == id)
            return k;
    return 0;
}

uint32_t qta_index(const uint8_t *c, size_t len, uint32_t parent, uint32_t type, int index, uint32_t *id)
{
    uint32_t p = parent_atom(parent);
    if (!is_atom(c, len, p) || index < 1)
        return 0;
    int n = 0;
    for (uint32_t k = first_child(c, p); k; k = next_child(c, p, k))
        if ((!type || qta_type(c, k) == type) && ++n == index) {
            if (id)
                *id = qta_id(c, k);
            return k;
        }
    return 0;
}

int qta_count(const uint8_t *c, size_t len, uint32_t parent, uint32_t type)
{
    uint32_t p = parent_atom(parent);
    if (!is_atom(c, len, p))
        return 0;
    int n = 0;
    for (uint32_t k = first_child(c, p); k; k = next_child(c, p, k))
        n += !type || qta_type(c, k) == type;
    return n;
}

const uint8_t *qta_data(const uint8_t *c, size_t len, uint32_t a, uint32_t *n)
{
    if (!is_atom(c, len, a) || child_count(c, a))
        return NULL;
    *n = atom_size(c, a) - QTA_ATOM;
    return c + a + QTA_ATOM;
}

/* ---- Containers in handles ---- */

typedef struct {
    gaddr h;
    uint8_t *b;
    size_t len;
} buf;

static void fresh(uint8_t *b)
{
    memset(b, 0, QTA_HEADER + QTA_ATOM);
    put32(b + QTA_HEADER, QTA_ATOM);
    put32(b + QTA_HEADER + 4, SEAN);
    put32(b + QTA_HEADER + 8, 1);
}

/* The container's bytes (an empty handle is taken as an empty container). */
static int16_t load(gaddr h, buf *out)
{
    out->h = h;
    out->b = NULL;
    if (!h || !rd32(h))
        return invalidAtomContainerErr;
    int32_t n = tb_GetHandleSize(h);
    if (n == 0) {
        out->len = QTA_HEADER + QTA_ATOM;
        out->b = malloc(out->len);
        fresh(out->b);
        return noErr;
    }
    out->len = (size_t)n;
    out->b = malloc(out->len);
    gget(out->b, rd32(h), (uint32_t)n);
    if (!valid_root(out->b, out->len)) {
        free(out->b);
        out->b = NULL;
        return invalidAtomContainerErr;
    }
    return noErr;
}

static int16_t store(buf *b)
{
    int16_t err = noErr;
    if ((size_t)tb_GetHandleSize(b->h) != b->len) {
        tb_SetHandleSize(b->h, (int32_t)b->len);
        err = (int16_t)rd16(LM_MemErr);
    }
    if (!err && rd32(b->h))
        gput(rd32(b->h), b->b, (uint32_t)b->len);
    free(b->b);
    b->b = NULL;
    return err;
}

static void drop(buf *b)
{
    free(b->b);
    b->b = NULL;
}

/* Grow (n > 0) or shrink the bytes at off, the atoms in chain (root first)
 * growing with them. */
static void resize_at(buf *b, uint32_t off, int64_t n, const uint32_t *chain, int depth)
{
    if (n > 0) {
        b->b = realloc(b->b, b->len + (size_t)n);
        memmove(b->b + off + n, b->b + off, b->len - off);
        memset(b->b + off, 0, (size_t)n);
    } else if (n < 0) {
        memmove(b->b + off, b->b + off - n, b->len - off + (size_t)n);
    }
    b->len = (size_t)((int64_t)b->len + n);
    for (int i = 0; i < depth; i++)
        put32(b->b + chain[i], (uint32_t)((int64_t)atom_size(b->b, chain[i]) + n));
}

static bool chain_of(const buf *b, uint32_t a, uint32_t *chain, int *depth)
{
    *depth = 0;
    return a >= QTA_HEADER && a + QTA_ATOM <= b->len && locate(b->b, QTA_HEADER, a, chain, depth, 64);
}

static int16_t insert_child(buf *b, uint32_t parent, uint32_t type, uint32_t id, int16_t index, const uint8_t *data,
                            uint32_t len, const uint8_t *tree, uint32_t *out)
{
    uint32_t p = parent_atom(parent), chain[64];
    int depth;
    if (!chain_of(b, p, chain, &depth))
        return invalidAtomErr;
    if (!child_count(b->b, p) && atom_size(b->b, p) > QTA_ATOM)
        return cannotBeLeafAtomErr; /* it holds data */
    if (!type)
        return invalidAtomTypeErr;
    uint32_t maxid = 0;
    for (uint32_t k = first_child(b->b, p); k; k = next_child(b->b, p, k))
        if (qta_type(b->b, k) == type) {
            if (id && qta_id(b->b, k) == id)
                return duplicateAtomTypeAndIDErr;
            if (qta_id(b->b, k) > maxid)
                maxid = qta_id(b->b, k);
        }
    if (!id)
        id = maxid + 1;
    /* before the index'th child of the type, or at the end */
    uint32_t at = p + atom_size(b->b, p);
    if (index > 0) {
        uint32_t k = qta_index(b->b, b->len, p, type, index, NULL);
        if (k)
            at = k;
        else if (index > qta_count(b->b, b->len, p, type) + 1)
            return atomIndexInvalidErr;
    }
    uint32_t n = tree ? get32(tree) : QTA_ATOM + len;
    resize_at(b, at, n, chain, depth);
    if (tree) {
        memcpy(b->b + at, tree, n);
    } else {
        put32(b->b + at, n);
        if (len && data)
            memcpy(b->b + at + QTA_ATOM, data, len);
    }
    put32(b->b + at + 4, type);
    put32(b->b + at + 8, id);
    put16(b->b + p + 14, (uint16_t)(child_count(b->b, p) + 1));
    if (out)
        *out = at;
    return noErr;
}

gaddr qt_atoms_new(void)
{
    gaddr h = tb_NewHandle(QTA_HEADER + QTA_ATOM, false, true);
    if (h && rd32(h)) {
        uint8_t b[QTA_HEADER + QTA_ATOM];
        fresh(b);
        gput(rd32(h), b, sizeof b);
    }
    return h;
}

uint32_t qt_atoms_insert(gaddr container, uint32_t parent, uint32_t type, uint32_t id, int16_t index,
                         const void *data, uint32_t len, int16_t *err)
{
    buf b;
    uint32_t at = 0;
    int16_t e = load(container, &b);
    if (!e)
        e = insert_child(&b, parent, type, id, index, data, len, NULL, &at);
    if (!e)
        e = store(&b);
    else
        drop(&b);
    if (err)
        *err = e;
    return e ? 0 : at;
}

int16_t tb_QTNewAtomContainer(gaddr atomData)
{
    gaddr h = qt_atoms_new();
    if (atomData)
        wr32(atomData, h);
    return h ? noErr : memFullErr;
}

int16_t tb_QTDisposeAtomContainer(int32_t atomData)
{
    if (atomData)
        tb_DisposeHandle((gaddr)atomData);
    return noErr;
}

int16_t tb_QTInsertChild(int32_t container, int32_t parentAtom, int32_t atomType, int32_t id, int16_t index,
                         int32_t dataSize, gaddr data, gaddr newAtom)
{
    buf b;
    uint32_t at = 0;
    int16_t err = load((gaddr)container, &b);
    uint8_t *bytes = NULL;
    if (!err && dataSize > 0 && data) {
        bytes = malloc((size_t)dataSize);
        gget(bytes, data, (uint32_t)dataSize);
    }
    if (!err)
        err = insert_child(&b, (uint32_t)parentAtom, (uint32_t)atomType, (uint32_t)id, index, bytes,
                           dataSize > 0 ? (uint32_t)dataSize : 0, NULL, &at);
    free(bytes);
    err = err ? (drop(&b), err) : store(&b);
    if (newAtom)
        wr32(newAtom, err ? 0 : at);
    return err;
}

int16_t tb_QTInsertChildren(int32_t container, int32_t parentAtom, int32_t childrenContainer)
{
    buf b, src;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    if ((err = load((gaddr)childrenContainer, &src)) != noErr) {
        drop(&b);
        return err;
    }
    for (uint32_t k = first_child(src.b, QTA_HEADER); k && !err; k = next_child(src.b, QTA_HEADER, k)) {
        uint32_t at; /* (each goes at the parent's end, so the parent stays where it is) */
        err = insert_child(&b, (uint32_t)parentAtom, qta_type(src.b, k), qta_id(src.b, k), 0, NULL, 0, src.b + k, &at);
    }
    drop(&src);
    return err ? (drop(&b), err) : store(&b);
}

int16_t tb_QTRemoveAtom(int32_t container, int32_t atom)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t chain[64];
    int depth;
    if (!atom || (uint32_t)atom == QTA_HEADER || !chain_of(&b, (uint32_t)atom, chain, &depth)) {
        drop(&b);
        return invalidAtomErr;
    }
    uint32_t n = atom_size(b.b, (uint32_t)atom), parent = chain[depth - 2];
    resize_at(&b, (uint32_t)atom, -(int64_t)n, chain, depth - 1);
    put16(b.b + parent + 14, (uint16_t)(child_count(b.b, parent) - 1));
    return store(&b);
}

int16_t tb_QTRemoveChildren(int32_t container, int32_t atom)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t a = parent_atom((uint32_t)atom), chain[64];
    int depth;
    if (!chain_of(&b, a, chain, &depth)) {
        drop(&b);
        return invalidAtomErr;
    }
    if (child_count(b.b, a)) {
        uint32_t n = atom_size(b.b, a) - QTA_ATOM;
        resize_at(&b, a + QTA_ATOM, -(int64_t)n, chain, depth);
        put16(b.b + a + 14, 0);
    }
    return store(&b);
}

/* Put an atom's contents (children or data, and their count) from another
 * atom's bytes, keeping its own type and ID. */
static int16_t put_contents(buf *b, uint32_t a, const uint8_t *from)
{
    uint32_t chain[64];
    int depth;
    if (!chain_of(b, a, chain, &depth))
        return invalidAtomErr;
    uint32_t had = atom_size(b->b, a) - QTA_ATOM, has = get32(from) - QTA_ATOM;
    if (has > had)
        resize_at(b, a + QTA_ATOM + had, has - had, chain, depth);
    else if (has < had)
        resize_at(b, a + QTA_ATOM + has, -(int64_t)(had - has), chain, depth);
    memcpy(b->b + a + QTA_ATOM, from + QTA_ATOM, has);
    put16(b->b + a + 14, get16(from + 14));
    return noErr;
}

int16_t tb_QTReplaceAtom(int32_t targetContainer, int32_t targetAtom, int32_t replacementContainer,
                         int32_t replacementAtom)
{
    buf b, src;
    int16_t err = load((gaddr)targetContainer, &b);
    if (err)
        return err;
    if ((err = load((gaddr)replacementContainer, &src)) != noErr) {
        drop(&b);
        return err;
    }
    uint32_t r = parent_atom((uint32_t)replacementAtom);
    if (!is_atom(src.b, src.len, r))
        err = invalidAtomErr;
    uint8_t *copy = NULL;
    if (!err) {
        copy = malloc(atom_size(src.b, r));
        memcpy(copy, src.b + r, atom_size(src.b, r));
        err = put_contents(&b, parent_atom((uint32_t)targetAtom), copy);
    }
    free(copy);
    drop(&src);
    return err ? (drop(&b), err) : store(&b);
}

int16_t tb_QTSwapAtoms(int32_t container, int32_t atom1, int32_t atom2)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    if (!is_atom(b.b, b.len, (uint32_t)atom1) || !is_atom(b.b, b.len, (uint32_t)atom2) || !atom1 || !atom2) {
        drop(&b);
        return invalidAtomErr;
    }
    if (qta_type(b.b, (uint32_t)atom1) != qta_type(b.b, (uint32_t)atom2)) {
        drop(&b);
        return atomsNotOfSameTypeErr;
    }
    if (atom1 == atom2) {
        drop(&b);
        return noErr;
    }
    uint32_t lo = (uint32_t)(atom1 < atom2 ? atom1 : atom2), hi = (uint32_t)(atom1 < atom2 ? atom2 : atom1);
    if (hi < lo + atom_size(b.b, lo)) { /* one inside the other */
        drop(&b);
        return invalidAtomErr;
    }
    uint8_t *x = malloc(atom_size(b.b, lo)), *y = malloc(atom_size(b.b, hi));
    memcpy(x, b.b + lo, atom_size(b.b, lo));
    memcpy(y, b.b + hi, atom_size(b.b, hi));
    /* the later one first, so the earlier keeps its place */
    err = put_contents(&b, hi, x);
    if (!err)
        err = put_contents(&b, lo, y);
    free(x);
    free(y);
    return err ? (drop(&b), err) : store(&b);
}

int16_t tb_QTSetAtomData(int32_t container, int32_t atom, int32_t dataSize, gaddr atomData)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t a = (uint32_t)atom, chain[64];
    int depth;
    if (!atom || !chain_of(&b, a, chain, &depth)) {
        drop(&b);
        return invalidAtomErr;
    }
    if (child_count(b.b, a)) {
        drop(&b);
        return notLeafAtomErr;
    }
    uint32_t had = atom_size(b.b, a) - QTA_ATOM, has = dataSize > 0 ? (uint32_t)dataSize : 0;
    if (has > had)
        resize_at(&b, a + QTA_ATOM + had, has - had, chain, depth);
    else if (has < had)
        resize_at(&b, a + QTA_ATOM + has, -(int64_t)(had - has), chain, depth);
    if (has && atomData)
        gget(b.b + a + QTA_ATOM, atomData, has);
    return store(&b);
}

int16_t tb_QTSetAtomID(int32_t container, int32_t atom, int32_t newID)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t a = (uint32_t)atom, chain[64];
    int depth;
    if (!atom || !chain_of(&b, a, chain, &depth) || depth < 2) {
        drop(&b);
        return invalidAtomErr;
    }
    uint32_t p = chain[depth - 2];
    for (uint32_t k = first_child(b.b, p); k; k = next_child(b.b, p, k))
        if (k != a && qta_type(b.b, k) == qta_type(b.b, a) && qta_id(b.b, k) == (uint32_t)newID) {
            drop(&b);
            return duplicateAtomTypeAndIDErr;
        }
    put32(b.b + a + 8, (uint32_t)newID);
    return store(&b);
}

/* Read-only calls: the container's bytes, or an error. */
static bool peek(int32_t container, buf *b) { return load((gaddr)container, b) == noErr; }

int32_t tb_QTFindChildByID(int32_t container, int32_t parentAtom, int32_t atomType, int32_t id, gaddr index)
{
    buf b;
    uint32_t found = 0;
    int16_t at = 0;
    if (peek(container, &b)) {
        uint32_t p = parent_atom((uint32_t)parentAtom);
        if (is_atom(b.b, b.len, p))
            for (uint32_t k = first_child(b.b, p); k && !found; k = next_child(b.b, p, k))
                if (qta_type(b.b, k) == (uint32_t)atomType) {
                    at++;
                    if (qta_id(b.b, k) == (uint32_t)id)
                        found = k;
                }
        drop(&b);
    }
    if (index)
        wr16(index, found ? (uint16_t)at : 0);
    return (int32_t)found;
}

int32_t tb_QTFindChildByIndex(int32_t container, int32_t parentAtom, int32_t atomType, int16_t index, gaddr id)
{
    buf b;
    uint32_t found = 0, aid = 0;
    if (peek(container, &b)) {
        found = qta_index(b.b, b.len, (uint32_t)parentAtom, (uint32_t)atomType, index, &aid);
        drop(&b);
    }
    if (id)
        wr32(id, found ? aid : 0);
    return (int32_t)found;
}

int16_t tb_QTCountChildrenOfType(int32_t container, int32_t parentAtom, int32_t childType)
{
    buf b;
    int n = 0;
    if (peek(container, &b)) {
        n = qta_count(b.b, b.len, (uint32_t)parentAtom, (uint32_t)childType);
        drop(&b);
    }
    return (int16_t)n;
}

int32_t tb_QTGetNextChildType(int32_t container, int32_t parentAtom, int32_t currentChildType)
{
    buf b;
    uint32_t next = 0;
    if (!peek(container, &b))
        return 0;
    uint32_t p = parent_atom((uint32_t)parentAtom);
    /* the types in the order they first appear */
    uint32_t types[256];
    int n = 0;
    if (is_atom(b.b, b.len, p))
        for (uint32_t k = first_child(b.b, p); k && n < 256; k = next_child(b.b, p, k)) {
            uint32_t t = qta_type(b.b, k);
            int i = 0;
            while (i < n && types[i] != t)
                i++;
            if (i == n)
                types[n++] = t;
        }
    if (!currentChildType)
        next = n ? types[0] : 0;
    else
        for (int i = 0; i + 1 < n && !next; i++)
            if (types[i] == (uint32_t)currentChildType)
                next = types[i + 1];
    drop(&b);
    return (int32_t)next;
}

int16_t tb_QTNextChildAnyType(int32_t container, int32_t parentAtom, int32_t currentChild, gaddr nextChild)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    uint32_t next = 0;
    if (!err) {
        uint32_t p = parent_atom((uint32_t)parentAtom);
        if (!is_atom(b.b, b.len, p))
            err = invalidAtomErr;
        else if (!currentChild)
            next = first_child(b.b, p);
        else if (is_atom(b.b, b.len, (uint32_t)currentChild))
            next = next_child(b.b, p, (uint32_t)currentChild);
        else
            err = invalidAtomErr;
        drop(&b);
    }
    if (nextChild)
        wr32(nextChild, next);
    return err;
}

int16_t tb_QTGetAtomTypeAndID(int32_t container, int32_t atom, gaddr atomType, gaddr id)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t a = parent_atom((uint32_t)atom);
    if (!is_atom(b.b, b.len, a))
        err = invalidAtomErr;
    if (!err && atomType)
        wr32(atomType, qta_type(b.b, a));
    if (!err && id)
        wr32(id, qta_id(b.b, a));
    drop(&b);
    return err;
}

int32_t tb_QTGetAtomParent(int32_t container, int32_t childAtom)
{
    buf b;
    uint32_t chain[64], parent = 0;
    int depth;
    if (peek(container, &b)) {
        if (childAtom && chain_of(&b, (uint32_t)childAtom, chain, &depth) && depth >= 3)
            parent = chain[depth - 2];
        drop(&b);
    }
    return (int32_t)parent;
}

int16_t tb_QTCopyAtom(int32_t container, int32_t atom, gaddr targetContainer)
{
    buf b;
    if (targetContainer)
        wr32(targetContainer, 0);
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t a = parent_atom((uint32_t)atom);
    if (!is_atom(b.b, b.len, a)) {
        drop(&b);
        return invalidAtomErr;
    }
    gaddr h = qt_atoms_new();
    if (!h) {
        drop(&b);
        return memFullErr;
    }
    if (a == QTA_HEADER) { /* the whole container */
        tb_SetHandleSize(h, (int32_t)b.len);
        if (rd32(h))
            gput(rd32(h), b.b, (uint32_t)b.len);
        wr8(rd32(h) + 10, 0);
        wr8(rd32(h) + 11, 0);
    } else {
        buf t;
        load(h, &t);
        err = insert_child(&t, 0, qta_type(b.b, a), qta_id(b.b, a), 0, NULL, 0, b.b + a, NULL);
        err = err ? (drop(&t), err) : store(&t);
    }
    drop(&b);
    if (targetContainer)
        wr32(targetContainer, err ? 0 : h);
    if (err)
        tb_DisposeHandle(h);
    return err;
}

int16_t tb_QTCopyAtomDataToHandle(int32_t container, int32_t atom, int32_t targetHandle)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t n;
    const uint8_t *d = atom ? qta_data(b.b, b.len, (uint32_t)atom, &n) : NULL;
    if (!atom || !is_atom(b.b, b.len, (uint32_t)atom))
        err = invalidAtomErr;
    else if (!d)
        err = notLeafAtomErr;
    else if (!targetHandle)
        err = paramErr;
    if (!err) {
        tb_SetHandleSize((gaddr)targetHandle, (int32_t)n);
        err = (int16_t)rd16(LM_MemErr);
        if (!err && n)
            gput(rd32((gaddr)targetHandle), d, n);
    }
    drop(&b);
    return err;
}

int16_t tb_QTCopyAtomDataToPtr(int32_t container, int32_t atom, int8_t sizeOrLessOK, int32_t size, gaddr targetPtr,
                               gaddr actualSize)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t n = 0;
    const uint8_t *d = atom ? qta_data(b.b, b.len, (uint32_t)atom, &n) : NULL;
    if (!atom || !is_atom(b.b, b.len, (uint32_t)atom))
        err = invalidAtomErr;
    else if (!d)
        err = notLeafAtomErr;
    else if ((int64_t)n > size || (!sizeOrLessOK && (int64_t)n != size))
        err = paramErr;
    if (!err && n && targetPtr)
        gput(targetPtr, d, n);
    if (actualSize)
        wr32(actualSize, err ? 0 : n);
    drop(&b);
    return err;
}

/* The lock count is the header's last two bytes; the handle stays locked
 * while it is above zero, so data pointers hold. */
int16_t tb_QTLockContainer(int32_t container)
{
    gaddr h = (gaddr)container;
    if (!h || !rd32(h) || tb_GetHandleSize(h) < QTA_HEADER)
        return invalidAtomContainerErr;
    uint16_t n = rd16(rd32(h) + 10);
    if (!n)
        tb_HLock(h);
    wr16(rd32(h) + 10, (uint16_t)(n + 1));
    return noErr;
}

int16_t tb_QTUnlockContainer(int32_t container)
{
    gaddr h = (gaddr)container;
    if (!h || !rd32(h) || tb_GetHandleSize(h) < QTA_HEADER)
        return invalidAtomContainerErr;
    uint16_t n = rd16(rd32(h) + 10);
    if (n) {
        wr16(rd32(h) + 10, (uint16_t)(n - 1));
        if (n == 1)
            tb_HUnlock(h);
    }
    return noErr;
}

int16_t tb_QTGetAtomDataPtr(int32_t container, int32_t atom, gaddr dataSize, gaddr atomData)
{
    buf b;
    int16_t err = load((gaddr)container, &b);
    if (err)
        return err;
    uint32_t n = 0;
    const uint8_t *d = atom ? qta_data(b.b, b.len, (uint32_t)atom, &n) : NULL;
    if (!atom || !is_atom(b.b, b.len, (uint32_t)atom))
        err = invalidAtomErr;
    else if (!d)
        err = notLeafAtomErr;
    if (dataSize)
        wr32(dataSize, err ? 0 : n);
    if (atomData)
        wr32(atomData, err ? 0 : rd32((gaddr)container) + (uint32_t)atom + QTA_ATOM);
    drop(&b);
    return err;
}

/* ---- User data ---- */

typedef struct {
    uint32_t type;
    uint8_t *data;
    uint32_t len;
} ud_item;

struct qt_udata {
    gaddr token;
    ud_item *items;
    int n;
};

#define MAX_UDATA 256
static qt_udata udatas[MAX_UDATA];

qt_udata *qt_udata_of(gaddr token)
{
    for (int i = 0; token && i < MAX_UDATA; i++)
        if (udatas[i].token == token)
            return &udatas[i];
    return NULL;
}

gaddr qt_udata_new(void)
{
    for (int i = 0; i < MAX_UDATA; i++)
        if (!udatas[i].token) {
            gaddr h = tb_NewHandle(4, false, true);
            if (!h)
                return 0;
            wr32(rd32(h), 0x75647461); /* 'udta' */
            udatas[i] = (qt_udata){ h, NULL, 0 };
            return h;
        }
    return 0;
}

static void clear_items(qt_udata *u)
{
    for (int i = 0; i < u->n; i++)
        free(u->items[i].data);
    free(u->items);
    u->items = NULL;
    u->n = 0;
}

void qt_udata_dispose(gaddr token)
{
    qt_udata *u = qt_udata_of(token);
    if (!u)
        return;
    clear_items(u);
    tb_DisposeHandle(u->token);
    u->token = 0;
}

void qt_udata_reset(void)
{
    for (int i = 0; i < MAX_UDATA; i++) {
        clear_items(&udatas[i]);
        udatas[i].token = 0;
    }
}

/* The index'th item of a type (1-based), or -1. */
static int item_at(const qt_udata *u, uint32_t type, int32_t index)
{
    int32_t n = 0;
    for (int i = 0; i < u->n; i++)
        if (u->items[i].type == type && ++n == index)
            return i;
    return -1;
}

static void add_item(qt_udata *u, uint32_t type, const uint8_t *data, uint32_t len)
{
    u->items = realloc(u->items, (size_t)(u->n + 1) * sizeof *u->items);
    uint8_t *copy = malloc(len ? len : 1);
    if (len)
        memcpy(copy, data, len);
    u->items[u->n++] = (ud_item){ type, copy, len };
}

static void remove_item(qt_udata *u, int i)
{
    free(u->items[i].data);
    memmove(&u->items[i], &u->items[i + 1], (size_t)(u->n - i - 1) * sizeof *u->items);
    u->n--;
}

uint8_t *qt_udata_flatten(gaddr token, uint32_t *len)
{
    qt_udata *u = qt_udata_of(token);
    uint32_t n = 0;
    for (int i = 0; u && i < u->n; i++)
        n += 8 + u->items[i].len;
    uint8_t *out = malloc(n ? n : 1), *p = out;
    for (int i = 0; u && i < u->n; i++) {
        put32(p, 8 + u->items[i].len);
        put32(p + 4, u->items[i].type);
        memcpy(p + 8, u->items[i].data, u->items[i].len);
        p += 8 + u->items[i].len;
    }
    *len = n;
    return out;
}

gaddr qt_udata_from(const uint8_t *p, uint32_t len)
{
    gaddr token = qt_udata_new();
    qt_udata *u = qt_udata_of(token);
    for (uint32_t off = 0; u && off + 8 <= len;) {
        uint32_t n = get32(p + off);
        if (n == 0 && off + 4 >= len - 4) /* the 32-bit zero some files end the list with */
            break;
        if (n < 8 || off + n > len)
            break;
        add_item(u, get32(p + off + 4), p + off + 8, n - 8);
        off += n;
    }
    return token;
}

int16_t tb_NewUserData(gaddr theUserData)
{
    gaddr h = qt_udata_new();
    if (theUserData)
        wr32(theUserData, h);
    return h ? noErr : memFullErr;
}

int16_t tb_DisposeUserData(int32_t theUserData)
{
    if (!qt_udata_of((gaddr)theUserData))
        return paramErr;
    qt_udata_dispose((gaddr)theUserData);
    return noErr;
}

int32_t tb_GetNextUserDataType(int32_t theUserData, int32_t udType)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    if (!u)
        return 0;
    /* the types in the order they first appear */
    bool past = udType == 0;
    for (int i = 0; i < u->n; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++)
            seen = u->items[j].type == u->items[i].type;
        if (seen)
            continue;
        if (past)
            return (int32_t)u->items[i].type;
        if (u->items[i].type == (uint32_t)udType)
            past = true;
    }
    return 0;
}

int16_t tb_CountUserDataType(int32_t theUserData, int32_t udType)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int16_t n = 0;
    for (int i = 0; u && i < u->n; i++)
        n += u->items[i].type == (uint32_t)udType;
    return n;
}

static uint8_t *handle_bytes(gaddr h, uint32_t *len)
{
    *len = h && rd32(h) ? (uint32_t)tb_GetHandleSize(h) : 0;
    uint8_t *b = malloc(*len ? *len : 1);
    if (*len)
        gget(b, rd32(h), *len);
    return b;
}

static int16_t to_handle(gaddr h, const uint8_t *d, uint32_t n)
{
    if (!h)
        return paramErr;
    tb_SetHandleSize(h, (int32_t)n);
    int16_t err = (int16_t)rd16(LM_MemErr);
    if (!err && n)
        gput(rd32(h), d, n);
    return err;
}

int16_t tb_AddUserData(int32_t theUserData, int32_t data, int32_t udType)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    if (!u || !data)
        return paramErr;
    uint32_t n;
    uint8_t *b = handle_bytes((gaddr)data, &n);
    add_item(u, (uint32_t)udType, b, n);
    free(b);
    return noErr;
}

int16_t tb_GetUserData(int32_t theUserData, int32_t data, int32_t udType, int32_t index)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int i = u ? item_at(u, (uint32_t)udType, index) : -1;
    if (i < 0)
        return userDataItemNotFound;
    return data ? to_handle((gaddr)data, u->items[i].data, u->items[i].len) : noErr;
}

int16_t tb_RemoveUserData(int32_t theUserData, int32_t udType, int32_t index)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int i = u ? item_at(u, (uint32_t)udType, index) : -1;
    if (i < 0)
        return userDataItemNotFound;
    remove_item(u, i);
    return noErr;
}

int16_t tb_GetUserDataItem(int32_t theUserData, gaddr data, int32_t size, int32_t udType, int32_t index)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int i = u ? item_at(u, (uint32_t)udType, index) : -1;
    if (i < 0)
        return userDataItemNotFound;
    if (size < 0 || (uint32_t)size != u->items[i].len)
        return paramErr;
    if (data && size)
        gput(data, u->items[i].data, (uint32_t)size);
    return noErr;
}

int16_t tb_SetUserDataItem(int32_t theUserData, gaddr data, int32_t size, int32_t udType, int32_t index)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    if (!u || size < 0)
        return paramErr;
    uint8_t *b = malloc(size ? (size_t)size : 1);
    if (size && data)
        gget(b, data, (uint32_t)size);
    int i = index > 0 ? item_at(u, (uint32_t)udType, index) : -1;
    if (i >= 0) {
        free(u->items[i].data);
        u->items[i].data = b;
        u->items[i].len = (uint32_t)size;
        return noErr;
    }
    /* a new item: past the last of its type (index 0 or count + 1) */
    if (index > tb_CountUserDataType(theUserData, udType) + 1) {
        free(b);
        return userDataItemNotFound;
    }
    add_item(u, (uint32_t)udType, b, (uint32_t)size);
    free(b);
    return noErr;
}

/* A text item's entries: each a 16-bit length, a 16-bit language (region
 * code), then the text.  The entry for a region, or -1; its offset. */
static int text_entry(const ud_item *it, int16_t region, uint32_t *off, uint32_t *len)
{
    uint32_t p = 0;
    for (int k = 0; p + 4 <= it->len; k++) {
        uint32_t n = get16(it->data + p);
        if (p + 4 + n > it->len)
            break;
        if ((int16_t)get16(it->data + p + 2) == region) {
            *off = p;
            *len = n;
            return k;
        }
        p += 4 + n;
    }
    return -1;
}

int16_t tb_AddUserDataText(int32_t theUserData, int32_t data, int32_t udType, int32_t index, int16_t itlRegionTag)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    if (!u || !data)
        return paramErr;
    uint32_t n;
    uint8_t *text = handle_bytes((gaddr)data, &n);
    if (n > 0xFFFF)
        n = 0xFFFF;
    int i = item_at(u, (uint32_t)udType, index);
    if (i < 0) {
        if (index > tb_CountUserDataType(theUserData, udType) + 1) {
            free(text);
            return userDataItemNotFound;
        }
        add_item(u, (uint32_t)udType, NULL, 0);
        i = u->n - 1;
    }
    ud_item *it = &u->items[i];
    uint32_t off, len;
    if (text_entry(it, itlRegionTag, &off, &len) >= 0) { /* replaces the language's text */
        memmove(it->data + off, it->data + off + 4 + len, it->len - off - 4 - len);
        it->len -= 4 + len;
    }
    it->data = realloc(it->data, it->len + 4 + n);
    put16(it->data + it->len, (uint16_t)n);
    put16(it->data + it->len + 2, (uint16_t)itlRegionTag);
    memcpy(it->data + it->len + 4, text, n);
    it->len += 4 + n;
    free(text);
    return noErr;
}

int16_t tb_GetUserDataText(int32_t theUserData, int32_t data, int32_t udType, int32_t index, int16_t itlRegionTag)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int i = u ? item_at(u, (uint32_t)udType, index) : -1;
    uint32_t off, len;
    if (i < 0 || text_entry(&u->items[i], itlRegionTag, &off, &len) < 0)
        return userDataItemNotFound;
    return data ? to_handle((gaddr)data, u->items[i].data + off + 4, len) : noErr;
}

int16_t tb_RemoveUserDataText(int32_t theUserData, int32_t udType, int32_t index, int16_t itlRegionTag)
{
    qt_udata *u = qt_udata_of((gaddr)theUserData);
    int i = u ? item_at(u, (uint32_t)udType, index) : -1;
    uint32_t off, len;
    if (i < 0 || text_entry(&u->items[i], itlRegionTag, &off, &len) < 0)
        return userDataItemNotFound;
    ud_item *it = &u->items[i];
    memmove(it->data + off, it->data + off + 4 + len, it->len - off - 4 - len);
    it->len -= 4 + len;
    if (!it->len)
        remove_item(u, i);
    return noErr;
}

int16_t tb_NewUserDataFromHandle(int32_t h, gaddr theUserData)
{
    if (theUserData)
        wr32(theUserData, 0);
    if (!h || !rd32((gaddr)h))
        return paramErr;
    uint32_t n;
    uint8_t *b = handle_bytes((gaddr)h, &n);
    gaddr token = qt_udata_from(b, n);
    free(b);
    if (theUserData)
        wr32(theUserData, token);
    return token ? noErr : memFullErr;
}

int16_t tb_PutUserDataIntoHandle(int32_t theUserData, int32_t h)
{
    if (!qt_udata_of((gaddr)theUserData))
        return paramErr;
    uint32_t n;
    uint8_t *b = qt_udata_flatten((gaddr)theUserData, &n);
    int16_t err = to_handle((gaddr)h, b, n);
    free(b);
    return err;
}

/* CopyUserData's rules: the destination's items replaced, or the source's
 * added to them. */
int16_t tb_CopyUserData(int32_t srcUserData, int32_t dstUserData, int32_t copyRule)
{
    qt_udata *s = qt_udata_of((gaddr)srcUserData), *d = qt_udata_of((gaddr)dstUserData);
    if (!s || !d)
        return paramErr;
    if (s == d)
        return noErr;
    if (copyRule == 0x72706C63) /* kQTCopyUserDataReplace 'rplc' */
        clear_items(d);
    else if (copyRule != 0x6D657267) /* kQTCopyUserDataMerge 'merg' */
        return paramErr;
    for (int i = 0; i < s->n; i++)
        add_item(d, s->items[i].type, s->items[i].data, s->items[i].len);
    return noErr;
}

/* ---- UUIDs (QuickTime 6's, version 4: random) ---- */

int16_t tb_QTCreateUUID(gaddr outUUID, int32_t creationFlags)
{
    (void)creationFlags;
    if (!outUUID)
        return paramErr;
    uint8_t u[16];
#if defined(__APPLE__) || defined(__OpenBSD__)
    arc4random_buf(u, sizeof u);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(u, 1, sizeof u, f) != sizeof u)
        for (int i = 0; i < 16; i++)
            u[i] = (uint8_t)rand();
    if (f)
        fclose(f);
#endif
    u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);
    u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
    gput(outUUID, u, sizeof u);
    return noErr;
}

int8_t tb_QTEqualUUIDs(gaddr uuid1, gaddr uuid2)
{
    if (!uuid1 || !uuid2)
        return 0;
    for (uint32_t i = 0; i < 16; i++)
        if (rd8(uuid1 + i) != rd8(uuid2 + i))
            return 0;
    return 1;
}
