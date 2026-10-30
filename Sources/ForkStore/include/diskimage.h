/*
 * Disk images and read-only HFS volumes (host side).
 *
 * diskimage_decode turns a disk image file into the raw bytes of its HFS
 * volume.  Supported containers: DiskCopy 4.2, Disk Copy 6 NDIF (raw and
 * ADC-compressed chunks), raw volumes, and Apple partition maps (CD-ROMs).
 */
#ifndef FORKSTORE_DISKIMAGE_H
#define FORKSTORE_DISKIMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forks.h"

/* What a file looks like as a disk image, judged from its forks. */
typedef enum {
    DISK_NONE = 0,
    DISK_RAW,      /* volume at offset 0 (or behind a partition map) */
    DISK_DC42,     /* DiskCopy 4.2 */
    DISK_NDIF,     /* Disk Copy 6 ('bcem' resource) */
} disk_kind;

disk_kind diskimage_detect(const mac_file *f);
/* Whether a file is a Disk Copy 6 image (its chunk map, 'bcem', in its
 * resource fork): its data fork can begin like a bare volume, the first
 * chunk stored as it is, and still be compressed after. */
bool diskimage_is_ndif(const char *path);
/* A raw image's volume (bare, or the first HFS partition of a partition
 * map), found in place: its offset and length.  False if it isn't one. */
bool diskimage_locate_raw(const uint8_t *d, size_t n, size_t *off, size_t *len);
/* Decode to the raw HFS volume (malloc'd). */
bool diskimage_decode(const mac_file *f, uint8_t **vol, size_t *len, char *err, size_t errlen);

/* Apple Data Compression (used by NDIF and UDIF). Returns bytes written or -1. */
long adc_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

typedef struct hfs_volume hfs_volume;

typedef struct {
    uint32_t cnid;
    uint32_t parent;       /* parent directory ID (1 for the root folder itself) */
    bool is_dir;
    uint8_t name[256];     /* Pascal string, MacRoman (HFS+ names converted from Unicode) */
    uint8_t finder_info[32]; /* FInfo+FXInfo (files) or DInfo+DXInfo (folders) */
    uint32_t create_date, modify_date;
    uint32_t data_len, rsrc_len;
    uint8_t data_ext[12], rsrc_ext[12]; /* HFS: first extent records */
    uint8_t data_fork[80], rsrc_fork[80]; /* HFS+: fork data records */
} hfs_entry;

/* What a volume's master directory block (or HFS+ volume header) says of
 * it, as the File Manager's volume control block keeps it. */
typedef struct {
    uint16_t signature;     /* 'BD' HFS, 0xD2D7 MFS, 'H+' or 'HX' */
    uint16_t attributes;    /* bit 7 locked by hardware, 8 unmounted cleanly, 15 locked by software */
    uint32_t create_date, modify_date, backup_date;
    uint32_t block_size, total_blocks, free_blocks;
    uint32_t first_block;   /* allocation block 0's (HFS) or 2's (MFS) 512-byte sector */
    uint32_t clump_size, next_cnid, write_count;
    uint32_t file_count, dir_count; /* the whole volume's */
    uint16_t root_files, root_dirs;
    uint8_t finder_info[32];
} hfs_volinfo;

/* Open a volume held in memory (the buffer must outlive the volume): HFS,
 * HFS+ or HFSX, the last two bare or inside an HFS wrapper, or MFS. */
hfs_volume *hfs_open(const uint8_t *vol, size_t len, char *err, size_t errlen);
void hfs_close(hfs_volume *v);
/* Volume name as a Pascal string (MacRoman); for HFS+, after hfs_list. */
const uint8_t *hfs_volume_name(const hfs_volume *v);
const hfs_volinfo *hfs_volume_info(const hfs_volume *v);
/* Every catalog entry (files and folders), in catalog order. */
bool hfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen);
/* Read one fork of a file. */
bool hfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len,
                   char *err, size_t errlen);

/* Copy every file out into a host folder (made already) standing for the
 * root: folders as folders, files as AppleDouble pairs, '/' in a name as
 * ':'.  keep (if any) is asked with each file's path under root; note is
 * told of a file that could not be read, which is skipped. */
typedef bool (*hfs_keep_fn)(void *ctx, const char *relpath);
typedef void (*hfs_note_fn)(void *ctx, const char *message);
bool hfs_extract(hfs_volume *v, const char *root, hfs_keep_fn keep, hfs_note_fn note, void *ctx, char *err,
                 size_t errlen);

/* ---- Writing an HFS volume ---- */

/* One file or folder of a volume to build.  IDs are the caller's (16 and
 * up, unique); the root folder is 2 and is not listed. */
typedef struct {
    uint32_t cnid, parent;
    bool is_dir;
    uint8_t name[32];        /* Pascal, MacRoman, at most 31 */
    uint8_t finder_info[32]; /* FInfo+FXInfo or DInfo+DXInfo */
    uint32_t create_date, modify_date;
    const uint8_t *data, *rsrc;
    uint32_t data_len, rsrc_len;
} hfs_build_entry;

typedef struct {
    uint8_t name[28];        /* Pascal, at most 27 */
    uint32_t size;           /* the disk's bytes; the volume fills it */
    uint32_t block_size;     /* allocation block size, or 0 for the smallest that fits */
    uint32_t create_date, modify_date, backup_date, write_count;
    uint16_t attributes;     /* the locked bits kept; "unmounted cleanly" is set */
    uint8_t finder_info[32];
    /* Catalog order of two names (Pascal strings): HFS's, without regard
     * to case.  NULL compares ASCII letters without case. */
    int (*compare)(const uint8_t *a, const uint8_t *b);
} hfs_build_params;

/* An HFS volume holding the entries, as a disk of params->size bytes
 * (malloc'd).  False when they don't fit (err says so). */
bool hfs_build(const hfs_build_params *params, const hfs_build_entry *entries, size_t count, uint8_t **out,
               char *err, size_t errlen);

#endif
