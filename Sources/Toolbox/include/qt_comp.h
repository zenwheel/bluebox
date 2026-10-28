/* QuickTime's components of our own, in C (qt_comp.c). */
#ifndef QT_COMP_H
#define QT_COMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "qt_calls.h"

enum {
    qtcBadInstance = (int32_t)0x80008001, /* badComponentInstance */
    qtcBadSelector = (int32_t)0x80008002, /* badComponentSelector */
    kQTCOpen = -1, kQTCClose = -2, kQTCCanDo = -3, kQTCVersion = -4, kQTCRegister = -5, kQTCTarget = -6,
    kQTCUnregister = -7,
};

typedef struct qtc_class qtc_class;

/* A call to one of our instances, its arguments in the order declared
 * (a byte or a short as its value). */
typedef struct {
    uint32_t ci;
    int16_t what;
    gaddr params;
    const qt_call_info *call; /* NULL for the standard selectors */
    uint32_t a[32];
    int n;
    void *self;               /* the instance's state (the class's size, zeroed at open) */
    const qtc_class *cls;
    uint32_t target;          /* ComponentSetTarget's, or the instance itself */
} qtc_call;

struct qtc_class {
    uint32_t type, subtype, manufacturer, flags;
    const char *name, *info;
    int32_t version;
    const char *families[8];  /* the call families it takes (qt_calls.tsv) */
    size_t size;
    /* The call (standard selectors included: open and close after the
     * state is made and before it goes; can-do gets the selector in a[0]
     * and answers for its own calls).  Unknown calls: qtcBadSelector. */
    int32_t (*fn)(qtc_call *k);
    uint32_t component;       /* set by qtc_register */
};

uint32_t qtc_register(qtc_class *cls);
/* Our instance's state and class, or NULL. */
void *qtc_self(uint32_t ci);
const qtc_class *qtc_class_of(uint32_t ci);
/* An instance of the class (opened through the Component Manager), or 0. */
uint32_t qtc_open(const qtc_class *cls);

/* Call any instance (ours or a program's) as its callers do: the call of
 * the family and selector, with the arguments given in declared order. */
int32_t qtc_call_instance(uint32_t ci, const char *family, int16_t what, const uint32_t *args);
int32_t qtc_call_named(uint32_t ci, const char *name, const uint32_t *args);

#endif
