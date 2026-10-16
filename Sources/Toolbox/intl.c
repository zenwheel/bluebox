/*
 * International Utilities (Pack6) and date conversion.
 *
 * The System file carries US 'itl0' (number, date and time formats) and
 * 'itl1' (day and month names) resources, built here; the date and time
 * strings follow them, so an application that supplies its own resources
 * gets its own formats.  String ordering is MacRoman: letters by their
 * plain upper-case form first, then case and diacritical marks.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tb_internal.h"

#define MAC_EPOCH_OFFSET 2082844800u /* seconds from 1904 to 1970 */

/* ---------------------------------------------------------------------- */
/* Default resources */

static void put_pstr(uint8_t *p, const char *s, int field)
{
    size_t n = strlen(s);
    memset(p, 0, (size_t)field);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
}

/* Intl0Rec, 32 bytes. */
size_t intl_default_itl0(uint8_t out[32])
{
    memset(out, 0, 32);
    out[0] = '.';                 /* decimalPt */
    out[1] = ',';                 /* thousSep */
    out[2] = ';';                 /* listSep */
    out[3] = '$';                 /* currSym1..3 */
    out[6] = 0x80 | 0x40 | 0x10;  /* currFmt: leading and trailing zeros, symbol first */
    out[7] = 0;                   /* dateOrder: mdy */
    out[8] = 0;                   /* shrtDateFmt: no leading zeros, two-digit year */
    out[9] = '/';                 /* dateSep */
    out[10] = 0xFF;               /* timeCycle: 12-hour */
    out[11] = 0x80 | 0x40;        /* timeFmt: minutes and seconds with leading zeros */
    memcpy(out + 12, " AM\0", 4); /* mornStr */
    memcpy(out + 16, " PM\0", 4); /* eveStr */
    out[20] = ':';                /* timeSep */
    out[29] = 0;                  /* metricSys: no */
    out[30] = 0;                  /* intl0Vers: US, version 0 */
    out[31] = 0;
    return 32;
}

/* Intl1Rec, 332 bytes: day and month names, long date format. */
size_t intl_default_itl1(uint8_t out[332])
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December" };
    memset(out, 0, 332);
    for (int i = 0; i < 7; i++)
        put_pstr(out + i * 16, days[i], 16);
    for (int i = 0; i < 12; i++)
        put_pstr(out + 112 + i * 16, months[i], 16);
    uint8_t *p = out + 304;
    p[0] = 0; /* suppressDay */
    p[1] = 0; /* lngDateFmt: day, month, date, year */
    p[2] = 0; /* dayLeading0 */
    p[3] = 3; /* abbrLen */
    memcpy(p + 4, "\0\0\0\0", 4);  /* st0: before */
    memcpy(p + 8, ", \0\0", 4);    /* st1: after the day */
    memcpy(p + 12, " \0\0\0", 4);  /* st2: after the month */
    memcpy(p + 16, ", \0\0", 4);   /* st3: after the date */
    memcpy(p + 20, "\0\0\0\0", 4); /* st4: after the year */
    out[324] = 0;                  /* intl1Vers */
    out[325] = 0;
    out[326] = 0x4E;               /* localRtn: RTS */
    out[327] = 0x75;
    return 332;
}

gaddr tb_GetIntlResource(int16_t id)
{
    char type[5];
    snprintf(type, sizeof type, "itl%d", id);
    return tb_GetResource((int32_t)fourcc(type), 0);
}

void tb_ClearIntlResourceCache(void) {}

/* ---------------------------------------------------------------------- */
/* Dates */

static void to_tm(uint32_t secs, struct tm *t)
{
    time_t u = (time_t)secs - (time_t)MAC_EPOCH_OFFSET;
    gmtime_r(&u, t); /* Mac time is already local */
}

void tb_SecondsToDate(uint32_t secs, gaddr d)
{
    struct tm t;
    to_tm(secs, &t);
    wr16(d + DateTimeRec_year_off, (uint16_t)(t.tm_year + 1900));
    wr16(d + DateTimeRec_month_off, (uint16_t)(t.tm_mon + 1));
    wr16(d + DateTimeRec_day_off, (uint16_t)t.tm_mday);
    wr16(d + DateTimeRec_hour_off, (uint16_t)t.tm_hour);
    wr16(d + DateTimeRec_minute_off, (uint16_t)t.tm_min);
    wr16(d + DateTimeRec_second_off, (uint16_t)t.tm_sec);
    wr16(d + DateTimeRec_dayOfWeek_off, (uint16_t)(t.tm_wday + 1));
}

