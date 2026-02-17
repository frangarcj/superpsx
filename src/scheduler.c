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

/* ---- Init ---- */
void Scheduler_Init(void)
{
    memset(events, 0, sizeof(events));
    global_cycles = 0;
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
}

/* ---- Remove ---- */
void Scheduler_RemoveEvent(int event_id)
{
    if (event_id < 0 || event_id >= SCHED_EVENT_COUNT)
        return;
    events[event_id].active = 0;
}

/* ---- Next deadline ---- */
uint64_t Scheduler_NextDeadline(void)
{
    uint64_t earliest = UINT64_MAX;
    int i;
    for (i = 0; i < SCHED_EVENT_COUNT; i++)
    {
        if (events[i].active && events[i].deadline < earliest)
            earliest = events[i].deadline;
    }
    return earliest;
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
            if (events[i].callback)
                events[i].callback();
            /* The callback is expected to reschedule itself if recurring */
        }
    }
}
