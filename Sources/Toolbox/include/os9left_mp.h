/* What MPLibrary's own calls (os9left_mp.c) and Multiprocessing Services
 * (multiprocessing.c) ask of each other. */
#ifndef OS9LEFT_MP_H
#define OS9LEFT_MP_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* Reference kinds (ref_new, carbon_events.h) of Multiprocessing Services'
 * objects, and MPLibrary's own. */
enum { REF_TASK = 101, REF_QUEUE, REF_SEMAPHORE, REF_REGION, REF_EVENT, REF_NOTIFICATION, REF_TIMER,
       REF_PROCESS, REF_CPU, REF_COHERENCE, REF_SPACE, REF_AREA = 130, REF_POOL, REF_OID };

/* A block MPFree has freed: the pool that has it forgets it. */
void os9left_mp_freed(gaddr p);

/* The next area ID after `after` (0 for the first) into *idp; the
 * iteration's error at the end. */
int32_t os9left_mp_next_area(gaddr after, gaddr idp);

/* multiprocessing.c */
extern bool mp_scheduling;
int mp_query(gaddr id, uint32_t out[4]);
bool mp_task_blocking(gaddr task, gaddr *on);
unsigned mp_task_ids(gaddr *ids, unsigned max);
void mp_terminate_all(int32_t status);

#endif
