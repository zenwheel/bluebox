/* Booting the environment and launching an application. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "forks.h"
#include "tb_internal.h"
#include "machine.h"

cpu68k *g_cpu;
char g_app_dir[1024];

static cpu68k cpu;
static uint64_t ticks_base_ns;

uint32_t mac_now(void);

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void init_lowmem(void)
{
    wr32(LM_MemTop, GA_ROM);
    wr32(LM_BufPtr, GA_A5WORLD_TOP);
    wr32(LM_ROMBase, GA_ROM);
    wr16(LM_ROM85, 0x3FFF);           /* new ROMs, Color QuickDraw */
    wr8(LM_CPUFlag, 2);               /* 68020 */
    wr8(LM_MMU32Bit, 1);              /* 32-bit addressing */
    wr16(LM_SysVersion, 0x0710);
    wr32(LM_Lo3Bytes, 0x00FFFFFF);
    wr32(LM_MinusOne, 0xFFFFFFFF);
    wr32(LM_OneOne, 0x00010001);
    wr32(LM_Time, mac_now());
    wr32(LM_Ticks, 0);
    wr8(LM_QDExist, 0xFF);            /* QuickDraw not initialised */
    wr8(LM_WWExist, 0xFF);            /* Window Manager not initialised */
    wr32(LM_DoubleTime, 30);
    wr32(LM_CaretTime, 32);
    wr16(LM_KeyThresh, 16);
    wr16(LM_KeyRepThresh, 4);
    wr16(LM_SysFontFam, 0);
    wr16(LM_SysFontSiz, 12);
    wr16(LM_MBarHeight, 20);
    wr32(LM_ScrnBase, GA_SCREEN);
    wr16(LM_ApFontID, 1);
    wr8(LM_ScrDmpEnb, 0xFF);

    /* Fake ROM header: version word at ROMBase+8. */
    wr32(GA_ROM, 0x420DBFF3);
    wr16(GA_ROM + 8, 0x077D);
}

bool machine_boot(bool census)
{
    if (!mem_init())
        return false;
    mem_reset();
    cpu_init(&cpu);
    g_cpu = &cpu;
    cpu.aline = trap_aline;
    cpu.native = trap_native;
    cpu.sr = 0x2000; /* supervisor, interrupts enabled */

    g_trap_census = census;
    init_lowmem();
    trap_init();
    gestalt_init();

    /* Heaps: the system zone, then a growable application zone. */
    mm_init_zone(GA_SYSHEAP, GA_SYSHEAP_END, 64);
    wr32(LM_SysZone, GA_SYSHEAP);
    mm_init_zone(GA_APPHEAP, GA_APPHEAP + 0x00100000u, 64);
    wr32(LM_ApplZone, GA_APPHEAP);
    wr32(LM_TheZone, GA_APPHEAP);
    wr32(LM_ApplLimit, GA_APPLLIMIT);
    wr32(LM_HeapEnd, Zone_bkLim(GA_APPHEAP));
    mm_set_appl_growth(GA_APPLLIMIT);

    /* The System file: an empty resource map for now. */
    rm_init();
    wr32(LM_TheZone, GA_SYSHEAP);
    if (rm_open_empty_system() < 0)
        return false;
    wr32(LM_TheZone, GA_APPHEAP);

    /* No documents to open. */
    gaddr parms = tb_NewHandle(4, false, true);
    wr32(LM_AppParmHandle, parms);

    ticks_base_ns = now_ns();
    return true;
}

