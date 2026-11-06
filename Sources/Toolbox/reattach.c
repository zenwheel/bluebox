/* Hibernating a program: its snapshot written at a safe point (drain.c),
 * and put back in a fresh process of the same build (after a reboot, say),
 * which then reattaches what the snapshot could only describe:
 *
 *   the clocks   the machine's clock goes on from where it stopped (Ticks,
 *                the Time Manager, sound, UpTime: no jump); the date
 *                catches up to the real one, as a Mac waking from sleep
 *                did (resume_date_catches_up, the one switch)
 *   the files    each open fork is checked against its file: one changed
 *                or gone while hibernated is marked bad, and the program
 *                gets an I/O error on its next use of it (filemgr.c)
 *   the host     what the snapshot held of the host's memory is cleared or
 *                made again (hibernate_reattach_host)
 *
 * What a snapshot cannot carry is refused before it is taken
 * (hibernate_io_blocker): connections, serial ports, print jobs.
 *
 * The fresh process is booted as the original was (the host replays its
 * launch record) but the program is not launched: the snapshot replaces
 * the machine machine_boot made, its guest memory laid out the same way
 * (24-bit mirrors included) before the regions are copied back. */
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hibernate.h"
#include "machine.h"
#include "mem.h"
#include "tb_internal.h"

/* Whether a resumed program's date catches up to the real one (the user's
 * decision, still open: a Mac waking from sleep did).  When it does not,
 * the guest's date goes on from where it stopped, as if no time passed. */
static const bool resume_date_catches_up = true;
BB_HOST static int date_override = -1; /* tests: 0 or 1 */

void machine_set_resume_date_catch_up(bool on) { date_override = on; }

/* Saved: what the machine was when its snapshot was taken. */
static struct {
    uint64_t time_ns;      /* machine_time_ns */
    int64_t wall;          /* the host's date (time()) */
    int layout;            /* g_mem_layout: the fresh process must have the same */
    bool resumed;          /* this machine was resumed (for the log) */
} saved;

const char *hibernate_io_blocker(void)
{
    BB_HOST static char reason[200];
    const char *what = network_io_open();
    if (!what)
        what = serial_io_open();
    if (!what)
        what = print_job_open();
    if (!what)
        what = host_media_open();
    if (!what)
        return NULL;
    snprintf(reason, sizeof reason, "the program %s", what);
    return reason;
}

bool machine_hibernate_save(const char *path, const char *record, uint64_t *bytes, char *err, size_t errlen)
{
    if (!machine_hibernatable()) {
        const char *why = machine_drain_reason();
        snprintf(err, errlen, "%s", *why ? why : "the program is not at a point where it can be saved");
        return false;
    }
    saved.time_ns = machine_time_ns();
    saved.wall = (int64_t)time(NULL);
    saved.layout = g_mem_layout;
    files_before_hibernate();
    /* BLUEBOX_HIBERNATE_SCAN=1: what the snapshot holds of the host's memory
     * (and any pointer the map misses), listed first. */
    const char *scan = getenv("BLUEBOX_HIBERNATE_SCAN");
    if (scan && *scan == '1')
        hb_scan(stderr, 200, 0);
    hb_stats st;
    bool ok = hb_save_file_extra(path, record, record ? strlen(record) : 0, &st, err, errlen);
    if (ok && bytes)
        *bytes = st.file_bytes;
    if (ok)
        LOGF(LOG_IDLE, "hibernate: %s: %llu bytes (%llu before compressing), %u blocks, %u pointers\n", path,
             (unsigned long long)st.file_bytes, (unsigned long long)st.bytes, st.blocks, st.relocs);
    return ok;
}

/* BLUEBOX_CRASH_TRACE=1: a crash prints the host's stack (for atos), as a
 * resumed process can't always be run under a debugger. */
static void crash_trace(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    dprintf(2, "bluebox: signal %d; the host's stack:\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void maybe_crash_trace(void)
{
    const char *e = getenv("BLUEBOX_CRASH_TRACE");
    if (e && *e == '1') {
        signal(SIGSEGV, crash_trace);
        signal(SIGBUS, crash_trace);
        signal(SIGABRT, crash_trace);
    }
}

bool machine_hibernate_restore(const char *path, char *err, size_t errlen)
{
    maybe_crash_trace();
    int layout = g_mem_layout;
    hb_stats st;
    if (!hb_restore_file(path, 0, &st, err, errlen))
        return false;
    if (saved.layout != layout) {
        /* The launch record replayed differently: nothing can go on. */
        snprintf(err, errlen, "the snapshot's memory layout (%d) is not this machine's (%d)", saved.layout, layout);
        return false;
    }
    saved.resumed = true;
    events_script_clear();
    bool catch_up = date_override >= 0 ? date_override : resume_date_catches_up;
    machine_resume_clock(saved.time_ns, catch_up, (int64_t)time(NULL) - saved.wall);
    hibernate_reattach_host();
    int bad = files_after_resume();
    LOGF(LOG_IDLE, "resume: %s: %u blocks, %u pointers (%u to the host's memory, cleared); %d file%s changed\n",
         path, st.blocks, st.relocs, st.host, bad, bad == 1 ? "" : "s");
    return true;
}

/* ---- What a snapshot cannot carry ---- */

const char *network_io_open(void)
{
    if (mactcp_any_open())
        return "has a MacTCP stream open";
    if (ot_endpoints_open())
        return "has an Open Transport endpoint open";
    if (appletalk_in_use())
        return "has AppleTalk sockets or names open";
    if (as_sessions_live())
        return "has an AppleShare volume connected";
    if (ppc_toolbox_open())
        return "has a PPC Toolbox port open";
    if (qt_streams_open())
        return "has a streamed movie open";
    return NULL;
}

const char *serial_io_open(void)
{
    if (serial_port_in_use(0) || serial_port_in_use(1))
        return "has a serial port open";
    if (ctb_connections_open())
        return "has a Communications Toolbox connection open";
    return NULL;
}

const char *print_job_open(void) { return print_job_in_progress() ? "is printing" : NULL; }

/* What lives in the host's process (its objects, its threads, its temporary
 * files) and is not reattached yet: refused too. */
const char *host_media_open(void)
{
    if (hostgl_any_live())
        return "has an OpenGL context open";
    if (rave_any_context())
        return "has a QuickDraw 3D RAVE context open";
    if (q3_any_view())
        return "has a QuickDraw 3D view open";
    if (qt_host_movies_open())
        return "has a QuickTime movie the host plays open";
    if (speech_any_busy())
        return "is speaking";
    if (sound_input_capturing())
        return "is recording sound";
    if (drivevol_any_mounted())
        return "has a disk image mounted";
    return NULL;
}

/* ---- The host's memory ---- */

void hibernate_reattach_host(void)
{
    aem_after_resume();
    aem_suspended_after_resume();
    display_after_resume();
}
