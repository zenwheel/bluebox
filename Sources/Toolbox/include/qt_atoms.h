/* QT atom containers and user data (qt_atoms.c). */
#ifndef QT_ATOMS_H
#define QT_ATOMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"

/* An atom container's bytes: a 12-byte header (10 reserved bytes and a
 * lock count), then the root atom 'sean' (ID 1).  An atom is its offset;
 * kParentAtomIsContainer (0) names the root.  An atom's header is its
 * size, type, ID, 2 reserved bytes, its child count and 4 reserved bytes;
 * a leaf holds data instead of children. */
enum { QTA_HEADER = 12, QTA_ATOM = 20 };

/* Read-only walks over a container held in host memory. */
uint32_t qta_root(const uint8_t *c, size_t len);
uint32_t qta_find(const uint8_t *c, size_t len, uint32_t parent, uint32_t type, uint32_t id);   /* 0: none */
uint32_t qta_index(const uint8_t *c, size_t len, uint32_t parent, uint32_t type, int index, uint32_t *id); /* 1-based */
int qta_count(const uint8_t *c, size_t len, uint32_t parent, uint32_t type);   /* type 0: all */
const uint8_t *qta_data(const uint8_t *c, size_t len, uint32_t atom, uint32_t *n);
uint32_t qta_type(const uint8_t *c, uint32_t atom);
uint32_t qta_id(const uint8_t *c, uint32_t atom);

/* A container in a guest handle, for natives. */
gaddr qt_atoms_new(void);
/* A leaf child (dataSize bytes from host memory), its offset or 0. */
uint32_t qt_atoms_insert(gaddr container, uint32_t parent, uint32_t type, uint32_t id, int16_t index,
                         const void *data, uint32_t len, int16_t *err);

/* User data: items of types, each its bytes; a text item is (length,
 * language, text) entries.  The token is a guest handle. */
typedef struct qt_udata qt_udata;
gaddr qt_udata_new(void);
qt_udata *qt_udata_of(gaddr token);
void qt_udata_dispose(gaddr token);
/* As a 'udta' atom's contents (atoms of type and data), malloc'd; and back. */
uint8_t *qt_udata_flatten(gaddr token, uint32_t *len);
gaddr qt_udata_from(const uint8_t *p, uint32_t len);
void qt_udata_reset(void);

#endif
