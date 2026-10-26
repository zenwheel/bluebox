/*
 * FontManager as Mac OS 9 exports it.  Most of its names are the QuickDraw
 * text and Script Manager calls under the names the Font Manager's own code
 * called them by (NQD* for the text calls, X_* for the justification and
 * hit-testing, __* for the Font Manager's traps); the rest is registering
 * the fonts in a file.  The TrueType scaler (fs_*) and the glyph-record
 * calls are internal to the Font Manager and ATSUI and not here.
 */
#include <string.h>

#include "ppc_bridge.h"
#include "os9svc.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

const os9_alias os9_font_aliases[] = {
    { "NQDCharExtra", "CharExtra" }, { "NQDCharWidth", "CharWidth" }, { "NQDDrawChar", "DrawChar" },
    { "NQDDrawString", "DrawString" }, { "NQDDrawText", "DrawText" }, { "NQDGetFontInfo", "GetFontInfo" },
    { "NQDMeasureText", "MeasureText" }, { "NQDSpaceExtra", "SpaceExtra" }, { "NQDStdText", "StdText" },
    { "NQDStdTxMeas", "StdTxMeas" }, { "NQDStringWidth", "StringWidth" }, { "NQDTextFace", "TextFace" },
    { "NQDTextFont", "TextFont" }, { "NQDTextMode", "TextMode" }, { "NQDTextSize", "TextSize" },
    { "NQDTextWidth", "TextWidth" },
    { "X_Char2Pixel", "Char2Pixel" }, { "X_CharToPixel", "CharToPixel" }, { "X_DrawJust", "DrawJust" },
    { "X_DrawJustified", "DrawJustified" }, { "X_GetFormatOrder", "GetFormatOrder" },
    { "X_HiliteText", "HiliteText" }, { "X_MeasureJust", "MeasureJust" },
    { "X_MeasureJustified", "MeasureJustified" }, { "X_Pixel2Char", "Pixel2Char" },
    { "X_PixelToChar", "PixelToChar" }, { "X_PortionLine", "PortionLine" }, { "X_PortionText", "PortionText" },
    { "X_VisibleLength", "VisibleLength" },
    { "__FMSwapFont", "FMSwapFont" }, { "__FlushFonts", "FlushFonts" }, { "__FontMetrics", "FontMetrics" },
    { "__GetFNum", "GetFNum" }, { "__GetFontName", "GetFontName" },
    { "__GetOutlinePreferred", "GetOutlinePreferred" }, { "__GetPreserveGlyph", "GetPreserveGlyph" },
    { "__InitFonts", "InitFonts" }, { "__IsOutline", "IsOutline" },
    { "__OutlineMetrics", "OutlineMetrics" },
    { "__RealFont", "RealFont" }, { "__SetFScaleDisable", "SetFScaleDisable" },
    { "__SetFontLock", "SetFontLock" }, { "__SetFractEnable", "SetFractEnable" },
    { "__SetOutlinePreferred", "SetOutlinePreferred" }, { "__SetPreserveGlyph", "SetPreserveGlyph" },
    { "FastFixMul", "FixMul" },
};
const unsigned os9_font_alias_count = sizeof os9_font_aliases / sizeof os9_font_aliases[0];

/* ---- Font files ---- */

/* The files whose fonts are registered: each is open as a resource file, so
 * its FONT, FOND and sfnt resources are in the chain the Font Manager looks
 * through. */
#define MAX_FONT_FILES 16
static struct {
    uint8_t spec[70]; /* the FSSpec: vRefNum, parID, name */
    int16_t refnum;
} font_files[MAX_FONT_FILES];
static int nfont_files;

static void spec_read(gaddr spec, uint8_t *out)
{
    for (int i = 0; i < 70; i++)
        out[i] = rd8(spec + i);
}

/* RegisterFontsInFile(spec): OSErr */
static void font_RegisterFontsInFile(ppc_cpu *c)
{
    gaddr spec = R(0);
    if (!spec)
        RETURN(paramErr);
    uint8_t s[70];
    spec_read(spec, s);
    for (int i = 0; i < nfont_files; i++)
        if (!memcmp(font_files[i].spec, s, 70))
            RETURN(noErr);
    if (nfont_files == MAX_FONT_FILES)
        RETURN(memFullErr);
    int16_t rn = tb_FSpOpenResFile(spec, 1);
    if (rn <= 0) {
        int16_t err = tb_ResError();
        RETURN(err ? err : fnfErr);
    }
    memcpy(font_files[nfont_files].spec, s, 70);
    font_files[nfont_files++].refnum = rn;
    RETURN(noErr);
}

/* UnregisterFontsInFile(spec): OSErr; the file's fonts leave the chain. */
static void font_UnregisterFontsInFile(ppc_cpu *c)
{
    gaddr spec = R(0);
    if (!spec)
        RETURN(paramErr);
    uint8_t s[70];
    spec_read(spec, s);
    for (int i = 0; i < nfont_files; i++)
        if (!memcmp(font_files[i].spec, s, 70)) {
            tb_CloseResFile(font_files[i].refnum);
            memmove(&font_files[i], &font_files[i + 1], (size_t)(nfont_files - i - 1) * sizeof font_files[0]);
            nfont_files--;
            RETURN(noErr);
        }
    RETURN(fnfErr);
}

const ppc_export os9_font_exports[] = {
    { "RegisterFontsInFile", font_RegisterFontsInFile },
    { "UnregisterFontsInFile", font_UnregisterFontsInFile },
};
const unsigned os9_font_export_count = sizeof os9_font_exports / sizeof os9_font_exports[0];
