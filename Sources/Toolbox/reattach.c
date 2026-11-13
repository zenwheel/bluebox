/* Hibernating a program: its snapshot written at a safe point (drain.c),
 * and put back in a fresh process (after a reboot, say) of a build whose
 * saved state has the same shape (hibernate.c), which then reattaches
 * what the snapshot could only describe:
 *
 *   the clocks   the machine's clock goes on from where it stopped (Ticks,
 *                the Time Manager, sound, UpTime: no jump); the date
 *                catches up to the real one, as a Mac waking from sleep
 *                did (resume_date_catches_up, the one switch)
 *   the files    each open fork is checked against its file: one changed
 *                or gone while hibernated is marked bad, and the program
 *                gets an I/O error on its next use of it (filemgr.c)
 *   the host     what the snapshot held of the host's memory is cleared or
 *                made again (hibernate_reattach_host), the program's
 *                threads' host threads among it (threadmgr.c)
 *   the sound    the sound channels' queues, callbacks and double buffers
 *                are the machine's and go on by its clock; the host is
 *                given again what its old self had queued and not played
 *                (sound.c), and its synthesizers are told the programs,
 *                controllers and held notes they had; an utterance being
 *                spoken is over, its completion callbacks made (speech.c);
 *                a recording stops, as SPBStopRecording stops one
 *                (sound_input.c)
 *   the 3D       QuickDraw 3D's and RAVE's frames and textures are the
 *                machine's (r3d.c), what the host had drawn and held read
 *                back into it for the snapshot; the host's are made again
 *                as each is first used; OpenGL's contexts the same way,
 *                their state, textures, framebuffers and display lists
 *                (opengl.c, opengl_hib.c)
 *   the movies   a movie VLC played is opened again where the machine's
 *                clock has it when it is next given time; the flattened
 *                copy VLC played from is made again (quicktime.c)
 *
 *   the network  what listens or is registered is made again on the new
 *                host; a connection is over, and the program hears so
 *                as each protocol tells it (network_after_resume)
 *   a driver     a program's synchronous call given up for the drain
 *                inside its driver's routine fails with abortErr, as a
 *                killed request does (drv68k_after_resume)
 *
 * What a snapshot cannot carry is refused before it is taken
 * (hibernate_io_blocker): a print job, a streamed movie, EtherTalk
 * bridged onto an interface, the host's media.
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
#include "hostgl.h"
#include "machine.h"
#include "mem.h"
#include "r3d.h"
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
    uint64_t host_ns;      /* host_clock_ns */
} saved;

/* The host's clock for the network's timers (AppleTalk's retries and
 * tickles, sessions' idle timeouts, the serial ports' timeouts): the
 * host's monotonic clock, which starts again at each boot, moved on so
 * that in a resumed process it goes on from where it stopped, as the
 * machine's clock does, and a retry due a second after the snapshot comes
 * a second after resuming. */
BB_HOST static int64_t host_clock_offset; /* (made again at each restore from saved.host_ns) */

static uint64_t host_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t host_clock_ns(void) { return (uint64_t)((int64_t)host_mono_ns() + host_clock_offset); }
uint64_t host_clock_ms(void) { return host_clock_ns() / 1000000u; }

/* This process put a snapshot back: the host's to know, not the program's
 * (a snapshot of a resumed machine is the same as of one never stopped). */
BB_HOST static bool resumed;

bool hibernate_resumed(void) { return resumed; }

const char *hibernate_io_blocker(void)
{
    BB_HOST static char reason[200];
    const char *what = network_io_open();
    if (!what)
        what = print_job_open();
    if (!what)
        what = host_media_open();
    if (!what)
        return NULL;
    snprintf(reason, sizeof reason, "the program %s", what);
    return reason;
}

static bool at_safe_point(char *err, size_t errlen)
{
    if (machine_hibernatable())
        return true;
    const char *why = machine_drain_reason();
    snprintf(err, errlen, "%s", *why ? why : "the program is not at a point where it can be saved");
    return false;
}

/* Before the snapshot: the clocks, the open files stamped as they are on
 * disk, what the host draws and plays read back into the machine.  After
 * it, the machine may go on (a save point's does). */
static void capture_begin(void)
{
    saved.time_ns = machine_time_ns();
    saved.wall = (int64_t)time(NULL);
    saved.layout = g_mem_layout;
    saved.host_ns = host_clock_ns();
    files_before_hibernate();
    r3d_before_hibernate(); /* what the host drew and holds, into the machine */
    hostgl_before_hibernate();
    qt_movies_before_hibernate(); /* where VLC has the movies it plays */
    ot_before_hibernate();
    /* BLUEBOX_HIBERNATE_SCAN=1: what the snapshot holds of the host's memory
     * (and any pointer the map misses), listed first. */
    const char *scan = getenv("BLUEBOX_HIBERNATE_SCAN");
    if (scan && *scan == '1')
        hb_scan(stderr, 200, 0);
}

