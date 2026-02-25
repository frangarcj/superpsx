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
#define EFFECTIVE_CYCLES  (global_cycles + chain_cycles_acc + partial_block_cycles)

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
    /* Timer0: mode 1 handled in SyncValue (reset at Hblank); mode 2 stopped;
     *         mode 3 initially stopped (handled lazily in SyncValue). */
    if (t == 0 && (sync_mode == 2 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    if (t == 1 && (sync_mode == 2 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    if (t == 2 && (sync_mode == 0 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    timer_stopped_cache[t] = 0;
}

static void Timer_SyncValue(int t)
{
    if (t < 0 || t > 2) return;
    uint64_t now = EFFECTIVE_CYCLES;

    /* ---- Timer0 sync modes (Hblank-related) ---- */
    if (t == 0 && (timers[0].mode & 1))
    {
        uint32_t sync_mode = (timers[0].mode >> 1) & 3;
        uint32_t hblank = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL : CYCLES_PER_HBLANK_NTSC;

        if (sync_mode == 1)
        {
            /* Reset counter to 0 at each Hblank.
             * Value = ticks within the current scanline. */
            uint64_t cif = now - hblank_frame_start_cycle;
            uint32_t cycle_in_scanline = (uint32_t)(cif % hblank);
            uint32_t ticks;
            if (timer0_dotclock_num > 0)
                ticks = (uint32_t)((uint64_t)cycle_in_scanline * 11 / timer0_dotclock_num);
            else
                ticks = cycle_in_scanline / timer_divider_cache[0];
            timers[0].value = ticks & 0xFFFF;
            timers[0].last_sync_cycle = now;
            return;
        }
        if (sync_mode == 3)
        {
            /* Pause until first Hblank after mode write, then free-run. */
            uint64_t set_cycle = timer_mode_set_cycle[0];

            /* If mode was set before current frame, Hblank has definitely occurred. */
            if (set_cycle < hblank_frame_start_cycle)
            {
                /* Timer should have started running at the first Hblank of
                 * the frame (or earlier).  Adjust last_sync_cycle once. */
                if (timers[0].last_sync_cycle < hblank_frame_start_cycle)
                    timers[0].last_sync_cycle = hblank_frame_start_cycle;
                timer_stopped_cache[0] = 0;
                /* Fall through to normal sync */
            }
            else
            {
                /* Mode was set within current frame — compute first Hblank. */
                uint64_t pos_in_scanline = (set_cycle - hblank_frame_start_cycle) % hblank;
                uint64_t cycles_to_hblank = hblank - pos_in_scanline;
                uint64_t first_hblank = set_cycle + cycles_to_hblank;

                if (now < first_hblank)
                {
                    /* Still paused, waiting for first Hblank. */
                    timers[0].last_sync_cycle = now;
                    return;
                }
                /* First Hblank occurred — start free-running from there. */
                if (timers[0].last_sync_cycle < first_hblank)
                    timers[0].last_sync_cycle = first_hblank;
                timer_stopped_cache[0] = 0;
                /* Fall through to normal sync */
            }
        }
        /* sync_mode 0 and 2: mode 0 treated as free-run (TODO: pause during
         * Hblank); mode 2 caught by stopped_cache above. */
    }

    /* ---- Timer1 sync modes (VBlank-related) ---- */
    if (t == 1 && (timers[1].mode & 1))
    {
        uint32_t sync_mode = (timers[1].mode >> 1) & 3;
        uint32_t hblank = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL : CYCLES_PER_HBLANK_NTSC;
        uint32_t vblank_start_sl = psx_config.region_pal ? VBLANK_START_SCANLINE_PAL : VBLANK_START_SCANLINE_NTSC;

        if (sync_mode == 1)
        {
            /* Reset counter to 0 at VBlank.
             * Value = ticks elapsed since frame start (= VBlank moment). */
            uint64_t elapsed = now - hblank_frame_start_cycle;
            uint32_t divider = timer_divider_cache[1];
            uint32_t ticks = (uint32_t)(elapsed / divider);
            timers[1].value = ticks & 0xFFFF;
            timers[1].last_sync_cycle = now;
            return;
        }
        if (sync_mode == 0)
        {
            /* Pause counter during VBlank.
             * Count ticks only during active video (scanlines 0..vblank_start-1). */
            uint64_t frame_pos = now - hblank_frame_start_cycle;
            uint64_t vblank_cycle = (uint64_t)vblank_start_sl * hblank;
            if (frame_pos >= vblank_cycle)
            {
                /* In VBlank — cap elapsed at VBlank boundary. */
                if (timers[1].last_sync_cycle < hblank_frame_start_cycle)
                    timers[1].last_sync_cycle = hblank_frame_start_cycle;
                uint64_t sync_pos = timers[1].last_sync_cycle - hblank_frame_start_cycle;
                uint64_t eff = (sync_pos < vblank_cycle) ? (vblank_cycle - sync_pos) : 0;
                uint32_t divider = timer_divider_cache[1];
                uint32_t ticks = (uint32_t)(eff / divider);
                timers[1].value = (timers[1].value + ticks) & 0xFFFF;
                timers[1].last_sync_cycle = now;
                return;
            }
            /* Active video — adjust last_sync if it was in previous VBlank. */
            if (timers[1].last_sync_cycle < hblank_frame_start_cycle)
                timers[1].last_sync_cycle = hblank_frame_start_cycle;
            /* Fall through to normal sync */
        }
        if (sync_mode == 3)
        {
            /* Pause until VBlank occurs once, then free-run. */
            uint64_t set_cycle = timer_mode_set_cycle[1];
            if (set_cycle < hblank_frame_start_cycle)
            {
                /* VBlank has occurred since mode was set. */
                if (timers[1].last_sync_cycle < hblank_frame_start_cycle)
                    timers[1].last_sync_cycle = hblank_frame_start_cycle;
                timer_stopped_cache[1] = 0;
            }
            else
            {
                /* Mode set in current frame — next VBlank at frame end. */
                uint32_t total_sl = psx_config.region_pal ? SCANLINES_PER_FRAME_PAL : SCANLINES_PER_FRAME;
                uint64_t next_vblank = hblank_frame_start_cycle + (uint64_t)total_sl * hblank;
                if (now < next_vblank)
                {
                    timers[1].last_sync_cycle = now;
                    return;
                }
                if (timers[1].last_sync_cycle < next_vblank)
                    timers[1].last_sync_cycle = next_vblank;
                timer_stopped_cache[1] = 0;
            }
        }
    }

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

    if ((mode & (1 << 3)) && target > 0)
    {
        if (new_val >= target)
        {
            timers[t].mode |= (1 << 11);
            if (target >= 0xFFFF && new_val >= 0x10000) timers[t].mode |= (1 << 12);
            new_val %= (target + 1);
        }
        timers[t].value = new_val;
    }
    else
    {
        if (target > 0 && new_val >= target) timers[t].mode |= (1 << 11);
        if (new_val >= 0x10000) timers[t].mode |= (1 << 12);
        timers[t].value = new_val & 0xFFFF;
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
    uint32_t ticks_to_event;
    if ((mode & (1 << 3)) && target > 0)
    {
        if (val < target) ticks_to_event = target - val;
        else ticks_to_event = (target + 1) - val;
    }
    else
    {
        uint32_t ticks_to_overflow = 0x10000 - val;
        ticks_to_event = ticks_to_overflow;
        if ((mode & (1 << 4)) && target > 0 && val < target)
        {
            uint32_t ticks_to_target = target - val;
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
    Timer_SyncValue(t);
    uint32_t mode = timers[t].mode;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t val = timers[t].value & 0xFFFF;
    int hit_target = 0;
    if ((mode & (1 << 3)) && target > 0) hit_target = 1;
    else if (target > 0 && val >= target && val < 0x10000) hit_target = 1;

    if (hit_target)
    {
        timers[t].mode |= (1 << 11);
        if (target >= 0xFFFF) timers[t].mode |= (1 << 12);
        if (mode & (1 << 4)) SignalInterrupt(4 + t);
    }
    else
    {
        timers[t].mode |= (1 << 12);
        if (mode & (1 << 5)) SignalInterrupt(4 + t);
    }
    timers[t].last_sync_cycle = global_cycles;
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
        case 1: timers[t].value = 0; timers[t].mode = data; timers[t].last_sync_cycle = now;
                timer_mode_set_cycle[t] = now;
                timer_update_stopped_cache(t); timer_update_divider_cache(t); break;
        case 2: timers[t].target = data; break;
    }
    Timer_ScheduleOne(t);
}

void Timer_RefreshDividerCache(void) { timer_update_divider_cache(0); }
void Timer_ScheduleAll(void) { for (int t = 0; t < 3; t++) Timer_ScheduleOne(t); }
