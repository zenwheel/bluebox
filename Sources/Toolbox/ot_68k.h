/*
 * Open Transport for 68k code: the table of functions the glue names
 * (ot_68k_table.c) and the Shared Library Manager that finds them (ot_68k.c).
 */
#ifndef OT_68K_H
#define OT_68K_H

typedef struct {
    const char *lib;  /* the shared library class the glue asks the manager for */
    int index;        /* the function's index in it */
    const char *name; /* the Open Transport call it is */
    char conv;        /* 'P' Pascal, 'C' C, '?' not known */
    const char *sig;  /* argument sizes: l long, w word, b byte */
    char ret;         /* result: l, w, b, v (none) */
} ot68k_fn;

extern const ot68k_fn ot68k_functions[];
extern const unsigned ot68k_function_count;

#endif
