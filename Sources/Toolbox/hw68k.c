/*
 * The hardware a classic Mac's programs touch directly (M22).
 *
 * Programs written before the Toolbox covered everything went to the chips:
 * the VIA for timers, the vertical retrace and the sound and screen page
 * bits, the SCC for the serial ports, the IWM for the floppy.  Here those
 * are memory-mapped devices at the addresses of the machine presented (a
 * profile: the Quadra every run has been, or a Plus, SE, Classic or Mac II
 * by --machine), answered in the 68k interpreter's data accesses (mem.h's
 * g_hw_read and g_hw_write).  Everything else is plain guest memory.
 *
 *   VIA 1    6522 timers 1 and 2 on the 783,360 Hz clock, the interrupt
 *            flag and enable registers, the 60 Hz vertical retrace (CA1)
 *            and the one-second interrupt (CA2), port A's screen and sound
 *            page and volume bits, port B's sound-off, mouse button and
 *            ADB lines; its interrupts reach a program's own handler in the
 *            Lvl1DT table or at the level 1 vector.
 *   SCC      the 8530's two channels with its register-pointer protocol,
 *            polled; channel A is the modem port and B the printer port,
 *            each on Blue Box's serial port (serial.c).
 *   IWM      the floppy chip, with the machine's internal drive on it
 *            and no disk in the drive.
 *
 * The devices are interpreted: a JIT's loads and stores don't reach them,
 * so a machine chosen by name runs without the JIT.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hw68k.h"
#include "legacy.h"
#include "m68k.h"
#include "tb_internal.h"

#define LM_ASC_BASE 0x0CC0 /* ASCBase */

/* ------------------------------------------------------------------ */
/* Machine profiles */

static const hw_profile profiles[] = {
    { "quadra", "Macintosh Quadra 800", 35, 68040, true, true, 4, 4, 0x3FFF, 0xFC00, 0x077D, 0x0710,
      0, 0, 0, false, false, true, 0x50F00000u, 0x50F04000u, 0x50F04000u, 0x50F16000u, true },
    { "macii", "Macintosh II", 6, 68020, true, true, 5, 5, 0x3FFF, 0xF400, 0x0178, 0x0608,
      640, 480, 8, false, false, true, 0x50F00000u, 0x50F04000u, 0x50F04000u, 0x50F16000u, false },
    { "plus", "Macintosh Plus", 4, 68000, false, false, 3, 3, 0x7FFF, 0xC000, 0x0075, 0x0608,
      512, 342, 1, true, true, false, 0x00EFE1FEu, 0x009FFFF8u, 0x00BFFFF9u, 0x00DFE1FFu, false },
    { "se", "Macintosh SE", 5, 68000, false, false, 5, 5, 0x7FFF, 0xC400, 0x0276, 0x0608,
      512, 342, 1, true, true, true, 0x00EFE1FEu, 0x009FFFF8u, 0x00BFFFF9u, 0x00DFE1FFu, false },
    { "classic", "Macintosh Classic", 17, 68000, false, false, 5, 5, 0x7FFF, 0xE400, 0x0376, 0x0608,
      512, 342, 1, true, true, true, 0x00EFE1FEu, 0x009FFFF8u, 0x00BFFFF9u, 0x00DFE1FFu, true },
};
enum { NPROFILES = sizeof profiles / sizeof profiles[0] };

static const hw_profile *machine = &profiles[0];
static bool chosen;

const hw_profile *hw_find_profile(const char *name)
{
    for (int i = 0; i < NPROFILES; i++)
        if (!strcasecmp(profiles[i].name, name))
            return &profiles[i];
    return NULL;
}
const hw_profile *hw_profile_at(int i) { return i >= 0 && i < NPROFILES ? &profiles[i] : NULL; }
const hw_profile *hw_machine(void) { return machine; }
bool hw_machine_chosen(void) { return chosen; }

bool hw_select_machine(const char *name)
{
    const hw_profile *p = name ? hw_find_profile(name) : &profiles[0];
    if (!p)
        return false;
    machine = p;
    chosen = p != &profiles[0];
    if (p->screen_w) {
        machine_set_screen(p->screen_w, p->screen_h);
        machine_set_screen_depth(p->screen_depth);
    }
    /* Its processor, FPU and addressing (the System 6 machines ran in
     * 24-bit mode); --cpu, --fpu and --addressing may change them after. */
    machine_set_cpu(p->cpu);
    machine_set_fpu(!p->fpu ? 0 : p->cpu == 68040 ? 68040 : 68881);
    machine_set_addressing(chosen ? 24 : 0);
    return true;
}

