/*
 * The File Manager's host side, for its HFS Plus calls (hfsplus.c).
 *
 * Every host path the File Manager has seen has a node number, the index
 * of its entry in the directory table (filemgr.c): a directory's ID, and
 * less 0x10000 a file's number.  A node follows its object when the File
 * Manager renames or moves it.
 */
#ifndef FILEMGR_INTERNAL_H
#define FILEMGR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"

#define FM_ROOT_ID 2
#define FM_FILE_ID_BASE 0x10000 /* a file's number: its node and this */

int32_t fm_node_of_path(const char *host);
bool fm_path_of_node(int32_t node, char *out, size_t outsize);
int16_t fm_vref_of_path(const char *host);
/* Whether a host path is a volume's root (and so has no parent). */
bool fm_is_volume_root(const char *host);
/* A directory's ID or a file's number, as its volume reports it. */
uint32_t fm_cat_id(const char *host);
/* The ID of the folder an object is in; 1 for a volume's root. */
uint32_t fm_parent_cat_id(const char *host);
/* The host path a (vRefNum, dirID, name) triple names (an HFS call). */
int16_t fm_resolve(int16_t vref, int32_t dirid, gaddr name, char *out, size_t outsize);
bool fm_is_dir(const char *host);
bool fm_visible(const char *host);
bool fm_writable(const char *host);
/* An FSSpec for an existing object, volume roots included. */
void fm_spec_of_path(const char *host, gaddr spec);
/* A host path's name as its volume shows it (UTF-8): the Host folder's,
 * and a volume's for its root. */
void fm_display_name(const char *host, char *out, size_t outsize);
/* The visible entries of a directory, sorted, as host paths (the Host
 * folder as "/"); the caller frees each and the array. */
int fm_list_dir(const char *dir, char ***paths);
/* An entry of a directory by its host name: the name itself when it
 * exists, else one equal to it ignoring case. */
void fm_child_path(const char *dir, const char *name, char *out, size_t outsize);

int16_t fm_create_file(const char *host);
int16_t fm_create_dir(const char *host);
int16_t fm_delete(const char *host);
int16_t fm_rename(const char *from, const char *to);
/* Move an object into a directory; its new path in dest. */
int16_t fm_move(const char *from, const char *dir, char *dest, size_t destsize);
int16_t fm_exchange(const char *a, const char *b);

/* Catalog information for a host path. */
typedef struct {
    bool dir;
    uint8_t finder_info[32]; /* FInfo/DInfo and FXInfo/DXInfo */
    uint32_t data_len, rsrc_len;
    uint32_t created, modified; /* Mac local time */
    uint32_t valence;           /* a directory's entries */
    bool data_open, rsrc_open;
} fm_info;
bool fm_info_of(const char *host, fm_info *out);
/* Change what a file keeps: its Finder information (either half may be
 * NULL) and dates (0 leaves one alone).  A directory keeps nothing. */
int16_t fm_set_info(const char *host, const uint8_t *finfo, const uint8_t *fxinfo, uint32_t created,
                    uint32_t modified);

/* Forks. */
int16_t fm_open_fork(const char *host, bool rsrc, uint8_t perm, int16_t *refnum);
int16_t fm_close_fork(int16_t refnum);
int16_t fm_flush_fork(int16_t refnum);
/* An open fork's state (a Resource Manager file's too, read-only). */
bool fm_fork_state(int16_t refnum, char *path, size_t pathsize, bool *rsrc, bool *write, uint32_t *pos,
                   uint32_t *len);
/* Read or write at a position (fsAtMark, fsFromStart, fsFromLEOF,
 * fsFromMark and an offset); the mark ends after the bytes moved. */
int16_t fm_fork_io(int16_t refnum, bool write, uint16_t mode, int64_t off, uint32_t count, gaddr buf,
                   uint32_t *actual);
int16_t fm_fork_seek(int16_t refnum, uint16_t mode, int64_t off);
int16_t fm_fork_set_eof(int16_t refnum, uint16_t mode, int64_t off);
/* The open forks in FCB order: the refnum after `after` (0 for the first),
 * or 0 when there are no more. */
int16_t fm_next_fork(int16_t after);

/* Volumes: by index (1 the main volume, then the mounted ones), or by
 * refnum when the index is 0. */
bool fm_volume(uint32_t index, int16_t vref, int16_t *vref_out, char *root, size_t rootsize, uint8_t name[32],
               int16_t *drive, int16_t *driver);
void fm_flush_volume(void);

/* Folder Manager: a folder type's host folder (made if create). */
int16_t fm_find_folder(uint32_t type, bool create, char *out, size_t outsize);

/* The Resource Manager by host path (rsrcmgr.c). */
int16_t rm_open_path(const char *path, int8_t perm);
void rm_create_path(const char *path, uint32_t type, uint32_t creator);
bool rm_path_open(const char *path, bool *in_chain, int16_t *refnum);

/* FSRefs (hfsplus.c): one made for a host path, and the path of one. */
void hfs_make_fsref(gaddr ref, const char *path);
int16_t hfs_fsref_path(gaddr ref, char *out, size_t outsize);

/* The HFS Plus calls' state, cleared at each launch (hfsplus.c). */
void hfsplus_reset(void);

#endif
