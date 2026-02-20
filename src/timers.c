#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "scheduler.h"
#include "psx_timers.h"
#include "gpu_state.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "TMR"

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
        if (disp_hres368) timer_divider_cache[0] = DOTCLOCK_DIV_368;
        else switch (disp_hres) {
            case 0: timer_divider_cache[0] = DOTCLOCK_DIV_256; break;
            case 1: timer_divider_cache[0] = DOTCLOCK_DIV_320; break;
            case 2: timer_divider_cache[0] = DOTCLOCK_DIV_512; break;
            case 3: timer_divider_cache[0] = DOTCLOCK_DIV_640; break;
            default: timer_divider_cache[0] = DOTCLOCK_DIV_320; break;
        }
        return;
    }
    if (t == 1 && (src == 1 || src == 3)) { timer_divider_cache[1] = CYCLES_PER_HBLANK; return; }
    if (t == 2 && (src == 2 || src == 3)) { timer_divider_cache[2] = 8; return; }
    timer_divider_cache[t] = 1;
}

static void timer_update_stopped_cache(int t)
{
    if (t < 0 || t > 2) return;
    uint32_t mode = timers[t].mode;
    if (!(mode & 1)) { timer_stopped_cache[t] = 0; return; }
    uint32_t sync_mode = (mode >> 1) & 3;
    if (t == 0 && sync_mode == 2) { timer_stopped_cache[t] = 1; return; }
    if (t == 1 && (sync_mode == 2 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    if (t == 2 && (sync_mode == 0 || sync_mode == 3)) { timer_stopped_cache[t] = 1; return; }
    timer_stopped_cache[t] = 0;
}

static void Timer_SyncValue(int t)
{
    if (t < 0 || t > 2) return;
    if (timer_stopped_cache[t]) { timers[t].last_sync_cycle = global_cycles; return; }
    uint32_t divider = timer_divider_cache[t];
    uint64_t elapsed = global_cycles - timers[t].last_sync_cycle;
    uint32_t ticks = (uint32_t)(elapsed / divider);
    timers[t].last_sync_cycle += (uint64_t)ticks * divider;
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
    Scheduler_ScheduleEvent(SCHED_EVENT_TIMER0 + t, global_cycles + (uint64_t)ticks_to_event * divider, timer_callbacks[t]);
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
        case 0: Timer_SyncValue(t); return timers[t].value & 0xFFFF;
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
    switch (reg) {
        case 0: timers[t].value = data; timers[t].last_sync_cycle = global_cycles; break;
        case 1: timers[t].value = 0; timers[t].mode = data; timers[t].last_sync_cycle = global_cycles;
                timer_update_stopped_cache(t); timer_update_divider_cache(t); break;
        case 2: timers[t].target = data; break;
    }
    Timer_ScheduleOne(t);
}

void Timer_RefreshDividerCache(void) { timer_update_divider_cache(0); }
void Timer_ScheduleAll(void) { for (int t = 0; t < 3; t++) Timer_ScheduleOne(t); }
