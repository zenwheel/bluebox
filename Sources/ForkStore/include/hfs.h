/*
 * Disk images and read-only HFS volumes (host side).
 *
 * diskimage_decode turns a disk image file into the raw bytes of its HFS
 * volume.  Supported containers: DiskCopy 4.2, Disk Copy 6 NDIF (raw and
 * ADC-compressed chunks), raw volumes, and Apple partition maps (CD-ROMs).
 */
#ifndef HFS_H
#define HFS_H

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
/* Decode to the raw HFS volume (malloc'd). */
bool diskimage_decode(const mac_file *f, uint8_t **vol, size_t *len, char *err, size_t errlen);

/* Apple Data Compression (used by NDIF and UDIF). Returns bytes written or -1. */
long adc_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

typedef struct hfs_volume hfs_volume;

typedef struct {
    uint32_t cnid;
    uint32_t parent;       /* parent directory ID (1 for the root folder itself) */
    bool is_dir;
    uint8_t name[32];      /* Pascal string, MacRoman */
    uint8_t finder_info[32]; /* FInfo+FXInfo (files) or DInfo+DXInfo (folders) */
    uint32_t create_date, modify_date;
    uint32_t data_len, rsrc_len;
    uint8_t data_ext[12], rsrc_ext[12]; /* first extent records */
} hfs_entry;

/* Open a volume held in memory (the buffer must outlive the volume). */
hfs_volume *hfs_open(const uint8_t *vol, size_t len, char *err, size_t errlen);
void hfs_close(hfs_volume *v);
/* Volume name as a Pascal string (MacRoman). */
const uint8_t *hfs_volume_name(const hfs_volume *v);
/* Every catalog entry (files and folders), in catalog order. */
bool hfs_list(hfs_volume *v, hfs_entry **entries, size_t *count, char *err, size_t errlen);
/* Read one fork of a file. */
bool hfs_read_fork(hfs_volume *v, const hfs_entry *e, bool rsrc, uint8_t **buf, size_t *len,
                   char *err, size_t errlen);

#endif