static void capture_end(void)
{
    r3d_after_hibernate();
    hostgl_after_hibernate();
    qt_movies_after_hibernate();
}

bool machine_hibernate_save(const char *path, const char *record, uint64_t *bytes, char *err, size_t errlen)
{
    if (!at_safe_point(err, errlen))
        return false;
    capture_begin();
    hb_stats st;
    bool ok = hb_save_file_extra(path, record, record ? strlen(record) : 0, &st, err, errlen);
    capture_end();
    if (ok && bytes)
        *bytes = st.file_bytes;
    if (ok) {
        sound_record_note("hibernated");
        speech_record_note("hibernated");
    }
    if (ok)
        LOGF(LOG_IDLE,
             "hibernate: %s: %llu bytes (%llu before compressing), %u blocks, %u pointers; into the binary %u: "
             "%u to functions, %u into them, %u to constants, %u into them, %u to strings, %u unnamed\n",
             path, (unsigned long long)st.file_bytes, (unsigned long long)st.bytes, st.blocks, st.relocs, st.to_image,
             st.to_functions, st.to_code_inside, st.to_data, st.to_data_inside, st.to_strings, st.to_image_unnamed);
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
    resumed = true;
    host_clock_offset = (int64_t)saved.host_ns - (int64_t)host_mono_ns();
    events_script_clear();
    bool catch_up = date_override >= 0 ? date_override : resume_date_catches_up;
    machine_resume_clock(saved.time_ns, catch_up, (int64_t)time(NULL) - saved.wall);
    hibernate_reattach_host();
    int bad = files_after_resume();
    LOGF(LOG_IDLE,
         "resume: %s: %u blocks, %u pointers (%u to the host's memory, cleared; %u into the binary by name, %u "
         "string constants this build has not kept in blocks); statics put back by name: %u dropped, %u new; "
         "%d file%s changed\n",
         path, st.blocks, st.relocs, st.host, st.to_image - st.to_image_unnamed, st.strings_kept, st.statics_dropped,
         st.statics_new, bad, bad == 1 ? "" : "s");
    return true;
}

/* ---- Save points ----
 *
 * A save point is a snapshot the program goes on from: taken at a safe
 * point as Hibernate's is, but the machine runs on afterwards, its
 * connections, sound, files and windows as they were (nothing the
 * snapshot reads back from the host is let go).  Putting the latest back
 * (the host's Restore) happens in the same process, over the running
 * machine, which stopped at a safe point first: what the host plays, says
 * or shows for the timeline left behind stops, its sockets close as a
 * process ending would close them, the snapshot replaces the machine, and
 * the host is reattached as for a resumed process (the clocks go on from
 * the save point; the network's listeners are made again and its
 * connections are over; the sound it had queued is played again).  A file
 * the program had open for writing goes back to the save point's copy
 * (files_after_savepoint_restore), rather than being marked gone: the
 * change is the program's own.  The host's input is taken afresh. */

bool machine_savepoint_take(uint8_t **buf, size_t *len, char *err, size_t errlen)
{
    if (!at_safe_point(err, errlen))
        return false;
    events_note_savepoint();
    capture_begin();
    hb_stats st;
    bool ok = hb_save_buffer(buf, len, &st, err, errlen);
    capture_end();
    if (ok) {
        sound_record_note("save point");
        speech_record_note("save point");
        LOGF(LOG_IDLE, "save point: %llu bytes, %u blocks, %u pointers\n", (unsigned long long)st.bytes, st.blocks,
             st.relocs);
    }
    return ok;
}

bool machine_savepoint_write(const char *path, const uint8_t *buf, size_t len, const char *record, uint64_t *bytes,
                             char *err, size_t errlen)
{
    hb_stats st;
    memset(&st, 0, sizeof st);
    if (!hb_write_file_extra(path, buf, len, record, record ? strlen(record) : 0, &st, err, errlen))
        return false;
    if (bytes)
        *bytes = st.file_bytes;
    return true;
}

void machine_savepoint_free(uint8_t *buf) { hb_free_buffer(buf); }

