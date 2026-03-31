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
uint32_t partial_block_cycles = 0;
int sched_unlimited_speed = 0;
uint64_t sched_cached_earliest = UINT64_MAX;
int sched_earliest_id = -1;
volatile int sched_interrupt_chain = 0;

/* ---- Init ---- */
void Sched_Init(void)
{
    memset(sched_events, 0, sizeof(sched_events));
    global_cycles = 0;
    partial_block_cycles = 0;
    sched_cached_earliest = UINT64_MAX;
    sched_earliest_id = -1;
    sched_interrupt_chain = 0;
    printf("Scheduler initialized (%d event slots)\n", SCHED_EVENT_COUNT);
}
