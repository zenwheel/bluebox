/*
 * Gestalt.  Values describe the machine we present; the environment profile
 * can override any selector (gestalt_set).
 */
#include "m68k.h"
#include "ppc_bridge.h"
#include "r3d.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_GESTALT 128

typedef struct {
    uint32_t selector;
    uint32_t value;
    gaddr proc; /* guest selector function, if installed with NewGestalt */
} gestalt_entry;

static gestalt_entry table[MAX_GESTALT];
static unsigned count;
/* The run's own answers (--gestalt), kept over the PowerPC ones. */
static gestalt_entry overrides[16];
static unsigned noverrides;

void gestalt_set(uint32_t selector, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == selector) {
            table[i].value = value;
            table[i].proc = 0;
            return;
        }
    }
    if (count < MAX_GESTALT)
        table[count++] = (gestalt_entry){ selector, value, 0 };
}

void gestalt_override(uint32_t selector, uint32_t value)
{
    gestalt_set(selector, value);
    if (selector == fourcc("sysv"))
        wr16(LM_SysVersion, (uint16_t)value);
    for (unsigned i = 0; i < noverrides; i++)
        if (overrides[i].selector == selector) {
            overrides[i].value = value;
            return;
        }
    if (noverrides < sizeof overrides / sizeof overrides[0])
        overrides[noverrides++] = (gestalt_entry){ selector, value, 0 };
}

bool gestalt_exists(uint32_t selector)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == selector)
            return true;
    return false;
}

void gestalt_delete(uint32_t selector)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == selector) {
            table[i] = table[--count];
            return;
        }
}

/* A selector's fixed value (not one answered by a guest function). */
uint32_t gestalt_value(uint32_t selector, uint32_t fallback)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == selector && !table[i].proc)
            return table[i].value;
    return fallback;
}