/* ------------------------------------------------------------------ */
/* Time: the VIA's clock is the 68000's divided by ten */

#define VIA_HZ 783360u
static uint64_t via_clock(void) { return machine_time_ns() * 306u / 390625u; }

/* ------------------------------------------------------------------ */
/* VIA 1 */

enum { vBufB, vBufA_hs, vDirB, vDirA, vT1C, vT1CH, vT1L, vT1LH, vT2C, vT2CH, vSR, vACR, vPCR, vIFR, vIER, vBufA };
enum { ifCA2 = 1, ifCA1 = 2, ifSR = 4, ifCB2 = 8, ifCB1 = 16, ifT2 = 32, ifT1 = 64 };

static struct {
    uint8_t orb, ora, ddrb, ddra, acr, pcr, ier, ifr, sr;
    uint16_t t1l, t2l;
    uint64_t t1_start, t2_start;
    bool t1_on, t2_on;
    uint64_t last_ns;       /* machine time of the last update */
    uint64_t vbl_count, sec_count;
    /* polling: the same registers read again and again */
    uint64_t poll_icount;
    int polls;
} via;

static const double VBL_HZ = 60.15;

static uint16_t t1_counter(uint64_t now)
{
    uint64_t e = now - via.t1_start;
    if (via.acr & 0x40)
        e %= (uint64_t)via.t1l + 2;
    return (uint16_t)(via.t1l - e);
}

static void via_update(void)
{
    uint64_t ns = machine_time_ns(), now = via_clock();
    /* timer 1 */
    if (via.t1_on) {
        uint64_t e = now - via.t1_start, period = (uint64_t)via.t1l + 2;
        if (e >= period) {
            via.ifr |= ifT1;
            if (via.acr & 0x40)
                via.t1_start += e / period * period; /* free-running: reloaded */
            else
                via.t1_on = false; /* one-shot: once until reloaded */
        }
    }
    if (via.t2_on && !(via.acr & 0x20)) {
        uint64_t e = now - via.t2_start;
        if (e >= (uint64_t)via.t2l + 2) {
            via.ifr |= ifT2;
            via.t2_on = false;
        }
    }
    uint64_t vbl = (uint64_t)((double)ns * VBL_HZ / 1e9), sec = ns / 1000000000u;
    if (vbl > via.vbl_count) {
        via.vbl_count = vbl;
        via.ifr |= ifCA1;
    }
    if (sec > via.sec_count) {
        via.sec_count = sec;
        via.ifr |= ifCA2;
    }
    via.last_ns = ns;
}

/* A program polling the VIA in a tight loop is waiting for time to pass:
 * headless, a few polls in a row let it, as machine_tick_polled does for
 * TickCount, rather than burning wall-clock time at emulation speed. */
static void via_polled(void)
{
    if (events_host_attached() || !g_cpu)
        return;
    uint64_t ic = g_cpu->icount;
    via.polls = ic - via.poll_icount < 100 ? via.polls + 1 : 0;
    via.poll_icount = ic;
    if (via.polls < 8)
        return;
    via.polls = 0;
    uint64_t ns = machine_time_ns();
    /* to the next event: the retrace, the second, or a timer */
    uint64_t next = (uint64_t)((double)(via.vbl_count + 1) * 1e9 / VBL_HZ) + 1;
    uint64_t sec = (via.sec_count + 1) * 1000000000u;
    if (sec < next)
        next = sec;
    uint64_t now = via_clock();
    if (via.t1_on) {
        uint64_t due = via.t1_start + (uint64_t)via.t1l + 2;
        if (due > now) {
            uint64_t t = ns + (due - now) * 390625u / 306u + 1;
            if (t < next)
                next = t;
        }
    }
    if (via.t2_on) {
        uint64_t due = via.t2_start + (uint64_t)via.t2l + 2;
        if (due > now) {
            uint64_t t = ns + (due - now) * 390625u / 306u + 1;
            if (t < next)
                next = t;
        }
    }
    if (next > ns)
        machine_skip_time(next - ns);
    machine_advance_ticks();
}

