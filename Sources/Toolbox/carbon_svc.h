/*
 * CarbonLib's services (carbon_svc.c): the Carbon Printing Manager
 * (carbon_print.c), Navigation Services' Carbon dialogs (carbon_nav.c),
 * the Carbon Scrap Manager (carbon_scrap.c), ATSUI (carbon_atsui.c) and
 * the Multilingual Text Engine (carbon_mlte.c).  What they share: their
 * arguments, their universal procedure pointers and Core Foundation's
 * strings.
 */
#ifndef CARBON_SVC_H
#define CARBON_SVC_H

#include "ppc_bridge.h"
#include "tb_internal.h"

/* Argument word n of a call (GPR slots, then the parameter area). */
static inline uint32_t svc_arg(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}
#define A(n) svc_arg(c, (n))
#define RET(v)                                                                                                         \
    do {                                                                                                               \
        c->gpr[3] = (uint32_t)(v);                                                                                     \
        c->pc = c->lr;                                                                                                 \
        return;                                                                                                        \
    } while (0)
#define DONE()                                                                                                         \
    do {                                                                                                               \
        c->pc = c->lr;                                                                                                 \
        return;                                                                                                        \
    } while (0)

/* Call a universal procedure pointer a Carbon program gave us: a routine
 * descriptor (either ISA), or a bare transition vector. */
uint32_t svc_call_upp(gaddr upp, uint32_t procinfo, const uint32_t *args, int nargs);
/* A routine descriptor for 68k code of ours, so a Carbon program can
 * call it through Invoke...UPP (cached per address). */
gaddr svc_upp_for_68k(gaddr code, uint32_t procinfo);

/* Call another library export by name (one that runs without calling
 * guest code), with up to 8 arguments; its r3.  0 when there is none. */
uint32_t svc_call_export(const char *name, const uint32_t *args, int nargs);

/* Core Foundation strings, through CarbonLib's own exports: a new
 * CFString of UTF-16 units (0 when CF can't make one), and a string's
 * units (its length returned, at most max copied). */
gaddr svc_cfstring(const uint16_t *u, uint32_t n);
gaddr svc_cfstring_pascal(gaddr pstr); /* MacRoman */
uint32_t svc_cfstring_get(gaddr s, uint16_t *out, uint32_t max);
/* A CFString as a MacRoman Pascal string in out (256 bytes). */
void svc_cfstring_to_pascal(gaddr s, uint8_t *out);
bool svc_cfstring_equals(gaddr s, const char *ascii);
void svc_cfretain(gaddr cf);
void svc_cfrelease(gaddr cf);

/* UTF-16 and MacRoman: one byte per unit, '?' for what MacRoman lacks
 * (a surrogate pair is one '?', so the counts can differ only there). */
size_t svc_utf16_to_macroman(const uint16_t *u, size_t n, uint8_t *out);
size_t svc_macroman_to_utf16(const uint8_t *b, size_t n, uint16_t *out);

/* Big-endian doubles in guest memory (PMRect, PMResolution). */
double svc_rdd(gaddr p);
void svc_wrd(gaddr p, double d);
/* A Float32 in a register's bits. */
float svc_f32(uint32_t bits);
uint32_t svc_f32_bits(float f);

/* Each area's exports and reset. */
extern const ppc_export carbon_print_exports[], carbon_nav_exports[], carbon_scrap_exports[],
    carbon_atsui_exports[], carbon_mlte_exports[];
extern const unsigned carbon_print_export_count, carbon_nav_export_count, carbon_scrap_export_count,
    carbon_atsui_export_count, carbon_mlte_export_count;
void carbon_print_reset(void);
void carbon_nav_reset(void);
void carbon_scrap_reset(void);
void carbon_atsui_reset(void);
void carbon_mlte_reset(void);

/* ATSUI for the text engine: a style's QuickDraw font, size, face and
 * colour (false when it isn't a style), and UTF-16 text drawn and
 * measured in the current port in a style. */
typedef struct {
    int16_t font, size;
    uint8_t face;
    bool has_color;
    uint16_t color[3];
} svc_qdstyle;
bool atsui_style_qd(gaddr style, svc_qdstyle *out);

#endif
