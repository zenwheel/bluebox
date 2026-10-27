/*
 * GetFormatOrder (the Script Manager's ScriptUtil, QuickdrawText.h): the
 * style runs of a line in display order, for 68k and PowerPC programs.
 */
#include "ppc_bridge.h"
#include "tb_internal.h"

/* rlDirProc is pascal Boolean (short run, void *dirParam): a bare
 * pointer is a transition vector when PowerPC code called, and 68k code
 * when 68k code did (a PowerPC program's 68k snippets call from 68k). */
static bool run_is_right_to_left(gaddr proc, int run, gaddr param, bool from_ppc)
{
    uint32_t args[2] = { (uint32_t)run, param };
    uint32_t r;
    if (from_ppc && !mm_is_descriptor(proc))
        r = ppc_call(proc, args, 2);
    else
        r = mm_call(proc, 0x390, args, 2);
    return (r & 0xFF) != 0;
}

/* The runs go right to left when rlDirProc says so.  A line running left
 * to right reverses each stretch of those, a right-to-left line reverses
 * each stretch of the others and then the whole. */
static void format_order(gaddr ordering, int16_t firstFormat, int16_t lastFormat, int8_t lineRight, gaddr rlDirProc,
                         gaddr dirParam, bool from_ppc)
{
    int first = firstFormat, n = lastFormat - firstFormat + 1;
    if (n <= 0 || n > 1024 || !ordering)
        return;
    int idx[1024];
    bool rtl[1024];
    for (int i = 0; i < n; i++) {
        idx[i] = first + i;
        rtl[i] = rlDirProc ? run_is_right_to_left(rlDirProc, first + i, dirParam, from_ppc) : false;
    }
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && rtl[j] == rtl[i])
            j++;
        if (rtl[i] != (lineRight != 0))
            for (int a = i, b = j - 1; a < b; a++, b--) {
                int t = idx[a];
                idx[a] = idx[b];
                idx[b] = t;
            }
        i = j;
    }
    for (int i = 0; i < n; i++)
        wr16(ordering + 2 * (uint32_t)i, (uint16_t)idx[lineRight ? n - 1 - i : i]);
}

void tb_GetFormatOrder(gaddr ordering, int16_t firstFormat, int16_t lastFormat, int8_t lineRight, gaddr rlDirProc,
                       gaddr dirParam)
{
    format_order(ordering, firstFormat, lastFormat, lineRight, rlDirProc, dirParam, false);
}

/* The PowerPC export: the same, called from PowerPC code. */
void ppc_GetFormatOrder(ppc_cpu *c)
{
    format_order(c->gpr[3], (int16_t)c->gpr[4], (int16_t)c->gpr[5], (int8_t)c->gpr[6], c->gpr[7], c->gpr[8], true);
    c->pc = c->lr;
}
