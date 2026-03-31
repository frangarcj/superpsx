#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "scheduler.h"
#include "joystick.h"
#include "psx_sio.h"
#include "memorycard.h"
#include "profiler.h"

#define LOG_TAG "SIO"

/* Joypad/Memcard interface — non-static for JIT inline fast paths */
uint32_t sio_data = 0xFF;       /* RX Data register */
uint32_t sio_stat = 0x00000005; /* TX Ready 1+2 */
static uint16_t sio_mode = 0;
static uint16_t sio_ctrl = 0;
static uint16_t sio_baud = 0;
int sio_tx_pending = 0; /* 1 = RX data available */

/* Controller protocol state machine — partially exposed for JIT */
int sio_state = 0;                /* Current byte index in protocol */
uint32_t sio_deferred_vblank = 0; /* VBlank deferred during MCD data xfer */

/* Flush any deferred VBlank when leaving MCD data transfer phase */
static inline void sio_flush_deferred_vblank(void)
{
    if (sio_deferred_vblank)
    {
        cpu.i_stat |= 1; /* VBlank bit 0 */
        cpu.irq_pending = (cpu.i_stat & cpu.i_mask & 0x7FF) != 0;
        if (cpu.irq_pending)
            sched_interrupt_chain = 1;
        sio_deferred_vblank = 0;
    }
}

static uint8_t sio_response[20]; /* Pre-built response buffer */
int sio_response_len = 0;        /* Number of valid bytes in sio_response */
int sio_selected = 0;            /* 1 = JOY SELECT is asserted */
static int sio_port = 0;         /* 0 = PSX port 1, 1 = PSX port 2 */
static int sio_device = 0;       /* 0=none, 1=pad, 2=memcard */

/* SIO Serial Port (0x1F801050-0x1F80105E) */
static uint16_t serial_mode = 0;
static uint16_t serial_ctrl = 0;
static uint16_t serial_baud = 0;

#define SIO_IRQ_DELAY 500      /* Pad IRQ delay */
#define SIO_MCD_IRQ_DELAY 1500 /* Memcard IRQ delay (longer — BIOS needs time) */
volatile uint64_t sio_irq_delay_cycle = 0;
int sio_irq_pending = 0;
int sio_ack_latch = 0; /* 1 = ACK pulse ready, consumed on STAT read */

/* SIO trace for debugging memory card protocol — disabled for performance */
#define SIO_TRACE(fmt, ...) \
    do                      \
    {                       \
    } while (0)

/* ---- Scheduler-driven SIO IRQ delay ---- */
static void Sched_SIO_IRQ_Callback(int ticks_late)
{
    (void)ticks_late;
    SIO_TRACE("IRQ CB fired cy=%llu pending=%d dev=%d state=%d\n",
              (unsigned long long)global_cycles, sio_irq_pending, sio_device, sio_state);
    sio_irq_delay_cycle = 0;
    sio_irq_pending = 0;  /* IRQ delivered — no longer pending */
    sio_stat |= (1 << 9); /* Latch IRQ flag — BIOS checks this to confirm SIO source */
    SignalInterrupt(7);
}

static inline void sio_schedule_irq(void)
{
    uint64_t deadline = global_cycles + partial_block_cycles + SIO_IRQ_DELAY;
    sio_irq_delay_cycle = deadline;
    Sched_Add(SCHED_EVENT_SIO_IRQ, deadline, Sched_SIO_IRQ_Callback);
}

/* Assert ACK: set latch + schedule IRQ with device-appropriate delay.
 * Also caps cpu.cycles_left so the JIT block exits before the IRQ
 * deadline, ensuring timely interrupt delivery. */
static inline void sio_assert_ack(void)
{
    sio_ack_latch = 1;
    int32_t delay;
    if (sio_device == 2)
    {
        /* Memcard: longer delay, NO sio_irq_pending (no block_aborted) */
        delay = SIO_MCD_IRQ_DELAY;
        uint64_t deadline = global_cycles + partial_block_cycles + delay;
        SIO_TRACE("ACK MCD: gc=%llu pbc=%u delay=%u -> deadline=%llu cached_earliest=%llu\n",
                  (unsigned long long)global_cycles, partial_block_cycles, SIO_MCD_IRQ_DELAY,
                  (unsigned long long)deadline, (unsigned long long)sched_cached_earliest);
        sio_irq_delay_cycle = deadline;
        Sched_Add(SCHED_EVENT_SIO_IRQ, deadline, Sched_SIO_IRQ_Callback);
    }
    else
    {
        /* Pad: shorter delay + irq_pending for ack4 abort */
        delay = SIO_IRQ_DELAY;
        sio_irq_pending = 1;
        sio_schedule_irq();
    }
    /* Cap remaining block cycles so the JIT returns to the outer
     * loop before the scheduled SIO IRQ deadline. */
    int32_t target = delay + 200;
    if ((int32_t)cpu.cycles_left > target)
    {
        cpu.cycles_left_correction += ((int32_t)cpu.cycles_left - target);
        cpu.cycles_left = (uint32_t)target;
    }
}

