#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <kernel.h>
#include "superpsx.h"
#include "scheduler.h"
#include "joystick.h"
#include "spu.h"
#include "gpu_state.h"

#define LOG_TAG "HW"

/* Forward declarations for timer cache */
static uint32_t timer_divider_cache[3];
static void timer_update_divider_cache(int t);

/*
 * PSX Hardware Register Emulation
 *
 * Physical addresses 0x1F801000-0x1F802FFF
 */

/* Memory Control */
static uint32_t mem_ctrl[9];           /* 0x1F801000-0x1F801020 */
static uint32_t ram_size = 0x00000B88; /* 0x1F801060 */

/* Interrupt Controller */
/* Interrupt Controller */
/* i_stat and i_mask are now members of cpu (see include/superpsx.h) */

static int VBlankHandler(int cause)
{
    /* PSX VBlank IRQ0 is now fired by the cycle-accurate HBlank scheduler
     * callback (Sched_HBlank_Callback in dynarec.c) every 263 scanlines.
     * We still flush the GIF here at PS2 VBlank rate so the display keeps
     * updating at 60 Hz even when the emulated PSX CPU runs slower. */
    gpu_pending_vblank_flush = 1;
    (void)cause;
    return -1; /* Call next handler */
}

void Init_Interrupts(void)
{
    int vblank_irq = INTC_VBLANK_S;
    /* Use Start or End VBlank? S is start. */
    AddIntcHandler(vblank_irq, VBlankHandler, 0);
    EnableIntc(vblank_irq);
    printf("Native PS2 VBlank Interrupt enabled.\n");
}

/* Called by GPU when display resolution changes (GP1(08h)) */
void Timer0_RefreshDividerCache(void)
{
    timer_update_divider_cache(0);
}

void SignalInterrupt(uint32_t irq)
{
    if (irq > 10)
        return;
    /*
       Do NOT use printf here! Called from ISR.
       We verified it works.
    */
    cpu.i_stat |= (1 << irq);
}

/* CheckInterrupts is now inline in superpsx.h */

/* DMA Controller */
static uint32_t dma_dpcr = 0x07654321; /* DMA priority control */
static uint32_t dma_dicr = 0;
typedef struct
{
    uint32_t madr; /* Base address */
    uint32_t bcr;  /* Block control */
    uint32_t chcr; /* Channel control */
} DmaChannel;
static DmaChannel dma_channels[7];

/* Timers */
typedef struct
{
    uint32_t value;
    uint32_t mode;
    uint32_t target;
    uint64_t last_sync_cycle; /* global_cycles when value was last updated */
} PsxTimer;
static PsxTimer timers[3];

/* SIO (Joypad/Memcard) - PSX controller protocol state machine */
static uint32_t sio_data = 0xFF;       /* RX Data register */
static uint32_t sio_stat = 0x00000005; /* TX Ready 1+2 */
static uint16_t sio_mode = 0;
static uint16_t sio_ctrl = 0;
static uint16_t sio_baud = 0;
static int sio_tx_pending = 0; /* 1 = RX data available */

/* Controller protocol state machine */
static int sio_state = 0;       /* Current byte index in protocol */
static uint8_t sio_response[5]; /* Pre-built response buffer */
static int sio_selected = 0;    /* 1 = JOY SELECT is asserted */

/* SIO Serial Port (0x1F801050-0x1F80105E) */
static uint16_t serial_mode = 0;
static uint16_t serial_ctrl = 0;
static uint16_t serial_baud = 0;

/* Delayed IRQ7: the PSX BIOS kernel waits ~100 cycles after sending a byte
 * before acknowledging any old IRQ7 and then polling for the new one.
 * Firing IRQ7 immediately causes the ack to eat the pending bit.
 *
 * With DBL (direct block linking), global_cycles is frozen during block
 * chains, so a cycle-based delay doesn't work.  Instead we use an
 * IO-operation trigger: set a pending flag on SIO byte exchange, then
 * deliver IRQ7 when the BIOS writes to I_STAT (clearing old IRQs) or
 * reads I_STAT while the pending flag is armed. */
#define SIO_IRQ_DELAY 500
volatile uint64_t sio_irq_delay_cycle = 0;

/* Pending SIO IRQ7 — 0=none, 1=armed (waiting for BIOS ACK), 2=ready to fire */
static int sio_irq_pending = 0;

/* SPU — see spu.c */

