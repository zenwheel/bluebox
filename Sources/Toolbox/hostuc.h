/*
 * The host's Unicode and locale services (hostuc.c), for the Locale
 * Manager, the Unicode Utilities and Language Analysis.  Text is UTF-16 in
 * host byte order, locales are identifier strings ("en_US"); NULL is the
 * user's own or none, as each call says.  Compiled where there is no host
 * service (not macOS), the calls say so (false or 0) and callers answer
 * with their documented errors.
 */
#ifndef HOSTUC_H
#define HOSTUC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether the host has these services. */
bool hu_available(void);

/* A locale identifier as the host canonicalises it ("en_us" -> "en_US"); false if it is not one it knows. */
bool hu_canonical_locale(const char *in, char *out, size_t cap);
/* The name of a locale in the language of displayLocale (NULL: the user's), as UTF-16. */
bool hu_locale_display_name(const char *locale, const char *displayLocale, uint16_t *out, size_t cap, size_t *n);
/* The name of a language ("en") in English, as UTF-16 (the locale's own language when region is false). */
bool hu_language_name(const char *lang, uint16_t *out, size_t cap, size_t *n);
/* The identifiers of the locales the host has, up to max (each at most 31 characters). */
size_t hu_available_locales(char (*out)[32], size_t max);
/* The user's own locale. */
bool hu_current_locale(char *out, size_t cap);

/* UCCollateOptions as UCTypes: compose-insensitive 2, width 4, case 8, diacritic 16,
 * punctuation significant 1<<15, digits override 1<<16, digits as number 1<<17. */
bool hu_compare_text(const uint16_t *a, size_t an, const uint16_t *b, size_t bn, uint32_t options,
                     const char *locale, bool use_locale, int *order);
/* A key whose lexical order is the text's order under the options; its length (max entries
 * stored, the whole length returned). */
size_t hu_collation_key(const uint16_t *t, size_t n, uint32_t options, uint32_t *out, size_t max);

/* Text breaks: unit 0 character, 1 cluster, 2 word, 3 line.  The offsets of the boundaries,
 * ascending, from 0 to n; the count (up to max stored). */
size_t hu_text_breaks(const uint16_t *t, size_t n, int unit, const char *locale, uint32_t *offs, size_t max);

/* Words of text in a language by the host's own dictionary: start and length of each
 * (punctuation and spaces are tokens too). */
size_t hu_tokens(const uint16_t *t, size_t n, const char *locale, uint32_t *start, uint32_t *len, size_t max);

#endif