/* The mouse button, for port B bit 3 (clear while pressed). */
static bool mouse_up(void) { return (rd8(LM_MBState) & 0x80) != 0; }

static uint8_t via_in_b(void)
{
    if (machine->adb)
        return 0x08; /* ADB INT idle; ST0 and ST1 are outputs */
    uint8_t v = mouse_up() ? 0x08 : 0;
    v |= 0x30; /* mouse quadrature lines idle */
    v |= (machine_time_ns() / 32600u) & 1 ? 0x40 : 0; /* H4: horizontal blanking */
    return v;
}

static uint8_t via_read(int reg)
{
    via_update();
    switch (reg) {
    case vBufB: {
        uint8_t in = via_in_b();
        return (uint8_t)((via.orb & via.ddrb) | (in & ~via.ddrb));
    }
    case vBufA:
    case vBufA_hs: {
        uint8_t in = 0x80 | 0x40 | 0x08; /* SCC wait/request idle; the pages as main */
        return (uint8_t)((via.ora & via.ddra) | (in & ~via.ddra));
    }
    case vDirB: return via.ddrb;
    case vDirA: return via.ddra;
    case vT1C:
        via.ifr &= (uint8_t)~ifT1;
        via_polled();
        return (uint8_t)t1_counter(via_clock());
    case vT1CH: return (uint8_t)(t1_counter(via_clock()) >> 8);
    case vT1L: return (uint8_t)via.t1l;
    case vT1LH: return (uint8_t)(via.t1l >> 8);
    case vT2C: {
        via.ifr &= (uint8_t)~ifT2;
        via_polled();
        uint64_t e = via_clock() - via.t2_start;
        return (uint8_t)(via.t2l - e);
    }
    case vT2CH: return (uint8_t)((uint16_t)(via.t2l - (via_clock() - via.t2_start)) >> 8);
    case vSR: via.ifr &= (uint8_t)~ifSR; return via.sr;
    case vACR: return via.acr;
    case vPCR: return via.pcr;
    case vIFR:
        via_polled();
        return (uint8_t)((via.ifr & 0x7F) | ((via.ifr & via.ier & 0x7F) ? 0x80 : 0));
    case vIER: return (uint8_t)(via.ier | 0x80);
    }
    return 0;
}

static void screen_page_changed(void);
static void sound_regs_changed(void);

static void via_write(int reg, uint8_t v)
{
    via_update();
    switch (reg) {
    case vBufB:
        via.orb = v;
        sound_regs_changed();
        break;
    case vBufA:
    case vBufA_hs: {
        uint8_t old = via.ora;
        via.ora = v;
        if ((old ^ v) & via.ddra & 0x40)
            screen_page_changed();
        sound_regs_changed();
        break;
    }
    case vDirB: via.ddrb = v; break;
    case vDirA: via.ddra = v; break;
    case vT1C:
    case vT1L: via.t1l = (uint16_t)((via.t1l & 0xFF00) | v); break;
    case vT1CH:
        via.t1l = (uint16_t)((via.t1l & 0x00FF) | (v << 8));
        via.ifr &= (uint8_t)~ifT1;
        via.t1_start = via_clock();
        via.t1_on = true;
        break;
    case vT1LH:
        via.t1l = (uint16_t)((via.t1l & 0x00FF) | (v << 8));
        via.ifr &= (uint8_t)~ifT1;
        break;
    case vT2C: via.t2l = (uint16_t)((via.t2l & 0xFF00) | v); break;
    case vT2CH:
        via.t2l = (uint16_t)((via.t2l & 0x00FF) | (v << 8));
        via.ifr &= (uint8_t)~ifT2;
        via.t2_start = via_clock();
        via.t2_on = true;
        break;
    case vSR: via.sr = v; via.ifr &= (uint8_t)~ifSR; break;
    case vACR: via.acr = v; break;
    case vPCR: via.pcr = v; break;
    case vIFR: via.ifr &= (uint8_t)~(v & 0x7F); break;
    case vIER:
        if (v & 0x80)
            via.ier |= v & 0x7F;
        else
            via.ier &= (uint8_t)~(v & 0x7F);
        break;
    }
}