bool machine_savepoint_restore(const uint8_t *buf, size_t len, int *differences, char *err, size_t errlen)
{
    if (!machine_drain_at_safe_point()) {
        const char *why = machine_drain_reason();
        snprintf(err, errlen, "%s", *why ? why : "the program did not come to a point where it could be put back");
        return false;
    }
    maybe_crash_trace();
    uint64_t was_at = g_cpu ? g_cpu->icount : 0;
    int layout = g_mem_layout;
    void *script = events_script_take();
    sound_before_restore();
    speech_before_restore();
    qt_movies_before_restore();
    network_close_sockets();
    hb_stats st;
    if (!hb_restore_buffer(buf, len, HB_RESTORE_FORGET_HOST, &st, err, errlen)) {
        events_script_give_back(script, 0, 0);
        return false;
    }
    if (saved.layout != layout) {
        snprintf(err, errlen, "the save point's memory layout (%d) is not this machine's (%d)", saved.layout, layout);
        return false;
    }
    if (differences) {
        /* The machine as put back, against the save point: none but the
         * host's pointers (cleared). */
        uint8_t *now;
        size_t now_len;
        char cerr[256];
        *differences = hb_save_buffer(&now, &now_len, NULL, cerr, sizeof cerr)
                           ? hb_compare(buf, len, now, now_len, HB_COMPARE_IGNORE_HOST, stdout, 20)
                           : -1;
        if (*differences >= 0)
            hb_free_buffer(now);
    }
    host_clock_offset = (int64_t)saved.host_ns - (int64_t)host_mono_ns();
    /* The date is the host's again, unless the clock is the instructions'
     * (headless, where a run repeats exactly). */
    machine_resume_clock(saved.time_ns, machine_wall_clock(), (int64_t)time(NULL) - saved.wall);
    hibernate_reattach_host();
    events_after_savepoint_restore();
    int back = files_after_savepoint_restore();
    if (events_host_attached()) {
        /* The host takes the windows, the menu bar and the cursor afresh. */
        display_host_attached_after_resume();
        cursor_after_resume();
    }
    events_script_give_back(script, was_at, g_cpu ? g_cpu->icount : 0);
    sound_record_note("restored");
    speech_record_note("restored");
    LOGF(LOG_IDLE, "restore: %u blocks, %u pointers (%u to the host's memory, cleared); %d file%s written back\n",
         st.blocks, st.relocs, st.host, back, back == 1 ? "" : "s");
    return true;
}

/* ---- What a snapshot cannot carry ---- */

const char *network_io_open(void)
{
    const char *at = appletalk_unresumable();
    if (at)
        return at;
    if (qt_streams_open())
        return "has a streamed movie open";
    return NULL;
}

const char *print_job_open(void) { return print_job_in_progress() ? "is printing" : NULL; }

/* What lives in the host's process (its objects, its threads, its temporary
 * files) and is not reattached yet: refused too. */
const char *host_media_open(void)
{
    const char *gl = hostgl_blocker();
    if (gl)
        return gl;
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
    threads_after_resume(); /* their host threads, made again when their turns come */
    cfm_after_resume();
    sound_after_resume();   /* the host told again what it had not played yet */
    speech_after_resume();  /* an utterance being spoken is over */
    sound_input_after_resume(); /* a recording stops */
    q3_view_after_resume();  /* QuickDraw 3D's textures, made again from its objects */
    r3d_after_resume();      /* QuickDraw 3D's and RAVE's frames and textures, made again as used */
    hostgl_after_resume();   /* OpenGL's contexts, made again as used */
    qt_movies_after_resume(); /* VLC's movies opened again, their flattened copies made again */
    network_after_resume();
    drv68k_after_resume();   /* a driver's read given up for the drain fails */
}

/* ---- The network ----
 *
 * The host's sockets did not come along: every descriptor the network's
 * state names is the old process's (a number that may now be another
 * file's here), so each is forgotten, never closed.  What only listens
 * or is registered is made again on the new host: the AppleTalk link and
 * this node (the old address if no one has taken it), NBP's names, DDP
 * sockets and ATP's transactions (which go on by their retries); MacTCP's
 * and Open Transport's listeners and UDP streams and endpoints, bound to
 * their old ports; the PPC Toolbox's ports; the serial ports' host ends.
 * A connection is over: the program hears of it at the next safe point as
 * each protocol says so (MacTCP's TCPTerminate and connectionTerminated,
 * Open Transport's T_DISCONNECT, ADSP's tear-down, ASP's aspSessClosed,
 * the PPC Toolbox's sessClosedErr, the modem's carrier lost), and its
 * calls under way end with that error; an AppleShare volume's session is
 * opened again on its next use, as for a server's restart. */
void network_after_resume(void)
{
    appletalk_after_resume(); /* first: the protocols on it send */
    xpp_after_resume();
    appleshare_after_resume();
    mactcp_after_resume();
    ot_after_resume();
    ppc_toolbox_after_resume();
    serial_after_resume();
    ctb_after_resume();
}

/* The network's host sockets closed, as when a process ends: for the
 * tests, which resume a snapshot in the process that took it. */
void network_close_sockets(void)
{
    mactcp_close_sockets();
    ot_close_sockets();
    ppc_toolbox_close_sockets();
    serial_close_sockets();
    ctb_close_sockets();
}
