/* Runner for the SingleStepTests 68000 vectors (see Tests/fetch-cpu-tests.sh). */
#ifndef SST_H
#define SST_H

#include <stdbool.h>

typedef struct {
    int total;
    int passed;
    int skipped;  /* address-error cases: a 68000-only behaviour we do not model */
    int failed;
    char first_failure[512];
} sst_result;

/* Run every vector in a decompressed .json file. Returns false if the file
 * cannot be read or parsed. */
bool sst_run_file(const char *path, sst_result *out);
/* Run each vector through the 68k JIT (a block of one instruction). */
extern bool sst_jit;

#endif
