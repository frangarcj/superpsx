#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "scheduler.h"
#include "psx_timers.h"
#include "gpu_state.h"

#include "config.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "TMR"

/* Effective cycle count: global_cycles + in-progress block cycles.
 * During JIT block execution, partial_block_cycles holds the compile-time
 * cycle offset for the current instruction, allowing mid-block timer reads
 * to see accurate elapsed time instead of stale global_cycles. */
/* Effective cycle count during JIT execution:
 * global_cycles is only advanced at block boundaries when a chain ends.
 * We add elapsed cycles within the current chain (initial - current) plus any
 * cycle cost from the partially executed current block. */
#define EFFECTIVE_CYCLES  (global_cycles + (cpu.initial_cycles_left - cpu.cycles_left) + partial_block_cycles)

typedef struct
{
    uint32_t value;
    uint32_t mode;
    uint32_t target;
    uint64_t last_sync_cycle;
} PsxTimer;

static PsxTimer timers[3];
static uint8_t timer_stopped_cache[3] = {0, 0, 0};
static uint32_t timer_divider_cache[3] = {1, 1, 1};
/* One-shot IRQ gating:
 * When mode bit 6 = 0 (one-shot), IRQ fires once then irq_fired blocks
 * further firing until the mode register is re-written.
 * When mode bit 6 = 1 (repeat), IRQ fires every time. */
static uint8_t timer_irq_fired[3] = {0, 0, 0};
static uint64_t timer_mode_set_cycle[3] = {0, 0, 0}; /* Cycle at which mode register was written */

/* Timer0 dotclock fractional accumulator.
 * Real dotclock dividers are N×7/11 CPU cycles (not integer).
 * timer0_dotclock_num = N×7 when in dotclock mode, 0 when in sysclk.
 * timer0_dotclock_residue = fractional sub-11 CPU cycle accumulator (0..10). */
static uint32_t timer0_dotclock_num = 0;
static uint32_t timer0_dotclock_residue = 0;

static void Timer_Callback0(void);
static void Timer_Callback1(void);
static void Timer_Callback2(void);
static const sched_callback_t timer_callbacks[3] = {Timer_Callback0, Timer_Callback1, Timer_Callback2};

static void timer_update_divider_cache(int t)
{
    if (t < 0 || t > 2) return;
    uint32_t src = (timers[t].mode >> 8) & 3;
    if (t == 0 && (src == 1 || src == 3))
    {
        /* Dotclock mode: set both integer divider (for scheduling) and
         * fractional numerator (for exact tick computation). */
        if (disp_hres368) {
            timer_divider_cache[0] = DOTCLOCK_DIV_368;
            timer0_dotclock_num = DOTCLOCK_NUM_368;
        } else switch (disp_hres) {
            case 0: timer_divider_cache[0] = DOTCLOCK_DIV_256; timer0_dotclock_num = DOTCLOCK_NUM_256; break;
            case 1: timer_divider_cache[0] = DOTCLOCK_DIV_320; timer0_dotclock_num = DOTCLOCK_NUM_320; break;
            case 2: timer_divider_cache[0] = DOTCLOCK_DIV_512; timer0_dotclock_num = DOTCLOCK_NUM_512; break;
            case 3: timer_divider_cache[0] = DOTCLOCK_DIV_640; timer0_dotclock_num = DOTCLOCK_NUM_640; break;
            default: timer_divider_cache[0] = DOTCLOCK_DIV_320; timer0_dotclock_num = DOTCLOCK_NUM_320; break;
        }
        timer0_dotclock_residue = 0;
        return;
    }
    if (t == 0) timer0_dotclock_num = 0; /* sysclk mode */
    if (t == 1 && (src == 1 || src == 3)) {
        /* Real hardware counts discrete HBlank events, not cycle fractions.
         * We approximate with cycle division, so use divider - 1 to provide
         * exactly SCANLINES_PER_FRAME cycles of margin per frame.  This
         * prevents the fencepost where measurement overhead (timer reset to
         * timer read spans slightly less than a full frame) would otherwise
         * cause floor(cycles / divider) to return 262 instead of 263. */
        uint32_t hblank = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL : CYCLES_PER_HBLANK_NTSC;
        timer_divider_cache[1] = hblank - 1;
        return;
    }
    if (t == 2 && (src == 2 || src == 3)) { timer_divider_cache[2] = 8; return; }
    timer_divider_cache[t] = 1;
}