/* Cache control */
static uint32_t cache_control = 0;

/* Forward declaration for timer interpolation in ReadHardware */
static uint32_t timer_clock_divider(int t);
static int timer_is_stopped(int t);
static void Timer_SyncValue(int t);

/* ---- Read ---- */
uint32_t ReadHardware(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
    {
        return mem_ctrl[(phys - 0x1F801000) >> 2];
    }

    /* SIO (Joypad/Memcard) */
    if (phys == 0x1F801040)
    {
        /* JOY_DATA - Return RX data from controller */
        uint32_t val = sio_data;
        sio_tx_pending = 0;
        return val;
    }
    if (phys == 0x1F801044)
    {
        /* JOY_STAT: bit0=TX Ready1, bit1=RX Not Empty, bit2=TX Ready2, bit7=/ACK, bit9=IRQ */
        uint32_t stat = 0x00000005; /* TX Ready 1 + 2 always set */
        if (sio_tx_pending)
            stat |= 0x02; /* RX Not Empty */
        /* Report /ACK low (active) during active transfer (bytes 0-3) */
        if (sio_selected && sio_state > 0 && sio_state < 5)
            stat |= 0x80; /* /ACK input level */
        /* Include the IRQ flag from sio_stat */
        stat |= (sio_stat & (1 << 9));
        return stat;
    }
    if (phys == 0x1F801048)
        return sio_mode & 0x003F; /* Only bits 0-5 are valid for JOY_MODE */
    if (phys == 0x1F80104A)
        return sio_ctrl;
    if (phys == 0x1F80104E)
        return sio_baud;

    /* SIO Serial Port */
    if (phys == 0x1F801050)
        return 0xFF; /* SIO_TX_DATA - empty */
    if (phys == 0x1F801054)
        return 0x00000005; /* SIO_STAT - TX Ready */
    if (phys == 0x1F801058)
        return serial_mode & 0xFF;
    if (phys == 0x1F80105A)
        return serial_ctrl;
    if (phys == 0x1F80105E)
        return serial_baud;

    /* RAM Size */
    if (phys == 0x1F801060)
        return ram_size;

    /* Interrupt Controller */
    if (phys == 0x1F801070)
        return cpu.i_stat;
    if (phys == 0x1F801074)
        return cpu.i_mask;

    /* DMA registers */
    if (phys >= 0x1F801080 && phys < 0x1F801100)
    {
        int ch = (phys - 0x1F801080) / 0x10;
        int reg = ((phys - 0x1F801080) % 0x10) / 4;
        if (ch < 7)
        {
            switch (reg)
            {
            case 0:
                return dma_channels[ch].madr;
            case 1:
                return dma_channels[ch].bcr;
            case 2:
                return dma_channels[ch].chcr & ~0x01000000; /* Clear busy */
            default:
                return 0;
            }
        }
    }
    if (phys == 0x1F8010F0)
        return dma_dpcr;
    if (phys == 0x1F8010F4)
    {
        /* DICR format:
         *   Bits 0-5:   R/W (unknown purpose)
         *   Bits 6-14:  Always 0 on read
         *   Bit 15:     Force IRQ (R/W)
         *   Bits 16-22: IRQ Enable per channel (R/W)
         *   Bit 23:     IRQ Master Enable (R/W)
         *   Bits 24-30: IRQ Flags per channel (R/W1C - write 1 to clear)
         *   Bit 31:     IRQ Master Flag (Read-only, computed)
         */
        uint32_t read_val = dma_dicr & 0x7F000000; /* flags bits 24-30 */
        read_val |= dma_dicr & 0x00FF803F;         /* enable, master en, force, bits 0-5 */
        /* Bit 31 = force || (master_en && (en & flg) != 0) */
        uint32_t force = (dma_dicr >> 15) & 1;
        uint32_t master_en = (dma_dicr >> 23) & 1;
        uint32_t en = (dma_dicr >> 16) & 0x7F;
        uint32_t flg = (dma_dicr >> 24) & 0x7F;
        if (force || (master_en && (en & flg)))
            read_val |= 0x80000000;
        return read_val;
    }

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130)
    {
        int t = (phys - 0x1F801100) / 0x10;
        int reg = ((phys - 0x1F801100) % 0x10) / 4;
        if (t < 3)
        {
            switch (reg)
            {
            case 0:
            {
                /* Sync counter to current cycle and return its value.
                 * Previous code interpolated with a second modulo here,
                 * causing off-by-one errors.  Now we just sync and read. */
                Timer_SyncValue(t);
                return timers[t].value & 0xFFFF;
            }
            case 1:
            {
                /* PSX hardware: reading mode clears bits 11-12 (reached-target/overflow flags) */
                uint32_t mode = timers[t].mode;
                timers[t].mode &= ~((1 << 11) | (1 << 12));
                return mode;
            }
            case 2:
                return timers[t].target;
            default:
                return 0;
            }
        }
    }

    /* CD-ROM (0x1F801800-0x1F801803) - byte-wide controller */
    if (phys >= 0x1F801800 && phys <= 0x1F801803)
    {
        uint32_t byte_val = CDROM_Read(phys) & 0xFF;
        /* Mirror byte to all 4 lanes for wider reads */
        return byte_val | (byte_val << 8) | (byte_val << 16) | (byte_val << 24);
    }

    /* GPU */
    if (phys == 0x1F801810)
        return GPU_Read();
    if (phys == 0x1F801814)
        return GPU_ReadStatus();

    /* MDEC */
    if (phys == 0x1F801820)
        return 0;
    if (phys == 0x1F801824)
        return 0x80040000; /* MDEC status: ready */

    /* SPU */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00)
    {
        return SPU_ReadReg((phys - 0x1F801C00) >> 1);
    }

    /* Expansion 2 - open bus returns all 1s when no device responds */
    if (phys >= 0x1F802000 && phys < 0x1F803000)
        return 0xFFFFFFFF;

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130)
        return cache_control;

    return 0;
}