void gestalt_init(void)
{
    count = 0;
    noverrides = 0;
    gestalt_set(fourcc("sysv"), 0x0710);      /* System 7.1 */
    gestalt_set(fourcc("thds"), 0x3);         /* the Thread Manager, with exact-match pools */
    gestalt_set(fourcc("sysa"), 1);           /* 68k architecture */
    /* The machine standing for the processor (machine.c): a Mac SE, II,
     * IIci or Quadra 800. */
    gestalt_set(fourcc("mach"), (uint32_t)machine_gestalt_type());
    /* Open Transport for 68k programs, on the host's sockets: through the glue and the
     * Shared Library Manager ot_68k.c stands in for (the same TCP/IP and AppleTalk
     * the PowerPC programs get) */
    gestalt_set(fourcc("otan"), 0x33 | (appletalk_enabled() ? 0x0C : 0));
    gestalt_set(fourcc("otvr"), 0x01208000);
    /* gestalt68000 1, gestalt68020 3, gestalt68030 4, gestalt68040 5; the
     * FPU (gestalt68881 1, gestalt68882 2, gestalt68040FPU 3) and MMU as
     * MMUType has them. */
    int model = machine_cpu_model(), fpu = machine_fpu_model();
    gestalt_set(fourcc("proc"), model == 68000 ? 1 : (uint32_t)(model - 68000) / 10 + 1);
    gestalt_set(fourcc("fpu "), fpu == 68881 ? 1 : fpu == 68882 ? 2 : fpu == 68040 ? 3 : 0);
    gestalt_set(fourcc("mmu "), (uint32_t)machine_mmu_type());
    gestalt_set(fourcc("qd  "), 0x0230);      /* gestalt32BitQD13: 32-Bit QuickDraw 1.3 */
    /* gestaltQuickdrawFeatures: colour, deep GWorlds, direct pixel maps,
     * grayish text-or. */
    gestalt_set(fourcc("qdrw"), 0x0F);
    /* gestaltAddressingModeAttr: 32-bit addressing, system zone and
     * capable; in 24-bit mode capable only (not on a 68000). */
    gestalt_set(fourcc("addr"), !g_mem_24bit ? 0x7 : model == 68000 ? 0 : 0x4);
    gestalt_set(fourcc("ram "), g_mem_report_cap);
    gestalt_set(fourcc("qtim"), 0x02508000); /* QuickTime 2.5 (movies play as zero length) */
    gestalt_set(fourcc("cpnt"), 0x00030000); /* the Component Manager, as QuickTime 2.5 has it */
    gestalt_set(fourcc("lram"), g_mem_report_cap);
    gestalt_set(fourcc("vm  "), 0);
    gestalt_set(fourcc("rsrc"), 0x3);         /* Resource Manager: partial, has GetResourceSizeOnDisk */
    gestalt_set(fourcc("rom "), 0x077D);      /* ROM version */
    gestalt_set(fourcc("romv"), 0x077D);
    gestalt_set(fourcc("kbd "), 4);           /* Apple Extended Keyboard */
    /* gestaltOSAttr: growable system zone, LaunchApplication that can
     * return, full file specs, launch control, temporary memory (real and
     * tracked); no PPC Toolbox or debugger support.  Solitaire Till Dawn
     * took the old lone bit for an older System. */
    gestalt_set(fourcc("os  "), 0x7F);
    gestalt_set(fourcc("evnt"), 0x1);         /* Apple Event Manager present */
    /* What else System 7.1 answered, for the managers we provide. */
    gestalt_set(fourcc("vers"), 1);           /* Gestalt itself */
    gestalt_set(fourcc("scri"), 0x0710);      /* Script Manager */
    gestalt_set(fourcc("scr#"), 1);           /* one script system: Roman */
    gestalt_set(fourcc("te  "), 5);           /* TextEdit (gestaltTE5) */
    gestalt_set(fourcc("hdwr"), 0x009B);      /* VIA1, VIA2, ASC, SCC, SCSI */
    gestalt_set(fourcc("scsi"), 0x3);         /* SCSI Manager 4.3, in ROM (scsi.c: no devices) */
    gestalt_set(fourcc("ppc "), 0x1000);      /* the PPC Toolbox, real-time sessions on this machine (ppctoolbox.c) */
    gestalt_set(fourcc("edtn"), 0x1);         /* Edition Manager present */
    gestalt_set(fourcc("help"), 0x1);         /* Help Manager present */
    gestalt_set(fourcc("alis"), 0x1);         /* Alias Manager present */
    gestalt_set(fourcc("fold"), 0x1);         /* Folder Manager (FindFolder) present */
    gestalt_set(fourcc("fs  "), 0x2);         /* FSSpec calls */
    gestalt_set(fourcc("stdf"), 0x1);         /* Standard File 5.8 calls */
    /* The Icon Utilities, with Icon Services' 48-pixel icons, 32-bit
     * colour and 8-bit masks, and Icon Services itself (IconServicesLib). */
    gestalt_set(fourcc("icon"), 0x1F);
    gestalt_set(fourcc("tmgr"), 3);           /* extended Time Manager */
    /* Sound Manager 3: stereo, stereo mixing, the Sound I/O Manager, a
     * built-in sound input device (the host's microphone), playing while
     * recording, 16-bit sound I/O, stereo input, SndPlayDoubleBuffer,
     * several channels, 16-bit audio. */
    gestalt_set(fourcc("snd "), 0x1DFB);
    gestalt_set(fourcc("qdgx"), 0x01100000);    /* gestaltGXVersion: QuickDraw GX 1.1 (printing, shapes) */
    gestalt_set(fourcc("pmgr"), 0x01100000);    /* gestaltGXPrintingMgrVersion */
    gestalt_set(fourcc("teat"), 0x1);           /* gestaltTEAttr: TEGetHiliteRgn */
    gestalt_set(fourcc("drag"), 0x1);           /* gestaltDragMgrAttr: the Drag Manager */
    gestalt_set(fourcc("cmnu"), 0x2);           /* gestaltContextualMenuTrapAvailable */
    /* gestaltPopupPresent (the pop-up menu CDEF, 63) and
     * gestaltNotificationPresent, which System 7 has: AgendaMaker stops
     * with an error at launch without them. */
    gestalt_set(fourcc("pop!"), 0x1);
    gestalt_set(fourcc("nmgr"), 0x1);
    gestalt_set(fourcc("ttsc"), 0x1);           /* gestaltSpeechAttr: the Speech Manager */
    /* gestaltOutlineFonts: the Font Manager has the outline font calls
     * (FontDispatch), as every System 7 has; the text itself is drawn
     * from bitmap strikes. MacPerl's runtime takes it to mean System 7
     * and won't run without it. */
    gestalt_set(fourcc("font"), 0x1);
    /* The Appearance Manager for 68k programs too (as if the Appearance
     * extension were installed): programs ask before using its calls, and
     * draw their lists and controls in Platinum when it is there.  Version
     * 1.1, Mac OS 8.5's: programs take it to mean Mac OS 8.5 and its
     * libraries (Greg's Browser then calls Icon Services), and some require
     * it (sixtyforce). */
    gestalt_set(fourcc("appr"), 0x1);         /* gestaltAppearanceExists */
    gestalt_set(fourcc("apvr"), 0x0110);      /* gestaltAppearanceVersion */
    /* Blue Box itself, for software written for it: version 1.0 in the
     * high word; the host, macOS (bit 0) or Linux (bit 1).  Not Classic's
     * 'bbox', which says Mac OS X is underneath. */
#ifdef __APPLE__
    gestalt_set(fourcc("blbx"), 0x01000001);
#else
    gestalt_set(fourcc("blbx"), 0x01000002);
#endif
}

