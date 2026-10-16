/*
 * Script Manager, for the Roman script only (System 7.1 U.S.).
 *
 * Text measurement goes through the current port's font; word and line
 * breaks follow the Roman rules (letters and digits make words, spaces
 * separate them).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { smRoman = 0 };
enum { smBreakWord = 0, smBreakChar = 1, smBreakOverflow = 2 };
enum { smCharPunct = 0x0000, smCharAscii = 0x0001, smCharEuro = 0x0007 };
enum { smCharLower = 0x0000, smCharUpper = 0x4000 };
enum { smPunctNormal = 0x0000, smPunctNumber = 0x0100, smPunctSymbol = 0x0200, smPunctBlank = 0x0300 };

static int32_t fond_size(int fond, int size) { return (int32_t)((uint32_t)fond << 16 | (uint16_t)size); }

int32_t tb_GetScriptManagerVariable(int16_t verb)
{
    switch (verb) {
    case 0: return 0x0710;      /* smVersion */
    case 4: return 1;           /* smEnabled: scripts enabled */
    case 34: return 0x0333;     /* smCharPortion */
    default: return 0;          /* Roman everywhere, US region */
    }
}

int16_t tb_SetScriptManagerVariable(int16_t verb, int32_t param) { return noErr; }

int32_t tb_GetScriptVariable(int16_t script, int16_t verb)
{
    if (script != smRoman)
        return 0;
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
int16_t tb_FontScript(void) { return smRoman; }
int16_t tb_IntlScript(void) { return smRoman; }
int16_t tb_FontToScript(int16_t fontnum) { return smRoman; }
void tb_KeyScript(int16_t scriptcode) {}
int16_t tb_CharByte(gaddr textBuf, int16_t textOffset) { return 0; } /* single-byte script */
int16_t tb_CharacterByteType(gaddr textBuf, int16_t textOffset, int16_t script) { return 0; }

static bool is_alpha(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x80 && c <= 0x9F) || c == 0xA7 || c == 0xAE ||
           c == 0xAF || c == 0xBE || c == 0xBF || (c >= 0xCB && c <= 0xCF) || c == 0xD8 || c == 0xD9;
}
static bool is_word_char(uint8_t c) { return is_alpha(c) || (c >= '0' && c <= '9') || c == '\''; }

static int16_t char_type(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return smCharAscii | smCharUpper;
    if (c >= 'a' && c <= 'z')
        return smCharAscii | smCharLower;
    if (is_alpha(c)) {
        bool upper = (c >= 0x80 && c <= 0x86) || c == 0xAE || c == 0xAF || (c >= 0xCB && c <= 0xCE) || c == 0xD9;
        return (int16_t)(smCharEuro | (upper ? smCharUpper : smCharLower));
    }
    if (c >= '0' && c <= '9')
        return smCharPunct | smPunctNumber;
    if (c == ' ' || c == '\t' || c == '\r' || c == 0xCA)
        return smCharPunct | smPunctBlank;
    if (c == '$' || c == '%' || c == '+' || c == '<' || c == '=' || c == '>' || c == '^' || c == '|' || c == '~' ||
        c >= 0xA0)
        return smCharPunct | smPunctSymbol;
    return smCharPunct | smPunctNormal;
}

int16_t tb_CharType(gaddr textbufp, int16_t offset) { return char_type(rd8(textbufp + (uint32_t)offset)); }
int16_t tb_CharacterType(gaddr textbufp, int16_t offset, int16_t script) { return tb_CharType(textbufp, offset); }

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

/* ---- Words and lines ---- */

void tb_FindWord(gaddr textbufp, int16_t length, int16_t offset, int8_t leftside, gaddr breaks, gaddr offsets)
{
    int o = offset;
    if (!leftside && o > 0)
        o--;
    if (o >= length)
        o = length > 0 ? length - 1 : 0;
    int a = o, b = o;
    if (length > 0) {
        bool word = is_word_char(rd8(textbufp + (uint32_t)o));
        while (a > 0 && is_word_char(rd8(textbufp + (uint32_t)(a - 1))) == word &&
               (word || rd8(textbufp + (uint32_t)(a - 1)) == rd8(textbufp + (uint32_t)o)))
            a--;
        while (b < length && is_word_char(rd8(textbufp + (uint32_t)b)) == word &&
               (word || rd8(textbufp + (uint32_t)b) == rd8(textbufp + (uint32_t)o)))
            b++;
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

uint8_t tb_StyledLineBreak(gaddr textp, int32_t length, int32_t text_start, int32_t text_end, int32_t flags,
                           gaddr text_width_fp, gaddr text_offset)
{
    int32_t avail = (int32_t)rd32(text_width_fp) >> 16;
    bool first_on_line = rd32(text_offset) != 0;
    int x = 0, last_break = -1;
    for (int32_t i = text_start; i < text_end; i++) {
        uint8_t c = rd8(textp + (uint32_t)i);
        if (c == '\r') {
            wr32(text_offset, (uint32_t)(i + 1));
            return smBreakWord;
        }
        int w = width_of(textp + (uint32_t)i, 1);
        if (x + w > avail && c != ' ') {
            int32_t at = last_break > text_start ? last_break : (first_on_line ? (i > text_start ? i : i + 1) : text_start);
            wr32(text_offset, (uint32_t)at);
            return last_break > text_start || !first_on_line ? smBreakWord : smBreakChar;
        }
        x += w;
        if (c == ' ')
            last_break = i + 1;
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

/* Truncate a Pascal string to fit width, ending (truncEnd) or middle with an ellipsis. */
int16_t tb_TruncString(int16_t width, gaddr string, int16_t code)
{
    int n = rd8(string);
    if (width_of(string + 1, n) <= width)
        return 0;
    uint8_t buf[256];
    gget(buf, string + 1, (uint32_t)n);
    int ell = qd_text_width(qd_port(), (const uint8_t *)"\xC9", 1);
    if (code == 0) { /* truncEnd */
        while (n > 0 && qd_text_width(qd_port(), buf, n) + ell > width)
            n--;
        buf[n++] = 0xC9;
    } else { /* truncMiddle */
        int keep = n;
        while (keep > 0) {
            int left = (keep + 1) / 2, right = keep / 2;
            uint8_t t[256];
            memcpy(t, buf, (size_t)left);
            t[left] = 0xC9;
            memcpy(t + left + 1, buf + n - right, (size_t)right);
            if (qd_text_width(qd_port(), t, keep + 1) <= width) {
                memcpy(buf, t, (size_t)keep + 1);
                n = keep + 1;
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

int8_t tb_ParseTable(gaddr table)
{
    gmemset(table, 0, 256);
    return 1;
}

int8_t tb_FillParseTable(gaddr table, int16_t script) { return tb_ParseTable(table); }

/* Replace every occurrence of key (a Str15) in base_text with subst_text. */
int16_t tb_ReplaceText(gaddr base_text, gaddr subst_text, gaddr key)
{
    int kl = rd8(key);
    if (kl == 0)
        return 0;
    uint8_t k[16];
    gget(k, key + 1, (uint32_t)kl);
    int32_t sl = tb_GetHandleSize(subst_text);
    int count = 0;
    for (int32_t pos = 0;;) {
        int32_t n = tb_GetHandleSize(base_text);
        int32_t found = -1;
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
