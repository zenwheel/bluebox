/*
 * Script Manager.  The System is 7.1 U.S., so Roman is the one script
 * installed; any other is installed when the System file has its bundle
 * ('itlb' with the script's code for its ID), and then the 2-byte scripts
 * (Japanese, the two Chineses, Korean) have their characters' first bytes
 * told apart, as their encodings define them.
 *
 * Text measurement goes through the current port's font; FindWord follows
 * the break tables of the U.S. 'itl2'.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    smSystemScript = -1, smCurrentScript = -2, smRoman = 0, smJapanese = 1, smTradChinese = 2, smKorean = 3,
    smArabic = 4, smHebrew = 5, smSimpChinese = 25, smUninterp = 32,
};
enum { smBreakWord = 0, smBreakChar = 1, smBreakOverflow = 2 };
enum { smCharPunct = 0x0000, smCharAscii = 0x0001, smCharEuro = 0x0007 };
enum {
    smCharKatakana = 0x0002, smCharHiragana = 0x0003, smCharIdeographic = 0x0004, smCharTwoByteGreek = 0x0005,
    smCharTwoByteRussian = 0x0006, smCharHangul = 0x000C, smCharJamo = 0x000D, smCharBopomofo = 0x000E,
};
enum { smIdeographicLevel1 = 0x0000, smIdeographicLevel2 = 0x0100 };
enum { smCharLower = 0x0000, smCharUpper = 0x4000, smChar2byte = 0x8000 };
enum { smPunctNormal = 0x0000, smPunctNumber = 0x0100, smPunctSymbol = 0x0200, smPunctBlank = 0x0300 };
enum { smFirstByte = -1, smSingleByte = 0, smLastByte = 1 };

/* Script Manager variables (GetScriptManagerVariable's selectors). */
enum {
    smVersion = 0, smMunged = 2, smEnabled = 4, smBidirect = 6, smFontForce = 8, smIntlForce = 10, smForced = 12,
    smDefault = 14, smSysScript = 18, smLastScript = 20, smKeyScript = 22, smSysRef = 24, smGenFlags = 30,
    smCharPortion = 34, smDoubleByte = 36, smRegionCode = 40, smKeyDisableState = 42,
};

/* KeyScript's codes below 0. */
enum {
    smKeyNextScript = -1, smKeySysScript = -2, smKeySwapScript = -3, smKeyDisableKybds = -6, smKeyEnableKybds = -7,
    smKeyToggleDirection = -9, smKeyDisableKybdSwitch = -12, smKeySetDirLeftRight = -15,
    smKeySetDirRightLeft = -16, smKeyRoman = -17,
};

static int32_t fond_size(int fond, int size) { return (int32_t)((uint32_t)fond << 16 | (uint16_t)size); }

/* The variables, set at startup from the system script's 'itlc'. */
static struct {
    int16_t sys_script, key_script, last_script, region;
    uint8_t font_force, intl_force, forced, defaulted, key_disable;
    int32_t munged, gen_flags;
} sm;

/* Whether a script is installed: Roman always, others by their bundle in
 * the System file. */
static bool script_enabled(int script)
{
    if (script == smRoman)
        return true;
    if (script < 0 || script > smUninterp)
        return false;
    int16_t saved = tb_CurResFile(), err = (int16_t)rd16(LM_ResErr);
    uint8_t load = rd8(LM_ResLoad);
    tb_UseResFile((int16_t)rd16(LM_SysMap));
    tb_SetResLoad(false);
    gaddr h = tb_Get1Resource((int32_t)fourcc("itlb"), (int16_t)script);
    tb_SetResLoad(load);
    tb_UseResFile(saved);
    wr16(LM_ResErr, (uint16_t)err);
    return h != 0;
}

static bool double_byte(int script)
{
    return script == smJapanese || script == smTradChinese || script == smKorean || script == smSimpChinese;
}

void scriptmgr_reset(void)
{
    memset(&sm, 0, sizeof sm);
    gaddr h = tb_GetResource((int32_t)fourcc("itlc"), 0);
    if (h && rd32(h) && tb_GetHandleSize(h) >= 12) {
        gaddr p = rd32(h);
        sm.sys_script = (int16_t)rd16(p);
        sm.font_force = rd8(p + 4);
        sm.intl_force = rd8(p + 5);
        sm.gen_flags = (int32_t)((uint32_t)rd8(p + 7) << 24);
        sm.region = (int16_t)rd16(p + 10);
    }
    if (!script_enabled(sm.sys_script))
        sm.sys_script = smRoman;
    sm.key_script = sm.last_script = sm.sys_script;
}