uint8_t hw_via_read(int reg) { return via_read(reg); }
void hw_via_write(int reg, uint8_t v) { via_write(reg, v); }

/* Port A as the compact Macs wire it: bit 6 the screen page (set: main),
 * bit 3 the sound page (set: main), bits 0 to 2 the volume.  Port B bit 7
 * turns the sound off. */
bool hw_screen_alternate(void) { return machine->via_sound && (via.ddra & 0x40) && !(via.ora & 0x40); }
bool hw_sound_alternate(void) { return machine->via_sound && (via.ddra & 0x08) && !(via.ora & 0x08); }
int hw_sound_volume(void) { return (via.ddra & 7) ? via.ora & 7 : rd8(LM_SdVolume) & 7; }
bool hw_sound_enabled(void) { return !((via.ddrb & 0x80) && (via.orb & 0x80)); }

/* ------------------------------------------------------------------ */
/* The SCC */

typedef struct {
    uint8_t wr[16];
    uint8_t ptr;
    bool opened;
    bool tx_pending_int;
    uint32_t baud;
} scc_chan;
static scc_chan scc[2]; /* 0: channel B (printer, serial port 1), 1: channel A (modem, port 0) */

static int chan_port(int ch) { return ch == 1 ? 0 : 1; }

static void scc_open(int ch)
{
    if (!scc[ch].opened) {
        scc[ch].opened = true;
        serial_port_open(chan_port(ch));
    }
}

static void scc_apply(int ch)
{
    scc_chan *c = &scc[ch];
    uint32_t tc = (uint32_t)c->wr[12] | ((uint32_t)c->wr[13] << 8);
    static const int mult[4] = { 1, 16, 32, 64 };
    int clk = mult[(c->wr[4] >> 6) & 3];
    uint32_t baud = 3686400u / (2u * (tc + 2u) * (uint32_t)clk);
    static const int bits[4] = { 5, 7, 6, 8 };
    int data = bits[(c->wr[5] >> 5) & 3];
    int parity = (c->wr[4] & 1) ? ((c->wr[4] & 2) ? 2 : 1) : 0;
    int stop = ((c->wr[4] >> 2) & 3) == 3 ? 2 : ((c->wr[4] >> 2) & 3) == 2 ? 15 : 1;
    if (baud < 50)
        baud = 9600;
    c->baud = baud;
    scc_open(ch);
    serial_port_setup(chan_port(ch), baud, data, parity, stop, 0);
}

static void scc_reset_channel(scc_chan *c)
{
    /* hardware reset values: WR3, WR5, WR9-WR15 cleared, WR4 x1 clock */
    memset(c->wr, 0, sizeof c->wr);
    c->ptr = 0;
    c->wr[4] = 0x04;
}

static uint8_t scc_read_reg(int ch, int r)
{
    scc_chan *c = &scc[ch];
    int port = chan_port(ch);
    switch (r) {
    case 0: {
        uint8_t v = 0x04 | 0x40; /* Tx buffer empty, Tx underrun/EOM */
        if (c->opened && serial_port_available(port) > 0)
            v |= 0x01;
        /* DCD and CTS follow the line when something is on it */
        if (serial_port_online(port))
            v |= 0x08 | 0x20;
        else
            v |= 0x20; /* CTS asserted: nothing holds the Mac back */
        return v;
    }
    case 1: return 0x01 | 0x06; /* all sent, residue code */
    case 2: return c->wr[2];
    case 3: return ch == 1 ? 0 : 0; /* interrupts pending: none */
    case 8: {
        uint8_t b = 0;
        scc_open(ch);
        if (serial_port_available(port) > 0)
            serial_port_read(port, &b, 1);
        return b;
    }
    case 10: return 0;
    case 12: return c->wr[12];
    case 13: return c->wr[13];
    case 15: return c->wr[15];
    case 9:
    case 11:
    case 14: return c->wr[r]; /* RR9, RR11 and RR14 read back WR13, WR15, WR10 on the 85C30; plain here */
    }
    return 0;
}

