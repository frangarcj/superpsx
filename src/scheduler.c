#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "scheduler.h"

#define LOG_TAG "SCHED"
#include "superpsx.h"

/*
 * SuperPSX Event-Driven Scheduler
 *
 * All hot-path functions (Schedule, Remove, Dispatch) are static inline
 * in scheduler.h.  Only Init and global definitions live here.
 */

/* ---- Shared state (accessed by inline functions in scheduler.h) ---- */
SchedEvent sched_events[SCHED_EVENT_COUNT];

uint64_t global_cycles = 0;
int scheduler_unlimited_speed = 0;
uint64_t scheduler_cached_earliest = UINT64_MAX;
int scheduler_earliest_id = -1;

/* ---- Init ---- */
void Scheduler_Init(void)
{
    memset(sched_events, 0, sizeof(sched_events));
    global_cycles = 0;
    scheduler_cached_earliest = UINT64_MAX;
    scheduler_earliest_id = -1;
    printf("Scheduler initialized (%d event slots)\n", SCHED_EVENT_COUNT);
}