static void timer_update_stopped_cache(int t)
{
    if (t < 0 || t > 2) return;
    uint32_t mode = timers[t].mode;
    if (!(mode & 1)) { timer_stopped_cache[t] = 0; return; }
    uint32_t sync_mode = (mode >> 1) & 3;
    /* On real hardware, Timer 0 and Timer 1 practically free-run
     * across their respective domains. Only Timer 2 has a true hardware disable
     * (Stop counter at current value / Stop until). */
    if (t == 2 && (sync_mode == 0 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    timer_stopped_cache[t] = 0;
}

static void Timer_SyncValue(int t)
{
    if (t < 0 || t > 2) return;
    uint64_t now = EFFECTIVE_CYCLES;

    /* Removed absolute phase overrides for Timer 0 and 1.
     * Timer 0/1 sync modes (Pause during VBlank, Reset at HBlank) require
     * continuous accumulation that respects when the user manually wrote `value=0`.
     * Computing absolute phase based on `now - hblank_frame_start_cycle` destroyed
     * user writes. For now, they will act practically as free-run, which
     * satisfies Crash Bandicoot's benchmarking without jumping to 197 arbitrarily. */

    if (timer_stopped_cache[t]) { timers[t].last_sync_cycle = now; return; }

    uint32_t divider = timer_divider_cache[t];
    uint64_t elapsed = now - timers[t].last_sync_cycle;
    uint32_t ticks;

    if (t == 0 && timer0_dotclock_num > 0)
    {
        /* Fractional dotclock accumulation: exact tick count using
         * sub-11 CPU cycle precision (denominator is always 11).
         * ticks = (elapsed × 11 + residue) / dotclock_num */
        uint64_t sub11 = elapsed * 11 + timer0_dotclock_residue;
        ticks = (uint32_t)(sub11 / timer0_dotclock_num);
        uint64_t consumed_sub11 = (uint64_t)ticks * timer0_dotclock_num;
        uint32_t remaining_sub11 = (uint32_t)(sub11 - consumed_sub11);
        timers[0].last_sync_cycle = now - remaining_sub11 / 11;
        timer0_dotclock_residue = remaining_sub11 % 11;
    }
    else
    {
        /* Integer divider path (sysclk, hblank, sysclk/8). */
        ticks = (uint32_t)(elapsed / divider);
        timers[t].last_sync_cycle += (uint64_t)ticks * divider;
    }
    if (ticks == 0) return;

    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t mode = timers[t].mode;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t new_val = val + ticks;

    int wrapped_target = 0;
    int wrapped_overflow = 0;

    if ((mode & (1 << 3)) && target > 0)
    {
        /* Target condition is hit when counter matches target.
         * BUT, the counter evaluates to `target` for exactly 1 tick before resetting to 0.
         * Thus, the wrap happens when new_val > target. */
        if (val < target && new_val >= target)
        {
            wrapped_target = 1;
            timers[t].mode |= (1 << 11);
        }

        if (new_val > target)
        {
            if (target >= 0xFFFF && new_val > 0xFFFF)
            {
                timers[t].mode |= (1 << 12);
                wrapped_overflow = 1;
            }
            new_val = (new_val - target) - 1; /* Reset to 0 for the first tick past target */
            if (new_val > target) new_val %= (target + 1); /* Safe fallback for massive jumps */
        }
        timers[t].value = new_val;
    }
    else
    {
        if (target > 0 && val < target && new_val >= target)
        {
            timers[t].mode |= (1 << 11);
            wrapped_target = 1;
        }
        if (new_val > 0xFFFF)
        {
            timers[t].mode |= (1 << 12);
            wrapped_overflow = 1;
            new_val -= 0x10000;
        }
        timers[t].value = new_val & 0xFFFF;
    }

    /* Signal Interrupt if conditions met.
     * Mode bit 6 = IRQ repeat mode:
     *   0 = one-shot: fire once, then block until mode is re-written
     *   1 = repeat: fire every time target/overflow is reached */
    int can_fire = (mode & (1 << 6)) || !timer_irq_fired[t];

    if (wrapped_target && (mode & (1 << 4)) && can_fire) {
        SignalInterrupt(4 + t);
        timer_irq_fired[t] = 1;
    }

    if (wrapped_overflow && (mode & (1 << 5)) && can_fire) {
        SignalInterrupt(4 + t);
        timer_irq_fired[t] = 1;
    }
}

static void Timer_ScheduleOne(int t)
{
    if (t < 0 || t > 2) return;
    Timer_SyncValue(t);
    uint32_t mode = timers[t].mode;
    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t divider = timer_divider_cache[t];
    if (timer_stopped_cache[t]) return;

    if (divider == 0) divider = 1;

    uint32_t ticks_to_event = 0;
    if ((mode & (1 << 3)) && target > 0)
    {
        if (val <= target) ticks_to_event = (target + 1) - val;
        else ticks_to_event = 1; /* Fallback if somehow past target */
    }
    else
    {
        uint32_t ticks_to_overflow = 0x10000 - val;
        ticks_to_event = ticks_to_overflow;
        if ((mode & (1 << 4)) && target > 0 && val <= target)
        {
            uint32_t ticks_to_target = (target + 1) - val;
            if (ticks_to_target < ticks_to_event) ticks_to_event = ticks_to_target;
        }
    }
    if (ticks_to_event == 0) ticks_to_event = 1;

    uint64_t deadline;
    if (t == 0 && timer0_dotclock_num > 0)
        deadline = EFFECTIVE_CYCLES + ((uint64_t)ticks_to_event * timer0_dotclock_num + 10) / 11;
    else
        deadline = EFFECTIVE_CYCLES + (uint64_t)ticks_to_event * divider;

    Scheduler_ScheduleEvent(SCHED_EVENT_TIMER0 + t, deadline, timer_callbacks[t]);
}

static void Timer_FireEvent(int t)
{
    if (t < 0 || t > 2) return;
    /* 
     * ScheduleOne immediately calls SyncValue, which now correctly
     * adds the elapsed ticks via exact math, wraps target/overflow using
     * exact subtraction, triggers SignalInterrupt directly, and preserves
     * the cycle residue (last_sync_cycle).
     * By not manually stomping last_sync_cycle to global_cycles, we eliminate
     * the cumulative scheduling drift.
     */
    Timer_ScheduleOne(t);
}

static void Timer_Callback0(void) { Timer_FireEvent(0); }
static void Timer_Callback1(void) { Timer_FireEvent(1); }
static void Timer_Callback2(void) { Timer_FireEvent(2); }

uint32_t Timers_Read(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFF;
    int t = (phys - 0x1F801100) / 0x10;
    int reg = ((phys - 0x1F801100) % 0x10) / 4;
    if (t < 0 || t > 2) return 0;
    switch (reg) {
        case 0:
            Timer_SyncValue(t);
            return timers[t].value & 0xFFFF;
        case 1: { uint32_t mode = timers[t].mode; timers[t].mode &= ~((1 << 11) | (1 << 12)); return mode; }
        case 2: return timers[t].target;
    }
    return 0;
}

void Timers_Write(uint32_t addr, uint32_t data)
{
    uint32_t phys = addr & 0x1FFFFFFF;
    int t = (phys - 0x1F801100) / 0x10;
    int reg = ((phys - 0x1F801100) % 0x10) / 4;
    if (t < 0 || t > 2) return;
    uint64_t now = EFFECTIVE_CYCLES;
    switch (reg) {
        case 0: timers[t].value = data; timers[t].last_sync_cycle = now; break;
        case 1: timers[t].value = 0; timers[t].mode = data & ~(0x1800); timers[t].last_sync_cycle = now;
                timer_mode_set_cycle[t] = now;
                timer_irq_fired[t] = 0; /* Reset one-shot gating on mode write */
                timer_update_stopped_cache(t); timer_update_divider_cache(t); break;
        case 2: timers[t].target = data; break;
    }
    Timer_ScheduleOne(t);
}

void Timer_RefreshDividerCache(void) { timer_update_divider_cache(0); }
void Timer_ScheduleAll(void) { for (int t = 0; t < 3; t++) Timer_ScheduleOne(t); }
