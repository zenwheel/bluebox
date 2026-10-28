/*
 * QuickTime's component calls from PowerPC programs, and the exports of
 * the QuickTime libraries that are ours.
 *
 * Most of QuickTimeLib is calls to component instances: the movie
 * exporters and importers, data handlers, media handlers and their derived
 * calls (TextMedia..., SpriteMedia...), image codecs, graphics importers
 * and exporters, the music components, clocks, tweens and the streaming
 * components.  Each is the instance, then its arguments, then a selector:
 * the 68k glue pushes them and calls _ComponentDispatch, and so does this,
 * so a call reaches the component wherever it is (one of ours, or the
 * program's own 68k or PowerPC component).  An instance that isn't there
 * answers badComponentInstance, as QuickTime's Component Manager did.
 *
 * The calls, their selectors and argument sizes are qt_calls_gen.c, from
 * the Universal Interfaces' declarations (Generators/qt_calls.py).
 */
#include <string.h>

#include "ppc_bridge.h"
#include "qt_calls.h"
#include "tb_internal.h"

uint32_t qt_call_param_size(const char *sizes)
{
    uint32_t n = 0;
    for (const char *s = sizes; *s; s++)
        n += *s == '4' || *s == 'f' ? 4 : 2;
    return n;
}

/* Argument word k of a PowerPC call: r3 to r10, then the caller's
 * parameter area. */
static uint32_t ppc_arg(const ppc_cpu *c, int k)
{
    return k < 8 ? c->gpr[3 + k] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)k);
}

void qt_call_ppc(ppc_cpu *c, unsigned index)
{
    const qt_call_info *q = &qt_calls[index];
    ppc_trap_begin();
    ppc_trap_push(4, 0);              /* the result */
    ppc_trap_push(4, c->gpr[3]);      /* the instance */
    int fpr = 1;
    for (int i = 0; q->sizes[i]; i++) {
        if (q->sizes[i] == 'f') { /* a float: in the next floating-point register */
            float f = (float)ppc_fpr(c, fpr++);
            uint32_t bits;
            memcpy(&bits, &f, 4);
            ppc_trap_push(4, bits);
        } else {
            ppc_trap_push(q->sizes[i] - '0', ppc_arg(c, 1 + i));
        }
    }
    ppc_trap_push(4, qt_call_param_size(q->sizes) << 16 | q->what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    c->gpr[3] = r;
    c->pc = c->lr;
}

const qt_call_info *qt_call_find(const char *family, int16_t what)
{
    for (unsigned i = 0; i < qt_call_count; i++)
        if (qt_calls[i].what == (uint16_t)what && !strcmp(qt_calls[i].family, family))
            return &qt_calls[i];
    return NULL;
}

void (*qt_export_fn(const char *name))(ppc_cpu *)
{
    unsigned lo = 0, hi = qt_call_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int d = strcmp(qt_calls[mid].name, name);
        if (!d)
            return qt_calls[mid].fn;
        if (d < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
