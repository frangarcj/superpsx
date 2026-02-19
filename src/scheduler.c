#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "scheduler.h"

#define LOG_TAG "SCHED"
#include "superpsx.h"

/*
 * SuperPSX Event-Driven Scheduler
 *
 * Fixed-size array of SCHED_EVENT_COUNT event slots.
 * O(N) scan with N=5 — negligible cost vs function call overhead.
 */

/* ---- Event slot ---- */
typedef struct
{
    int active;
    uint64_t deadline;
    sched_callback_t callback;
} SchedEvent;

/* ---- State ---- */
static SchedEvent events[SCHED_EVENT_COUNT];

uint64_t global_cycles = 0;
int scheduler_unlimited_speed = 0;

/* Cached earliest deadline (UINT64_MAX = none or invalid).
 * Updated on every schedule/remove/dispatch so callers can read it
 * without a function call (see Scheduler_NextDeadlineFast in scheduler.h). */
uint64_t scheduler_cached_earliest = UINT64_MAX;
static int cached_valid = 0;
static int cached_event_id = -1;

static void recompute_cached_earliest(void)
{
    uint64_t earliest = UINT64_MAX;
    int i, found_id = -1;
    for (i = 0; i < SCHED_EVENT_COUNT; i++)
    {
        if (events[i].active && events[i].deadline < earliest)
        {
            earliest = events[i].deadline;
            found_id = i;
        }
    }
    scheduler_cached_earliest = earliest;
    cached_event_id = found_id;
    cached_valid = 1;
}

/* ---- Init ---- */
void Scheduler_Init(void)
{
    memset(events, 0, sizeof(events));
    global_cycles = 0;
    cached_valid = 0;
    cached_event_id = -1;
    scheduler_cached_earliest = UINT64_MAX;
    printf("Scheduler initialized (%d event slots)\n", SCHED_EVENT_COUNT);
}

/* ---- Schedule ---- */
void Scheduler_ScheduleEvent(int event_id, uint64_t absolute_cycle,
                             sched_callback_t cb)
{
    if (event_id < 0 || event_id >= SCHED_EVENT_COUNT)
        return;
    events[event_id].active = 1;
    events[event_id].deadline = absolute_cycle;
    events[event_id].callback = cb;

    if (!cached_valid)
        recompute_cached_earliest();
    else if (absolute_cycle < scheduler_cached_earliest || cached_event_id == event_id)
        recompute_cached_earliest();
}

/* ---- Remove ---- */
void Scheduler_RemoveEvent(int event_id)
{
    if (event_id < 0 || event_id >= SCHED_EVENT_COUNT)
        return;
    events[event_id].active = 0;

    if (cached_valid && cached_event_id == event_id)
        recompute_cached_earliest();
}

/* ---- Next deadline ---- */
uint64_t Scheduler_NextDeadline(void)
{
    /* Fast path: return cached value */
    return scheduler_cached_earliest;
}

/* ---- Dispatch ---- */
void Scheduler_DispatchEvents(uint64_t current_cycle)
{
    int i;
    for (i = 0; i < SCHED_EVENT_COUNT; i++)
    {
        if (events[i].active && events[i].deadline <= current_cycle)
        {
            events[i].active = 0; /* Mark inactive BEFORE callback */
            int was_cached = (cached_valid && cached_event_id == i);
            if (events[i].callback)
                events[i].callback();
            if (was_cached)
                recompute_cached_earliest();
            /* The callback is expected to reschedule itself if recurring */
        }
    }
}
