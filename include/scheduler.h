#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "psx_timing.h"

/*
 * SuperPSX Event-Driven Scheduler
 *
 * Replaces per-block UpdateTimers/CDROM_Update polling with a cycle-accurate
 * event system. The CPU executes blocks until the nearest event deadline,
 * then dispatches it. Each event callback reschedules itself for the next
 * occurrence.
 */

/* ---- Event IDs ---- */
#define SCHED_EVENT_TIMER0 0
#define SCHED_EVENT_TIMER1 1
#define SCHED_EVENT_TIMER2 2
#define SCHED_EVENT_SIO_IRQ 3 /* SIO IRQ delay (was VBLANK no-op) */
#define SCHED_EVENT_CDROM 4
#define SCHED_EVENT_CDROM_DEFERRED 5 /* Deferred first-response delivery */
#define SCHED_EVENT_CDROM_IRQ 6      /* IRQ signal delay (I_STAT assertion) */
#define SCHED_EVENT_CDROM_PENDING 7  /* Pending (2nd) response delivery      */
#define SCHED_EVENT_HBLANK 8         /* Per-scanline HBlank event           */
#define SCHED_EVENT_DMA 9            /* Deferred DMA completion event        */
#define SCHED_EVENT_COUNT 10

/* ---- Callback type ---- */
/* ticks_late = how many cycles past the scheduled deadline the event fired. */
typedef void (*sched_callback_t)(int ticks_late);

/* ---- Event slot (visible for inlining) ---- */
typedef struct
{
    int active;
    uint64_t deadline;
    sched_callback_t callback;
} SchedEvent;

/* ---- Shared state (defined in scheduler.c) ---- */
extern SchedEvent sched_events[SCHED_EVENT_COUNT];
extern uint64_t global_cycles;
extern uint32_t partial_block_cycles;
extern volatile uint32_t chain_cycles_acc;
extern int scheduler_unlimited_speed;
extern uint64_t scheduler_cached_earliest;
extern int scheduler_earliest_id;
extern volatile int scheduler_interrupt_chain; /* Set by SignalInterrupt when irq_pending; forces C loop exit */
extern uint64_t hblank_frame_start_cycle; /* in dynarec_run.c */

/* ---- Init (in scheduler.c) ---- */
void Scheduler_Init(void);

/* ---- Inline helpers ---- */

static inline void sched_recompute_cached(void)
{
    uint64_t earliest = UINT64_MAX;
    int earliest_id = -1;
    int i;
    for (i = 0; i < SCHED_EVENT_COUNT; i++)
    {
        if (sched_events[i].active && sched_events[i].deadline < earliest)
        {
            earliest = sched_events[i].deadline;
            earliest_id = i;
        }
    }
    scheduler_cached_earliest = earliest;
    scheduler_earliest_id = earliest_id;
}

static inline void Scheduler_ScheduleEvent(int event_id, uint64_t absolute_cycle,
                                           sched_callback_t cb)
{
    int was_earliest = (event_id == scheduler_earliest_id);

    sched_events[event_id].active = 1;
    sched_events[event_id].deadline = absolute_cycle;
    sched_events[event_id].callback = cb;

    if (absolute_cycle <= scheduler_cached_earliest)
    {
        scheduler_cached_earliest = absolute_cycle;
        scheduler_earliest_id = event_id;
    }
    else if (was_earliest)
    {
        /* We just pushed the earliest event further into the future;
         * we must re-scan to find the new true earliest. */
        sched_recompute_cached();
    }
}

static inline void Scheduler_RemoveEvent(int event_id)
{
    int was_earliest = (event_id == scheduler_earliest_id);
    sched_events[event_id].active = 0;

    if (was_earliest)
        sched_recompute_cached();
}

static inline void Scheduler_DispatchEvents(uint64_t current_cycle)
{
    int i;
    int needed_recompute = 0;
    for (i = 0; i < SCHED_EVENT_COUNT; i++)
    {
        if (sched_events[i].active && sched_events[i].deadline <= current_cycle)
        {
            if (i == scheduler_earliest_id)
                needed_recompute = 1;

            int ticks_late = (int)(current_cycle - sched_events[i].deadline);
            sched_events[i].active = 0;
            if (sched_events[i].callback)
                sched_events[i].callback(ticks_late);
        }
    }
    if (needed_recompute)
        sched_recompute_cached();
}

#endif /* SCHEDULER_H */
