#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

/*
 * SuperPSX Event-Driven Scheduler
 *
 * Replaces per-block UpdateTimers/CDROM_Update polling with a cycle-accurate
 * event system. The CPU executes blocks until the nearest event deadline,
 * then dispatches it. Each event callback reschedules itself for the next
 * occurrence.
 *
 * PSX CPU: 33.868800 MHz (R3000A)
 * NTSC: 60 Hz → ~564480 cycles/frame
 * PAL:  50 Hz → ~677376 cycles/frame
 */

/* ---- PSX timing constants ---- */
#define PSX_CPU_FREQ          33868800U  /* 33.8688 MHz */
#define CYCLES_PER_FRAME_NTSC (PSX_CPU_FREQ / 60)   /* ~564480 */
#define CYCLES_PER_FRAME_PAL  (PSX_CPU_FREQ / 50)   /* ~677376 */
#define CYCLES_PER_HBLANK     2152U                  /* 33868800 / 15734 */
#define CYCLES_PER_SCANLINE   CYCLES_PER_HBLANK
#define SCANLINES_PER_FRAME   263U                   /* NTSC */

/* Approximate CD-ROM sector read delay (1x speed, ~150 sectors/s) */
#define CDROM_READ_CYCLES     (PSX_CPU_FREQ / 150)   /* ~225792 */
/* Fast approximation for usability (not exact) */
#define CDROM_READ_CYCLES_FAST 50000U

/* ---- Event IDs ---- */
#define SCHED_EVENT_TIMER0          0
#define SCHED_EVENT_TIMER1          1
#define SCHED_EVENT_TIMER2          2
#define SCHED_EVENT_VBLANK          3
#define SCHED_EVENT_CDROM           4
#define SCHED_EVENT_CDROM_DEFERRED  5  /* Deferred first-response delivery */
#define SCHED_EVENT_CDROM_IRQ       6  /* IRQ signal delay (I_STAT assertion) */
#define SCHED_EVENT_COUNT           7

/* ---- Callback type ---- */
typedef void (*sched_callback_t)(void);

/* ---- Global cycle counter ---- */
extern uint64_t global_cycles;

/* ---- Speed control ---- */
extern int scheduler_unlimited_speed;  /* 1 = no frame pacing */

/* ---- API ---- */

/* Initialize scheduler, reset all events and global_cycles */
void Scheduler_Init(void);

/* Schedule an event to fire at absolute_cycle.
 * If the event is already scheduled, its deadline is updated. */
void Scheduler_ScheduleEvent(int event_id, uint64_t absolute_cycle,
                             sched_callback_t cb);

/* Remove a scheduled event (mark inactive) */
void Scheduler_RemoveEvent(int event_id);

/* Return the earliest deadline among active events.
 * Returns UINT64_MAX if no events are active. */
uint64_t Scheduler_NextDeadline(void);

/* Exposed cache for fastest-read access (updated on schedule/remove/dispatch).
 * Read-only for callers who prefer direct access. */
extern uint64_t scheduler_cached_earliest;

/* Fast inline accessor for hot paths. Prefer this in inner loops to avoid
 * function call overhead. Returns UINT64_MAX when no events are active. */
static inline uint64_t Scheduler_NextDeadlineFast(void)
{
    return scheduler_cached_earliest;
}

/* Dispatch all events whose deadline <= current_cycle.
 * Callbacks are responsible for rescheduling themselves. */
void Scheduler_DispatchEvents(uint64_t current_cycle);

#endif /* SCHEDULER_H */