int32_t tb_GetScriptManagerVariable(int16_t verb)
{
    switch (verb) {
    case smVersion: return 0x0710;
    case smMunged: return sm.munged;
    case smEnabled: {
        int n = 0;
        for (int s = 0; s <= smUninterp; s++)
            n += script_enabled(s);
        return n;
    }
    case smBidirect: return script_enabled(smArabic) || script_enabled(smHebrew) ? 0xFF : 0;
    case smFontForce: return sm.font_force;
    case smIntlForce: return sm.intl_force;
    case smForced: return sm.forced;
    case smDefault: return sm.defaulted;
    case smSysScript: return sm.sys_script;
    case smLastScript: return sm.last_script;
    case smKeyScript: return sm.key_script;
    case smGenFlags: return sm.gen_flags;
    case smCharPortion: return 0x0333;
    case smDoubleByte: {
        static const int scripts[] = { smJapanese, smTradChinese, smKorean, smSimpChinese };
        for (int i = 0; i < 4; i++)
            if (script_enabled(scripts[i]))
                return 0xFF;
        return 0;
    }
    case smRegionCode: return sm.region;
    case smKeyDisableState: return sm.key_disable;
    default: return 0;
    }
}

/* The flags and the keyboard script may be set (the latter properly with
 * KeyScript); the rest are the Script Manager's own. */
int16_t tb_SetScriptManagerVariable(int16_t verb, int32_t param)
{
    switch (verb) {
    case smFontForce: sm.font_force = (uint8_t)param; break;
    case smIntlForce: sm.intl_force = (uint8_t)param; break;
    case smKeyScript: sm.key_script = (int16_t)param; break;
    case smGenFlags: sm.gen_flags = param; break;
    case smKeyDisableState: sm.key_disable = (uint8_t)param; break;
    default: return noErr;
    }
    sm.munged++;
    return noErr;
}

int32_t tb_GetScriptVariable(int16_t script, int16_t verb)
{
    if (script == smSystemScript)
        script = sm.sys_script;
    if (script != smRoman)
        return verb == 4 && script_enabled(script) ? 1 : 0; /* smScriptEnabled; the rest are its own */
    switch (verb) {
    case 0: return 0x0710;                  /* smScriptVersion */
    case 4: return 1;                       /* smScriptEnabled */
    case 12: return 0;                      /* smScriptSysFond: system font */
    case 14: return 3;                      /* smScriptAppFond: Geneva */
    case 34: return -16491;                 /* smScriptIcon */
    case 78: return fond_size(4, 9);        /* smScriptMonoFondSize: Monaco 9 */
    case 80: return fond_size(3, 12);       /* smScriptPrefFondSize */
    case 82: return fond_size(3, 9);        /* smScriptSmallFondSize */
    case 84: return fond_size(0, 12);       /* smScriptSysFondSize */
    case 86: return fond_size(3, 12);       /* smScriptAppFondSize */
    case 88: return fond_size(3, 9);        /* smScriptHelpFondSize */
    default: return 0;
    }
}

int16_t tb_SetScriptVariable(int16_t script, int16_t verb, int32_t param) { return noErr; }

/* ---- Script codes from fonts ----
 * As Inside Macintosh: Text describes (6-22): font 0 and 1 are the system
 * and application fonts; IDs from $4000 to $BFFF belong to non-Roman
 * scripts, 512 to each ((ID - 16384) / 512 + 1), the rest are Roman.  A
 * Roman font is the system script's when the font force flag is set (for
 * IntlScript, any font when the international resources selection flag
 * is), and a script that isn't installed gives way to the system script. */

