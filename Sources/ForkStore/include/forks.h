/*
 * Host-side file forks: AppleDouble (the format we write), MacBinary (import
 * only), and the resource fork format itself (parsing and decompression).
 *
 * Everything here works on host memory buffers; nothing touches guest memory.
 */
#ifndef FORKS_H
#define FORKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A file's two forks plus Finder info, held in host memory. */
typedef struct {
    uint8_t *data;
    size_t data_len;
    uint8_t *rsrc;
    size_t rsrc_len;
    uint8_t finder_info[32]; /* FInfo + FXInfo, big-endian as on disk */
    bool has_finder_info;
    char name[64];           /* Mac name (MacRoman), when known */
    uint32_t create_date, modify_date; /* Mac epoch seconds; 0 if unknown */
} mac_file;

void mac_file_free(mac_file *f);

static inline uint32_t mac_file_type(const mac_file *f)
{
    const uint8_t *p = f->finder_info;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t mac_file_creator(const mac_file *f)
{
    const uint8_t *p = f->finder_info + 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- MacBinary (I, II, III) ---- */

/* True if buf looks like a MacBinary header. */
bool macbinary_detect(const uint8_t *buf, size_t len);
/* Parse a whole MacBinary file. Returns false with err set on failure. */
bool macbinary_parse(const uint8_t *buf, size_t len, mac_file *out, char *err, size_t errlen);

/* ---- AppleDouble ----
 * A file "foo" stores its data fork in "foo" and everything else (resource
 * fork, Finder info, dates) in the sidecar "._foo" next to it. */

/* Path of the sidecar for a data-fork path. Caller frees. */
char *appledouble_sidecar_path(const char *path);
/* Read both forks. A missing sidecar means an empty resource fork. */
bool appledouble_read(const char *path, mac_file *out, char *err, size_t errlen);
/* Write both forks atomically (temp file + rename). */
bool appledouble_write(const char *path, const mac_file *f, char *err, size_t errlen);
/* Encode/decode just the sidecar contents. */
bool appledouble_decode(const uint8_t *buf, size_t len, mac_file *out, char *err, size_t errlen);
uint8_t *appledouble_encode(const mac_file *f, size_t *len_out);

/* ---- Resource fork format ---- */

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    char name[256];      /* empty if unnamed (MacRoman) */
    uint32_t data_off;   /* offset of the 4-byte length + data, from data start */
    uint32_t data_len;
} rsrc_entry;

typedef struct {
    uint32_t data_start, map_start, data_len, map_len;
    uint16_t file_attrs;
    rsrc_entry *entries;
    size_t count;
} rsrc_index;

/* Parse a resource fork into a flat index. */
bool rsrc_parse(const uint8_t *fork, size_t len, rsrc_index *out, char *err, size_t errlen);
void rsrc_index_free(rsrc_index *idx);
/* Pointer to the resource's data inside the fork. */
const uint8_t *rsrc_data(const uint8_t *fork, const rsrc_index *idx, const rsrc_entry *e);

/* Build a resource fork from a list of resources (malloc'd). */
typedef struct {
    uint32_t type;
    int16_t id;
    const char *name; /* NULL or "" for none (MacRoman) */
    uint8_t attrs;
    const uint8_t *data;
    uint32_t len;
} rsrc_spec;
bool rsrc_build(const rsrc_spec *specs, size_t n, uint8_t **out, size_t *out_len);

/* A new fork holding only the resources of the given types (malloc'd). */
bool rsrc_filter(const uint8_t *fork, size_t len, const uint32_t *types, size_t ntypes, uint8_t **out,
                 size_t *out_len, char *err, size_t errlen);

/* Compressed resources ('dcmp' 0, 1 and 2).  Returns true and a malloc'd
 * buffer if data is a compressed resource that decompressed correctly. */
bool rsrc_is_compressed(const uint8_t *data, size_t len);
uint32_t rsrc_decompressed_size(const uint8_t *data, size_t len);
bool rsrc_decompress(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len,
                     char *err, size_t errlen);

/* Resource attribute bits. */
enum {
    RES_SYS_HEAP = 0x40, RES_PURGEABLE = 0x20, RES_LOCKED = 0x10,
    RES_PROTECTED = 0x08, RES_PRELOAD = 0x04, RES_CHANGED = 0x02, RES_COMPRESSED = 0x01,
};

/* MacRoman <-> UTF-8 (file names).  Return bytes written. */
size_t macroman_to_utf8(const uint8_t *in, size_t n, char *out, size_t outsize);
size_t utf8_to_macroman(const char *in, uint8_t *out, size_t outsize);

/* Whole-file helpers. */
bool read_file(const char *path, uint8_t **buf, size_t *len);

#endif