static inline void sio_cancel_irq(void)
{
    sio_irq_delay_cycle = 0;
    Sched_Remove(SCHED_EVENT_SIO_IRQ);
}

static inline uint32_t SIO_Read_Inner(uint32_t phys)
{
    switch (phys - 0x1F801040)
    {
    case 0x00: /* SIO_DATA */
    {
        uint32_t val = sio_data;
        sio_tx_pending = 0;
        sio_data = 0xFF; /* Consumed — second read returns 0xFF */
        SIO_TRACE("RD DATA=0x%02X state=%d dev=%d cy=%llu\n", val, sio_state, sio_device, (unsigned long long)global_cycles);
        return val;
    }
    case 0x04: /* SIO_STAT */
    {
        uint32_t stat = 0x00000005;
        if (sio_tx_pending)
            stat |= 0x02;
        if (sio_selected && sio_state > 0)
        {
            if (sio_device == 2)
            {
                /* Memcard: ACK latch model (consume on read) */
                if (sio_ack_latch)
                {
                    stat |= 0x80;
                    sio_ack_latch = 0;
                }
            }
            else if (sio_state < sio_response_len)
            {
                /* Pad: ACK while more response bytes remain */
                stat |= 0x80;
            }
        }
        stat |= (sio_stat & (1 << 9));
        return stat;
    }
    case 0x08: /* SIO_MODE */
        return sio_mode & 0x003F;
    case 0x0A: /* SIO_CTRL */
        return sio_ctrl;
    case 0x0E: /* SIO_BAUD */
        return sio_baud;
    case 0x10: /* Serial DATA */
        return 0xFF;
    case 0x14: /* Serial STAT */
        return 0x00000005;
    case 0x18: /* Serial MODE */
        return serial_mode & 0xFF;
    case 0x1A: /* Serial CTRL */
        return serial_ctrl;
    case 0x1E: /* Serial BAUD */
        return serial_baud;
    default:
        return 0;
    }
}

uint32_t SIO_Read(uint32_t phys) /* caller passes physical addr */
{
    PROF_PUSH(PROF_SIO);
    uint32_t result = SIO_Read_Inner(phys);
    PROF_POP(PROF_SIO);
    return result;
}

