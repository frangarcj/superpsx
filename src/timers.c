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
#define EFFECTIVE_CYCLES (global_cycles + (cpu.initial_cycles_left - cpu.cycles_left) + partial_block_cycles)

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

/* ---- Sync mode state ---- */

/* Visible portion of each scanline in CPU cycles (before HBlank starts).
 * PSX-SPX: 2560 active video clocks out of 3413 total per scanline (NTSC).
 * CPU cycles = 2560 × 7/11 ≈ 1630.  HBlank = 2173 - 1630 = 543 cycles. */
static uint32_t hblank_visible_cycles = 1630;

/* Mode 3 one-shot flag: set when first HBlank/VBlank occurs after mode write. */
static uint8_t timer_sync_mode3_freed[3] = {0, 0, 0};

/* Count "active" (visible, non-HBlank) CPU cycles in the range [from, to).
 * Returns the number of CPU cycles where the scanline phase < vis. */
static uint32_t count_active_cycles_hblank(uint64_t from, uint64_t to)
{
    uint32_t cps = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL
                                         : CYCLES_PER_HBLANK_NTSC;
    uint32_t vis = hblank_visible_cycles;
    uint64_t elapsed = to - from;
    if (elapsed == 0 || cps == 0)
        return 0;

    uint32_t phase = (uint32_t)((from - hblank_frame_start_cycle) % cps);
    uint32_t remaining = (uint32_t)elapsed;
    uint32_t active = 0;

    /* First partial scanline */
    uint32_t scanline_remaining = cps - phase;
    if (remaining <= scanline_remaining)
    {
        if (phase < vis)
        {
            uint32_t vis_remain = vis - phase;
            active += (remaining < vis_remain) ? remaining : vis_remain;
        }
        return active;
    }
    if (phase < vis)
        active += vis - phase;
    remaining -= scanline_remaining;

    /* Full scanlines */
    uint32_t full_scanlines = remaining / cps;
    active += full_scanlines * vis;
    remaining -= full_scanlines * cps;

    /* Final partial scanline (starts at phase 0 → visible) */
    active += (remaining < vis) ? remaining : vis;
    return active;
}

static void Timer_Callback0(int ticks_late);
static void Timer_Callback1(int ticks_late);
static void Timer_Callback2(int ticks_late);
static const sched_callback_t timer_callbacks[3] = {Timer_Callback0, Timer_Callback1, Timer_Callback2};

