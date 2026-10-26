/*
 * SoundLib's calls beyond the Sound Manager's channels: the sound
 * component calls (a Component Manager call to the instance, as the 68k
 * inline code makes it), compression names, and the mixer and output
 * device components, which are whatever a program (or QuickTime) has
 * registered: Blue Box has no mixer or sound output device component of its
 * own, so with none registered the Component Manager's error comes back.
 */
#include <string.h>

#include "os9svc.h"
#include "ppc_bridge.h"
#include "qt_codec.h"
#include "tb_internal.h"

enum { badComponentType = -3000 };
enum { kMixerType = 0x6D697872 /* 'mixr' */ };

/* ---- Sound component calls ---- */

#define CALL(name, what, sizes) static void snd_##name(ppc_cpu *c) { qt_component_call_ppc(c, what, sizes); }
CALL(SoundComponentInitOutputDevice, 0x0001, "4")
CALL(SoundComponentSetSource, 0x0002, "44")
CALL(SoundComponentGetSource, 0x0003, "44")
CALL(SoundComponentGetSourceData, 0x0004, "4")
CALL(SoundComponentSetOutput, 0x0005, "44")
CALL(SoundComponentAddSource, 0x0101, "4")
CALL(SoundComponentRemoveSource, 0x0102, "4")
CALL(SoundComponentGetInfo, 0x0103, "444")
CALL(SoundComponentSetInfo, 0x0104, "444")
CALL(SoundComponentStartSource, 0x0105, "24")
CALL(SoundComponentStopSource, 0x0106, "24")
CALL(SoundComponentPauseSource, 0x0107, "24")
CALL(SoundComponentPlaySourceBuffer, 0x0108, "444")
CALL(SoundComponentAddMChannelSource, 0x0109, "44")
CALL(SoundComponentValidSource, 0x0110, "4")
CALL(AudioGetVolume, 0x0000, "24")
CALL(AudioSetVolume, 0x0001, "22")
CALL(AudioGetMute, 0x0002, "24")
CALL(AudioSetMute, 0x0003, "22")
CALL(AudioSetToDefaults, 0x0004, "")
CALL(AudioGetInfo, 0x0005, "4")
CALL(AudioGetBass, 0x0006, "24")
CALL(AudioSetBass, 0x0007, "22")
CALL(AudioGetTreble, 0x0008, "24")
CALL(AudioSetTreble, 0x0009, "22")
CALL(AudioGetOutputDevice, 0x000A, "4")
CALL(AudioMuteOnEvent, 0x0081, "2")

const ppc_export os9_sound_exports[] = {
#define E(name) { #name, snd_##name }
    E(SoundComponentInitOutputDevice), E(SoundComponentSetSource), E(SoundComponentGetSource),
    E(SoundComponentGetSourceData), E(SoundComponentSetOutput), E(SoundComponentAddSource),
    E(SoundComponentRemoveSource), E(SoundComponentGetInfo), E(SoundComponentSetInfo),
    E(SoundComponentStartSource), E(SoundComponentStopSource), E(SoundComponentPauseSource),
    E(SoundComponentPlaySourceBuffer), E(SoundComponentAddMChannelSource), E(SoundComponentValidSource),
    E(AudioGetVolume), E(AudioSetVolume), E(AudioGetMute), E(AudioSetMute), E(AudioSetToDefaults),
    E(AudioGetInfo), E(AudioGetBass), E(AudioSetBass), E(AudioGetTreble), E(AudioSetTreble),
    E(AudioGetOutputDevice), E(AudioMuteOnEvent),
#undef E
};
const unsigned os9_sound_export_count = sizeof os9_sound_exports / sizeof os9_sound_exports[0];

const os9_alias os9_sound_aliases[] = {
    /* the version of the tool a SoundDispatch selector reaches */
    { "SndDispatchVersion", "SndDispVersion" },
};
const unsigned os9_sound_alias_count = sizeof os9_sound_aliases / sizeof os9_sound_aliases[0];

/* ---- Compression names ---- */

/* GetCompressionName(type, *name): the name the compressor's component
 * gives, a Pascal string. */
int16_t tb_GetCompressionName(int32_t type, gaddr name)
{
    static const struct { const char *type, *name; } names[] = {
        { "NONE", "Not compressed" }, { "raw ", "8-bit offset binary" }, { "twos", "16-bit big endian" },
        { "sowt", "16-bit little endian" }, { "fl32", "32-bit floating point" },
        { "fl64", "64-bit floating point" }, { "in24", "24-bit integer" }, { "in32", "32-bit integer" },
        { "MAC3", "MACE 3:1" }, { "MAC6", "MACE 6:1" }, { "ima4", "IMA 4:1" },
        { "ulaw", "\xB5Law 2:1" }, { "alaw", "aLaw 2:1" },
    };
    if (!name)
        return paramErr;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if ((uint32_t)type == fourcc(names[i].type)) {
            size_t n = strlen(names[i].name);
            wr8(name, (uint8_t)n);
            for (size_t k = 0; k < n; k++)
                wr8(name + 1 + k, (uint8_t)names[i].name[k]);
            return noErr;
        }
    return badComponentType;
}

/* ---- The mixer and the default output device ---- */

/* OpenMixerSoundComponent(outputDescription, outputFlags, *mixer): an
 * instance of the mixer component. */
int16_t tb_OpenMixerSoundComponent(gaddr outputDescription, int32_t outputFlags, gaddr mixer)
{
    (void)outputDescription;
    (void)outputFlags;
    if (!mixer)
        return paramErr;
    wr32(mixer, 0);
    gaddr ci = components_open_default(kMixerType, 0);
    if (!ci)
        return badComponentType;
    wr32(mixer, ci);
    return noErr;
}

int16_t tb_CloseMixerSoundComponent(int32_t ci)
{
    if (!ci)
        return paramErr;
    components_close((uint32_t)ci);
    return noErr;
}

static uint32_t default_output;

void os9svc_sound_reset(void) { default_output = 0; }

/* The default sound output device component, as set. */
int16_t tb_GetDefaultSoundOutput(gaddr sc)
{
    if (!sc)
        return paramErr;
    wr32(sc, default_output);
    return default_output ? noErr : badComponentType;
}

int16_t tb_SetDefaultSoundOutput(int32_t sc)
{
    default_output = (uint32_t)sc;
    return noErr;
}