static void scc_write_reg(int ch, int r, uint8_t v)
{
    scc_chan *c = &scc[ch];
    int port = chan_port(ch);
    uint8_t old = c->wr[r & 15];
    switch (r) {
    case 0: break;
    case 8: /* transmit data */
        scc_open(ch);
        if (c->wr[5] & 0x08) { /* Tx enabled */
            serial_port_write(port, &v, 1);
        }
        return;
    case 9:
        if (v & 0xC0) { /* reset commands: channel B, channel A, or both */
            int cmd = v >> 6;
            if (cmd == 1 || cmd == 3)
                scc_reset_channel(&scc[0]);
            if (cmd == 2 || cmd == 3)
                scc_reset_channel(&scc[1]);
        }
        scc[0].wr[9] = scc[1].wr[9] = (uint8_t)(v & 0x3F);
        return;
    default: break;
    }
    c->wr[r & 15] = v;
    if (r == 4 || r == 12 || r == 13 || r == 14 || r == 3 || r == 5)
        if (v != old || !c->opened)
            scc_apply(ch);
    if (r == 5) {
        if ((old ^ v) & 0x80)
            serial_port_dtr(port, (v & 0x80) != 0);
        if ((old ^ v) & 0x10)
            serial_port_break(port, (v & 0x10) != 0);
    }
}

/* The SCC's addresses: bit 1 picks the channel (clear: B, set: A), bit 2
 * data rather than control. */
static uint32_t scc_access(gaddr a, bool write, uint8_t v)
{
    int ch = (a & 2) ? 1 : 0;
    bool data = (a & 4) != 0;
    scc_chan *c = &scc[ch];
    if (data) {
        if (write) {
            scc_write_reg(ch, 8, v);
            return 0;
        }
        return scc_read_reg(ch, 8);
    }
    int r = c->ptr;
    if (write) {
        if (r == 0) {
            int cmd = (v >> 3) & 7;
            c->ptr = (uint8_t)((v & 7) | (cmd == 1 ? 8 : 0));
            c->wr[0] = v & 0xC7;
        } else {
            c->ptr = 0;
            scc_write_reg(ch, r, v);
        }
        return 0;
    }
    c->ptr = 0;
    return scc_read_reg(ch, r);
}

/* ------------------------------------------------------------------ */
/* The IWM and the machine's internal floppy drive, with no disk in it */

/* state: bit n of the 8 switch pairs (CA0-CA2, LSTRB, ENABLE, SELECT, Q6, Q7) */
static struct { uint8_t state; uint8_t mode; } iwm;
static struct { int track; bool dir_out, motor; } drive; /* the internal drive */

/* The drive's status line for the register CA2-CA0 and SEL (VIA port A
 * bit 5) choose, as a Sony drive answers: there, double-sided (and on a
 * SuperDrive, saying so and in GCR mode), with no disk, so not ready.
 * Nothing else is plugged in: drive 2 and a drive not enabled leave the
 * line high. */
static bool drive_sense(void)
{
    if (!(iwm.state & 0x10) || (iwm.state & 0x20))
        return true;
    int reg = (iwm.state & 7) | ((via.ora & 0x20) ? 8 : 0);
    switch (reg) {
    case 0x0: return drive.dir_out;  /* DIRTN: stepping outward */
    case 0x1: return true;           /* /STEP: no step under way */
    case 0x2: return !drive.motor;   /* /MOTORON */
    case 0x3: return false;          /* EJECT: not ejecting */
    case 0x4: return true;           /* RDDATA0: nothing under the head */
    case 0x5: return machine->superdrive; /* a 1.44 MB SuperDrive */
    case 0x6: return machine->superdrive; /* it reads MFM */
    case 0x7: return true;           /* SIDES: double-sided */
    case 0x8: return false;          /* /DRVIN: the drive is there */
    case 0x9: return true;           /* /CSTIN: no disk in place */
    case 0xA: return true;           /* /WRTPRT: (no disk to protect) */
    case 0xB: return drive.track != 0; /* /TK0 */
    case 0xC: return true;           /* /TACH: no pulses, the motor still */
    case 0xD: return true;           /* RDDATA1 */
    case 0xE: return false;          /* MFM mode: off, in GCR */
    default: return true;            /* /READY: not ready, no disk */
    }
}

/* LSTRB raised: the drive acts on the command CA0-CA2 and SEL select,
 * with CA2 the value. */