static int16_t script_for(int16_t font, bool intl)
{
    sm.forced = sm.defaulted = 0;
    if (font == 0)
        font = (int16_t)rd16(LM_SysFontFam);
    else if (font == 1)
        font = (int16_t)rd16(LM_ApFontID);
    uint16_t id = (uint16_t)font;
    int script = id >= 0x4000 && id <= 0xBFFF ? (id - 0x4000) / 512 + 1 : smRoman;
    if (intl ? sm.intl_force && script != sm.sys_script : script == smRoman && sm.font_force && sm.sys_script) {
        script = sm.sys_script;
        sm.forced = 0xFF;
    }
    if (!script_enabled(script)) {
        script = sm.sys_script;
        sm.forced = 0;
        sm.defaulted = 0xFF;
    }
    return (int16_t)script;
}

static int16_t port_font(void)
{
    gaddr port = qd_port();
    return port ? (int16_t)rd16(port + GrafPort_txFont_off) : 0;
}

int16_t tb_FontScript(void) { return script_for(port_font(), false); }
int16_t tb_IntlScript(void) { return script_for(port_font(), true); }
int16_t tb_FontToScript(int16_t fontnum) { return script_for(fontnum, false); }

/* ---- The keyboard script ---- */

static bool key_allowed(int script)
{
    if (!script_enabled(script))
        return false;
    /* smKeyDisableKybds leaves Roman and the system script. */
    return sm.key_disable != 0xFF || script == smRoman || script == sm.sys_script;
}

static void set_key_script(int script)
{
    if (sm.key_disable == 1 || !key_allowed(script)) /* switching disabled, or not there */
        return;
    if (script != sm.key_script) {
        sm.last_script = sm.key_script;
        sm.key_script = (int16_t)script;
    }
    sm.munged++;
}

void tb_KeyScript(int16_t code)
{
    switch (code) {
    case smKeyNextScript: /* the next one, in script code order, round to the first */
        for (int i = 1; i <= smUninterp; i++) {
            int s = (sm.key_script + i) % (smUninterp + 1);
            if (key_allowed(s)) {
                set_key_script(s);
                break;
            }
        }
        break;
    case smKeySysScript: set_key_script(sm.sys_script); break;
    case smKeySwapScript: set_key_script(sm.last_script); break;
    case smKeyDisableKybds:
        sm.key_disable = 0xFF;
        if (!key_allowed(sm.key_script))
            set_key_script(sm.sys_script);
        break;
    case smKeyEnableKybds: sm.key_disable = 0; break;
    case smKeyDisableKybdSwitch: sm.key_disable = 1; break;
    case smKeyRoman: /* only when there is something else */
        if (tb_GetScriptManagerVariable(smEnabled) > 1)
            set_key_script(smRoman);
        break;
    case smKeyToggleDirection: /* line direction: with a bidirectional script */
    case smKeySetDirLeftRight:
    case smKeySetDirRightLeft:
        if (tb_GetScriptManagerVariable(smBidirect)) {
            int16_t dir = code == smKeySetDirLeftRight ? 0 : code == smKeySetDirRightLeft ? -1
                                                                                   : (int16_t)~tb_GetSysDirection();
            tb_SetSysDirection(dir);
        }
        break;
    default: /* a script code; the keyboard layouts and input methods are one each */
        if (code >= 0)
            set_key_script(code);
        break;
    }
}

/* ---- Character sizes ----
 * The 2-byte scripts' first bytes, as the Macintosh encodings define them
 * (the rest are 1-byte characters): Shift-JIS's, and EUC's for the others
 * up to $FC or $FE, where the Chinese encodings keep 1-byte symbols (©,
 * ™, …) at $FD-$FF. */

static bool first_byte(int script, uint8_t c)
{
    switch (script) {
    case smJapanese: return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
    case smTradChinese:
    case smSimpChinese: return c >= 0xA1 && c <= 0xFC;
    case smKorean: return c >= 0xA1 && c <= 0xFE;
    default: return false;
    }
}

/* The script a script-aware call means: the font script for
 * smCurrentScript, the system script for smSystemScript; Roman (all 1-byte)
 * for one not installed. */
static int text_script(int16_t script)
{
    int s = script == smCurrentScript ? tb_FontScript() : script == smSystemScript ? sm.sys_script : script;
    return script_enabled(s) ? s : smRoman;
}

/* From the start of the text, as Inside Macintosh says: offset 0 begins a
 * character, and each first byte takes the next with it. */
static int16_t byte_type(gaddr text, int offset, int script)
{
    if (!double_byte(script) || offset < 0)
        return smSingleByte;
    int i = 0;
    while (i < offset)
        i += first_byte(script, rd8(text + (uint32_t)i)) ? 2 : 1;
    if (i > offset)
        return smLastByte;
    return first_byte(script, rd8(text + (uint32_t)offset)) ? smFirstByte : smSingleByte;
}