/* A PowerPC application: a PowerPC Mac's answers (a G3 with Mac OS 8.1,
 * the Code Fragment Manager, Mixed Mode, QuickTime's PowerPC library).
 * 8.1, as the Appearance Manager, Navigation Services, Contextual Menus
 * and Open Transport are there: Setback, Spades and sixtyforce ask for
 * it, and 8.5 would have programs look for Icon Services. */
void gestalt_powerpc(void)
{
    gestalt_set(fourcc("sysa"), 2);      /* gestaltPowerPC */
    gestalt_set(fourcc("sysv"), 0x0810);
    wr16(LM_SysVersion, 0x0810);         /* SysEnvirons reads it */
    gestalt_set(fourcc("qtrs"), 0x1);    /* gestaltPPCQuickTimeLibPresent */
    gestalt_set(fourcc("dplv"), 0x00020000); /* the Display Manager, 2.0 */
    gestalt_set(fourcc("dply"), 0x1);    /* gestaltDisplayMgrPresent */
    gestalt_set(fourcc("thds"), 0x7);    /* and gestaltThreadsLibraryPresent */
    /* Open Transport with TCP/IP over the host's sockets (opentransport.c),
     * and AppleTalk on our stack when it's on (ot_appletalk.c) */
    gestalt_set(fourcc("otan"), 0x33 | (appletalk_enabled() ? 0x0C : 0)); /* present and loaded; TCP, AppleTalk */
    gestalt_set(fourcc("otvr"), 0x01208000); /* 1.2 */
    gestalt_set(fourcc("cput"), 0x0108); /* gestaltCPU750 */
    gestalt_set(fourcc("cfrg"), 0x1);    /* gestaltCFMPresent */
    gestalt_set(fourcc("mixd"), 0x1);    /* gestaltPowerPCAware */
    /* CarbonLib 1.1 (gestaltCarbonVersion): the Carbon Event Manager, the
     * version's mark, is there (carbon_events.c), with what CarbonLib
     * shares with InterfaceLib and its own additions as far as they go. */
    gestalt_set(fourcc("cbon"), 0x0110);
    gestalt_set(fourcc("fpu "), 0);
    /* QuickDraw 3D 1.6 and its Viewer, where the host can render them
     * (q3_*.c, r3d.h). */
    if (r3d_available()) {
        gestalt_set(fourcc("qd3d"), 0x1);        /* gestaltQD3DAvailable */
        gestalt_set(fourcc("q3v "), 0x01608000); /* gestaltQD3DVersion: 1.6 */
        gestalt_set(fourcc("q3vc"), 0x1);        /* gestaltQD3DViewerAvailable */
    }
    for (unsigned i = 0; i < noverrides; i++)
        gestalt_override(overrides[i].selector, overrides[i].value);
}

