/*
 * The Mac OS 9 libraries' remaining exports (os9rest*.c): each group's
 * file has a table of its routines; os9rest_export_fn (os9rest.c) looks a
 * name up in all of them.
 */
#ifndef OS9REST_H
#define OS9REST_H

#include "ppc_bridge.h"

void (*os9rest_export_fn(const char *name))(ppc_cpu *);
bool os9rest_data_known(const char *name);
gaddr os9rest_data_export(const char *name);
void os9rest_reset(void);

typedef struct {
    const ppc_export *exports;
    const unsigned *count;
    void (*reset)(void);
} os9rest_group;

/* Argument word n of a call (GPR slots r3 on, then the parameter area). */
static inline uint32_t os9rest_word(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}

void os9rest_debug_reset(void);
extern const ppc_export os9rest_hw_exports[];
extern const unsigned os9rest_hw_export_count;
void os9rest_hw_reset(void);
extern const ppc_export os9rest_locale_exports[];
extern const unsigned os9rest_locale_export_count;
void os9rest_locale_reset(void);
void os9rest_icons_reset(void);
extern const ppc_export os9rest_math_exports[];
extern const unsigned os9rest_math_export_count;

#endif