static inline void SIO_Write_Inner(uint32_t phys, uint32_t data)
{
    switch (phys - 0x1F801040)
    {
    case 0x00: /* 0x1F801040: SIO_DATA */
    {
        uint8_t tx = (uint8_t)(data & 0xFF);
        SIO_TRACE("WR TX=0x%02X state=%d dev=%d sel=%d cy=%llu pbc=%u cl=%d icl=%d corr=%d\n",
                  tx, sio_state, sio_device, sio_selected,
                  (unsigned long long)global_cycles, partial_block_cycles,
                  (int)cpu.cycles_left, (int)cpu.initial_cycles_left,
                  (int)cpu.cycles_left_correction);
        if (!sio_selected)
        {
            sio_data = 0xFF;
            sio_tx_pending = 1;
            return;
        }

        if (sio_state == 0)
        {
            if (tx == 0x01)
            {
                sio_device = 1; /* pad */
                if (Joystick_HasMultitap(sio_port))
                {
                    int slot;
                    sio_response[0] = 0xFF;
                    sio_response[1] = 0x80;
                    sio_response[2] = 0x5A;
                    for (slot = 0; slot < 4; slot++)
                    {
                        int base = 3 + slot * 4;
                        if (Joystick_IsConnected(sio_port, slot))
                        {
                            uint8_t pad[3];
                            Joystick_GetPSXDigitalResponse(sio_port, slot, pad);
                            sio_response[base] = pad[0];
                            sio_response[base + 1] = 0x5A;
                            sio_response[base + 2] = pad[1];
                            sio_response[base + 3] = pad[2];
                        }
                        else
                        {
                            sio_response[base] = 0xFF;
                            sio_response[base + 1] = 0xFF;
                            sio_response[base + 2] = 0xFF;
                            sio_response[base + 3] = 0xFF;
                        }
                    }
                    sio_response_len = 19;
                }
                else
                {
                    uint8_t pad[3];
                    Joystick_GetPSXDigitalResponse(sio_port, 0, pad);
                    sio_response[0] = 0xFF;
                    sio_response[1] = pad[0];
                    sio_response[2] = 0x5A;
                    sio_response[3] = pad[1];
                    sio_response[4] = pad[2];
                    sio_response_len = 5;
                }
                sio_data = sio_response[0];
                sio_state = 1;
                sio_tx_pending = 1;
                sio_assert_ack();
            }
            else if (tx == 0x81)
            {
                /* Memory card access — always reset card to IDLE for a
                 * fresh exchange.  The BIOS sometimes starts a new
                 * exchange without fully completing the previous one. */
                sio_device = 2;
                MCD_Reset(sio_port);
                uint8_t rx = MCD_Tick(sio_port, tx);
                sio_data = rx;
                sio_state = 1;
                sio_tx_pending = 1;
                if (!MCD_IsIdle(sio_port))
                {
                    sio_assert_ack();
                }
            }
            else
            {
                sio_data = 0xFF;
                sio_tx_pending = 1;
            }
        }
        else if (sio_device == 2)
        {
            /* Memcard: route byte to card state machine */
            uint8_t rx = MCD_Tick(sio_port, tx);
            sio_data = rx;
            sio_tx_pending = 1;
            sio_state++;
            SIO_TRACE("MCD TICK rx=0x%02X state=%d idle=%d\n", rx, sio_state, MCD_IsIdle(sio_port));
            if (!MCD_IsIdle(sio_port))
            {
                sio_assert_ack();
            }
            else
            {
                int target = 200;
                if ((int32_t)cpu.cycles_left > target)
                {
                    cpu.cycles_left_correction += ((int32_t)cpu.cycles_left - target);
                    cpu.cycles_left = target;
                }
            }
        }
        else if (sio_state < sio_response_len)
        {
            sio_data = sio_response[sio_state];
            sio_tx_pending = 1;
            if (sio_state < sio_response_len - 1)
            {
                sio_assert_ack();
            }
            sio_state++;
        }
        else
        {
            sio_data = 0xFF;
            sio_tx_pending = 1;
        }
        return;
    }
    case 0x08: /* 0x1F801048: SIO_MODE */
        sio_mode = (uint16_t)(data & 0x003F);
        return;
    case 0x0A: /* 0x1F80104A: SIO_CTRL */
    {
        sio_ctrl = (uint16_t)data;
        if (data & 0x40)
        {
            sio_ctrl = 0;
            sio_mode = 0;
            sio_baud = 0;
            sio_tx_pending = 0;
            sio_flush_deferred_vblank();
            sio_state = 0;
            sio_response_len = 0;
            sio_selected = 0;
            sio_port = 0;
            sio_device = 0;
            sio_data = 0xFF;
            sio_irq_pending = 0;
            sio_ack_latch = 0;
            sio_cancel_irq();
            return;
        }
        if (data & 0x10)
        {
            sio_stat &= ~(1 << 9);
            if (sio_irq_pending)
            {
                sio_cancel_irq();
                psx_abort_pc = cpu.current_pc + 4;
                cpu.block_aborted = 1;
            }
        }
        int old_port = sio_port;
        sio_port = (data >> 13) & 1;
        if (data & 0x02)
        {
            if (!sio_selected || sio_port != old_port)
            {
                /* Fresh select OR port changed → reset exchange state */
                if (sio_selected && sio_device == 2 && sio_port != old_port)
                    MCD_Reset(old_port);
                sio_flush_deferred_vblank();
                sio_state = 0;
                sio_device = 0;
                sio_ack_latch = 0;
            }
            sio_selected = 1;
            SIO_TRACE("CTRL SELECT port=%d\n", sio_port);
        }
        else
        {
            if (sio_selected && sio_device == 2)
                MCD_Reset(sio_port);
            sio_selected = 0;
            SIO_TRACE("CTRL DESELECT port=%d\n", sio_port);
            sio_flush_deferred_vblank();
            sio_state = 0;
            sio_device = 0;
            sio_irq_pending = 0;
            sio_ack_latch = 0;
            sio_cancel_irq();
        }
        return;
    }
    case 0x0E: /* 0x1F80104E: SIO_BAUD */
        sio_baud = (uint16_t)data;
        return;
    case 0x18: /* 0x1F801058: Serial MODE */
        serial_mode = (uint16_t)(data & 0xFF);
        return;
    case 0x1A: /* 0x1F80105A: Serial CTRL */
        serial_ctrl = (uint16_t)data;
        if (data & 0x40)
        {
            serial_ctrl = 0;
            serial_mode = 0;
            serial_baud = 0;
        }
        return;
    case 0x1E: /* 0x1F80105E: Serial BAUD */
        serial_baud = (uint16_t)data;
        return;
    }
}

void SIO_Write(uint32_t phys, uint32_t data) /* caller passes physical addr */
{
    PROF_PUSH(PROF_SIO);
    SIO_Write_Inner(phys, data);
    PROF_POP(PROF_SIO);
}