static void drive_command(void)
{
    if (!(iwm.state & 0x10) || (iwm.state & 0x20))
        return;
    bool ca2 = iwm.state & 4;
    switch ((iwm.state & 3) | ((via.ora & 0x20) ? 4 : 0)) {
    case 0: drive.dir_out = ca2; break;     /* direction */
    case 1:                                 /* step */
        if (!ca2)
            drive.track += drive.dir_out ? -1 : 1;
        if (drive.track < 0)
            drive.track = 0;
        if (drive.track > 79)
            drive.track = 79;
        break;
    case 2: drive.motor = !ca2; break;      /* motor on, off */
    default: break;                         /* eject: there is no disk */
    }
}

static uint32_t iwm_access(gaddr a, bool write, uint8_t v)
{
    int sw = (int)((a >> 9) & 15);
    bool strobe = sw == 7 && !(iwm.state & 0x08);
    if (sw & 1)
        iwm.state |= (uint8_t)(1 << (sw >> 1));
    else
        iwm.state &= (uint8_t)~(1 << (sw >> 1));
    if (strobe)
        drive_command();
    bool q6 = iwm.state & 0x40 ? 1 : 0, q7 = iwm.state & 0x80 ? 1 : 0;
    if (write) {
        if (q6 && q7)
            iwm.mode = v & 0x1F;
        return 0;
    }
    if (!q7 && q6) /* status register: the sense line, mode, drive enabled */
        return (uint32_t)((drive_sense() ? 0x80 : 0) | (iwm.mode & 0x1F) | ((iwm.state & 0x10) ? 0x20 : 0));
    if (q7 && !q6) /* write handshake: ready, underrun clear */
        return 0xC0;
    return 0xFF; /* read data: nothing under the head */
}

/* ------------------------------------------------------------------ */
/* Other chips on the Mac II's bus: registers that hold what is written */

static uint8_t spare_regs[2][8192];

/* ------------------------------------------------------------------ */
/* The windows */

typedef struct {
    gaddr lo, len;
    int dev; /* 0 VIA, 1 SCC read, 2 SCC write (compact) or SCC (II), 3 IWM, 4 VIA 2, 5 ASC */
} window;
static window windows[12];
static int nwindows;

static void map_window(gaddr lo, gaddr len, int dev)
{
    if (nwindows == 12)
        return;
    windows[nwindows++] = (window){ lo, len, dev };
    for (gaddr m = lo >> 20; m <= (lo + len - 1) >> 20; m++)
        g_hw_map[m] = 1;
}

static uint32_t dev_access(const window *w, gaddr a, bool write, uint8_t v)
{
    switch (w->dev) {
    case 0:
        if (write) {
            via_write((int)((a >> 9) & 15), v);
            return 0;
        }
        return via_read((int)((a >> 9) & 15));
    case 1:
    case 2: return scc_access(a, write, v);
    case 3: return iwm_access(a, write, v);
    case 4:
    case 5: {
        uint8_t *r = spare_regs[w->dev - 4];
        gaddr off = (a - w->lo) & 8191;
        if (write) {
            r[off] = v;
            return 0;
        }
        return r[off];
    }
    }
    return 0;
}

static const window *window_at(gaddr a)
{
    for (int i = 0; i < nwindows; i++)
        if (a - windows[i].lo < windows[i].len)
            return &windows[i];
    return NULL;
}

static uint32_t hw_read_cb(gaddr a, int size)
{
    const window *w = window_at(a);
    if (!w)
        return size == 1 ? rd8(a) : size == 2 ? rd16(a) : rd32(a);
    /* A word or long access reaches the chip once, on the byte lane of
     * its even address; the other lanes float, here repeating it. */
    uint32_t b = dev_access(w, a, false, 0) & 0xFF;
    return size == 1 ? b : size == 2 ? (b << 8 | b) : (b << 24 | b << 16 | b << 8 | b);
}

static void hw_write_cb(gaddr a, int size, uint32_t v)
{
    const window *w = window_at(a);
    if (!w) {
        if (size == 1) wr8(a, (uint8_t)v);
        else if (size == 2) wr16(a, (uint16_t)v);
        else wr32(a, v);
        return;
    }
    uint8_t b = size == 1 ? (uint8_t)v : size == 2 ? (uint8_t)(v >> 8) : (uint8_t)(v >> 24);
    dev_access(w, a, true, b);
}