/* ---- Timer clock divider for a given timer + mode ---- */
static uint32_t timer_clock_divider(int t)
{
    uint32_t src = (timers[t].mode >> 8) & 3;

    if (t == 0 && (src == 1 || src == 3))
    {
        /* Timer 0: Dot clock — divider depends on horizontal resolution.
         * disp_hres (bits 17-18 of gpu_stat) + disp_hres368 (bit 16)
         * encode the current display width set by GP1(08h). */
        if (disp_hres368)
            return DOTCLOCK_DIV_368;
        switch (disp_hres)
        {
        case 0:
            return DOTCLOCK_DIV_256;
        case 1:
            return DOTCLOCK_DIV_320;
        case 2:
            return DOTCLOCK_DIV_512;
        case 3:
            return DOTCLOCK_DIV_640;
        }
        return DOTCLOCK_DIV_320; /* fallback */
    }

    if (t == 1 && (src == 1 || src == 3))
        return CYCLES_PER_HBLANK; /* HBlank mode */
    if (t == 2 && (src == 2 || src == 3))
        return 8; /* Sysclk/8 */
    return 1;     /* Sysclk (default) */
}

/* ---- Calculate cycles until the next timer event and schedule it ---- */
static void Timer_Callback0(void);
static void Timer_Callback1(void);
static void Timer_Callback2(void);

static const sched_callback_t timer_callbacks[3] = {
    Timer_Callback0, Timer_Callback1, Timer_Callback2};

/* Sync timer value to current global_cycles (call before reading/modifying value) */
/* Cached stopped state per timer — updated only when mode is written */
static uint8_t timer_stopped_cache[3] = {0, 0, 0};
/* Cached clock divider per timer — updated on mode write or resolution change */
static uint32_t timer_divider_cache[3] = {1, 1, 1};

/* Recompute clock divider for timer t */
static void timer_update_divider_cache(int t)
{
    uint32_t src = (timers[t].mode >> 8) & 3;

    if (t == 0 && (src == 1 || src == 3))
    {
        if (disp_hres368)
            timer_divider_cache[0] = DOTCLOCK_DIV_368;
        else
            switch (disp_hres)
            {
            case 0:
                timer_divider_cache[0] = DOTCLOCK_DIV_256;
                break;
            case 1:
                timer_divider_cache[0] = DOTCLOCK_DIV_320;
                break;
            case 2:
                timer_divider_cache[0] = DOTCLOCK_DIV_512;
                break;
            case 3:
                timer_divider_cache[0] = DOTCLOCK_DIV_640;
                break;
            default:
                timer_divider_cache[0] = DOTCLOCK_DIV_320;
                break;
            }
        return;
    }
    if (t == 1 && (src == 1 || src == 3))
    {
        timer_divider_cache[1] = CYCLES_PER_HBLANK;
        return;
    }
    if (t == 2 && (src == 2 || src == 3))
    {
        timer_divider_cache[2] = 8;
        return;
    }
    timer_divider_cache[t] = 1;
}

