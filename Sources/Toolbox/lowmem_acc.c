/*
 * What each low-memory accessor of lowmem_acc_data.c does: Mac OS 8's own
 * were a load or a store, an address, or a BlockMove, on the real globals.
 */
#include <stdint.h>

#include "lowmem_acc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

static uint32_t read_sized(gaddr a, unsigned size)
{
    return size == 1 ? rd8(a) : size == 2 ? rd16(a) : rd32(a);
}

static void write_sized(gaddr a, unsigned size, uint32_t v)
{
    if (size == 1)
        wr8(a, (uint8_t)v);
    else if (size == 2)
        wr16(a, (uint16_t)v);
    else
        wr32(a, v);
}

static uint32_t extend(uint32_t v, unsigned size)
{
    return size == 1 ? (uint32_t)(int32_t)(int8_t)v : size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}

void lmacc_run(const lm_entry *e, ppc_cpu *c)
{
    gaddr a = e->addr;
    switch ((lm_kind)e->kind) {
    case LM_GETU:
        c->gpr[3] = read_sized(a, e->size);
        break;
    case LM_GETS:
        c->gpr[3] = extend(read_sized(a, e->size), e->size);
        break;
    case LM_SET:
        write_sized(a, e->size, c->gpr[3]);
        break;
    case LM_ADDR:
        c->gpr[3] = a;
        break;
    case LM_COPYIN:
        ppc_copy(a, c->gpr[3], e->size);
        break;
    case LM_OUT:
        ppc_copy(c->gpr[3], a, e->size);
        break;
    case LM_IGETU:
    case LM_IGETS: {
        gaddr at = a + (gaddr)((int32_t)(int16_t)c->gpr[3] * (int32_t)e->stride);
        uint32_t v = read_sized(at, e->size);
        c->gpr[3] = e->kind == LM_IGETS ? extend(v, e->size) : v;
        break;
    }
    case LM_ISET:
        /* Mac OS 8's LMSetOmegaSANE stored the index where the value
         * belonged; the value is what is stored here, as for the others. */
        write_sized(a + (gaddr)((int32_t)(int16_t)c->gpr[4] * (int32_t)e->stride), e->size, c->gpr[3]);
        break;
    }
    c->pc = c->lr;
}