/* ------------------------------------------------------------------ */
/* The screen pages and the sound buffers */

gaddr hw_screen_page(bool alternate) { return alternate ? GA_SCREEN - 0x8000u : GA_SCREEN; }
/* Below the top of memory: the sound buffer is $300 under it, the
 * alternate $5C00 further down, as in a 4 MB compact Mac (mem.h's
 * GAC_SOUND). */
static gaddr sound_main, sound_alt;
gaddr hw_sound_buffer(bool alternate) { return alternate ? sound_alt : sound_main; }

void qd_screen_set_display_base(gaddr base);
void sound_hw_play(const uint8_t *samples, int n, int volume);

static void screen_page_changed(void)
{
    if (!machine->via_sound)
        return;
    qd_screen_set_display_base(hw_screen_page(hw_screen_alternate()));
}

static void sound_regs_changed(void) {}

static uint64_t sound_ticks; /* retraces whose buffer has been played */
static uint8_t sound_last[370];
static bool sound_last_valid;

static void hw_sound_pump(void)
{
    if (!machine->via_sound || !sound_main)
        return;
    uint64_t v = via.vbl_count;
    if (v == sound_ticks)
        return;
    if (v - sound_ticks > 4)
        sound_ticks = v - 4;
    while (sound_ticks < v) {
        sound_ticks++;
        if (!hw_sound_enabled() || hw_sound_volume() == 0)
            continue;
        gaddr buf = hw_sound_buffer(hw_sound_alternate());
        uint8_t s[370];
        for (int i = 0; i < 370; i++)
            s[i] = rd8(buf + (gaddr)i * 2);
        bool flat = true;
        for (int i = 1; i < 370 && flat; i++)
            flat = s[i] == s[0];
        /* a buffer of one value is silence (a constant level), played
         * once to bring the speaker back to rest and then not at all */
        if (flat && (!sound_last_valid || memcmp(sound_last, s, sizeof s) == 0)) {
            sound_last_valid = true;
            memcpy(sound_last, s, sizeof s);
            continue;
        }
        sound_last_valid = true;
        memcpy(sound_last, s, sizeof s);
        sound_hw_play(s, 370, hw_sound_volume());
    }
}

/* ------------------------------------------------------------------ */
/* Interrupts: the VIA's to a program's own handlers */

static gaddr tramp;

static gaddr default_level1; /* the vector as the boot left it: nobody's handler */
static bool program_code(gaddr a) { return a >= 0x1000 && a < GA_ROM && a != default_level1; }

/* Call a handler that ends in RTE (a level 1 interrupt handler) from
 * interrupt-time code: the frame of the exception, the handler's RTE
 * returning to the stub that ends the call. */
static void interrupt_to(gaddr handler)
{
    cpu68k *c = g_cpu;
    if (!tramp) {
        tramp = mm_new_ptr_in(GA_SYSHEAP, 24, false);
        if (!tramp)
            return;
    }
    bool frame_word = c->model != 68000;
    gaddr p = tramp;
    wr16(p, 0x2E1F); p += 2;                    /* MOVE.L (SP)+,D7 */
    if (frame_word) {
        wr16(p, 0x3F3C); wr16(p + 2, 0x0064); p += 4; /* MOVE.W #$64,-(SP): format 0, vector 25 */
    }
    wr16(p, 0x2F07); p += 2;                    /* MOVE.L D7,-(SP) */
    wr16(p, 0x40E7); p += 2;                    /* MOVE.W SR,-(SP) */
    wr16(p, 0x4EF9); wr32(p + 2, handler);      /* JMP handler */
    guest_call(tramp);
}

void time_interrupt_call(gaddr fn, gaddr a0, gaddr a1);

static bool in_hw_interrupt;

static void via_interrupts(void)
{
    uint8_t pend = (uint8_t)(via.ifr & via.ier & 0x7F);
    if (!pend || in_hw_interrupt)
        return;
    in_hw_interrupt = true;
    gaddr v1 = rd32(0x64);
    if (program_code(v1)) {
        /* a program that took the level 1 vector over handles all of it */
        interrupt_to(v1);
    } else {
        for (int bit = 0; bit < 7; bit++) {
            if (!(pend & (1 << bit)))
                continue;
            gaddr h = rd32(LM_Lvl1DT + (gaddr)bit * 4);
            if (program_code(h))
                time_interrupt_call(h, 0, 0);
        }
    }
    in_hw_interrupt = false;
}