static int16_t gestalt_lookup(int32_t selector, uint32_t *responsep)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == (uint32_t)selector) {
            if (table[i].proc) {
                /* FUNCTION selFunc(selector: OSType; VAR response: LONGINT): OSErr */
                cpu68k *c = g_cpu;
                gaddr resp = tb_NewPtr(4, false, true);
                cpu_push16(c, 0);
                cpu_push32(c, (uint32_t)selector);
                cpu_push32(c, resp);
                guest_call(table[i].proc);
                int16_t err = (int16_t)cpu_pop16(c);
                *responsep = rd32(resp);
                tb_DisposePtr(resp);
                return err;
            }
            *responsep = table[i].value;
            return noErr;
        }
    }
    *responsep = 0;
    return gestaltUnknownErr;
}

/* Open Transport's selectors asked from 68k code (the trap, as the glue linked
 * into 68k programs and the 68k code in PowerPC ones asks): the glue goes on
 * to find the Shared Library Manager through the Expand Memory record, which
 * ot_68k.c makes ready. */
static bool asked_by_68k_code(uint32_t selector)
{
    if (selector != fourcc("otan") && selector != fourcc("otvr"))
        return false;
    if (!g_cpu || (rd16(g_cpu->op_pc) & 0xFEFF) != 0xA0AD) /* not from the trap */
        return false;
    return !ppc_trap_word_at(g_cpu->op_pc);
}

int16_t tb_Gestalt(int32_t selector, uint32_t *responsep)
{
    int16_t err = gestalt_lookup(selector, responsep);
    if (!err && asked_by_68k_code((uint32_t)selector))
        ot_68k_prepare();
    if (g_log_mask & LOG_TRAPS) {
        char tc[5];
        log_printf("  Gestalt '%s' -> %d, %08X\n", fourcc_str((uint32_t)selector, tc), err, *responsep);
    }
    return err;
}

int16_t tb_NewGestalt(int32_t selector, gaddr selFunc)
{
    for (unsigned i = 0; i < count; i++)
        if (table[i].selector == (uint32_t)selector)
            return -5552; /* gestaltDupSelectorErr */
    if (count >= MAX_GESTALT)
        return memFullErr;
    table[count++] = (gestalt_entry){ (uint32_t)selector, 0, selFunc };
    return noErr;
}

/* Mac OS 7.5's values (_GestaltValueDispatch, and InterfaceLib's): a
 * selector answered with a value rather than a function.  New fails on one
 * that exists, Replace and Delete on one that doesn't, Set does either. */
int16_t tb_NewGestaltValue(int32_t selector, int32_t newValue)
{
    if (gestalt_exists((uint32_t)selector))
        return -5552; /* gestaltDupSelectorErr */
    if (count >= MAX_GESTALT)
        return memFullErr;
    gestalt_set((uint32_t)selector, (uint32_t)newValue);
    return noErr;
}

int16_t tb_ReplaceGestaltValue(int32_t selector, int32_t replacementValue)
{
    if (!gestalt_exists((uint32_t)selector))
        return -5551; /* gestaltUndefSelectorErr */
    gestalt_set((uint32_t)selector, (uint32_t)replacementValue);
    return noErr;
}

int16_t tb_SetGestaltValue(int32_t selector, int32_t newValue)
{
    if (!gestalt_exists((uint32_t)selector) && count >= MAX_GESTALT)
        return memFullErr;
    gestalt_set((uint32_t)selector, (uint32_t)newValue);
    return noErr;
}

int16_t tb_DeleteGestaltValue(int32_t selector)
{
    if (!gestalt_exists((uint32_t)selector))
        return -5551; /* gestaltUndefSelectorErr */
    gestalt_delete((uint32_t)selector);
    return noErr;
}

int16_t tb_ReplaceGestalt(int32_t selector, gaddr selFunc, uint32_t *oldSelFuncp)
{
    for (unsigned i = 0; i < count; i++) {
        if (table[i].selector == (uint32_t)selector) {
            *oldSelFuncp = table[i].proc;
            table[i].proc = selFunc;
            return noErr;
        }
    }
    *oldSelFuncp = 0;
    return gestaltUnknownErr;
}
