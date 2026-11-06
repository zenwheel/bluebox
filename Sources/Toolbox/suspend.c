/* Suspending a program: the whole machine held still until it is resumed.
 *
 * The host asks (machine_pause) from its own thread; the machine's thread
 * stops at the next pause point (between emulation slices, and wherever
 * Ticks are brought up to date, which every wait in the Toolbox does) and
 * sleeps there, using no processor time, until machine_unpause.  The
 * machine's clock stands still from the moment of asking: Ticks, the
 * Time Manager, VBL tasks, the Sound Manager's channels and the date all
 * read the time less the time spent suspended, so a program finds no
 * jump when it goes on. */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "machine.h"
#include "tb_internal.h"

static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_changed = PTHREAD_COND_INITIALIZER;
static atomic_bool pause_wanted;
static bool pause_parked;
static uint64_t pause_since;   /* the clock when it was asked for */
static _Atomic uint64_t pause_total; /* time spent suspended before that */
static uint64_t pause_until;   /* a timed pause (pause:N): when it ends, or 0 */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* With the lock held: wait for a change, at most ns. */
static void wait_ns(uint64_t ns)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ns / 1000000000u);
    ts.tv_nsec += (long)(ns % 1000000000u);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&pause_changed, &pause_lock, &ts);
}

/* With the lock held. */
static void unpause_locked(void)
{
    if (!atomic_load(&pause_wanted))
        return;
    pause_total += mono_ns() - pause_since;
    pause_until = 0;
    atomic_store(&pause_wanted, false);
    pthread_cond_broadcast(&pause_changed);
}

void machine_pause(void)
{
    pthread_mutex_lock(&pause_lock);
    if (!atomic_load(&pause_wanted)) {
        pause_since = mono_ns();
        pause_until = 0;
        atomic_store(&pause_wanted, true);
    }
    pthread_mutex_unlock(&pause_lock);
}

void machine_unpause(void)
{
    pthread_mutex_lock(&pause_lock);
    unpause_locked();
    pthread_mutex_unlock(&pause_lock);
}

void machine_pause_for(uint32_t ms)
{
    machine_pause();
    pthread_mutex_lock(&pause_lock);
    pause_until = mono_ns() + (uint64_t)ms * 1000000u;
    pthread_mutex_unlock(&pause_lock);
}

bool machine_paused(void) { return atomic_load(&pause_wanted); }

bool machine_pause_parked(void)
{
    pthread_mutex_lock(&pause_lock);
    bool parked = pause_parked;
    pthread_mutex_unlock(&pause_lock);
    return parked;
}

/* The host's monotonic clock as the machine counts it: standing still
 * while suspended, and less every suspension before. */
uint64_t machine_unpaused_ns(uint64_t now)
{
    if (!atomic_load(&pause_wanted) && !pause_total)
        return now;
    pthread_mutex_lock(&pause_lock);
    if (atomic_load(&pause_wanted) && now > pause_since)
        now = pause_since;
    now -= pause_total;
    pthread_mutex_unlock(&pause_lock);
    return now;
}

void machine_pause_point(void)
{
    if (!atomic_load(&pause_wanted))
        return;
#ifdef __APPLE__
    /* The host's main thread never stops here: only the machine's does
     * (a headless run's machine runs on the main thread, and only timed
     * pauses reach it). */
    if (events_host_attached() && pthread_main_np())
        return;
#endif
    pthread_mutex_lock(&pause_lock);
    pause_parked = true;
    pthread_cond_broadcast(&pause_changed);
    while (atomic_load(&pause_wanted)) {
        if (pause_until) {
            uint64_t now = mono_ns();
            if (now >= pause_until) {
                unpause_locked();
                break;
            }
            wait_ns(pause_until - now);
        } else {
            pthread_cond_wait(&pause_changed, &pause_lock);
        }
    }
    pause_parked = false;
    pthread_cond_broadcast(&pause_changed);
    pthread_mutex_unlock(&pause_lock);
}

bool machine_wait_parked(uint32_t ms)
{
    uint64_t until = mono_ns() + (uint64_t)ms * 1000000u;
    pthread_mutex_lock(&pause_lock);
    while (!pause_parked && atomic_load(&pause_wanted)) {
        uint64_t now = mono_ns();
        if (now >= until)
            break;
        wait_ns(until - now);
    }
    bool parked = pause_parked;
    pthread_mutex_unlock(&pause_lock);
    return parked;
}