void hw_pump(void)
{
    if (!nwindows)
        return;
    via_update();
    hw_sound_pump();
    via_interrupts();
}

/* ------------------------------------------------------------------ */
/* Boot */

void hw_boot(void)
{
    memset(g_hw_map, 0, sizeof g_hw_map);
    nwindows = 0;
    memset(&via, 0, sizeof via);
    memset(scc, 0, sizeof scc);
    memset(&iwm, 0, sizeof iwm);
    memset(&drive, 0, sizeof drive);
    memset(spare_regs, 0, sizeof spare_regs);
    scc_reset_channel(&scc[0]);
    scc_reset_channel(&scc[1]);
    via.ora = 0x7F;       /* main pages, full volume */
    via.orb = 0x80;       /* sound off */
    via.t1l = via.t2l = 0xFFFF;
    via.vbl_count = via.sec_count = 0;
    sound_ticks = 0;
    sound_last_valid = false;
    tramp = 0;
    in_hw_interrupt = false;
    default_level1 = rd32(0x64);
    g_hw_read = hw_read_cb;
    g_hw_write = hw_write_cb;
    g_hw_jit_off = chosen;

    const hw_profile *m = machine;
    if (m->compact_io) {
        map_window(0x00800000u, 0x00200000u, 1);                 /* SCC read: $800000-$9FFFFF */
        map_window(0x00A00000u, 0x00200000u, 2);                 /* SCC write: $A00000-$BFFFFF */
        map_window(0x00C00000u, 0x00200000u, 3);                 /* IWM: $C00000-$DFFFFF */
        map_window(0x00E80000u, 0x00080000u, 0);                 /* VIA: $E80000-$EFFFFF */
    } else {
        /* At $50Fxxxxx, and in 24-bit mode at $F0xxxx too, which the Mac
         * II's 24-bit map mirrored them at (a program switched to 32-bit
         * mode by SwapMMUMode reaches the first). */
        for (int pass = 0; pass < (g_mem_24bit ? 2 : 1); pass++) {
            gaddr io = (m->via & ~0x1FFFu) & (pass ? 0x00FFFFFFu : 0xFFFFFFFFu);
            map_window(io, 0x2000, 0);                           /* VIA 1 */
            map_window(io + 0x2000, 0x2000, 4);                  /* VIA 2 */
            map_window(io + ((m->scc_rd & ~0x1FFFu) - (m->via & ~0x1FFFu)), 0x2000, 2); /* SCC */
            map_window(io + ((m->iwm & ~0x1FFFu) - (m->via & ~0x1FFFu)), 0x2000, 3);    /* IWM */
            map_window(io + 0x14000, 0x2000, 5);                 /* the Apple Sound Chip */
        }
    }
    wr32(LM_VIA, m->via);
    wr32(LM_SCCRd, m->scc_rd);
    wr32(LM_SCCWr, m->scc_wr);
    wr32(LM_IWM, m->iwm);
    /* ASCBase ($CC0): the Apple Sound Chip, as the ROM set it.  Programs
     * read its registers there: HyperCard 1.0.1 waits after a beep while
     * the chip's mode byte (+$801) says it is playing, and with ASCBase
     * left 0 read low memory there instead and waited for ever. */
    if (!m->compact_io)
        wr32(LM_ASC_BASE, (m->via & ~0x1FFFu) + 0x14000);
    wr16(LM_ROM85, m->rom85);
    /* HWCfgFlags and CPUFlag are machine.c's, from the processor run. */
    wr16(GA_ROM + 8, m->rom_version);
    if (chosen)
        wr16(LM_SysVersion, m->sys_version);
    wr8(LM_MBState, 0x80);
    if (m->via_sound) {
        /* The sound buffers at the top of RAM, the alternate $5C00 below
         * the main, as on a Plus. */
        sound_main = GAC_SOUND;
        sound_alt = GAC_SOUND - 0x5C00u;
        wr32(LM_SoundBase, sound_main);
        wr32(LM_ScrnBase, GA_SCREEN);
    }
}