/* Recompute stopped state for timer t from its mode register */
static void timer_update_stopped_cache(int t)
{
    uint32_t mode = timers[t].mode;
    if (!(mode & 1))
    {
        timer_stopped_cache[t] = 0;
        return;
    }
    uint32_t sync_mode = (mode >> 1) & 3;
    if (t == 0 && sync_mode == 2)
    {
        timer_stopped_cache[t] = 1;
        return;
    }
    if (t == 1 && (sync_mode == 2 || sync_mode == 3))
    {
        timer_stopped_cache[t] = 1;
        return;
    }
    if (t == 2 && (sync_mode == 0 || sync_mode == 3))
    {
        timer_stopped_cache[t] = 1;
        return;
    }
    timer_stopped_cache[t] = 0;
}

/* Check if a timer is stopped — uses cached result */
static inline int timer_is_stopped(int t)
{
    return timer_stopped_cache[t];
}

static void Timer_SyncValue(int t)
{
    /* If timer is stopped, don't advance */
    if (timer_is_stopped(t))
    {
        timers[t].last_sync_cycle = global_cycles;
        return;
    }

    uint32_t divider = timer_divider_cache[t];
    uint64_t elapsed = global_cycles - timers[t].last_sync_cycle;
    uint32_t ticks = (uint32_t)(elapsed / divider);
    /* Only consume the cycles that produced whole ticks — keep the
     * fractional remainder for next sync so we never lose sub-tick
     * precision (critical for HBlank / dotclock dividers). */
    timers[t].last_sync_cycle += (uint64_t)ticks * divider;

    if (ticks == 0)
        return;

    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t mode = timers[t].mode;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t new_val = val + ticks;

    /* Reset-on-target mode (bit 3): counter resets when reaching target.
     * Use pcsx-redux's subtraction approach instead of modulo to correctly
     * model the counter cycling through target repeatedly. */
    if ((mode & (1 << 3)) && target > 0)
    {
        if (new_val >= target)
        {
            timers[t].mode |= (1 << 11); /* reached target flag */
            uint32_t period = target + 1;
            /* In reset-on-target mode the counter cycles 0..target and
             * never actually reaches 0xFFFF unless target itself is
             * 0xFFFF.  Only set the overflow flag in that case. */
            if (target >= 0xFFFF && new_val >= 0x10000)
                timers[t].mode |= (1 << 12); /* overflow flag */
            new_val %= period;
        }
        timers[t].value = new_val;
    }
    else
    {
        /* Free-running: counter wraps at 0x10000 */
        if (target > 0 && new_val >= target)
            timers[t].mode |= (1 << 11); /* reached target flag */
        if (new_val >= 0x10000)
            timers[t].mode |= (1 << 12); /* overflow flag */
        timers[t].value = new_val & 0xFFFF;
    }
}

/* Schedule the next event for timer t based on its current value/target/mode */
static void Timer_ScheduleOne(int t)
{
    /* Sync counter to current cycle before scheduling */
    Timer_SyncValue(t);

    uint32_t mode = timers[t].mode;
    uint32_t val = timers[t].value & 0xFFFF;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t divider = timer_divider_cache[t];

    /* If timer is stopped, don't schedule any event */
    if (timer_is_stopped(t))
        return;

    /* Find how many timer ticks until next event.
     * Always schedule BOTH target and overflow events properly so that
     * the counter stays alive and flag bits get set on time. */
    uint32_t ticks_to_event;

    if ((mode & (1 << 3)) && target > 0)
    {
        /* Reset-on-target: schedule at target hit */
        if (val < target)
            ticks_to_event = target - val;
        else
            ticks_to_event = (target + 1) - val; /* at/past target, schedule next period */
    }
    else
    {
        /* Free-running: schedule at whichever comes first — target or overflow */
        uint32_t ticks_to_overflow = 0x10000 - val;
        ticks_to_event = ticks_to_overflow;

        /* If target IRQ enabled, check if target comes first */
        if ((mode & (1 << 4)) && target > 0 && val < target)
        {
            uint32_t ticks_to_target = target - val;
            if (ticks_to_target < ticks_to_event)
                ticks_to_event = ticks_to_target;
        }
    }

    if (ticks_to_event == 0)
        ticks_to_event = 1;

    /* Convert timer ticks to CPU cycles */
    uint64_t cycles_to_event = (uint64_t)ticks_to_event * divider;

    Scheduler_ScheduleEvent(SCHED_EVENT_TIMER0 + t,
                            global_cycles + cycles_to_event,
                            timer_callbacks[t]);
}