int16_t tb_CharacterByteType(gaddr textBuf, int16_t textOffset, int16_t script)
{
    return byte_type(textBuf, textOffset, text_script(script));
}

int16_t tb_CharByte(gaddr textBuf, int16_t textOffset) { return tb_CharacterByteType(textBuf, textOffset, smCurrentScript); }

/* The length of the character at the start of text (1 or 2 bytes), for
 * the routines that must not cut one in two. */
static int char_len(int script, const uint8_t *text, int len)
{
    return len >= 2 && double_byte(script) && first_byte(script, text[0]) ? 2 : 1;
}

/* Always TRUE: 1 for a byte that begins a 2-byte character, 0 for the
 * others (every byte, in a 1-byte script). */
int8_t tb_FillParseTable(gaddr table, int16_t script)
{
    int s = text_script(script);
    for (int c = 0; c < 256; c++)
        wr8(table + (uint32_t)c, double_byte(s) && first_byte(s, (uint8_t)c) ? 1 : 0);
    return 1;
}

int8_t tb_ParseTable(gaddr table) { return tb_FillParseTable(table, smCurrentScript); }

/* CharacterType by the 'itl2' type list: the type in bits 0-3 of the
 * character's class entry, the punctuation class in bits 4-6, upper case
 * in bit 7. */
static int16_t char_type(uint8_t c)
{
    uint32_t n;
    uint8_t *p = intl_itl2(&n);
    uint32_t types = (uint32_t)p[12] << 8 | p[13], cls = (uint32_t)p[14] << 8 | p[15];
    uint32_t at = cls + c < n ? types + p[cls + c] : n;
    uint8_t t = at < n ? p[at] : 0;
    free(p);
    return (int16_t)((t & 0x0F) | (t & 0x70) << 4 | (t & 0x80 ? smCharUpper : smCharLower));
}

int host_text_decode(uint32_t enc, const uint8_t *in, size_t n, size_t *used, uint16_t *out, size_t cap,
                     size_t *produced);

/* A 2-byte character's type, from what the host's tables make of it (the
 * Macintosh text encodings have the script codes' numbers): kana,
 * ideographs (Shift-JIS's level 2 from $989F), Hangul and its letters,
 * Bopomofo, Greek and Cyrillic, full-width letters and digits; anything
 * else is punctuation. */
static int16_t wide_type(int script, const uint8_t ch[2])
{
    uint16_t u = 0;
    size_t used, got = 0;
    host_text_decode((uint32_t)script, ch, 2, &used, &u, 1, &got);
    int16_t t = smCharPunct;
    if (!got) /* not a character the encoding has */
        return (int16_t)(t | smChar2byte);
    if (u >= 0x3041 && u <= 0x309F)
        t = smCharHiragana;
    else if ((u >= 0x30A1 && u <= 0x30FF) || (u >= 0xFF66 && u <= 0xFF9F))
        t = smCharKatakana;
    else if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF) || (u >= 0xF900 && u <= 0xFAFF))
        t = smCharIdeographic |
            (script == smJapanese && (ch[0] << 8 | ch[1]) >= 0x989F ? smIdeographicLevel2 : smIdeographicLevel1);
    else if (u >= 0xAC00 && u <= 0xD7A3)
        t = smCharHangul;
    else if ((u >= 0x3131 && u <= 0x318E) || (u >= 0x1100 && u <= 0x11FF))
        t = smCharJamo;
    else if (u >= 0x3105 && u <= 0x312F)
        t = smCharBopomofo;
    else if (u >= 0x0391 && u <= 0x03C9)
        t = smCharTwoByteGreek | (u <= 0x03A9 ? smCharUpper : smCharLower);
    else if (u >= 0x0401 && u <= 0x045F)
        t = smCharTwoByteRussian | ((u <= 0x042F) ? smCharUpper : smCharLower);
    else if (u >= 0xFF21 && u <= 0xFF3A)
        t = smCharAscii | smCharUpper;
    else if (u >= 0xFF41 && u <= 0xFF5A)
        t = smCharAscii;
    else if (u >= 0xFF10 && u <= 0xFF19)
        t = smCharPunct | smPunctNumber;
    else if (u == 0x3000)
        t = smCharPunct | smPunctBlank;
    return (int16_t)(t | smChar2byte);
}