bool machine_launch(const char *app_path, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(app_path, &mf, err, errlen))
        return false;
    if (mf.rsrc_len == 0) {
        snprintf(err, errlen, "%s has no resource fork", app_path);
        mac_file_free(&mf);
        return false;
    }
    /* The app's folder stands in for the default volume until the File Manager exists. */
    snprintf(g_app_dir, sizeof g_app_dir, "%s", app_path);
    char *slash = strrchr(g_app_dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(g_app_dir, ".");

    const char *base = strrchr(app_path, '/');
    base = base ? base + 1 : app_path;
    pstr_from_c(LM_CurApName, mf.name[0] ? mf.name : base);
    if (rd8(LM_CurApName) > 31)
        wr8(LM_CurApName, 31);

    int16_t rn = rm_open_host_file(app_path, mf.rsrc, (uint32_t)mf.rsrc_len, true);
    mac_file_free(&mf);
    if (rn < 0) {
        snprintf(err, errlen, "cannot open the application's resource fork");
        return false;
    }
    wr16(LM_CurApRefNum, (uint16_t)rn);

    uint32_t a5;
    uint32_t entry = seg_launch(rn, &a5, err, errlen);
    if (!entry)
        return false;
    cpu.pc = entry;
    return true;
}

run_status machine_run(uint64_t max)
{
    const uint64_t slice = 20000;
    uint64_t done = 0;
    while (!cpu.halted && done < max) {
        uint64_t n = max - done < slice ? max - done : slice;
        done += cpu_run(&cpu, n);
        /* 60.15 Hz tick counter from host time. */
        uint64_t ticks = (now_ns() - ticks_base_ns) * 60 / 1000000000u;
        if ((uint32_t)ticks > rd32(LM_Ticks))
            wr32(LM_Ticks, (uint32_t)ticks);
        if (cpu.stopped)
            break;
    }
    return cpu.halted ? RUN_HALTED : RUN_LIMIT;
}

const char *machine_halt_reason(void) { return cpu.halt_reason; }
uint64_t machine_instruction_count(void) { return cpu.icount; }
uint32_t machine_pc(void) { return cpu.pc; }

static void trace_cpu(cpu68k *c)
{
    char buf[64];
    cpu_disasm(c->pc, buf, sizeof buf);
    log_printf("%08X  %-16s d0=%08X d1=%08X a0=%08X a7=%08X sr=%04X\n", c->pc, buf, c->d[0], c->d[1], c->a[0],
               c->a[7], c->sr);
}

void machine_set_log(unsigned mask)
{
    g_log_mask = mask;
    cpu.trace = (mask & LOG_CPU) ? trace_cpu : NULL;
}

unsigned machine_census(census_row *rows, unsigned max)
{
    unsigned n;
    const trap_count *tc = trap_census(&n);
    for (unsigned i = 0; i < n && i < max; i++) {
        const char *name = NULL;
        if (tc[i].selector >= 0) {
            for (unsigned j = 0; j < dispatch_sig_count; j++) {
                uint16_t t = dispatch_sigs[j].trap;
                bool same = (t & 0x0800) ? (tc[i].trap & 0x3FF) == (t & 0x3FF) && (tc[i].trap & 0x0800)
                                         : (tc[i].trap & 0xFF) == (t & 0xFF) && !(tc[i].trap & 0x0800);
                if (same && dispatch_sigs[j].selector == tc[i].selector) {
                    name = dispatch_sigs[j].name;
                    break;
                }
            }
        }
        rows[i] = (census_row){ tc[i].trap, tc[i].selector, name ? name : trap_name(tc[i].trap), tc[i].calls,
                                tc[i].implemented };
    }
    return n;
}

void machine_describe_cpu(char *buf, size_t len)
{
    snprintf(buf, len,
             "pc %08X  sr %04X  instructions %llu\n"
             "d0 %08X d1 %08X d2 %08X d3 %08X d4 %08X d5 %08X d6 %08X d7 %08X\n"
             "a0 %08X a1 %08X a2 %08X a3 %08X a4 %08X a5 %08X a6 %08X a7 %08X",
             cpu.pc, cpu.sr, (unsigned long long)cpu.icount, cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3], cpu.d[4],
             cpu.d[5], cpu.d[6], cpu.d[7], cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5], cpu.a[6],
             cpu.a[7]);
}
