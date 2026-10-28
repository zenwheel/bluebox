/* The interpreter again, for the models whose differences cost every
 * access something: the 68000's address errors and the 68020's and
 * 68030's instruction caches (see cpu68k.c). */
#define CPU68K_STRICT 1
#include "cpu68k.c"
