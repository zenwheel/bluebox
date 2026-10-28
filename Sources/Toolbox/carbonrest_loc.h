/* The Locale Manager's refs, for the Unicode utilities (carbonrest_loc.c). */
#ifndef CARBONREST_LOC_H
#define CARBONREST_LOC_H

#include <stddef.h>
#include <stdint.h>

/* The identifier a LocaleRef names ("en_US"); for none, the user's own (in buf); NULL for a bad ref. */
const char *rest_locale_id(uint32_t ref, char *buf, size_t cap);

#endif
