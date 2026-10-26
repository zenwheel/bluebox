/* The low-memory accessor table (lowmem_acc_data.c) and its one runner. */
#ifndef LOWMEM_ACC_H
#define LOWMEM_ACC_H

#include <stdint.h>

#include "ppc.h"

typedef enum {
    LM_GETU,    /* r3 = the value, zero-extended */
    LM_GETS,    /* r3 = the value, sign-extended */
    LM_SET,     /* the value in r3, its low size bytes, stored */
    LM_ADDR,    /* r3 = the address (an array or a record is left where it is) */
    LM_COPYIN,  /* size bytes copied from *r3 into low memory */
    LM_OUT,     /* size bytes of low memory copied to *r3 */
    LM_IGETU,   /* r3 = element (short)r3 of the array at addr, stride bytes apart */
    LM_IGETS,   /* the same, sign-extended */
    LM_ISET,    /* element (short)r4 of the array = r3 */
} lm_kind;

typedef struct {
    const char *name;
    uint8_t kind;   /* lm_kind */
    uint8_t size;   /* the width of a value (1, 2, 4) or the bytes of a copy */
    uint8_t stride; /* an array element's distance */
    uint16_t addr;
} lm_entry;

void lmacc_run(const lm_entry *e, ppc_cpu *c);

#endif
