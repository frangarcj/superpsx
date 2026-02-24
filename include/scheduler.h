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
#define PSX_CPU_FREQ 33868800U  /* 33.8688 MHz */
#define CYCLES_PER_HBLANK 2152U /* ~33868800 / 15734 */
#define CYCLES_PER_SCANLINE CYCLES_PER_HBLANK
#define SCANLINES_PER_FRAME 263U     /* NTSC */
#define SCANLINES_PER_FRAME_PAL 314U /* PAL  */
/* Accurate frame timing: scanlines × cycles/scanline (not CPU_FREQ/fps) */
#define CYCLES_PER_FRAME_NTSC (SCANLINES_PER_FRAME * CYCLES_PER_HBLANK)    /* 565976 */
#define CYCLES_PER_FRAME_PAL (SCANLINES_PER_FRAME_PAL * CYCLES_PER_HBLANK) /* 675728 */

/* Timer0 dotclock dividers: CPU_FREQ / dotclock_freq (integer approx)
 * 256-wide:  5.3222400 MHz → div≈7  (exact: 6.366)
 * 320-wide:  6.6528000 MHz → div≈5  (exact: 5.091)  [most common]
 * 368-wide:  7.6032000 MHz → div≈4  (exact: 4.454)
 * 512-wide: 10.6444800 MHz → div≈3  (exact: 3.183)
 * 640-wide: 13.3056000 MHz → div≈3  (exact: 2.546)   */
#define DOTCLOCK_DIV_256 7U
#define DOTCLOCK_DIV_320 5U
#define DOTCLOCK_DIV_368 4U /* actually ~4.45, fractional handled */
#define DOTCLOCK_DIV_512 3U
#define DOTCLOCK_DIV_640 3U /* actually ~2.55, fractional handled */

/* Approximate CD-ROM sector read delay (1x speed, ~150 sectors/s) */
#define CDROM_READ_CYCLES (PSX_CPU_FREQ / 150) /* ~225792 */
/* Fast approximation for usability (not exact) */
#define CDROM_READ_CYCLES_FAST 50000U

/* ---- Event IDs ---- */
#define SCHED_EVENT_TIMER0 0
#define SCHED_EVENT_TIMER1 1
#define SCHED_EVENT_TIMER2 2
#define SCHED_EVENT_VBLANK 3
#define SCHED_EVENT_CDROM 4
#define SCHED_EVENT_CDROM_DEFERRED 5 /* Deferred first-response delivery */
#define SCHED_EVENT_CDROM_IRQ 6      /* IRQ signal delay (I_STAT assertion) */
#define SCHED_EVENT_HBLANK 7         /* Per-scanline HBlank event           */
#define SCHED_EVENT_DMA    8         /* Deferred DMA completion event        */
#define SCHED_EVENT_COUNT 9

/* ---- Callback type ---- */
typedef void (*sched_callback_t)(void);

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

static inline uint64_t Scheduler_NextDeadline(void)
{
    return scheduler_cached_earliest;
}

static inline uint64_t Scheduler_NextDeadlineFast(void)
{
    return scheduler_cached_earliest;
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

            sched_events[i].active = 0;
            if (sched_events[i].callback)
                sched_events[i].callback();
        }
    }
    if (needed_recompute)
        sched_recompute_cached();
}

#endif /* SCHEDULER_H */