/* A byte of a 2-byte character, either one, gives the character's type;
 * 1-byte characters go by the U.S. 'itl2' (the 2-byte scripts' 'itl5'
 * tables are not here). */
int16_t tb_CharacterType(gaddr textbufp, int16_t offset, int16_t script)
{
    int s = text_script(script);
    int16_t bt = byte_type(textbufp, offset, s);
    if (bt == smSingleByte)
        return char_type(rd8(textbufp + (uint32_t)offset));
    uint8_t ch[2];
    gget(ch, textbufp + (uint32_t)(bt == smLastByte ? offset - 1 : offset), 2);
    return wide_type(s, ch);
}

int16_t tb_CharType(gaddr textbufp, int16_t offset) { return tb_CharacterType(textbufp, offset, smCurrentScript); }

/* ---- Measuring ---- */

static int width_of(gaddr text, int len)
{
    if (len <= 0)
        return 0;
    uint8_t *b = malloc((size_t)len);
    gget(b, text, (uint32_t)len);
    int w = qd_text_width(qd_port(), b, len);
    free(b);
    return w;
}

/* The character offset nearest a pixel position. */
static int pixel_to_char(gaddr text, int len, int pixel, bool *leading)
{
    int x = 0;
    for (int i = 0; i < len; i++) {
        int w = width_of(text + (uint32_t)i, 1);
        if (pixel < x + w) {
            bool left = pixel < x + w / 2;
            if (leading)
                *leading = left;
            return left ? i : i + 1;
        }
        x += w;
    }
    if (leading)
        *leading = false;
    return len;
}

int16_t tb_Pixel2Char(gaddr textbufp, int16_t len, int16_t slop, int16_t pixwidth, gaddr leftsidep)
{
    bool leading;
    int r = pixel_to_char(textbufp, len, pixwidth, &leading);
    if (leftsidep)
        wr8(leftsidep, leading ? 1 : 0);
    return (int16_t)r;
}

int16_t tb_PixelToChar(gaddr textBuf, int32_t textLen, int32_t slop, int32_t pixelWidth, gaddr leadingEdgep,
                       gaddr widthRemainingp, int16_t styleRunPosition, uint32_t numer, uint32_t denom)
{
    bool leading;
    int px = pixelWidth >> 16;
    int r = pixel_to_char(textBuf, textLen, px, &leading);
    if (leadingEdgep)
        wr8(leadingEdgep, leading ? 1 : 0);
    int total = width_of(textBuf, textLen);
    if (widthRemainingp)
        wr32(widthRemainingp, (uint32_t)(px > total ? (px - total) * 65536 : -65536));
    return (int16_t)r;
}

int16_t tb_Char2Pixel(gaddr textbufp, int16_t len, int16_t slop, int16_t offset, int8_t dir)
{
    return (int16_t)width_of(textbufp, offset < len ? offset : len);
}

int16_t tb_CharToPixel(gaddr textBuf, int32_t textLen, int32_t slop, int32_t offset, int16_t direction,
                       int16_t styleRunPosition, uint32_t numer, uint32_t denom)
{
    return (int16_t)width_of(textBuf, offset < textLen ? offset : textLen);
}

void tb_MeasureJust(gaddr textbufp, int16_t length, int16_t slop, gaddr charlocs)
{
    int x = 0;
    for (int i = 0; i <= length; i++) {
        wr16(charlocs + (uint32_t)i * 2, (uint16_t)x);
        if (i < length)
            x += width_of(textbufp + (uint32_t)i, 1);
    }
}

void tb_MeasureJustified(gaddr text, int32_t length, int32_t slop, gaddr charLocs, int16_t run_pos, uint32_t numer,
                         uint32_t denom)
{
    tb_MeasureJust(text, (int16_t)length, 0, charLocs);
}

void tb_DrawJust(gaddr textbufp, int16_t length, int16_t slop) { tb_DrawText(textbufp, 0, length); }

void tb_DrawJustified(gaddr textPtr, int32_t textLength, int32_t slop, int16_t styleRunPosition, uint32_t numer,
                      uint32_t denom)
{
    tb_DrawText(textPtr, 0, (int16_t)textLength);
}