/* Out-of-range fields roll over, as the ROM's did. */
void tb_DateToSeconds(gaddr d, uint32_t *s)
{
    struct tm t = { 0 };
    t.tm_year = (int16_t)rd16(d + DateTimeRec_year_off) - 1900;
    t.tm_mon = (int16_t)rd16(d + DateTimeRec_month_off) - 1;
    t.tm_mday = (int16_t)rd16(d + DateTimeRec_day_off);
    t.tm_hour = (int16_t)rd16(d + DateTimeRec_hour_off);
    t.tm_min = (int16_t)rd16(d + DateTimeRec_minute_off);
    t.tm_sec = (int16_t)rd16(d + DateTimeRec_second_off);
    *s = (uint32_t)(timegm(&t) + (time_t)MAC_EPOCH_OFFSET);
}

/* The itl0/itl1 to format with: the handle given, else the current ones. */
static bool intl_bytes(gaddr h, int id, uint8_t *out, size_t n)
{
    if (!h)
        h = tb_GetIntlResource((int16_t)id);
    if (h && rd32(h) && (size_t)tb_GetHandleSize(h) >= n) {
        gget(out, rd32(h), (uint32_t)n);
        return true;
    }
    if (id == 0)
        intl_default_itl0(out);
    else
        intl_default_itl1(out);
    return true;
}

/* IsMetric: the current itl0's metricSys (255 for metric). */
int8_t tb_IsMetric(void)
{
    uint8_t itl0[32];
    intl_bytes(0, 0, itl0, sizeof itl0);
    return itl0[29] == 255;
}

static void put_result(gaddr p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    wr8(p, (uint8_t)n);
    gput(p + 1, s, (uint32_t)n);
}

/* Up to four bytes of a separator field, stopping at a zero. */
static void cat4(char *dst, size_t cap, const uint8_t *f)
{
    size_t len = strlen(dst);
    for (int i = 0; i < 4 && f[i] && len + 1 < cap; i++)
        dst[len++] = (char)f[i];
    dst[len] = 0;
}

static void cat_name(char *dst, size_t cap, const uint8_t *pstr, int maxlen)
{
    int n = pstr[0] < 15 ? pstr[0] : 15;
    if (maxlen > 0 && n > maxlen)
        n = maxlen;
    size_t len = strlen(dst);
    for (int i = 0; i < n && len + 1 < cap; i++)
        dst[len++] = (char)pstr[1 + i];
    dst[len] = 0;
}

static void format_date(uint32_t secs, int form, gaddr h, char *out, size_t cap)
{
    struct tm t;
    to_tm(secs, &t);
    out[0] = 0;
    if (form == 0) { /* shortDate */
        uint8_t i0[32];
        intl_bytes(h, 0, i0, sizeof i0);
        int fmt = i0[8], order = i0[7];
        char sep = (char)i0[9];
        char m[8], d[8], y[8];
        snprintf(m, sizeof m, (fmt & 0x40) ? "%02d" : "%d", t.tm_mon + 1);
        snprintf(d, sizeof d, (fmt & 0x20) ? "%02d" : "%d", t.tm_mday);
        if (fmt & 0x80)
            snprintf(y, sizeof y, "%d", t.tm_year + 1900);
        else
            snprintf(y, sizeof y, "%02d", (t.tm_year + 1900) % 100);
        const char *a = m, *b = d, *c = y; /* mdy */
        switch (order) {
        case 1: a = d; b = m; c = y; break; /* dmy */
        case 2: a = y; b = m; c = d; break; /* ymd */
        case 3: a = m; b = y; c = d; break; /* myd */
        case 4: a = d; b = y; c = m; break; /* dym */
        case 5: a = y; b = d; c = m; break; /* ydm */
        default: break;
        }
        snprintf(out, cap, "%s%c%s%c%s", a, sep, b, sep, c);
        return;
    }
    /* longDate (1) and abbrevDate (2). */
    uint8_t i1[332];
    intl_bytes(h, 1, i1, sizeof i1);
    const uint8_t *p = i1 + 304;
    int abbr = form == 2 ? p[3] : 0;
    char num[16];
    cat4(out, cap, p + 4);
    if (!p[0]) { /* suppressDay */
        cat_name(out, cap, i1 + t.tm_wday * 16, abbr);
        cat4(out, cap, p + 8);
    }
    cat_name(out, cap, i1 + 112 + t.tm_mon * 16, abbr);
    cat4(out, cap, p + 12);
    snprintf(num, sizeof num, p[2] ? "%02d" : "%d", t.tm_mday);
    strncat(out, num, cap - strlen(out) - 1);
    cat4(out, cap, p + 16);
    snprintf(num, sizeof num, "%d", t.tm_year + 1900);
    strncat(out, num, cap - strlen(out) - 1);
    cat4(out, cap, p + 20);
}

