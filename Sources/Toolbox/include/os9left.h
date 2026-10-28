/*
 * The Mac OS 9 libraries' leftovers (os9left_*.c): the exports of MPLibrary,
 * DisplayLib, TextCommon, HTMLRenderingLib and the others that the other
 * tables (os9svc, os9rest) did not give.  os9left_export_fn (os9left.c)
 * looks a name up in each file's table.
 */
#ifndef OS9LEFT_H
#define OS9LEFT_H

#include "ppc_bridge.h"

void (*os9left_export_fn(const char *name))(ppc_cpu *);
bool os9left_data_known(const char *name);
gaddr os9left_data_export(const char *name);
void os9left_reset(void);

#define OS9LEFT_GROUP(n)                                      \
    extern const ppc_export os9left_##n##_exports[];          \
    extern const unsigned os9left_##n##_export_count;         \
    void os9left_##n##_reset(void)

OS9LEFT_GROUP(text);
OS9LEFT_GROUP(mp);
OS9LEFT_GROUP(display);
OS9LEFT_GROUP(threads);
OS9LEFT_GROUP(ui);
OS9LEFT_GROUP(sound);
OS9LEFT_GROUP(html);
OS9LEFT_GROUP(priv);
OS9LEFT_GROUP(misc);

/* Argument word n of a call (GPR slots r3 on, then the parameter area). */
#define L_ARG(c, n) ((n) < 8 ? (c)->gpr[3 + (n)] : rd32((c)->gpr[1] + 24 + 4 * (n)))
#define R(n) L_ARG(c, n)
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

#endif
