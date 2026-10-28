/*
 * CarbonLib's remaining managers (carbonrest*.c): the ones that are neither
 * QuickTime nor Open Transport and that no other Carbon file takes: ColorSync,
 * HTML Rendering, the Dictionary Manager, Speech Recognition, the Name
 * Service Library, FontSync, Find By Content, the Apple Event helpers, Image
 * Capture, the OSA's debugging calls, Language Analysis, Locales, Unicode
 * utilities, Internet Config's profiles, Translation and the small ones.
 * Each has its own export table; carbonrest.c is the lookup over them.
 */
#ifndef CARBONREST_H
#define CARBONREST_H

#include "carbon_svc.h"

/* Argument n as the C types the headers give it. */
#define S16(n) ((int16_t)A(n))
#define U16(n) ((uint16_t)A(n))
#define S8(n) ((int8_t)A(n))
#define BOOL(n) ((A(n) & 0xFF) != 0)

/* Common error codes (MacErrors.h) the areas share. */
enum {
    rest_unimpErr = -4,
    rest_nilHandleErr = -109,
    rest_noHardwareErr = -200,
    rest_notOpenErr = -28,
    rest_paramErr = -50,
    rest_memFullErr = -108,
    rest_fnfErr = -43,
    rest_afpItemNotFound = -5012,
    rest_dupFNErr = -48,
    rest_badFormat = -206,
    rest_eofErr = -39,
};

/* Pascal-string and C-string helpers. */
size_t rest_cstr(gaddr p, char *out, size_t max);          /* a C string from guest memory */
void rest_pstr_put(gaddr dst, const char *s, size_t max);   /* a Pascal string at dst (max chars) */
void rest_pstr_get(gaddr p, char *out, size_t max);         /* a Pascal string as a C string */

/* Each area's exports and reset. */
#define REST_AREA(name)                                                                                                \
    extern const ppc_export name##_exports[];                                                                          \
    extern const unsigned name##_export_count;                                                                         \
    void name##_reset(void)
REST_AREA(rest_misc);
REST_AREA(rest_ae);
REST_AREA(rest_osa);
REST_AREA(rest_ic);

/* The lookup over every area, and the reset (carbonrest.c). */
void (*carbonrest_export_fn(const char *name))(ppc_cpu *);
void carbonrest_reset(void);

#endif