static void format_time(uint32_t secs, bool with_secs, gaddr h, char *out, size_t cap)
{
    struct tm t;
    to_tm(secs, &t);
    uint8_t i0[32];
    intl_bytes(h, 0, i0, sizeof i0);
    bool cycle12 = i0[10] != 0; /* timeCycle24 is 0 */
    int hour = t.tm_hour;
    const uint8_t *suffix = NULL;
    if (cycle12) {
        suffix = hour < 12 ? i0 + 12 : i0 + 16;
        hour %= 12;
        if (hour == 0 && i0[10] == 0xFF)
            hour = 12;
    }
    char sep = (char)i0[20];
    int fmt = i0[11];
    char buf[32];
    snprintf(buf, sizeof buf, (fmt & 0x20) ? "%02d" : "%d", hour);
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof buf - n, (fmt & 0x40) ? "%c%02d" : "%c%d", sep, t.tm_min);
    if (with_secs) {
        n = strlen(buf);
        snprintf(buf + n, sizeof buf - n, (fmt & 0x80) ? "%c%02d" : "%c%d", sep, t.tm_sec);
    }
    snprintf(out, cap, "%s", buf);
    if (suffix)
        cat4(out, cap, suffix);
}

void tb_IUDateString(int32_t date, int8_t form, gaddr p)
{
    char s[256];
    format_date((uint32_t)date, form, 0, s, sizeof s);
    put_result(p, s);
}

void tb_IUDatePString(int32_t date, int8_t form, gaddr p, gaddr h)
{
    char s[256];
    format_date((uint32_t)date, form, h, s, sizeof s);
    put_result(p, s);
}

void tb_IUTimeString(int32_t date, int8_t secs, gaddr p)
{
    char s[64];
    format_time((uint32_t)date, secs != 0, 0, s, sizeof s);
    put_result(p, s);
}

void tb_IUTimePString(int32_t date, int8_t secs, gaddr p, gaddr h)
{
    char s[64];
    format_time((uint32_t)date, secs != 0, h, s, sizeof s);
    put_result(p, s);
}

/* LongDateTime: a 64-bit count of seconds since 1904. */
static uint32_t long_secs(gaddr p) { return rd32(p + 4); }

void tb_IULDateString(gaddr datetimep, int8_t form, gaddr result, gaddr h)
{
    char s[256];
    format_date(long_secs(datetimep), form, h, s, sizeof s);
    put_result(result, s);
}

void tb_IULTimeString(gaddr datetimep, int8_t secs, gaddr result, gaddr h)
{
    char s[64];
    format_time(long_secs(datetimep), secs != 0, h, s, sizeof s);
    put_result(result, s);
}

/* ---------------------------------------------------------------------- */
/* Ordering */

static int order(gaddr a, int la, gaddr b, int lb)
{
    /* Primary: plain upper case. */
    int n = la < lb ? la : lb;
    for (int i = 0; i < n; i++) {
        int d = mac_char_key(rd8(a + (uint32_t)i), false, true) - mac_char_key(rd8(b + (uint32_t)i), false, true);
        if (d)
            return d < 0 ? -1 : 1;
    }
    if (la != lb)
        return la < lb ? -1 : 1;
    /* Then marks, then case. */
    for (int i = 0; i < n; i++) {
        int d = mac_char_key(rd8(a + (uint32_t)i), false, false) - mac_char_key(rd8(b + (uint32_t)i), false, false);
        if (d)
            return d < 0 ? -1 : 1;
    }
    for (int i = 0; i < n; i++) {
        int d = (int)rd8(a + (uint32_t)i) - (int)rd8(b + (uint32_t)i);
        if (d)
            return d < 0 ? 1 : -1; /* lower case sorts after upper */
    }
    return 0;
}

int16_t tb_IUMagString(gaddr a, gaddr b, int16_t la, int16_t lb) { return (int16_t)order(a, la, b, lb); }
int16_t tb_IUMagPString(gaddr a, gaddr b, int16_t la, int16_t lb, gaddr h) { return (int16_t)order(a, la, b, lb); }

/* Equality, ignoring case but not diacritical marks: 0 equal, 1 not. */
static int16_t ident(gaddr a, int la, gaddr b, int lb)
{
    if (la != lb)
        return 1;
    for (int i = 0; i < la; i++)
        if (mac_char_key(rd8(a + (uint32_t)i), false, false) != mac_char_key(rd8(b + (uint32_t)i), false, false))
            return 1;
    return 0;
}

int16_t tb_IUMagIDString(gaddr a, gaddr b, int16_t la, int16_t lb) { return ident(a, la, b, lb); }
int16_t tb_IUMagIDPString(gaddr a, gaddr b, int16_t la, int16_t lb, gaddr h) { return ident(a, la, b, lb); }

int16_t tb_IUTextOrder(gaddr a, gaddr b, int16_t la, int16_t lb, int16_t sa, int16_t sb, int16_t lga, int16_t lgb)
{
    return (int16_t)order(a, la, b, lb);
}

/* The tables inside the itl resources (word break, truncation …): none. */
void tb_GetIntlResourceTable(int16_t script, int16_t table, gaddr itlp, gaddr offsetp, gaddr lengthp)
{
    if (itlp)
        wr32(itlp, 0);
    if (offsetp)
        wr32(offsetp, 0);
    if (lengthp)
        wr32(lengthp, 0);
}
