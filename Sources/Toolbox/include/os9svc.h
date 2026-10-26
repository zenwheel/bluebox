/*
 * Mac OS 9's service libraries that programs import directly (FontManager,
 * SoundLib, MPLibrary, DriverServicesLib, PowerMgrLib, URLAccessLib,
 * NameRegistryLib, DriverLoaderLib, KeychainLib): the names beyond what the
 * Toolbox's other tables give, found by os9svc_export_fn (os9svc.c).
 */
#ifndef OS9SVC_H
#define OS9SVC_H

#include "ppc_bridge.h"

/* A name that is another export under a second name, as Mac OS 9 gave the
 * same routine several (FontManager's NQDDrawChar is DrawChar). */
typedef struct {
    const char *name, *target;
} os9_alias;

void (*os9svc_export_fn(const char *name))(ppc_cpu *);

extern const ppc_export os9_font_exports[];
extern const unsigned os9_font_export_count;
extern const os9_alias os9_font_aliases[];
extern const unsigned os9_font_alias_count;

extern const ppc_export os9_sound_exports[];
extern const unsigned os9_sound_export_count;
extern const os9_alias os9_sound_aliases[];
extern const unsigned os9_sound_alias_count;
void os9svc_sound_reset(void);
extern const ppc_export os9_driver_exports[];
extern const unsigned os9_driver_export_count;
extern const os9_alias os9_driver_aliases[];
extern const unsigned os9_driver_alias_count;
void os9svc_driver_reset(void);

/* A new run, and the Time Manager's pump (timers and queued handlers). */
void os9svc_reset(void);
void os9svc_pump(void);

#endif
