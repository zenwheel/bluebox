/*
 * Snapshots of the C targets' state, for hibernating a program: the
 * saved state sections (bbstate.h), every tracked block (bbmem.h) and the
 * regions (the guest's address space), written so that another process,
 * of this build or of any whose saved state has the same shape (the state
 * fingerprint), can make every block again wherever it lands and rewrite
 * the pointers to it (hibernate.c says how).
 *
 * The machine must be standing still while a snapshot is taken or put
 * back: no Toolbox call running on any thread.
 */
#ifndef TOOLBOX_HIBERNATE_H
#define TOOLBOX_HIBERNATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct hb_stats {
    uint64_t bytes;                       /* the snapshot's size */
    uint64_t section_bytes, block_bytes, region_bytes;
    uint32_t sections, blocks, regions;
    uint32_t relocs;                      /* pointers rewritten */
    uint32_t to_blocks, to_statics, to_regions, to_image, to_libraries;
    /* Of those to the binary (by name, which another build puts back): to
     * a function's start, into a function, to a constant's (or a static of
     * the host's) start, into one, to a string constant; and those with no
     * symbol (the same build only). */
    uint32_t to_functions, to_code_inside, to_data, to_data_inside, to_strings, to_image_unnamed;
    uint32_t strings_kept; /* string constants this build has not: kept in blocks */
    uint32_t host;                        /* pointers to the host's memory: kept only in this process */
    uint32_t untyped_blocks;              /* blocks no tag describes */
    uint32_t stale_types;                 /* tags the map has no layout for (or a different size) */
    uint32_t statics;                     /* saved statics the map placed */
    /* Put back by name, from a build whose statics are elsewhere: the
     * snapshot's statics this build has not (dropped), and this build's the
     * snapshot has not (kept as they booted, being as compiled). */
    uint32_t statics_dropped, statics_new;
    uint64_t file_bytes;                  /* the file written, compressed */
} hb_stats;

/* Writes a snapshot into *out (free it with hb_free_buffer). */
bool hb_save_buffer(uint8_t **out, size_t *len, hb_stats *stats, char *err, size_t errlen);
void hb_free_buffer(uint8_t *buf);
/* A snapshot file: deflated, after a header and the caller's bytes (extra:
 * the record of the launch, for the host).  hb_file_extra reads those back,
 * and whether this build can put the snapshot back (*resumable: the build
 * that wrote it, or one whose saved state has the same fingerprint), and
 * if not, why (the first difference, in words, into why); hb_file_load
 * inflates the snapshot (free it with hb_free_buffer). */
bool hb_save_file(const char *path, hb_stats *stats, char *err, size_t errlen);
bool hb_save_file_extra(const char *path, const void *extra, size_t extra_len, hb_stats *stats, char *err,
                        size_t errlen);
/* The same for a snapshot hb_save_buffer made already (from any thread:
 * the machine may go on meanwhile). */
bool hb_write_file_extra(const char *path, const uint8_t *buf, size_t len, const void *extra, size_t extra_len,
                         hb_stats *stats, char *err, size_t errlen);
bool hb_file_extra(const char *path, uint8_t **extra, size_t *extra_len, bool *resumable, char *why, size_t whylen,
                   char *err, size_t errlen);
bool hb_file_load(const char *path, uint8_t **out, size_t *len, char *err, size_t errlen);

enum {
    /* Put each region (the guest's memory) in fresh memory too, as a new
     * process would have it, rather than over the one there now. */
    HB_RESTORE_MOVE_REGIONS = 1 << 0,
    /* Pointers to the host's memory cleared as for another process, even in
     * the one that saved it (a save point put back: what they pointed to
     * may be gone since). */
    HB_RESTORE_FORGET_HOST = 1 << 1,
};

/* Puts a snapshot back: refused unless the build that wrote it had the same
 * state fingerprint (or was this one), saying why. */
bool hb_restore_buffer(const uint8_t *buf, size_t len, int flags, hb_stats *stats, char *err, size_t errlen);
bool hb_restore_file(const char *path, int flags, hb_stats *stats, char *err, size_t errlen);

/* The untracked-pointer scan: each word of the saved state that looks like
 * a pointer to a tracked block, a static, the guest's memory or the binary
 * but that the map does not say is one, where a pointer could be that the
 * map would miss: in an untyped block, anywhere; in a static or a typed
 * block, an integer as wide as a pointer, or bytes no type covers; in a
 * block of bytes, a pointer to a block (only: a file's bytes look like
 * anything).  And each word of the host's sections pointing into a tracked
 * block (which a restore frees).  Also each field the map knows that holds
 * the host's memory (a library's handle or object, Swift's memory), which
 * a snapshot carries only within the process: what resuming must make
 * again.  HB_SCAN_ALL_WORDS looks at every word
 * the map does not know (and finds text and numbers too).  Writes up to
 * max lines to out (NULL: none); returns how many words. */
typedef struct hb_scan_result {
    uint32_t in_statics, in_blocks, in_untyped, host_to_blocks;
    uint32_t host_fields; /* not untracked: the host's, listed for resuming */
} hb_scan_result;
enum { HB_SCAN_ALL_WORDS = 1 << 0 };
hb_scan_result hb_scan(FILE *out, int max, int flags);

/* The differences between two snapshots this binary wrote, each described
 * (a static by name, a block by number and type, a page of the guest's
 * memory, a pointer by where it is); up to max lines to out (NULL: none).
 * HB_COMPARE_IGNORE_HOST leaves out pointers to the host's memory, which a
 * snapshot keeps only as they were (and a new block may land where one
 * pointed, once that memory was freed).  Returns how many. */
enum { HB_COMPARE_IGNORE_HOST = 1 << 0 };
int hb_compare(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, int flags, FILE *out, int max);

/* The map against this binary: each saved static the binary has must be
 * in it, and each tagged type of the size the map says.  Writes the
 * differences to out; returns how many. */
int hb_check_map(FILE *out);

/* The build's identity (LC_UUID), as 32 hex digits and a NUL. */
void hb_build_id(char out[33]);
/* The state fingerprint (hibernate.c says what it covers), the same way,
 * and the manifest it is a hash of (text, a line for each thing). */
void hb_fingerprint(char out[33]);
size_t hb_manifest(const char **out);
/* Why a snapshot with this manifest cannot be put back here (the first
 * difference), into why. */
void hb_explain_manifest(const char *old, size_t len, char *why, size_t whylen);
/* A pointer into the binary as a snapshot saves it: "file:symbol+offset"
 * (or "symbol+offset" for a global, "\"text\"+offset" for a string
 * constant) into out, false when it has no name; and back.  For tests. */
bool hb_symbolize(const void *p, char *out, size_t n);
const void *hb_resolve_symbol(const char *file, const char *name, uint64_t off);

#endif
