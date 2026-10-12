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

/* Screen size in pixels (call before machine_boot; default 1024x768). */
void machine_set_screen(int width, int height);
/* Open the fonts (FOND/NFNT/FONT) of a resource file below the application.
 * Call after machine_boot and before machine_launch. */
bool machine_add_resource_file(const char *path, char *err, size_t errlen);
/* The 1-bit framebuffer (1 = black), for the host to display. */
const uint8_t *machine_framebuffer(int *width, int *height, int *row_bytes);
bool machine_halted(void);
/* Write the screen to a PNG file. */
bool machine_screenshot(const char *path);

/* Override a Gestalt selector (before launch). */
void gestalt_set(uint32_t selector, uint32_t value);
/* Logging mask (see log.h) and destination. */
void machine_set_log(unsigned mask);

/* Host input (thread-safe).  Coordinates are global screen pixels;
 * button: 1 down, 0 up, -1 unchanged. */
void machine_post_mouse(int x, int y, int button);
void machine_post_key(int charcode, int keycode, unsigned modifiers, int down, int repeat);
/* Tell the Event Manager a host is attached (it may then sleep for input). */
void machine_attach_host(bool attached);

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