int32_t tb_PortionLine(gaddr textPtr, int32_t textLen, int16_t styleRunPosition, uint32_t numer, uint32_t denom)
{
    return textLen << 16; /* justification slop shares by character count */
}

int16_t tb_FindScriptRun(gaddr textPtr, int32_t textLen, gaddr lenUsedp)
{
    if (lenUsedp)
        wr32(lenUsedp, (uint32_t)textLen);
    return smRoman;
}

/* ---- Words and lines ----
 * FindWord and FindWordBreaks run the state machines of an NBreakTable
 * (Inside Macintosh: Text, B-44): the 'itl2' word-selection table, its
 * line-break table when breaks is -1, or the program's own.  Each
 * character's class and the state pick an action: mark the character's
 * offset or not, and the next state, 0 to stop at the last mark. */

static uint32_t be16_of(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

typedef struct {
    const uint8_t *t;
    uint32_t n;
} break_table;

/* The action for character class k in state s of the state table at off. */
static int break_action(const break_table *b, uint32_t off, int s, uint8_t ch)
{
    uint32_t cls = be16_of(b->t + 4);
    int k = (int8_t)b->t[cls + ch]; /* negative: 2-byte characters, none here */
    uint32_t row = off + 2u * (uint32_t)s;
    if (k < 0 || row + 2 > b->n)
        return 0;
    uint32_t at = off + be16_of(b->t + row) + (uint32_t)k;
    return at < b->n ? b->t[at] : 0;
}

/* Forward from a boundary to the next one. */
static int break_forward(const break_table *b, const uint8_t *text, int len, int start)
{
    uint32_t off = be16_of(b->t + 10);
    int state = 1, mark = start;
    for (int i = start; i < len; i++) {
        int a = break_action(b, off, state, text[i]);
        if (a & 0x80)
            mark = i;
        state = (a & 0x7F) / 2;
        if (!state)
            return mark > start ? mark : start + 1;
    }
    return len;
}

/* Back from character i to a boundary at or before it. */
static int break_backward(const break_table *b, const uint8_t *text, int i)
{
    uint32_t off = be16_of(b->t + 8);
    int state = 1, mark = i;
    for (; i >= 0; i--) {
        int a = break_action(b, off, state, text[i]);
        if (a & 0x80)
            mark = i;
        state = (a & 0x7F) / 2;
        if (!state)
            return mark;
    }
    return 0;
}

void tb_FindWord(gaddr textbufp, int16_t length, int16_t offset, int8_t leftside, gaddr breaks, gaddr offsets)
{
    int a = 0, b = 0;
    if (length > 0 && textbufp) {
        uint32_t n;
        uint8_t *itl2 = intl_itl2(&n), *own = NULL;
        bool line = breaks == 0xFFFFFFFFu;
        uint32_t at = be16_of(itl2 + (line ? 24 : 22)); /* the word-selection or line-break table */
        break_table t = { itl2 + at, at < n ? n - at : 0 };
        if (breaks && !line && (rd8(breaks) & 0x80)) { /* the program's own NBreakTable */
            uint32_t tn = rd16(breaks + 14);
            if (tn >= 16 + 256) {
                own = malloc(tn);
                gget(own, breaks, tn);
                t = (break_table){ own, tn };
            }
        }
        uint8_t *text = malloc((size_t)length);
        gget(text, textbufp, (uint32_t)length);
        int o = offset;
        if (!leftside && o > 0)
            o--;
        if (o >= length)
            o = length - 1;
        if (o < 0)
            o = 0;
        if (t.n >= 16 + 256) {
            a = o < (int)be16_of(t.t + 12) ? 0 : break_backward(&t, text, o);
            for (;;) {
                b = break_forward(&t, text, length, a);
                if (b > o || b >= length)
                    break;
                a = b;
            }
        }
        free(text);
        free(own);
        free(itl2);
    }
    wr16(offsets, (uint16_t)a);
    wr16(offsets + 2, (uint16_t)b);
    for (int i = 2; i < 6; i++)
        wr16(offsets + (uint32_t)i * 2, 0);
}

void tb_HiliteText(gaddr textbufp, int16_t firstoffset, int16_t secondoffset, gaddr offsets)
{
    wr16(offsets, (uint16_t)firstoffset);
    wr16(offsets + 2, (uint16_t)secondoffset);
    for (int i = 2; i < 6; i++)
        wr16(offsets + (uint32_t)i * 2, 0);
}

/* A 2-byte character is measured and broken as one. */
uint8_t tb_StyledLineBreak(gaddr textp, int32_t length, int32_t text_start, int32_t text_end, int32_t flags,
                           gaddr text_width_fp, gaddr text_offset)
{
    int32_t avail = (int32_t)rd32(text_width_fp) >> 16;
    bool first_on_line = rd32(text_offset) != 0;
    int script = text_script(smCurrentScript);
    int x = 0, last_break = -1;
    for (int32_t i = text_start; i < text_end;) {
        uint8_t c = rd8(textp + (uint32_t)i);
        if (c == '\r') {
            wr32(text_offset, (uint32_t)(i + 1));
            return smBreakWord;
        }
        int n = i + 1 < text_end && double_byte(script) && first_byte(script, c) ? 2 : 1;
        int w = width_of(textp + (uint32_t)i, n);
        if (x + w > avail && c != ' ') {
            int32_t at = last_break > text_start ? last_break : (first_on_line ? (i > text_start ? i : i + n) : text_start);
            wr32(text_offset, (uint32_t)at);
            return last_break > text_start || !first_on_line ? smBreakWord : smBreakChar;
        }
        x += w;
        if (c == ' ')
            last_break = i + 1;
        i += n;
    }
    wr32(text_offset, (uint32_t)text_end);
    wr32(text_width_fp, (uint32_t)((avail - x) << 16));
    return smBreakOverflow;
}

int32_t tb_VisibleLength(gaddr textp, int32_t len)
{
    while (len > 0) {
        uint8_t c = rd8(textp + (uint32_t)(len - 1));
        if (c != ' ' && c != '\t' && c != '\r' && c != 0xCA)
            break;
        len--;
    }
    return len;
}

/* Truncate a Pascal string to fit width, ending (truncEnd) or middle with
 * an ellipsis, keeping 2-byte characters whole. */
int16_t tb_TruncString(int16_t width, gaddr string, int16_t code)
{
    int n = rd8(string);
    if (width_of(string + 1, n) <= width)
        return 0;
    uint8_t buf[256];
    gget(buf, string + 1, (uint32_t)n);
    /* Where each character starts (and the end). */
    int script = text_script(smCurrentScript), starts[257], chars = 0;
    for (int i = 0; i < n; i += char_len(script, buf + i, n - i))
        starts[chars++] = i;
    starts[chars] = n;
    int ell = qd_text_width(qd_port(), (const uint8_t *)"\xC9", 1);
    if (code == 0) { /* truncEnd */
        int k = chars;
        while (k > 0 && qd_text_width(qd_port(), buf, starts[k]) + ell > width)
            k--;
        n = starts[k];
        buf[n++] = 0xC9;
    } else { /* truncMiddle */
        int keep = chars;
        while (keep > 0) {
            int left = starts[(keep + 1) / 2], right = n - starts[chars - keep / 2];
            uint8_t t[256];
            memcpy(t, buf, (size_t)left);
            t[left] = 0xC9;
            memcpy(t + left + 1, buf + n - right, (size_t)right);
            if (qd_text_width(qd_port(), t, left + right + 1) <= width) {
                memcpy(buf, t, (size_t)(left + right + 1));
                n = left + right + 1;
                break;
            }
            keep--;
        }
        if (keep == 0) {
            buf[0] = 0xC9;
            n = 1;
        }
    }
    wr8(string, (uint8_t)n);
    gput(string + 1, buf, (uint32_t)n);
    return 1;
}

/* Transliterate: case changes only (Roman has nothing else to convert). */
int16_t tb_Transliterate(gaddr srch, gaddr dsth, int16_t target, int32_t srcmask)
{
    int32_t n = tb_GetHandleSize(srch);
    tb_SetHandleSize(dsth, n);
    for (int32_t i = 0; i < n; i++) {
        uint8_t c = rd8(rd32(srch) + (uint32_t)i);
        if ((target & 0x60) == 0x20 && c >= 'a' && c <= 'z') /* smTransUpper */
            c = (uint8_t)(c - 32);
        else if ((target & 0x60) == 0x40 && c >= 'A' && c <= 'Z') /* smTransLower */
            c = (uint8_t)(c + 32);
        wr8(rd32(dsth) + (uint32_t)i, c);
    }
    return noErr;
}

int16_t tb_TransliterateText(gaddr srch, gaddr dsth, int16_t target, int32_t srcmask, int16_t script)
{
    return tb_Transliterate(srch, dsth, target, srcmask);
}

/* Replace every occurrence of key (a Str15) in base_text with subst_text. */
int16_t tb_ReplaceText(gaddr base_text, gaddr subst_text, gaddr key)
{
    int kl = rd8(key);
    if (kl == 0)
        return 0;
    uint8_t k[256]; /* a Str15 by the book, but programs pass longer strings */
    gget(k, key + 1, (uint32_t)kl);
    int32_t sl = tb_GetHandleSize(subst_text);
    if (sl < 0)
        return 0;
    int count = 0;
    for (int32_t pos = 0;;) {
        int32_t n = tb_GetHandleSize(base_text);
        int32_t found = -1;
        if (n < 0)
            break;
        for (int32_t i = pos; i + kl <= n; i++) {
            int j = 0;
            while (j < kl && rd8(rd32(base_text) + (uint32_t)(i + j)) == k[j])
                j++;
            if (j == kl) {
                found = i;
                break;
            }
        }
        if (found < 0)
            break;
        uint8_t *rest = malloc((size_t)(n - found - kl) + 1);
        gget(rest, rd32(base_text) + (uint32_t)(found + kl), (uint32_t)(n - found - kl));
        tb_SetHandleSize(base_text, n - kl + sl);
        gmemcpy(rd32(base_text) + (uint32_t)found, rd32(subst_text), (uint32_t)sl);
        gput(rd32(base_text) + (uint32_t)(found + sl), rest, (uint32_t)(n - found - kl));
        free(rest);
        pos = found + sl;
        count++;
    }
    return (int16_t)count;
}

/* ---- Dates ---- */

#define MAC_EPOCH_OFFSET 2082844800LL

/* LongDateRec: era, year, month, day, hour, minute, second, dayOfWeek,
 * dayOfYear, weekOfYear, pm, res1-3 (each INTEGER). */
void tb_LongSecondsToDate(gaddr secs_inp, gaddr ldatep)
{
    int64_t secs = ((int64_t)(int32_t)rd32(secs_inp) << 32) | rd32(secs_inp + 4);
    time_t t = (time_t)(secs - MAC_EPOCH_OFFSET);
    struct tm tm;
    gmtime_r(&t, &tm);
    int16_t f[14] = { 0, (int16_t)(tm.tm_year + 1900), (int16_t)(tm.tm_mon + 1), (int16_t)tm.tm_mday,
                      (int16_t)tm.tm_hour, (int16_t)tm.tm_min, (int16_t)tm.tm_sec, (int16_t)(tm.tm_wday + 1),
                      (int16_t)(tm.tm_yday + 1), (int16_t)(tm.tm_yday / 7 + 1), (int16_t)(tm.tm_hour >= 12), 0, 0, 0 };
    for (int i = 0; i < 14; i++)
        wr16(ldatep + (uint32_t)i * 2, (uint16_t)f[i]);
}

void tb_LongDateToSeconds(gaddr ldatep, gaddr secs_outp)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (int16_t)rd16(ldatep + 2) - 1900;
    tm.tm_mon = (int16_t)rd16(ldatep + 4) - 1;
    tm.tm_mday = (int16_t)rd16(ldatep + 6);
    tm.tm_hour = (int16_t)rd16(ldatep + 8);
    tm.tm_min = (int16_t)rd16(ldatep + 10);
    tm.tm_sec = (int16_t)rd16(ldatep + 12);
    int64_t secs = (int64_t)timegm(&tm) + MAC_EPOCH_OFFSET;
    wr32(secs_outp, (uint32_t)(secs >> 32));
    wr32(secs_outp + 4, (uint32_t)secs);
}

int16_t tb_InitDateCache(gaddr theCache) { return noErr; }

/* The system's line direction: TESysJust. */
int16_t tb_GetSysDirection(void) { return (int16_t)rd16(LM_TESysJust); }
void tb_SetSysDirection(int16_t just) { wr16(LM_TESysJust, (uint16_t)just); }
