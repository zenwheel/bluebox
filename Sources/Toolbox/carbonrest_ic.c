/*
 * Internet Config's calls from the 2.0 and 2.5 interfaces that had no
 * PowerPC export: the profiles, the defaults, GURL events and the access
 * mode.  The component answers (components.c); these call it as any
 * client does.
 */
#include "carbonrest.h"

/* A call to the instance (ComponentCallNow): the instance and the
 * arguments (sizes 4 or 2) pushed, the parameter size and selector last,
 * then _ComponentDispatch with D0 = 0. */
static uint32_t instance_call(ppc_cpu *c, int16_t what, const char *sizes)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, A(0));
    uint32_t size = 0;
    for (int i = 0; sizes[i]; i++) {
        int n = sizes[i] - '0';
        ppc_trap_push(n, A(1 + (unsigned)i));
        size += n == 4 ? 4 : 2; /* a byte takes a word */
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    return r;
}

#define CALL(name, sel, sizes) static void ic_##name(ppc_cpu *c) { RET(instance_call(c, sel, sizes)); }
CALL(ICGetPerm, 0x0D, "4")
/* The component-level names (ICC...) and the later calls with no library glue */
CALL(ICCAddMapEntry, 0x0017, "44")
CALL(ICCAddProfile, 0x002B, "44")
CALL(ICCBegin, 0x0005, "1")
CALL(ICCChooseConfig, 0x0021, "")
CALL(ICCChooseNewConfig, 0x0022, "")
CALL(ICCCountMapEntries, 0x0012, "44")
CALL(ICCCountPref, 0x0008, "4")
CALL(ICCCountProfiles, 0x0027, "4")
CALL(ICCCreateGURLEvent, 0x0033, "444")
CALL(ICCDefaultFileName, 0x000B, "4")
CALL(ICCDeleteMapEntry, 0x0016, "44")
CALL(ICCDeletePref, 0x000C, "4")
CALL(ICCDeleteProfile, 0x002C, "4")
CALL(ICCEditPreferences, 0x000F, "4")
CALL(ICCEnd, 0x000A, "")
CALL(ICCFindConfigFile, 0x0002, "24")
CALL(ICCFindPrefHandle, 0x0024, "444")
CALL(ICCFindUserConfigFile, 0x000E, "4")
CALL(ICCGeneralFindConfigFile, 0x001E, "1124")
CALL(ICCGetConfigName, 0x0023, "14")
CALL(ICCGetConfigReference, 0x001F, "4")
CALL(ICCGetCurrentProfile, 0x0025, "4")
CALL(ICCGetDefaultPref, 0x0031, "44")
CALL(ICCGetIndMapEntry, 0x0013, "4444")
CALL(ICCGetIndPref, 0x0009, "44")
CALL(ICCGetIndProfile, 0x0028, "44")
CALL(ICCGetMapEntry, 0x0014, "444")
CALL(ICCGetMappingInterruptSafe, 0x002E, "44")
CALL(ICCGetPerm, 0x000D, "4")
CALL(ICCGetPref, 0x0006, "4444")
CALL(ICCGetPrefHandle, 0x001A, "444")
CALL(ICCGetProfileName, 0x0029, "44")
CALL(ICCGetSeed, 0x0004, "4")
CALL(ICCGetSeedInterruptSafe, 0x0030, "4")
CALL(ICCGetVersion, 0x0032, "44")
CALL(ICCLaunchURL, 0x0011, "44444")
CALL(ICCMapEntriesFilename, 0x001C, "444")
CALL(ICCMapEntriesTypeCreator, 0x001D, "44444")
CALL(ICCMapFilename, 0x0018, "44")
CALL(ICCMapTypeCreator, 0x0019, "4444")
CALL(ICCParseURL, 0x0010, "444444")
CALL(ICCRefreshCaches, 0x002F, "")
CALL(ICCRequiresInterruptSafe, 0x002D, "")
CALL(ICCSendGURLEvent, 0x0034, "4")
CALL(ICCSetConfigReference, 0x0020, "44")
CALL(ICCSetCurrentProfile, 0x0026, "4")
CALL(ICCSetMapEntry, 0x0015, "444")
CALL(ICCSetPref, 0x0007, "4444")
CALL(ICCSetPrefHandle, 0x001B, "444")
CALL(ICCSetProfileName, 0x002A, "44")
CALL(ICCSpecifyConfigFile, 0x0003, "4")
CALL(ICChooseNewConfig, 0x0022, "")
CALL(ICDefaultFileName, 0x000B, "4")
CALL(ICGetMappingInterruptSafe, 0x002E, "44")
CALL(ICGetSeedInterruptSafe, 0x0030, "4")
CALL(ICRefreshCaches, 0x002F, "")
CALL(ICRequiresInterruptSafe, 0x002D, "")
CALL(ICSpecifyConfigFile, 0x0003, "4")
CALL(ICGetCurrentProfile, 0x25, "4")
CALL(ICSetCurrentProfile, 0x26, "4")
CALL(ICCountProfiles, 0x27, "4")
CALL(ICGetIndProfile, 0x28, "44")
CALL(ICGetProfileName, 0x29, "44")
CALL(ICSetProfileName, 0x2A, "44")
CALL(ICAddProfile, 0x2B, "44")
CALL(ICDeleteProfile, 0x2C, "4")
CALL(ICGetDefaultPref, 0x31, "44")
CALL(ICCreateGURLEvent, 0x33, "444")
CALL(ICSendGURLEvent, 0x34, "4")