static void timer_update_divider_cache(int t)
{
    if (t < 0 || t > 2)
        return;
    uint32_t src = (timers[t].mode >> 8) & 3;
    if (t == 0 && (src == 1 || src == 3))
    {
        /* Dotclock mode: set both integer divider (for scheduling) and
         * fractional numerator (for exact tick computation). */
        if (disp_hres368)
        {
            timer_divider_cache[0] = DOTCLOCK_DIV_368;
            timer0_dotclock_num = DOTCLOCK_NUM_368;
        }
        else
            switch (disp_hres)
            {
            case 0:
                timer_divider_cache[0] = DOTCLOCK_DIV_256;
                timer0_dotclock_num = DOTCLOCK_NUM_256;
                break;
            case 1:
                timer_divider_cache[0] = DOTCLOCK_DIV_320;
                timer0_dotclock_num = DOTCLOCK_NUM_320;
                break;
            case 2:
                timer_divider_cache[0] = DOTCLOCK_DIV_512;
                timer0_dotclock_num = DOTCLOCK_NUM_512;
                break;
            case 3:
                timer_divider_cache[0] = DOTCLOCK_DIV_640;
                timer0_dotclock_num = DOTCLOCK_NUM_640;
                break;
            default:
                timer_divider_cache[0] = DOTCLOCK_DIV_320;
                timer0_dotclock_num = DOTCLOCK_NUM_320;
                break;
            }
        timer0_dotclock_residue = 0;
        return;
    }
    if (t == 0)
        timer0_dotclock_num = 0; /* sysclk mode */
    if (t == 1 && (src == 1 || src == 3))
    {
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
    if (t == 2 && (src == 2 || src == 3))
    {
        timer_divider_cache[2] = 8;
        return;
    }
    timer_divider_cache[t] = 1;
}

static void timer_update_stopped_cache(int t)
{
    if (t < 0 || t > 2)
        return;
    uint32_t mode = timers[t].mode;
    if (!(mode & 1))
    {
        timer_stopped_cache[t] = 0;
        return;
    }
    uint32_t sync_mode = (mode >> 1) & 3;
    /* On real hardware, Timer 0 and Timer 1 practically free-run
     * across their respective domains. Only Timer 2 has a true hardware disable
     * (Stop counter at current value / Stop until). */
    if (t == 2 && (sync_mode == 0 || sync_mode == 3))
    {
        timer_stopped_cache[t] = 1;
        return;
    }
    timer_stopped_cache[t] = 0;
}

static void Timer_SyncValue(int t)
{
    if (t < 0 || t > 2)
        return;
    uint64_t now = EFFECTIVE_CYCLES;

    if (timer_stopped_cache[t])
    {
        timers[t].last_sync_cycle = now;
        return;
    }

    uint32_t divider = timer_divider_cache[t];
    uint64_t elapsed = now - timers[t].last_sync_cycle;
    uint32_t mode = timers[t].mode;

    /* ---- Sync mode pre-processing for Timer 0 (HBlank) ---- */
    if ((mode & 1) && t == 0)
    {
        uint32_t sync_mode = (mode >> 1) & 3;
        uint32_t cps = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL
                                             : CYCLES_PER_HBLANK_NTSC;
        uint32_t vis = hblank_visible_cycles;
        uint64_t frame_offs_from = timers[t].last_sync_cycle - hblank_frame_start_cycle;
        uint64_t frame_offs_to = now - hblank_frame_start_cycle;
        uint32_t phase_from = (uint32_t)(frame_offs_from % cps);
        uint32_t phase_to = (uint32_t)(frame_offs_to % cps);
        uint32_t scan_from = (uint32_t)(frame_offs_from / cps);
        uint32_t scan_to = (uint32_t)(frame_offs_to / cps);
        int hblank_crossed = (scan_to > scan_from) ||
                             (scan_to == scan_from && phase_from < vis && phase_to >= vis);

        switch (sync_mode)
        {
        case 0: /* Pause during HBlanks — count only visible cycles */
            elapsed = count_active_cycles_hblank(timers[t].last_sync_cycle, now);
            break;

        case 1: /* Reset counter to 0 at HBlanks */
            if (hblank_crossed)
            {
                timers[t].value = 0;
                if (timer0_dotclock_num > 0)
                    timer0_dotclock_residue = 0;
                /* Most recent HBlank start */
                uint64_t last_hblank;
                if (phase_to >= vis)
                    last_hblank = hblank_frame_start_cycle + (uint64_t)scan_to * cps + vis;
                else if (scan_to > 0)
                    last_hblank = hblank_frame_start_cycle + (uint64_t)(scan_to - 1) * cps + vis;
                else
                    last_hblank = timers[t].last_sync_cycle;
                elapsed = now - last_hblank;
            }
            break;

        case 2: /* Reset at HBlank + pause outside HBlank */
            if (phase_to >= vis)
            {
                /* In HBlank: count from HBlank start of this scanline */
                timers[t].value = 0;
                if (timer0_dotclock_num > 0)
                    timer0_dotclock_residue = 0;
                uint64_t hblank_start = hblank_frame_start_cycle +
                                        (uint64_t)scan_to * cps + vis;
                elapsed = now - hblank_start;
            }
            else
            {
                /* In visible: counter reads 0 */
                timers[t].value = 0;
                timers[t].last_sync_cycle = now;
                if (timer0_dotclock_num > 0)
                    timer0_dotclock_residue = 0;
                return;
            }
            break;

        case 3: /* Pause until first HBlank, then free run */
            if (!timer_sync_mode3_freed[0])
            {
                if (hblank_crossed)
                {
                    timer_sync_mode3_freed[0] = 1;
                    timers[t].value = 0;
                    if (timer0_dotclock_num > 0)
                        timer0_dotclock_residue = 0;
                    /* First HBlank start in [from, to) */
                    uint64_t first_hblank;
                    if (phase_from < vis)
                        first_hblank = hblank_frame_start_cycle +
                                       (uint64_t)scan_from * cps + vis;
                    else
                        first_hblank = hblank_frame_start_cycle +
                                       (uint64_t)(scan_from + 1) * cps + vis;
                    elapsed = now - first_hblank;
                }
                else
                {
                    timers[t].last_sync_cycle = now;
                    return; /* Still paused */
                }
            }
            /* After freed: free run (elapsed unchanged) */
            break;
        }
    }

    /* ---- Sync mode pre-processing for Timer 1 (VBlank) ---- */
    if ((mode & 1) && t == 1)
    {
        uint32_t sync_mode = (mode >> 1) & 3;
        uint32_t cps = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL
                                             : CYCLES_PER_HBLANK_NTSC;
        uint32_t vblank_sl = psx_config.region_pal ? VBLANK_START_SCANLINE_PAL
                                                   : VBLANK_START_SCANLINE_NTSC;
        uint32_t scanlines = psx_config.region_pal ? SCANLINES_PER_FRAME_PAL
                                                   : SCANLINES_PER_FRAME;
        uint32_t cpf = scanlines * cps;
        uint64_t frame_offset = now - hblank_frame_start_cycle;
        uint32_t frame_phase = (uint32_t)(frame_offset % cpf);
        uint32_t vblank_start_cycle = vblank_sl * cps;
        int in_vblank = (frame_phase >= vblank_start_cycle);

        switch (sync_mode)
        {
        case 0: /* Pause during VBlanks */
            if (in_vblank)
            {
                timers[t].last_sync_cycle = now;
                return;
            }
            break;

        case 1: /* Reset at VBlanks — check if VBlank boundary crossed */
        {
            uint64_t from_frame = (timers[t].last_sync_cycle - hblank_frame_start_cycle);
            uint32_t from_phase = (uint32_t)(from_frame % cpf);
            int vblank_crossed = 0;
            if (elapsed >= cpf)
                vblank_crossed = 1;
            else if (from_phase < vblank_start_cycle && frame_phase >= vblank_start_cycle)
                vblank_crossed = 1;
            else if (frame_phase < from_phase) /* frame wrapped */
                vblank_crossed = 1;

            if (vblank_crossed)
            {
                timers[t].value = 0;
                /* Count from most recent VBlank start */
                uint64_t frames_elapsed = frame_offset / cpf;
                uint64_t vb_start = hblank_frame_start_cycle +
                                    frames_elapsed * cpf + vblank_start_cycle;
                if (vb_start > now)
                    vb_start -= cpf;
                elapsed = now - vb_start;
            }
            break;
        }

        case 2: /* Only count during VBlank */
            if (!in_vblank)
            {
                timers[t].value = 0;
                timers[t].last_sync_cycle = now;
                return;
            }
            break;

        case 3: /* Pause until first VBlank, then free run */
            if (!timer_sync_mode3_freed[1])
            {
                if (in_vblank || (now - timer_mode_set_cycle[1]) >= cpf)
                {
                    timer_sync_mode3_freed[1] = 1;
                    timers[t].value = 0;
                    timers[t].last_sync_cycle = now;
                }
                else
                {
                    timers[t].last_sync_cycle = now;
                    return; /* Still paused */
                }
            }
            break;
        }
    }

    /* ---- Tick computation ---- */
    uint32_t ticks;

    if (t == 0 && timer0_dotclock_num > 0)
    {
        uint64_t sub11 = elapsed * 11 + timer0_dotclock_residue;
        ticks = (uint32_t)(sub11 / timer0_dotclock_num);
        uint64_t consumed_sub11 = (uint64_t)ticks * timer0_dotclock_num;
        uint32_t remaining_sub11 = (uint32_t)(sub11 - consumed_sub11);
        timers[0].last_sync_cycle = now - remaining_sub11 / 11;
        timer0_dotclock_residue = remaining_sub11 % 11;
    }
    else
    {
        ticks = (uint32_t)(elapsed / divider);
        timers[t].last_sync_cycle += (uint64_t)ticks * divider;
    }
    if (ticks == 0)
        return;

    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t new_val = val + ticks;

    int wrapped_target = 0;
    int wrapped_overflow = 0;

    if ((mode & (1 << 3)) && target > 0)
    {
        /* PSX hardware reset-at-target: after the counter reaches the
         * target value, it resets to 0 with a 1-tick dead period where
         * the counter reads as 0 before incrementing.  Effective period
         * is target + 2 (not target + 1).  Internally value = target + 1
         * represents the dead-0 phase; external reads map it back to 0.
         * This matches PSX reference: value 0 appears 2x as often as
         * other values in the counter distribution test. */
        if (val < target && new_val >= target)
        {
            wrapped_target = 1;
            timers[t].mode |= (1 << 11);
        }
        else if (val == target + 1 && ticks > (uint32_t)target)
        {
            /* From dead-0 phase, counter wraps through target */
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
            uint32_t period = target + 2;
            new_val %= period;
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

    int can_fire = (mode & (1 << 6)) || !timer_irq_fired[t];

    if (wrapped_target && (mode & (1 << 4)) && can_fire)
    {
        SignalInterrupt(4 + t);
        timer_irq_fired[t] = 1;
    }

    if (wrapped_overflow && (mode & (1 << 5)) && can_fire)
    {
        SignalInterrupt(4 + t);
        timer_irq_fired[t] = 1;
    }
}

static void Timer_ScheduleOne(int t)
{
    if (t < 0 || t > 2)
        return;
    Timer_SyncValue(t);
    uint32_t mode = timers[t].mode;
    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t divider = timer_divider_cache[t];
    if (timer_stopped_cache[t])
        return;

    /* No scheduler event needed when no timer IRQ can fire.
     * The counter value syncs lazily via Timer_SyncValue on reads.
     * This prevents frequent timer events (every ~target cycles) from
     * starving the JIT with tiny cycle budgets. */
    int irq_can_fire = (mode & ((1 << 4) | (1 << 5))) &&
                       ((mode & (1 << 6)) || !timer_irq_fired[t]);
    if (!irq_can_fire)
    {
        Scheduler_RemoveEvent(SCHED_EVENT_TIMER0 + t);
        return;
    }

    if (divider == 0)
        divider = 1;

    uint32_t ticks_to_event = 0;
    if ((mode & (1 << 3)) && target > 0)
    {
        if (val <= target)
            ticks_to_event = (target + 1) - val;
        else
            ticks_to_event = (target + 2); /* dead-0: full period to next crossing */
    }
    else
    {
        uint32_t ticks_to_overflow = 0x10000 - val;
        ticks_to_event = ticks_to_overflow;
        if ((mode & (1 << 4)) && target > 0 && val <= target)
        {
            uint32_t ticks_to_target = (target + 1) - val;
            if (ticks_to_target < ticks_to_event)
                ticks_to_event = ticks_to_target;
        }
    }
    if (ticks_to_event == 0)
        ticks_to_event = 1;

    uint64_t deadline;
    if (t == 0 && timer0_dotclock_num > 0)
        deadline = EFFECTIVE_CYCLES + ((uint64_t)ticks_to_event * timer0_dotclock_num + 10) / 11;
    else
        deadline = EFFECTIVE_CYCLES + (uint64_t)ticks_to_event * divider;

    Scheduler_ScheduleEvent(SCHED_EVENT_TIMER0 + t, deadline, timer_callbacks[t]);
}

static void Timer_FireEvent(int t)
{
    if (t < 0 || t > 2)
        return;
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

static void Timer_Callback0(int ticks_late) { (void)ticks_late; Timer_FireEvent(0); }
static void Timer_Callback1(int ticks_late) { (void)ticks_late; Timer_FireEvent(1); }
static void Timer_Callback2(int ticks_late) { (void)ticks_late; Timer_FireEvent(2); }

uint32_t Timers_Read(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFF;
    int t = (phys - 0x1F801100) / 0x10;
    int reg = ((phys - 0x1F801100) % 0x10) / 4;
    if (t < 0 || t > 2)
        return 0;
    switch (reg)
    {
    case 0:
    {
        Timer_SyncValue(t);
        uint32_t v = timers[t].value;
        uint32_t tgt = timers[t].target & 0xFFFF;
        /* Map dead-0 phase (internal value = target+1) to 0 for reads */
        if ((timers[t].mode & (1 << 3)) && tgt > 0 && v > tgt)
            v = 0;
        return v & 0xFFFF;
    }
    case 1:
    {
        uint32_t mode = timers[t].mode;
        timers[t].mode &= ~((1 << 11) | (1 << 12));
        return mode;
    }
    case 2:
        return timers[t].target;
    }
    return 0;
}

void Timers_Write(uint32_t addr, uint32_t data)
{
    uint32_t phys = addr & 0x1FFFFFFF;
    int t = (phys - 0x1F801100) / 0x10;
    int reg = ((phys - 0x1F801100) % 0x10) / 4;
    if (t < 0 || t > 2)
        return;
    uint64_t now = EFFECTIVE_CYCLES;
    switch (reg)
    {
    case 0:
        timers[t].value = data;
        timers[t].last_sync_cycle = now;
        break;
    case 1:
        timers[t].value = 0;
        timers[t].mode = data & ~(0x1800);
        timers[t].last_sync_cycle = now;
        timer_mode_set_cycle[t] = now;
        timer_irq_fired[t] = 0; /* Reset one-shot gating on mode write */
        timer_sync_mode3_freed[t] = 0; /* Reset mode 3 one-shot flag */
        timer_update_stopped_cache(t);
        timer_update_divider_cache(t);
        break;
    case 2:
        timers[t].target = data;
        break;
    }
    Timer_ScheduleOne(t);
}

void Timer_RefreshDividerCache(void) { timer_update_divider_cache(0); }
void Timer_ScheduleAll(void)
{
    for (int t = 0; t < 3; t++)
        Timer_ScheduleOne(t);
}
