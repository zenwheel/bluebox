/* Public interface to the classic environment, used by the host shells and CLI. */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RUN_LIMIT = 0,   /* instruction limit reached, still runnable */
    RUN_HALTED = 1,  /* the app quit or an error stopped the CPU */
} run_status;

/* Reset everything and set up an empty machine. Returns false on failure. */
bool machine_boot(bool census);
/* Load an application (AppleDouble data-fork path) and prepare to run it. */
bool machine_launch(const char *app_path, char *err, size_t errlen);
/* Run up to max_instructions. */
run_status machine_run(uint64_t max_instructions);
const char *machine_halt_reason(void);
uint64_t machine_instruction_count(void);
uint32_t machine_pc(void);

/* Override a Gestalt selector (before launch). */
void gestalt_set(uint32_t selector, uint32_t value);
/* Logging mask (see log.h) and destination. */
void machine_set_log(unsigned mask);

/* Trap census for the current run. */
typedef struct {
    uint16_t trap;
    int32_t selector;   /* -1 if not a dispatched routine */
    const char *name;
    uint32_t calls;
    bool implemented;
} census_row;
/* Fills rows (up to max); returns the number of distinct routines seen. */
unsigned machine_census(census_row *rows, unsigned max);

/* Dump the register state to a buffer (for diagnostics). */
void machine_describe_cpu(char *buf, size_t len);

#endif
