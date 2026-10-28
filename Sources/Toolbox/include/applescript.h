/*
 * AppleScript, our own runtime (M14): compiled scripts ('scpt' resources,
 * "FasdUAS" data) read and run by Blue Box itself, never by the host.
 * The Open Scripting Architecture's component (osa.c) and the tests use
 * these; scripts and values are named by IDs as OSA names them.
 */
#ifndef APPLESCRIPT_H
#define APPLESCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AppleScript's error numbers that the runtime itself gives. */
enum {
    asErrUserCanceled = -128,
    asErrProcNotFound = -600,
    asErrCantCoerce = -1700,
    asErrCorruptData = -1702,
    asErrNotHandled = -1708,
    asErrCantGet = -1728,
    asErrNotDefined = -2753,
    asErrUnsupported = -2700,
    asErrCantCompile = -2741, /* a syntax error: our runtime compiles no source yet */
    asErrDivideByZero = -2701,
};

/* Forget every script and value (a new run). */
void as_reset(void);

/* Load a compiled script; 0 or an error (errOSACorruptData ...). */
int32_t as_load_script(const uint8_t *data, size_t len, uint32_t *id_out);
/* The data a script was loaded from (OSAStore), or NULL. */
const uint8_t *as_script_data(uint32_t id, size_t *len);
/* Run a script's run handler, or the handler for an event (class and ID)
 * with an optional direct parameter (a value ID, or 0).  0 or the script's
 * error (as_error_number, as_error_message); the result's ID in *result
 * (0 for none). */
int32_t as_run_script(uint32_t id, uint32_t *result);
int32_t as_run_event(uint32_t id, uint32_t cls, uint32_t eid, uint32_t direct, uint32_t *result, int *found);
/* Run the script's idle handler: the seconds until the next idle, or a
 * negative error; 30 when it returns no number. */
int32_t as_run_idle(uint32_t id, int *found);
/* The value as AppleScript would show it (OSADisplay): MacRoman, NUL-ended. */
int32_t as_display(uint32_t id, char *out, size_t cap);
void as_dispose(uint32_t id);
uint32_t as_copy_id(uint32_t id);
/* A script object of its own, its values copied (OSACopyScript); 0 for none. */
uint32_t as_copy_script(uint32_t id);
/* A value's ID for text (MacRoman), an integer, or an Apple Event
 * descriptor in guest memory (Open Documents' list of aliases). */
uint32_t as_new_text(const char *s);
uint32_t as_new_int(int32_t v);
uint32_t as_new_from_desc(uint32_t desc);
/* A script's properties and handlers by name (ASDebugging.h): the names
 * as a list of text, and one by name (MacRoman), got or set (a name the
 * context lacks is defined unless dont_define); value IDs. */
int32_t as_context_names(uint32_t ctx, bool handlers, uint32_t *list_id);
int32_t as_context_get(uint32_t ctx, const uint8_t *name, size_t n, bool handler, uint32_t *value_id);
int32_t as_context_set(uint32_t ctx, const uint8_t *name, size_t n, bool handler, uint32_t value_id, bool dont_define);
/* The elements of a list of text value, one at a time: its length and item
 * i as MacRoman (not NUL-ended). */
size_t as_list_length(uint32_t id);
size_t as_list_text(uint32_t id, size_t i, const uint8_t **text);
/* The last error. */
int32_t as_error_number(void);
const char *as_error_message(void);
/* The interpreter's instructions run so far (tests). */
uint64_t as_instructions(void);

/* Scripting additions shown by the host (display dialog, choose from
 * list): see machine_set_script_dialog_handler in machine.h. */

#endif