/* ICCStart, ICCStop and ICCGetComponentInstance are the library's glue under other names. */
static void ic_forward(ppc_cpu *c, const char *name)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (fn)
        fn(c);
    else
        RET(rest_unimpErr);
}
static void ic_ICCStart(ppc_cpu *c) { ic_forward(c, "ICStart"); }
static void ic_ICCStop(ppc_cpu *c) { ic_forward(c, "ICStop"); }
static void ic_ICCGetComponentInstance(ppc_cpu *c) { ic_forward(c, "ICGetComponentInstance"); }

void rest_ic_reset(void) {}

#define X(name) { #name, ic_##name }
const ppc_export rest_ic_exports[] = {
    X(ICAddProfile),
    X(ICCAddMapEntry),
    X(ICCAddProfile),
    X(ICCBegin),
    X(ICCChooseConfig),
    X(ICCChooseNewConfig),
    X(ICCCountMapEntries),
    X(ICCCountPref),
    X(ICCCountProfiles),
    X(ICCCreateGURLEvent),
    X(ICCDefaultFileName),
    X(ICCDeleteMapEntry),
    X(ICCDeletePref),
    X(ICCDeleteProfile),
    X(ICCEditPreferences),
    X(ICCEnd),
    X(ICCFindConfigFile),
    X(ICCFindPrefHandle),
    X(ICCFindUserConfigFile),
    X(ICCGeneralFindConfigFile),
    X(ICCGetComponentInstance),
    X(ICCGetConfigName),
    X(ICCGetConfigReference),
    X(ICCGetCurrentProfile),
    X(ICCGetDefaultPref),
    X(ICCGetIndMapEntry),
    X(ICCGetIndPref),
    X(ICCGetIndProfile),
    X(ICCGetMapEntry),
    X(ICCGetMappingInterruptSafe),
    X(ICCGetPerm),
    X(ICCGetPref),
    X(ICCGetPrefHandle),
    X(ICCGetProfileName),
    X(ICCGetSeed),
    X(ICCGetSeedInterruptSafe),
    X(ICCGetVersion),
    X(ICCLaunchURL),
    X(ICCMapEntriesFilename),
    X(ICCMapEntriesTypeCreator),
    X(ICCMapFilename),
    X(ICCMapTypeCreator),
    X(ICCParseURL),
    X(ICCRefreshCaches),
    X(ICCRequiresInterruptSafe),
    X(ICCSendGURLEvent),
    X(ICCSetConfigReference),
    X(ICCSetCurrentProfile),
    X(ICCSetMapEntry),
    X(ICCSetPref),
    X(ICCSetPrefHandle),
    X(ICCSetProfileName),
    X(ICCSpecifyConfigFile),
    X(ICCStart),
    X(ICCStop),
    X(ICChooseNewConfig),
    X(ICCountProfiles),
    X(ICCreateGURLEvent),
    X(ICDefaultFileName),
    X(ICDeleteProfile),
    X(ICGetCurrentProfile),
    X(ICGetDefaultPref),
    X(ICGetIndProfile),
    X(ICGetMappingInterruptSafe),
    X(ICGetPerm),
    X(ICGetProfileName),
    X(ICGetSeedInterruptSafe),
    X(ICRefreshCaches),
    X(ICRequiresInterruptSafe),
    X(ICSendGURLEvent),
    X(ICSetCurrentProfile),
    X(ICSetProfileName),
    X(ICSpecifyConfigFile),
};
const unsigned rest_ic_export_count = sizeof rest_ic_exports / sizeof rest_ic_exports[0];
