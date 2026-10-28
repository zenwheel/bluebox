/*
 * SoundLib's remaining exports that Blue Box can back.  Their argument
 * lists are Blue Box's own (no header gives them):
 *
 *   SPBGetDefaultDevice(*name), SPBSetDefaultDevice(name): the sound input
 *     device calls without a refNum go to ("Built-in", a Pascal string)
 *   OpenSoundOutputDevice(subType, *instance), CloseSoundOutputDevice(
 *     instance): an instance of the sound output device component
 *     ('sdev'), an error from the Component Manager when none is registered
 *   MakeSoundComponentChain(instances[], count, *last): each component's
 *     source set to the one before, so that audio flows from the first to
 *     the last
 *   SndDispatchAddTool(selector, proc), SndDispatchRemoveTool(selector),
 *     SndDispatchToolAddress(selector): the table of tools that take the
 *     selectors of _SoundDispatch beyond the Sound Manager's own
 *   DisposeSoundGlobals(): every channel the program opened is disposed of
 *
 * Left out: the mixer channels' calls (MChannel*, MoreMChannelData and
 * MChannelDataComplete with their routine descriptors) and the sound clock's
 * (ClockRestartSound, ClockUpdateSoundSamples).  They exist in Apple's
 * mixer component and its sound clock; Blue Box has neither, no header
 * gives their protocol, and the clock it has follows the machine's time,
 * not the samples played.
 */
#include <string.h>

#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9rest.h"
#include "os9left.h"

enum { siBadDeviceName = -228, badComponentType = -3000 };

void sound_app_dead(void);

static void sl_SPBGetDefaultDevice(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    gaddr icon = tb_NewPtr(4, false, true);
    if (!icon)
        RETURN(memFullErr);
    int16_t e = tb_SPBGetIndexedDevice(1, R(0), icon);
    gaddr h = rd32(icon);
    tb_DisposePtr(icon);
    if (h)
        tb_DisposeHandle(h);
    RETURN(e);
}

static void sl_SPBSetDefaultDevice(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    gaddr name = tb_NewPtr(256, false, true), icon = tb_NewPtr(4, false, true);
    if (!name || !icon)
        RETURN(memFullErr);
    int16_t e = tb_SPBGetIndexedDevice(1, name, icon);
    if (rd32(icon))
        tb_DisposeHandle(rd32(icon));
    bool same = !e && rd8(name) == rd8(R(0));
    for (unsigned i = 1; same && i <= rd8(name); i++)
        same = rd8(name + i) == rd8(R(0) + i);
    tb_DisposePtr(name);
    tb_DisposePtr(icon);
    RETURN(same ? noErr : siBadDeviceName);
}

static void sl_OpenSoundOutputDevice(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), 0);
    gaddr ci = components_open_default(0x73646576 /* 'sdev' */, R(0));
    if (!ci)
        RETURN(badComponentType);
    wr32(R(1), ci);
    RETURN(noErr);
}

static void sl_CloseSoundOutputDevice(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    components_close(R(0));
    RETURN(noErr);
}

static void sl_MakeSoundComponentChain(ppc_cpu *c)
{
    if (!R(0) || (int32_t)R(1) < 1 || !R(2))
        RETURN(paramErr);
    gaddr list = R(0), out = R(2);
    uint32_t n = R(1);
    for (uint32_t i = 1; i < n; i++) {
        uint32_t a[3] = { rd32(list + 4 * i), 1, rd32(list + 4 * (i - 1)) };
        int32_t e = (int32_t)os9rest_call(c, "SoundComponentSetSource", a, 3);
        if (e)
            RETURN(e);
    }
    wr32(out, rd32(list + 4 * (n - 1)));
    RETURN(noErr);
}

#define MAX_TOOLS 16
static struct { bool used; uint16_t selector; gaddr proc; } tools[MAX_TOOLS];

static void sl_SndDispatchAddTool(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    int free_slot = -1;
    for (int i = 0; i < MAX_TOOLS; i++) {
        if (tools[i].used && tools[i].selector == (uint16_t)R(0))
            RETURN(paramErr);
        if (!tools[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        RETURN(memFullErr);
    tools[free_slot].used = true;
    tools[free_slot].selector = (uint16_t)R(0);
    tools[free_slot].proc = R(1);
    RETURN(noErr);
}

static void sl_SndDispatchRemoveTool(ppc_cpu *c)
{
    for (int i = 0; i < MAX_TOOLS; i++)
        if (tools[i].used && tools[i].selector == (uint16_t)R(0)) {
            tools[i].used = false;
            RETURN(noErr);
        }
    RETURN(paramErr);
}

static void sl_SndDispatchToolAddress(ppc_cpu *c)
{
    for (int i = 0; i < MAX_TOOLS; i++)
        if (tools[i].used && tools[i].selector == (uint16_t)R(0))
            RETURN(tools[i].proc);
    RETURN(0);
}

static void sl_DisposeSoundGlobals(ppc_cpu *c)
{
    sound_app_dead();
    RETURN(noErr);
}

void os9left_sound_reset(void) { memset(tools, 0, sizeof tools); }

const ppc_export os9left_sound_exports[] = {
    { "CloseSoundOutputDevice", sl_CloseSoundOutputDevice },
    { "DisposeSoundGlobals", sl_DisposeSoundGlobals },
    { "MakeSoundComponentChain", sl_MakeSoundComponentChain },
    { "OpenSoundOutputDevice", sl_OpenSoundOutputDevice },
    { "SPBGetDefaultDevice", sl_SPBGetDefaultDevice },
    { "SPBSetDefaultDevice", sl_SPBSetDefaultDevice },
    { "SndDispatchAddTool", sl_SndDispatchAddTool },
    { "SndDispatchRemoveTool", sl_SndDispatchRemoveTool },
    { "SndDispatchToolAddress", sl_SndDispatchToolAddress },
};
const unsigned os9left_sound_export_count = sizeof os9left_sound_exports / sizeof os9left_sound_exports[0];