/* ---- Timer event callback: fire IRQs, update counter, reschedule ---- */
static void Timer_FireEvent(int t)
{
    /* Sync counter to current cycle first */
    Timer_SyncValue(t);

    uint32_t mode = timers[t].mode;
    uint32_t target = timers[t].target & 0xFFFF;
    uint32_t val = timers[t].value & 0xFFFF;

    /* Determine whether we hit target or overflow.
     * In reset-on-target mode (bit 3), the event was scheduled at target.
     * In free-running mode, it depends on what came first. */
    int hit_target = 0;
    if ((mode & (1 << 3)) && target > 0)
    {
        /* Reset-on-target: SyncValue already wrapped the counter.
         * We know this event was scheduled at the target boundary. */
        hit_target = 1;
    }
    else if (target > 0 && val >= target && val < 0x10000)
    {
        /* Free-running mode, value at/past target but not overflowed */
        hit_target = 1;
    }

    if (hit_target)
    {
        timers[t].mode |= (1 << 11); /* bit 11 = reached target */
        if (target >= 0xFFFF)
            timers[t].mode |= (1 << 12); /* also overflow if target is max */
        if (mode & (1 << 4))
            SignalInterrupt(4 + t);

        /* Reset counter if reset-on-target is enabled */
        if (mode & (1 << 3))
        {
            /* Counter was already wrapped by SyncValue; just ensure it's <= target */
            /* value is already correct from SyncValue */
        }
    }
    else
    {
        /* Overflow event */
        timers[t].mode |= (1 << 12); /* bit 12 = overflow */
        if (mode & (1 << 5))
            SignalInterrupt(4 + t);
        /* SyncValue already wrapped the value via & 0xFFFF */
    }

    timers[t].last_sync_cycle = global_cycles;

    /* Reschedule for next event */
    Timer_ScheduleOne(t);
}

static void Timer_Callback0(void) { Timer_FireEvent(0); }
static void Timer_Callback1(void) { Timer_FireEvent(1); }
static void Timer_Callback2(void) { Timer_FireEvent(2); }

/* ---- Schedule all 3 timers (called at startup) ---- */
void Timer_ScheduleAll(void)
{
    int t;
    for (t = 0; t < 3; t++)
        Timer_ScheduleOne(t);
}

/* ---- Legacy UpdateTimers (kept for backward compat, now a no-op) ---- */
/* Timers are now driven by the scheduler. This function is retained
 * so existing call sites don't break, but it does nothing. */
void UpdateTimers(uint32_t cycles)
{
    (void)cycles;
}

/* DMA Channel 3: CD-ROM → RAM */
static void CDROM_DMA3(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    /* BCR format for block mode: bits 0-15 = block size (words), bits 16-31 = block count */
    uint32_t block_size_words = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0)
        block_count = 1;
    if (block_size_words == 0)
        block_size_words = 1;

    uint32_t total_bytes = block_size_words * block_count * 4; /* words to bytes */

    uint32_t phys_addr = madr & 0x1FFFFC;
    if (phys_addr + total_bytes > PSX_RAM_SIZE)
    {
        DLOG("DMA3 Transfer would overflow RAM (addr=0x%08" PRIx32 ", size=%" PRIu32 ")\n",
             phys_addr, total_bytes);
        total_bytes = PSX_RAM_SIZE - phys_addr;
    }

    /* Copy from CD-ROM data FIFO to PSX RAM */
    CDROM_ReadDataFIFO(psx_ram + phys_addr, total_bytes);

    (void)chcr;
}

