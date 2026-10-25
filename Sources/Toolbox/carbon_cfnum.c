/*
 * Core Foundation's values as CarbonLib had them: CFNumber, CFBoolean,
 * CFDate with the Gregorian calendar, CFUUID; and Math64 (Math64.h),
 * CarbonLib's 64-bit integer routines.
 *
 * A CFNumber keeps the canonical type it was made with (kCFNumberIntType
 * is kCFNumberSInt32Type ...).  Absolute time is seconds since 1 January
 * 2001 GMT; there are no CFTimeZone objects in CarbonLib, so the calendar
 * calls work in GMT whatever zone they are given.  UUIDs are unique by
 * their bytes, as CF made them.
 *
 * PowerPC passes a 64-bit integer in two GPRs (high word first), a double
 * in an FPR with two GPR slots skipped, a struct by value in its words,
 * and returns a struct through a pointer in r3; long double is MathLib's
 * double-double, in f1 and f2.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "carbon_cf.h"

uint32_t mac_now(void); /* osutil.c */

static double rd_double(gaddr p)
{
    uint64_t b = (uint64_t)rd32(p) << 32 | rd32(p + 4);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static void wr_double_at(gaddr p, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    wr32(p, (uint32_t)(b >> 32));
    wr32(p + 4, (uint32_t)b);
}

static double words_double(uint32_t hi, uint32_t lo)
{
    uint64_t b = (uint64_t)hi << 32 | lo;
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* ---------------------------------------------------------------------- */
/* CFNumber */

typedef struct {
    cf_obj o;
    int type; /* canonical: SInt8 ... Float64 */
    int64_t i;
    double d;
} cf_number;

static int canonical(uint32_t t)
{
    switch (t) {
    case kCFNumberSInt8Type: case kCFNumberCharType:
        return kCFNumberSInt8Type;
    case kCFNumberSInt16Type: case kCFNumberShortType:
        return kCFNumberSInt16Type;
    case kCFNumberSInt32Type: case kCFNumberIntType: case kCFNumberLongType: case kCFNumberCFIndexType:
        return kCFNumberSInt32Type;
    case kCFNumberSInt64Type: case kCFNumberLongLongType:
        return kCFNumberSInt64Type;
    case kCFNumberFloat32Type: case kCFNumberFloatType:
        return kCFNumberFloat32Type;
    case kCFNumberFloat64Type: case kCFNumberDoubleType:
        return kCFNumberFloat64Type;
    default:
        return 0;
    }
}

static bool is_float_type(int t) { return t == kCFNumberFloat32Type || t == kCFNumberFloat64Type; }
static int type_size(int t) { return t == kCFNumberSInt8Type ? 1 : t == kCFNumberSInt16Type ? 2 : t == kCFNumberSInt32Type || t == kCFNumberFloat32Type ? 4 : 8; }

static cf_number *number_of(gaddr ref) { return cf_get_type(ref, CFT_NUMBER); }

static int number_compare(const cf_number *a, const cf_number *b)
{
    if (is_float_type(a->type) || is_float_type(b->type)) {
        double x = is_float_type(a->type) ? a->d : (double)a->i, y = is_float_type(b->type) ? b->d : (double)b->i;
        return x < y ? kCFCompareLessThan : x > y ? kCFCompareGreaterThan : kCFCompareEqualTo;
    }
    return a->i < b->i ? kCFCompareLessThan : a->i > b->i ? kCFCompareGreaterThan : kCFCompareEqualTo;
}

static bool number_equal(cf_obj *a, cf_obj *b) { return number_compare((cf_number *)a, (cf_number *)b) == 0; }

static uint32_t number_hash(cf_obj *o)
{
    cf_number *n = (cf_number *)o;
    if (is_float_type(n->type) && n->d != floor(n->d)) {
        uint64_t b;
        memcpy(&b, &n->d, 8);
        return (uint32_t)(b ^ b >> 32);
    }
    int64_t v = is_float_type(n->type) ? (int64_t)n->d : n->i;
    return (uint32_t)(v < 0 ? -v : v);
}

static void number_describe(cf_obj *o, cf_text *out)
{
    static const char *const names[] = { "", "kCFNumberSInt8Type", "kCFNumberSInt16Type", "kCFNumberSInt32Type",
                                         "kCFNumberSInt64Type", "kCFNumberFloat32Type", "kCFNumberFloat64Type" };
    cf_number *n = (cf_number *)o;
    if (is_float_type(n->type))
        text_printf(out, "value = %+.20f, type = %s", n->d, names[n->type]);
    else
        text_printf(out, "value = %+lld, type = %s", (long long)n->i, names[n->type]);
}

static const cf_class number_class = { "CFNumber", NULL, NULL, number_equal, number_hash, number_describe };

static cf_number *number_make(gaddr alloc, int type)
{
    cf_number *n = cf_new(CFT_NUMBER, sizeof *n, alloc, 0);
    if (n)
        n->type = type;
    return n;
}

gaddr cfnum_new_int(gaddr alloc, int64_t v)
{
    cf_number *n = number_make(alloc, v == (int32_t)v ? kCFNumberSInt32Type : kCFNumberSInt64Type);
    if (!n)
        return 0;
    n->i = v;
    n->d = (double)v;
    return n->o.ref;
}

gaddr cfnum_new_double(gaddr alloc, double v)
{
    cf_number *n = number_make(alloc, kCFNumberFloat64Type);
    if (!n)
        return 0;
    n->d = v;
    n->i = isfinite(v) && fabs(v) < 9.2e18 ? (int64_t)v : 0;
    return n->o.ref;
}

bool cfnum_value(gaddr ref, bool *is_float, int64_t *i, double *d)
{
    cf_number *n = number_of(ref);
    if (!n)
        return false;
    if (is_float)
        *is_float = is_float_type(n->type);
    if (i)
        *i = n->i;
    if (d)
        *d = is_float_type(n->type) ? n->d : (double)n->i;
    return true;
}

static void cf_CFNumberGetTypeID(ppc_cpu *c) { RETURN(CFT_NUMBER); }

/* CFNumberCreate(allocator, theType, valuePtr) */
static void cf_CFNumberCreate(ppc_cpu *c)
{
    int t = canonical(R(1));
    gaddr p = R(2);
    if (!t || !p)
        RETURN(0);
    cf_number *n = number_make(R(0), t);
    if (!n)
        RETURN(0);
    switch (t) {
    case kCFNumberSInt8Type: n->i = (int8_t)rd8(p); break;
    case kCFNumberSInt16Type: n->i = (int16_t)rd16(p); break;
    case kCFNumberSInt32Type: n->i = (int32_t)rd32(p); break;
    case kCFNumberSInt64Type: n->i = (int64_t)((uint64_t)rd32(p) << 32 | rd32(p + 4)); break;
    case kCFNumberFloat32Type: {
        uint32_t b = rd32(p);
        float f;
        memcpy(&f, &b, 4);
        n->d = f;
        break;
    }
    case kCFNumberFloat64Type: n->d = rd_double(p); break;
    }
    if (is_float_type(t))
        n->i = isfinite(n->d) && fabs(n->d) < 9.2e18 ? (int64_t)n->d : 0;
    else
        n->d = (double)n->i;
    RETURN(n->o.ref);
}

static void cf_CFNumberGetType(ppc_cpu *c)
{
    cf_number *n = number_of(R(0));
    RETURN(n ? n->type : 0);
}

static void cf_CFNumberGetByteSize(ppc_cpu *c)
{
    cf_number *n = number_of(R(0));
    RETURN(n ? type_size(n->type) : 0);
}

static void cf_CFNumberIsFloatType(ppc_cpu *c)
{
    cf_number *n = number_of(R(0));
    RETURN(n && is_float_type(n->type));
}

/* CFNumberGetValue(number, theType, valuePtr): false if the value lost
 * something on the way. */
static void cf_CFNumberGetValue(ppc_cpu *c)
{
    cf_number *n = number_of(R(0));
    int t = canonical(R(1));
    gaddr p = R(2);
    if (!n || !t || !p)
        RETURN(false);
    bool exact = true;
    bool from_float = is_float_type(n->type);
    if (is_float_type(t)) {
        double d = from_float ? n->d : (double)n->i;
        if (!from_float && (int64_t)d != n->i)
            exact = false;
        if (t == kCFNumberFloat32Type) {
            float f = (float)d;
            uint32_t b;
            memcpy(&b, &f, 4);
            wr32(p, b);
            exact &= isnan(d) || (double)f == d;
        } else {
            wr_double_at(p, d);
        }
        RETURN(exact);
    }
    int64_t v = n->i;
    if (from_float) {
        double d = n->d;
        exact = isfinite(d) && d == floor(d) && fabs(d) < 9.2e18;
        v = isfinite(d) ? (d >= 9.2e18 ? INT64_MAX : d <= -9.2e18 ? INT64_MIN : (int64_t)d) : 0;
    }
    switch (t) {
    case kCFNumberSInt8Type: wr8(p, (uint8_t)v); exact &= v == (int8_t)v; break;
    case kCFNumberSInt16Type: wr16(p, (uint16_t)v); exact &= v == (int16_t)v; break;
    case kCFNumberSInt32Type: wr32(p, (uint32_t)v); exact &= v == (int32_t)v; break;
    case kCFNumberSInt64Type:
        wr32(p, (uint32_t)((uint64_t)v >> 32));
        wr32(p + 4, (uint32_t)v);
        break;
    }
    RETURN(exact);
}

/* CFNumberCompare(number, otherNumber, context) */
static void cf_CFNumberCompare(ppc_cpu *c)
{
    cf_number *a = number_of(R(0)), *b = number_of(R(1));
    RETURN(a && b ? (uint32_t)number_compare(a, b) : kCFCompareEqualTo);
}

/* ---------------------------------------------------------------------- */
/* CFBoolean: two objects, never freed. */

typedef struct {
    cf_obj o;
    bool v;
} cf_boolean;

static gaddr booleans[2];

static void boolean_describe(cf_obj *o, cf_text *out) { text_add_utf8(out, ((cf_boolean *)o)->v ? "value = true" : "value = false"); }
static uint32_t boolean_hash(cf_obj *o) { return ((cf_boolean *)o)->v ? 1 : 0; }

static const cf_class boolean_class = { "CFBoolean", NULL, NULL, NULL, boolean_hash, boolean_describe };

gaddr cfbool(bool v)
{
    if (!booleans[v]) {
        cf_boolean *b = cf_new(CFT_BOOLEAN, sizeof *b, cf_allocator_system(), 0);
        if (!b)
            return 0;
        b->v = v;
        cf_make_constant(&b->o);
        booleans[v] = b->o.ref;
    }
    return booleans[v];
}

static void cf_CFBooleanGetTypeID(ppc_cpu *c) { RETURN(CFT_BOOLEAN); }

static void cf_CFBooleanGetValue(ppc_cpu *c)
{
    cf_boolean *b = cf_get_type(R(0), CFT_BOOLEAN);
    RETURN(b && b->v);
}

/* ---------------------------------------------------------------------- */
/* CFDate and the calendar */

typedef struct {
    cf_obj o;
    double at;
} cf_date;

static bool date_equal(cf_obj *a, cf_obj *b) { return ((cf_date *)a)->at == ((cf_date *)b)->at; }
static uint32_t date_hash(cf_obj *o) { return (uint32_t)(int64_t)((cf_date *)o)->at; }
static void date_describe(cf_obj *o, cf_text *out) { text_printf(out, "time = %.6f", ((cf_date *)o)->at); }

static const cf_class date_class = { "CFDate", NULL, NULL, date_equal, date_hash, date_describe };

gaddr cfdate_new(gaddr alloc, double at)
{
    cf_date *d = cf_new(CFT_DATE, sizeof *d, alloc, 0);
    if (!d)
        return 0;
    d->at = at;
    return d->o.ref;
}

bool cfdate_value(gaddr ref, double *at)
{
    cf_date *d = cf_get_type(ref, CFT_DATE);
    if (d && at)
        *at = d->at;
    return d != NULL;
}

/* Now: the host's clock, moved as the Mac's has been (SetDateTime). */
double cf_absolute_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm lt;
    localtime_r(&t, &lt);
    int64_t mac_host = (int64_t)t + lt.tm_gmtoff + 2082844800LL;
    int64_t moved = (int64_t)mac_now() - mac_host;
    return (double)(t + moved) + tv.tv_usec / 1e6 - 978307200.0;
}

/* Days since 1 January 2001 from a date, and back (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 - 11323; /* 11323: 1970 to 2001 */
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468 + 11323;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(int64_t y, int m)
{
    static const int n[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : n[(m - 1) % 12];
}

typedef struct {
    int64_t year;
    int month, day, hour, minute;
    double second;
} gdate;

static gdate gregorian(double at)
{
    gdate g;
    double days = floor(at / 86400.0), rem = at - days * 86400.0;
    civil_from_days((int64_t)days, &g.year, &g.month, &g.day);
    g.hour = (int)(rem / 3600);
    rem -= g.hour * 3600.0;
    g.minute = (int)(rem / 60);
    g.second = rem - g.minute * 60.0;
    return g;
}

static double absolute(int64_t y, int64_t m, int64_t d, double secs)
{
    /* Months outside 1-12 move the year. */
    y += (m - 1 >= 0 ? (m - 1) / 12 : -((12 - m) / 12));
    m = ((m - 1) % 12 + 12) % 12 + 1;
    return (double)days_from_civil(y, m, 1) * 86400.0 + (double)(d - 1) * 86400.0 + secs;
}

static void cf_CFAbsoluteTimeGetCurrent(ppc_cpu *c) { cf_return_double(c, cf_absolute_now()); }

static void cf_CFDateGetTypeID(ppc_cpu *c) { RETURN(CFT_DATE); }

/* CFDateCreate(allocator, at): at in f1. */
static void cf_CFDateCreate(ppc_cpu *c) { RETURN(cfdate_new(R(0), ppc_fpr(c, 1))); }

static void cf_CFDateGetAbsoluteTime(ppc_cpu *c)
{
    double at = 0;
    cfdate_value(R(0), &at);
    cf_return_double(c, at);
}

static void cf_CFDateGetTimeIntervalSinceDate(ppc_cpu *c)
{
    double a = 0, b = 0;
    cfdate_value(R(0), &a);
    cfdate_value(R(1), &b);
    cf_return_double(c, a - b);
}

static void cf_CFDateCompare(ppc_cpu *c)
{
    double a = 0, b = 0;
    cfdate_value(R(0), &a);
    cfdate_value(R(1), &b);
    RETURN((uint32_t)(a < b ? kCFCompareLessThan : a > b ? kCFCompareGreaterThan : kCFCompareEqualTo));
}

/* A CFGregorianDate passed by value in four words. */
static gdate gdate_of_words(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    gdate g = { (int32_t)w0, (int8_t)(w1 >> 24), (int8_t)(w1 >> 16), (int8_t)(w1 >> 8), (int8_t)w1,
                words_double(w2, w3) };
    return g;
}

enum { kCFGregorianUnitsYears = 1, kCFGregorianUnitsMonths = 2, kCFGregorianUnitsDays = 4,
       kCFGregorianUnitsHours = 8, kCFGregorianUnitsMinutes = 16, kCFGregorianUnitsSeconds = 32 };

/* CFGregorianDateIsValid(gdate, unitFlags) */
static void cf_CFGregorianDateIsValid(ppc_cpu *c)
{
    gdate g = gdate_of_words(R(0), R(1), R(2), R(3));
    uint32_t f = R(4);
    bool ok = true;
    if (f & kCFGregorianUnitsMonths)
        ok &= g.month >= 1 && g.month <= 12;
    if (f & kCFGregorianUnitsDays)
        ok &= g.day >= 1 && g.day <= 31 && (g.month < 1 || g.month > 12 || g.day <= month_days(g.year, g.month));
    if (f & kCFGregorianUnitsHours)
        ok &= g.hour >= 0 && g.hour < 24;
    if (f & kCFGregorianUnitsMinutes)
        ok &= g.minute >= 0 && g.minute < 60;
    if (f & kCFGregorianUnitsSeconds)
        ok &= g.second >= 0 && g.second < 60;
    RETURN(ok);
}

/* CFGregorianDateGetAbsoluteTime(gdate, tz) */
static void cf_CFGregorianDateGetAbsoluteTime(ppc_cpu *c)
{
    gdate g = gdate_of_words(R(0), R(1), R(2), R(3));
    cf_return_double(c, absolute(g.year, g.month, g.day, g.hour * 3600.0 + g.minute * 60.0 + g.second));
}

/* CFAbsoluteTimeGetGregorianDate(at, tz): through r3, at in f1. */
static void cf_CFAbsoluteTimeGetGregorianDate(ppc_cpu *c)
{
    gaddr res = R(0);
    gdate g = gregorian(ppc_fpr(c, 1));
    if (res) {
        wr32(res, (uint32_t)(int32_t)g.year);
        wr8(res + 4, (uint8_t)g.month);
        wr8(res + 5, (uint8_t)g.day);
        wr8(res + 6, (uint8_t)g.hour);
        wr8(res + 7, (uint8_t)g.minute);
        wr_double_at(res + 8, g.second);
    }
    RETURN(res);
}

typedef struct {
    int32_t u[5]; /* years, months, days, hours, minutes */
    double seconds;
} gunits;

static double add_units(double at, const gunits *g)
{
    gdate d = gregorian(at);
    double in_day = at - floor(at / 86400.0) * 86400.0;
    double r = absolute(d.year + g->u[0], (int64_t)d.month + g->u[1], d.day, in_day);
    return r + g->u[2] * 86400.0 + g->u[3] * 3600.0 + g->u[4] * 60.0 + g->seconds;
}

/* CFAbsoluteTimeAddGregorianUnits(at, tz, units): at in f1 (words 0-1),
 * tz word 2, the units words 3-9. */
static void cf_CFAbsoluteTimeAddGregorianUnits(ppc_cpu *c)
{
    gunits g;
    for (int i = 0; i < 5; i++)
        g.u[i] = (int32_t)cf_arg(c, 3 + (unsigned)i);
    g.seconds = words_double(cf_arg(c, 8), cf_arg(c, 9));
    cf_return_double(c, add_units(ppc_fpr(c, 1), &g));
}

/* CFAbsoluteTimeGetDifferenceAsGregorianUnits(at1, at2, tz, unitFlags):
 * through r3, at1 in f1 and at2 in f2 (words 1-4), tz word 5, flags word 6.
 * Each unit in turn by successive approximation, as CF does. */
static void cf_CFAbsoluteTimeGetDifferenceAsGregorianUnits(ppc_cpu *c)
{
    gaddr res = R(0);
    double at1 = ppc_fpr(c, 1), at2 = ppc_fpr(c, 2);
    uint32_t flags = cf_arg(c, 6);
    static const double secs[] = { 366 * 86400.0, 31 * 86400.0, 86400.0, 3600.0, 60.0 };
    gunits g = { { 0 }, 0 };
    double atnew = at2, atold;
    int incr = at2 < at1 ? 1 : -1;
    for (int i = 0; i < 5; i++) {
        if (!(flags & (1u << i)))
            continue;
        g.u[i] = -3 * incr + (int32_t)((at1 - atnew) / secs[i]);
        do {
            atold = atnew;
            g.u[i] += incr;
            atnew = add_units(at2, &g);
        } while ((incr == 1 && atnew <= at1) || (incr == -1 && at1 <= atnew));
        g.u[i] -= incr;
        atnew = atold;
    }
    if (flags & kCFGregorianUnitsSeconds)
        g.seconds = at1 - atnew;
    if (res) {
        for (int i = 0; i < 5; i++)
            wr32(res + 4 * (uint32_t)i, (uint32_t)g.u[i]);
        wr_double_at(res + 20, g.seconds);
    }
    RETURN(res);
}

static int64_t day_number(double at) { return (int64_t)floor(at / 86400.0); }

/* Monday is 1 (1 January 2001 was one). */
static int day_of_week(double at) { return (int)(((day_number(at) % 7) + 7) % 7) + 1; }

static int day_of_year(double at)
{
    gdate g = gregorian(at);
    return (int)(day_number(at) - days_from_civil(g.year, 1, 1)) + 1;
}

static void cf_CFAbsoluteTimeGetDayOfWeek(ppc_cpu *c) { RETURN(day_of_week(ppc_fpr(c, 1))); }
static void cf_CFAbsoluteTimeGetDayOfYear(ppc_cpu *c) { RETURN(day_of_year(ppc_fpr(c, 1))); }

/* ISO 8601: the week with the year's first Thursday is 1. */
static void cf_CFAbsoluteTimeGetWeekOfYear(ppc_cpu *c)
{
    double at = ppc_fpr(c, 1);
    int64_t day = day_number(at);
    int64_t thursday = day - day_of_week(at) + 4;
    int64_t y;
    int m, d;
    civil_from_days(thursday, &y, &m, &d);
    RETURN((uint32_t)((thursday - days_from_civil(y, 1, 1)) / 7 + 1));
}

/* ---------------------------------------------------------------------- */
/* CFUUID */

typedef struct {
    cf_obj o;
    uint8_t b[16];
} cf_uuid;

static gaddr *uuids;
static unsigned nuuids, uuids_cap;

static void uuid_finalize(cf_obj *o)
{
    for (unsigned i = 0; i < nuuids; i++)
        if (uuids[i] == o->ref) {
            uuids[i] = uuids[--nuuids];
            break;
        }
}

static bool uuid_equal(cf_obj *a, cf_obj *b) { return !memcmp(((cf_uuid *)a)->b, ((cf_uuid *)b)->b, 16); }
static uint32_t uuid_hash(cf_obj *o)
{
    uint32_t h = 0;
    for (int i = 0; i < 16; i++)
        h = h * 31 + ((cf_uuid *)o)->b[i];
    return h;
}

static void uuid_text(const uint8_t *b, char out[37])
{
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void uuid_describe(cf_obj *o, cf_text *out)
{
    char t[37];
    uuid_text(((cf_uuid *)o)->b, t);
    text_add_utf8(out, t);
}

static const cf_class uuid_class = { "CFUUID", uuid_finalize, NULL, uuid_equal, uuid_hash, uuid_describe };

gaddr cfuuid_new(gaddr alloc, const uint8_t b[16], bool constant)
{
    for (unsigned i = 0; i < nuuids; i++) {
        cf_uuid *u = cf_get_type(uuids[i], CFT_UUID);
        if (u && !memcmp(u->b, b, 16)) {
            if (constant)
                cf_make_constant(&u->o);
            return constant ? u->o.ref : cf_retain(u->o.ref);
        }
    }
    cf_uuid *u = cf_new(CFT_UUID, sizeof *u, constant ? cf_allocator_system() : alloc, 0);
    if (!u)
        return 0;
    memcpy(u->b, b, 16);
    if (constant)
        cf_make_constant(&u->o);
    if (nuuids == uuids_cap) {
        unsigned cap = uuids_cap * 2 + 16;
        gaddr *p = realloc(uuids, sizeof *p * cap);
        if (p) {
            uuids = p;
            uuids_cap = cap;
        }
    }
    if (nuuids < uuids_cap)
        uuids[nuuids++] = u->o.ref;
    return u->o.ref;
}

bool cfuuid_bytes(gaddr ref, uint8_t out[16])
{
    cf_uuid *u = cf_get_type(ref, CFT_UUID);
    if (u)
        memcpy(out, u->b, 16);
    return u != NULL;
}

static void cf_CFUUIDGetTypeID(ppc_cpu *c) { RETURN(CFT_UUID); }

/* A random (version 4) UUID. */
static void cf_CFUUIDCreate(ppc_cpu *c)
{
    static uint64_t state;
    if (!state) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        state = (uint64_t)tv.tv_sec * 1000003u ^ (uint64_t)tv.tv_usec ^ (uint64_t)(uintptr_t)&state;
    }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        b[i] = (uint8_t)(state >> 24);
    }
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    RETURN(cfuuid_new(R(0), b, false));
}

/* The sixteen bytes as arguments: words 1-16. */
static void bytes_args(ppc_cpu *c, uint8_t b[16])
{
    for (unsigned i = 0; i < 16; i++)
        b[i] = (uint8_t)cf_arg(c, i + 1);
}

static void cf_CFUUIDCreateWithBytes(ppc_cpu *c)
{
    uint8_t b[16];
    bytes_args(c, b);
    RETURN(cfuuid_new(R(0), b, false));
}

static void cf_CFUUIDGetConstantUUIDWithBytes(ppc_cpu *c)
{
    uint8_t b[16];
    bytes_args(c, b);
    RETURN(cfuuid_new(R(0), b, true));
}

/* 32 hex digits, dashes (and braces) anywhere. */
bool cfuuid_parse(gaddr str, uint8_t b[16])
{
    uint32_t n;
    const uint16_t *u = cfstr_chars(str, &n);
    int k = 0;
    memset(b, 0, 16);
    for (uint32_t i = 0; u && i < n && k < 32; i++) {
        uint16_t ch = u[i];
        int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (ch == '-' || ch == '{' || ch == '}')
            continue;
        if (v < 0)
            return false;
        b[k / 2] = (uint8_t)(b[k / 2] << 4 | v);
        k++;
    }
    return k == 32;
}

/* CFUUIDCreateFromString(alloc, uuidStr) */
static void cf_CFUUIDCreateFromString(ppc_cpu *c)
{
    uint8_t b[16];
    RETURN(cfuuid_parse(R(1), b) ? cfuuid_new(R(0), b, false) : 0);
}

static void cf_CFUUIDCreateString(ppc_cpu *c)
{
    uint8_t b[16];
    char t[37];
    if (!cfuuid_bytes(R(1), b))
        RETURN(0);
    uuid_text(b, t);
    RETURN(cfstr_from_utf8(t));
}

/* CFUUIDGetUUIDBytes(uuid): a CFUUIDBytes through r3. */
static void cf_CFUUIDGetUUIDBytes(ppc_cpu *c)
{
    gaddr res = R(0);
    uint8_t b[16] = { 0 };
    cfuuid_bytes(R(1), b);
    if (res)
        gput(res, b, 16);
    RETURN(res);
}

/* CFUUIDCreateFromUUIDBytes(alloc, bytes): the struct in words 1-4. */
static void cf_CFUUIDCreateFromUUIDBytes(ppc_cpu *c)
{
    uint8_t b[16];
    for (unsigned w = 0; w < 4; w++) {
        uint32_t v = cf_arg(c, w + 1);
        b[4 * w] = (uint8_t)(v >> 24), b[4 * w + 1] = (uint8_t)(v >> 16);
        b[4 * w + 2] = (uint8_t)(v >> 8), b[4 * w + 3] = (uint8_t)v;
    }
    RETURN(cfuuid_new(R(0), b, false));
}

/* ---------------------------------------------------------------------- */
/* Math64 */

static int64_t s64(ppc_cpu *c, unsigned w) { return (int64_t)((uint64_t)cf_arg(c, w) << 32 | cf_arg(c, w + 1)); }
static uint64_t u64(ppc_cpu *c, unsigned w) { return (uint64_t)cf_arg(c, w) << 32 | cf_arg(c, w + 1); }

static void m64_S64Max(ppc_cpu *c) { cf_return64(c, (uint64_t)INT64_MAX); }
static void m64_S64Min(ppc_cpu *c) { cf_return64(c, (uint64_t)INT64_MIN); }
static void m64_U64Max(ppc_cpu *c) { cf_return64(c, UINT64_MAX); }
static void m64_S64Add(ppc_cpu *c) { cf_return64(c, u64(c, 0) + u64(c, 2)); }
static void m64_S64Subtract(ppc_cpu *c) { cf_return64(c, u64(c, 0) - u64(c, 2)); }
static void m64_S64Negate(ppc_cpu *c) { cf_return64(c, 0 - u64(c, 0)); }
static void m64_S64Absolute(ppc_cpu *c)
{
    int64_t v = s64(c, 0);
    cf_return64(c, v < 0 ? 0 - (uint64_t)v : (uint64_t)v);
}
static void m64_S64Multiply(ppc_cpu *c) { cf_return64(c, u64(c, 0) * u64(c, 2)); }

/* S64Divide(dividend, divisor, *remainder): truncated, the remainder with
 * the dividend's sign; by zero, nothing and the dividend left over. */
static void m64_S64Divide(ppc_cpu *c)
{
    int64_t a = s64(c, 0), b = s64(c, 2);
    gaddr rem = cf_arg(c, 4);
    int64_t q = 0, r = a;
    if (b && !(a == INT64_MIN && b == -1)) {
        q = a / b;
        r = a % b;
    } else if (b) {
        q = INT64_MIN;
        r = 0;
    }
    if (rem) {
        wr32(rem, (uint32_t)((uint64_t)r >> 32));
        wr32(rem + 4, (uint32_t)r);
    }
    cf_return64(c, (uint64_t)q);
}

static void m64_S64Mod(ppc_cpu *c)
{
    int64_t a = s64(c, 0), b = s64(c, 2);
    cf_return64(c, (uint64_t)(b && !(a == INT64_MIN && b == -1) ? a % b : b ? 0 : a));
}

static void m64_S64Set(ppc_cpu *c) { cf_return64(c, (uint64_t)(int64_t)(int32_t)R(0)); }
static void m64_S64SetU(ppc_cpu *c) { cf_return64(c, R(0)); }
static void m64_S32Set(ppc_cpu *c) { RETURN(R(1)); }
static void m64_S64And(ppc_cpu *c) { RETURN(u64(c, 0) && u64(c, 2)); }
static void m64_S64Or(ppc_cpu *c) { RETURN(u64(c, 0) || u64(c, 2)); }
static void m64_S64Eor(ppc_cpu *c) { RETURN(!u64(c, 0) != !u64(c, 2)); }
static void m64_S64Not(ppc_cpu *c) { RETURN(!u64(c, 0)); }
static void m64_S64Compare(ppc_cpu *c)
{
    int64_t a = s64(c, 0), b = s64(c, 2);
    RETURN((uint32_t)(a < b ? -1 : a > b ? 1 : 0));
}
static void m64_BitwiseAnd(ppc_cpu *c) { cf_return64(c, u64(c, 0) & u64(c, 2)); }
static void m64_BitwiseOr(ppc_cpu *c) { cf_return64(c, u64(c, 0) | u64(c, 2)); }
static void m64_BitwiseEor(ppc_cpu *c) { cf_return64(c, u64(c, 0) ^ u64(c, 2)); }
static void m64_BitwiseNot(ppc_cpu *c) { cf_return64(c, ~u64(c, 0)); }
static void m64_S64ShiftRight(ppc_cpu *c)
{
    int64_t v = s64(c, 0);
    uint32_t n = R(2);
    cf_return64(c, (uint64_t)(n >= 64 ? (v < 0 ? -1 : 0) : v >> n));
}
static void m64_ShiftLeft(ppc_cpu *c)
{
    uint32_t n = R(2);
    cf_return64(c, n >= 64 ? 0 : u64(c, 0) << n);
}

/* long double: a double-double, the head in f1 and the tail in f2. */
static void return_long_double(ppc_cpu *c, __int128 v)
{
    double hi = (double)v;
    double lo = (double)(v - (__int128)hi);
    ppc_set_fpr(c, 1, hi);
    ppc_set_fpr(c, 2, lo);
    c->pc = c->lr;
}

static __int128 long_double_arg(ppc_cpu *c)
{
    double hi = ppc_fpr(c, 1), lo = ppc_fpr(c, 2);
    if (!isfinite(hi))
        return isnan(hi) ? 0 : hi > 0 ? (__int128)UINT64_MAX : -(__int128)UINT64_MAX;
    if (fabs(hi) > 1.8e19)
        return hi > 0 ? (__int128)UINT64_MAX : -(__int128)UINT64_MAX;
    /* The sum truncated toward zero. */
    __int128 whole = (__int128)trunc(hi);
    double frac = (hi - trunc(hi)) + lo;
    __int128 v = whole + (__int128)trunc(frac);
    double rest = frac - trunc(frac);
    if (v > 0 && rest < 0)
        v -= 1;
    else if (v < 0 && rest > 0)
        v += 1;
    return v;
}

static void m64_SInt64ToLongDouble(ppc_cpu *c) { return_long_double(c, s64(c, 0)); }
static void m64_UInt64ToLongDouble(ppc_cpu *c) { return_long_double(c, u64(c, 0)); }

static void m64_LongDoubleToSInt64(ppc_cpu *c)
{
    __int128 v = long_double_arg(c);
    cf_return64(c, (uint64_t)(v > INT64_MAX ? INT64_MAX : v < INT64_MIN ? INT64_MIN : (int64_t)v));
}

static void m64_LongDoubleToUInt64(ppc_cpu *c)
{
    __int128 v = long_double_arg(c);
    cf_return64(c, v < 0 ? 0 : v > (__int128)UINT64_MAX ? UINT64_MAX : (uint64_t)v);
}

static void m64_U64Add(ppc_cpu *c) { cf_return64(c, u64(c, 0) + u64(c, 2)); }
static void m64_U64Subtract(ppc_cpu *c) { cf_return64(c, u64(c, 0) - u64(c, 2)); }
static void m64_U64Multiply(ppc_cpu *c) { cf_return64(c, u64(c, 0) * u64(c, 2)); }

static void m64_U64Divide(ppc_cpu *c)
{
    uint64_t a = u64(c, 0), b = u64(c, 2);
    gaddr rem = cf_arg(c, 4);
    uint64_t q = b ? a / b : 0, r = b ? a % b : a;
    if (rem) {
        wr32(rem, (uint32_t)(r >> 32));
        wr32(rem + 4, (uint32_t)r);
    }
    cf_return64(c, q);
}

static void m64_U64Mod(ppc_cpu *c)
{
    uint64_t a = u64(c, 0), b = u64(c, 2);
    cf_return64(c, b ? a % b : a);
}

static void m64_U64Set(ppc_cpu *c) { cf_return64(c, (uint64_t)(int64_t)(int32_t)R(0)); }
static void m64_U64SetU(ppc_cpu *c) { cf_return64(c, R(0)); }
static void m64_U32SetU(ppc_cpu *c) { RETURN(R(1)); }
static void m64_U64Compare(ppc_cpu *c)
{
    uint64_t a = u64(c, 0), b = u64(c, 2);
    RETURN((uint32_t)(a < b ? -1 : a > b ? 1 : 0));
}
static void m64_U64ShiftRight(ppc_cpu *c)
{
    uint32_t n = R(2);
    cf_return64(c, n >= 64 ? 0 : u64(c, 0) >> n);
}
static void m64_Same64(ppc_cpu *c) { cf_return64(c, u64(c, 0)); }

/* ---------------------------------------------------------------------- */

void cfnum_reset(void)
{
    booleans[0] = booleans[1] = 0;
    free(uuids);
    uuids = NULL;
    nuuids = uuids_cap = 0;
    cf_register_class(CFT_NUMBER, &number_class);
    cf_register_class(CFT_BOOLEAN, &boolean_class);
    cf_register_class(CFT_DATE, &date_class);
    cf_register_class(CFT_UUID, &uuid_class);
}

const ppc_export cfnum_exports[] = {
    { "CFNumberGetTypeID", cf_CFNumberGetTypeID },
    { "CFNumberCreate", cf_CFNumberCreate },
    { "CFNumberGetType", cf_CFNumberGetType },
    { "CFNumberGetByteSize", cf_CFNumberGetByteSize },
    { "CFNumberIsFloatType", cf_CFNumberIsFloatType },
    { "CFNumberGetValue", cf_CFNumberGetValue },
    { "CFNumberCompare", cf_CFNumberCompare },
    { "CFBooleanGetTypeID", cf_CFBooleanGetTypeID },
    { "CFBooleanGetValue", cf_CFBooleanGetValue },
    { "CFAbsoluteTimeGetCurrent", cf_CFAbsoluteTimeGetCurrent },
    { "CFDateGetTypeID", cf_CFDateGetTypeID },
    { "CFDateCreate", cf_CFDateCreate },
    { "CFDateGetAbsoluteTime", cf_CFDateGetAbsoluteTime },
    { "CFDateGetTimeIntervalSinceDate", cf_CFDateGetTimeIntervalSinceDate },
    { "CFDateCompare", cf_CFDateCompare },
    { "CFGregorianDateIsValid", cf_CFGregorianDateIsValid },
    { "CFGregorianDateGetAbsoluteTime", cf_CFGregorianDateGetAbsoluteTime },
    { "CFAbsoluteTimeGetGregorianDate", cf_CFAbsoluteTimeGetGregorianDate },
    { "CFAbsoluteTimeAddGregorianUnits", cf_CFAbsoluteTimeAddGregorianUnits },
    { "CFAbsoluteTimeGetDifferenceAsGregorianUnits", cf_CFAbsoluteTimeGetDifferenceAsGregorianUnits },
    { "CFAbsoluteTimeGetDayOfWeek", cf_CFAbsoluteTimeGetDayOfWeek },
    { "CFAbsoluteTimeGetDayOfYear", cf_CFAbsoluteTimeGetDayOfYear },
    { "CFAbsoluteTimeGetWeekOfYear", cf_CFAbsoluteTimeGetWeekOfYear },
    { "CFUUIDGetTypeID", cf_CFUUIDGetTypeID },
    { "CFUUIDCreate", cf_CFUUIDCreate },
    { "CFUUIDCreateWithBytes", cf_CFUUIDCreateWithBytes },
    { "CFUUIDCreateFromString", cf_CFUUIDCreateFromString },
    { "CFUUIDCreateString", cf_CFUUIDCreateString },
    { "CFUUIDGetConstantUUIDWithBytes", cf_CFUUIDGetConstantUUIDWithBytes },
    { "CFUUIDGetUUIDBytes", cf_CFUUIDGetUUIDBytes },
    { "CFUUIDCreateFromUUIDBytes", cf_CFUUIDCreateFromUUIDBytes },
    { "S64Max", m64_S64Max },
    { "S64Min", m64_S64Min },
    { "S64Add", m64_S64Add },
    { "S64Subtract", m64_S64Subtract },
    { "S64Negate", m64_S64Negate },
    { "S64Absolute", m64_S64Absolute },
    { "S64Multiply", m64_S64Multiply },
    { "S64Mod", m64_S64Mod },
    { "S64Divide", m64_S64Divide },
    { "S64Set", m64_S64Set },
    { "S64SetU", m64_S64SetU },
    { "S32Set", m64_S32Set },
    { "S64And", m64_S64And },
    { "S64Or", m64_S64Or },
    { "S64Eor", m64_S64Eor },
    { "S64Not", m64_S64Not },
    { "S64Compare", m64_S64Compare },
    { "S64BitwiseAnd", m64_BitwiseAnd },
    { "S64BitwiseOr", m64_BitwiseOr },
    { "S64BitwiseEor", m64_BitwiseEor },
    { "S64BitwiseNot", m64_BitwiseNot },
    { "S64ShiftRight", m64_S64ShiftRight },
    { "S64ShiftLeft", m64_ShiftLeft },
    { "SInt64ToLongDouble", m64_SInt64ToLongDouble },
    { "LongDoubleToSInt64", m64_LongDoubleToSInt64 },
    { "U64Max", m64_U64Max },
    { "U64Add", m64_U64Add },
    { "U64Subtract", m64_U64Subtract },
    { "U64Multiply", m64_U64Multiply },
    { "U64Mod", m64_U64Mod },
    { "U64Divide", m64_U64Divide },
    { "U64Set", m64_U64Set },
    { "U64SetU", m64_U64SetU },
    { "U32SetU", m64_U32SetU },
    { "U64And", m64_S64And },
    { "U64Or", m64_S64Or },
    { "U64Eor", m64_S64Eor },
    { "U64Not", m64_S64Not },
    { "U64Compare", m64_U64Compare },
    { "U64BitwiseAnd", m64_BitwiseAnd },
    { "U64BitwiseOr", m64_BitwiseOr },
    { "U64BitwiseEor", m64_BitwiseEor },
    { "U64BitwiseNot", m64_BitwiseNot },
    { "U64ShiftRight", m64_U64ShiftRight },
    { "U64ShiftLeft", m64_ShiftLeft },
    { "UInt64ToLongDouble", m64_UInt64ToLongDouble },
    { "LongDoubleToUInt64", m64_LongDoubleToUInt64 },
    { "UInt64ToSInt64", m64_Same64 },
    { "SInt64ToUInt64", m64_Same64 },
};
const unsigned cfnum_export_count = sizeof cfnum_exports / sizeof cfnum_exports[0];
