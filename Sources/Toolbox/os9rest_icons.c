/*
 * Icon Services' calls beyond iconservices.c's: its start-up and
 * shut-down (nothing to do: the services are ready when a program runs),
 * and the switch for deep icons, on until a program turns it off.  Mac
 * OS 9's CloneIconRef and DisposeIconRef are AcquireIconRef and
 * ReleaseIconRef under their earlier names (os9rest.c's aliases).
 */
#include "tb_internal.h"

static bool deep_icons = true;

void os9rest_icons_reset(void) { deep_icons = true; }

int16_t tb_IconServicesInit(gaddr initBlockPtr) { return noErr; }
void tb_IconServicesTerminate(void) {}

int16_t tb_GetDeepIconsEnabled(gaddr deepIconsEnabled)
{
    if (deepIconsEnabled)
        wr8(deepIconsEnabled, deep_icons);
    return noErr;
}

int16_t tb_SetDeepIconsEnabled(int8_t enableDeepIcons)
{
    deep_icons = enableDeepIcons != 0;
    return noErr;
}
