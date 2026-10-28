/*
 * UnicodeConverter's calls under their Text Encoding Converter 1.0 names
 * (Mac OS 8.5), which Mac OS 9 still exports: ConvertTextToUnicode and
 * ConvertUnicodeToText are ConvertFromTextToUnicode and
 * ConvertFromUnicodeToText without the separate array the converted
 * offsets go to (they replace the offsets given, in place), and the
 * "plain" run conversions are the run conversions with no offsets.
 */
#include <stdlib.h>

#include "os9rest.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

/* The ten arguments both old calls take. */
static void convert_in_place(ppc_cpu *c, const char *forward)
{
    uint32_t a[10];
    for (int i = 0; i < 10; i++)
        a[i] = os9rest_word(c, (unsigned)i);
    uint32_t count = a[4], offsets = a[5];
    gaddr temp = count && offsets ? tb_NewPtr((int32_t)(count * 4), true, false) : 0;
    gaddr actual = tb_NewPtr(4, true, false);
    if ((count && offsets && !temp) || !actual) {
        if (temp)
            tb_DisposePtr(temp);
        if (actual)
            tb_DisposePtr(actual);
        RETURN(memFullErr);
    }
    wr32(actual, 0);
    uint32_t args[12] = { a[0], a[1], a[2], a[3], count, offsets, actual, temp, a[6], a[7], a[8], a[9] };
    uint32_t r = os9rest_call(c, forward, args, 12);
    uint32_t n = rd32(actual);
    for (uint32_t i = 0; temp && i < n && i < count; i++)
        wr32(offsets + 4 * i, rd32(temp + 4 * i));
    tb_DisposePtr(actual);
    if (temp)
        tb_DisposePtr(temp);
    RETURN(r);
}

static void tx_ConvertTextToUnicode(ppc_cpu *c) { convert_in_place(c, "ConvertFromTextToUnicode"); }
static void tx_ConvertUnicodeToText(ppc_cpu *c) { convert_in_place(c, "ConvertFromUnicodeToText"); }

/* ConvertPlainUnicodeToTextRun(info, len, str, flags, outBufLen, *inputRead,
 * *outputLen, outStr, runBufLen, *runOutLen, runs): no offsets in or out. */
static void plain_runs(ppc_cpu *c, const char *forward)
{
    uint32_t a[11];
    for (int i = 0; i < 11; i++)
        a[i] = os9rest_word(c, (unsigned)i);
    uint32_t args[15] = { a[0], a[1], a[2], a[3], 0, 0, 0, 0, a[4], a[5], a[6], a[7], a[8], a[9], a[10] };
    RETURN(os9rest_call(c, forward, args, 15));
}

static void tx_ConvertPlainUnicodeToTextRun(ppc_cpu *c) { plain_runs(c, "ConvertFromUnicodeToTextRun"); }
static void tx_ConvertPlainUnicodeToScriptCodeRun(ppc_cpu *c) { plain_runs(c, "ConvertFromUnicodeToScriptCodeRun"); }

/* TECGetOption and TECSetOption: Mac OS 9 has no options to give or take
 * and answers unimpErr to both. */
static void tx_TECOption(ppc_cpu *c) { RETURN(-4); }

const ppc_export os9rest_text_exports[] = {
    { "TECGetOption", tx_TECOption },
    { "TECSetOption", tx_TECOption },
    { "ConvertTextToUnicode", tx_ConvertTextToUnicode },
    { "ConvertUnicodeToText", tx_ConvertUnicodeToText },
    { "ConvertPlainUnicodeToTextRun", tx_ConvertPlainUnicodeToTextRun },
    { "ConvertPlainUnicodeToScriptCodeRun", tx_ConvertPlainUnicodeToScriptCodeRun },
};
const unsigned os9rest_text_export_count = sizeof os9rest_text_exports / sizeof os9rest_text_exports[0];