static void GPU_DMA6(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    // OTC - Clear Ordering Table (Reverse Clear)
    // Writes pointers to form a linked list from Top to Bottom

    uint32_t addr = madr & 0x1FFFFC;
    uint32_t length = bcr;
    if (length == 0)
        length = 0x10000; // 0 means max words? usually on PSX DMA

    // "Write value of MADR (current address - 4) to the current address."
    // "Decrement MADR in 4."
    // "Repeat BCR times."

    // DLOG("Starting DMA6 (OTC) Addr=%08X Count=%d\n", addr, length);

    while (length > 0)
    {
        uint32_t next_addr = (addr - 4) & 0x1FFFFC;

        // Write pointer to previous entry
        WriteWord(addr, next_addr);

        addr = next_addr;
        length--;
    }

    // "At the end, must write terminator 0xFFFFFF in the last processed address."
    // The last address we WROTE to was 'addr + 4'
    uint32_t last_written = (addr + 4) & 0x1FFFFC;
    WriteWord(last_written, 0xFFFFFF);

    // DLOG("DMA6 Complete. Terminator at %08X\n", last_written);
}

/* ---- Write ---- */
void WriteHardware(uint32_t addr, uint32_t data)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
    {
        mem_ctrl[(phys - 0x1F801000) >> 2] = data;
        return;
    }

    /* SIO (Joypad/Memcard) */
    if (phys == 0x1F801040)
    {
        /* JOY_DATA - TX byte: PSX controller protocol state machine */
        uint8_t tx = (uint8_t)(data & 0xFF);

        if (!sio_selected)
        {
            /* No device selected, respond with 0xFF (hi-z) */
            sio_data = 0xFF;
            sio_tx_pending = 1;
            return;
        }

        switch (sio_state)
        {
        case 0:
            /* Byte 0: Host sends 0x01 to select controller */
            if (tx == 0x01)
            {
                /* Snapshot the buttons now for this entire transfer */
                Joystick_GetPSXDigitalResponse(sio_response + 1);
                sio_response[0] = 0xFF; /* hi-z during address byte */
                /* response[1]=0x41  response[2]=0x5A  put button bytes after */
                /* Rearrange: [0]=0xFF [1]=0x41 [2]=0x5A [3]=btnLo [4]=btnHi */
                {
                    uint8_t id = sio_response[1]; /* 0x41 */
                    uint8_t lo = sio_response[2]; /* buttons low */
                    uint8_t hi = sio_response[3]; /* buttons high */
                    sio_response[1] = id;         /* 0x41 */
                    sio_response[2] = 0x5A;       /* data-start marker */
                    sio_response[3] = lo;
                    sio_response[4] = hi;
                }
                sio_data = sio_response[0]; /* 0xFF */
                sio_state = 1;
                sio_tx_pending = 1;
                /* Arm SIO IRQ7 — will fire when BIOS ACKs old IRQ via I_STAT */
                sio_irq_pending = 1;
                sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            }
            else
            {
                /* Not addressing controller - ignore */
                sio_data = 0xFF;
                sio_tx_pending = 1;
            }
            break;
        case 1:
            /* Byte 1: Host sends 0x42 (Read command) */
            sio_data = sio_response[1]; /* 0x41 = digital pad ID */
            sio_state = 2;
            sio_tx_pending = 1;
            sio_irq_pending = 1;
            sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            break;
        case 2:
            /* Byte 2: Host sends 0x00, controller responds 0x5A */
            sio_data = sio_response[2]; /* 0x5A */
            sio_state = 3;
            sio_tx_pending = 1;
            sio_irq_pending = 1;
            sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            break;
        case 3:
            /* Byte 3: Host sends 0x00, controller responds button low byte */
            sio_data = sio_response[3];
            sio_state = 4;
            sio_tx_pending = 1;
            sio_irq_pending = 1;
            sio_irq_delay_cycle = global_cycles + SIO_IRQ_DELAY;
            break;
        case 4:
            /* Byte 4: Host sends 0x00, controller responds button high byte.
             * This is the LAST byte — the real controller does NOT pulse /ACK
             * after it, so no IRQ7 should be generated. */
            sio_data = sio_response[4];
            sio_state = 5; /* transfer complete */
            sio_tx_pending = 1;
            /* No IRQ7 for last byte — /ACK is "more-data-request" */
            break;
        default:
            /* Beyond protocol length - return hi-z */
            sio_data = 0xFF;
            sio_tx_pending = 1;
            break;
        }
        return;
    }
    if (phys == 0x1F801048)
    {
        sio_mode = (uint16_t)(data & 0x003F); /* Only bits 0-5 valid */
        return;
    }
    if (phys == 0x1F80104A)
    {
        sio_ctrl = (uint16_t)data;
        if (data & 0x40)
        {
            /* Reset - clear all state including JOY_CTRL itself */
            sio_ctrl = 0;
            sio_mode = 0;
            sio_baud = 0;
            sio_tx_pending = 0;
            sio_state = 0;
            sio_selected = 0;
            sio_data = 0xFF;
            sio_irq_pending = 0; /* Cancel any pending SIO IRQ */
            sio_irq_delay_cycle = 0;
            return;
        }
        if (data & 0x10)
        {
            /* ACK - acknowledge interrupt */
            sio_stat &= ~(1 << 9); /* Clear IRQ flag */
            /* If a SIO IRQ7 is pending, DON'T fire SignalInterrupt here.
             * The BIOS sequence is: CTRL ACK → IREG = ~IRQ7 (clear) → poll IREG.
             * If we fire the IRQ now, the BIOS immediately clears it and the
             * poll loop never sees it.  Instead, leave sio_irq_pending=1 so
             * the I_STAT write handler (0x1F801070) re-fires the IRQ AFTER
             * the BIOS's clear.  We still abort the block chain so we return
             * to C dispatch promptly. */
            if (sio_irq_pending)
            {
                sio_irq_delay_cycle = 0;
                /* Force block chain abort — write to JOY_CTRL goes through
                 * WriteHalf (size=2), so emit_abort_check is present. */
                psx_abort_pc = cpu.current_pc + 4;
                cpu.block_aborted = 1;
            }
        }
        /* Bit 1: JOYn output select - directly controls /SEL line */
        if (data & 0x02)
        {
            if (!sio_selected)
            {
                /* Freshly asserted: reset protocol state */
                sio_state = 0;
            }
            sio_selected = 1;
        }
        else
        {
            sio_selected = 0;
            sio_state = 0;
            sio_irq_pending = 0; /* Cancel any pending SIO IRQ */
            sio_irq_delay_cycle = 0;
        }
        return;
    }
    if (phys == 0x1F80104E)
    {
        sio_baud = (uint16_t)data;
        return;
    }

    /* SIO Serial Port writes */
    if (phys == 0x1F801058)
    {
        serial_mode = (uint16_t)(data & 0xFF);
        return;
    }
    if (phys == 0x1F80105A)
    {
        serial_ctrl = (uint16_t)data;
        if (data & 0x40)
        {
            /* Reset serial port */
            serial_ctrl = 0;
            serial_mode = 0;
            serial_baud = 0;
        }
        return;
    }
    if (phys == 0x1F80105E)
    {
        serial_baud = (uint16_t)data;
        return;
    }

    /* RAM Size */
    if (phys == 0x1F801060)
    {
        ram_size = data;
        return;
    }

    /* Interrupt Controller */
    if (phys == 0x1F801070)
    {
        cpu.i_stat &= data; /* Write-to-acknowledge (AND with written value) */
        /* If SIO IRQ7 is armed and the BIOS just cleared bit 7, deliver the
         * IRQ NOW — after the AND, so bit 7 gets re-set.  This models the
         * real hardware timing where /ACK fires after the BIOS's clear. */
        if (sio_irq_pending && !(data & (1 << 7)))
        {
            sio_irq_pending = 0;
            sio_irq_delay_cycle = 0;
            SignalInterrupt(7);
        }
        return;
    }
    if (phys == 0x1F801074)
    {
        cpu.i_mask = data & 0xFFFF07FF; /* Bits 11-15 always 0; rest preserved */
        DLOG("I_MASK = %08X (VSync=%d CD=%d Timer0=%d Timer1=%d Timer2=%d)\n",
             (unsigned)cpu.i_mask, (int)(cpu.i_mask & 1), (int)((cpu.i_mask >> 2) & 1),
             (int)((cpu.i_mask >> 4) & 1), (int)((cpu.i_mask >> 5) & 1), (int)((cpu.i_mask >> 6) & 1));
        return;
    }

    /* DMA channel registers (0x1F801080-0x1F8010EF) */
    if (phys >= 0x1F801080 && phys < 0x1F8010F0)
    {
        int ch = (phys - 0x1F801080) / 0x10;
        int reg = ((phys - 0x1F801080) % 0x10) / 4;
        if (ch < 7)
        {
            switch (reg)
            {
            case 0:
                dma_channels[ch].madr = data & 0x00FFFFFF; /* 24-bit address */
                break;
            case 1:
                dma_channels[ch].bcr = data;
                break;
            case 2:
                dma_channels[ch].chcr = data;
                /* If starting a DMA transfer */
                if (data & 0x01000000)
                {
                    if (ch == 2)
                    {
                        /* GPU DMA */
                        GPU_DMA2(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    }
                    else if (ch == 3)
                    {
                        /* CD-ROM DMA */
                        CDROM_DMA3(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    }
                    else if (ch == 4)
                    {
                        /* SPU DMA */
                        SPU_DMA4(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    }
                    else if (ch == 6)
                    {
                        /* OTC DMA */
                        GPU_DMA6(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    }
                    dma_channels[ch].chcr &= ~0x01000000;

                    /* Handle DMA Interrupts */
                    /* DICR (0x1F8010F4) */
                    /* Bits 16-22: IM (Interrupt Mask/Enable) for Ch 0-6 */
                    /* Bits 23: Master Enable */
                    /* Bits 24-30: IP (Interrupt Pending) for Ch 0-6 */

                    /* Set IP bit only if this channel's IRQ is enabled (IM bit) */
                    if (dma_dicr & (1 << (16 + ch)))
                    {
                        dma_dicr |= (1 << (24 + ch));

                        /* Check for master IRQ generation */
                        if (dma_dicr & 0x00800000)
                        {
                            dma_dicr |= 0x80000000;
                            SignalInterrupt(3); /* DMA IRQ */
                        }
                    }
                }
                break;
            }
        }
        return;
    }
    if (phys == 0x1F8010F0)
    {
        dma_dpcr = data;
        return;
    }
    if (phys == 0x1F8010F4)
    {
        /* DICR write behavior:
         *   Bits 0-5:   R/W
         *   Bits 6-14:  Not used (ignored)
         *   Bit 15:     Force IRQ (R/W)
         *   Bits 16-22: IRQ Enable per channel (R/W)
         *   Bit 23:     IRQ Master Enable (R/W)
         *   Bits 24-30: Write-1-to-clear (acknowledge flags)
         *   Bit 31:     Read-only (ignored on write)
         */
        uint32_t rw_mask = 0x00FF803F;         /* bits 0-5, 15-23 */
        uint32_t ack_bits = data & 0x7F000000; /* bits 24-30 written as 1 clear the flag */
        dma_dicr = (data & rw_mask) | ((dma_dicr & 0x7F000000) & ~ack_bits);
        return;
    }

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130)
    {
        int t = (phys - 0x1F801100) / 0x10;
        int reg = ((phys - 0x1F801100) % 0x10) / 4;
        if (t < 3)
        {
            switch (reg)
            {
            case 0:
                timers[t].value = data;
                timers[t].last_sync_cycle = global_cycles;
                Timer_ScheduleOne(t); /* Reschedule on value change */
                break;
            case 1:
                /* PSX hardware: writing mode resets counter to 0 */
                timers[t].value = 0;
                timers[t].mode = data;
                timers[t].last_sync_cycle = global_cycles;
                timer_update_stopped_cache(t);
                timer_update_divider_cache(t);
                Timer_ScheduleOne(t); /* Reschedule on mode change */
                break;
            case 2:
                timers[t].target = data;
                Timer_ScheduleOne(t); /* Reschedule on target change */
                break;
            }
        }
        return;
    }

    /* CD-ROM (0x1F801800-0x1F801803) */
    if (phys >= 0x1F801800 && phys <= 0x1F801803)
    {
        CDROM_Write(phys, data);
        return;
    }

    /* GPU */
    if (phys == 0x1F801810)
    {
        /* GP0 command */
        GPU_WriteGP0(data);
        return;
    }
    if (phys == 0x1F801814)
    {
        /* GP1 command */
        GPU_WriteGP1(data);
        return;
    }

    /* MDEC */
    if (phys == 0x1F801820 || phys == 0x1F801824)
        return;

    /* SPU — registers are 16-bit; a 32-bit store must write both halves.
     * WriteHalf zero-extends data so (data >> 16) == 0 for half-word writes. */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00)
    {
        uint32_t off = (phys - 0x1F801C00) >> 1;
        SPU_WriteReg(off, (uint16_t)data);
        if ((data >> 16) && (phys + 2) < 0x1F801E00)
            SPU_WriteReg(off + 1, (uint16_t)(data >> 16));
        return;
    }

    /* Expansion 2 */
    if (phys >= 0x1F802000 && phys < 0x1F803000)
    {
        if (phys == 0x1F802002)
            printf("%c", (char)data);
        return;
    }

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130)
    {
        cache_control = data;
        return;
    }
}
