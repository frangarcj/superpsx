#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "scheduler.h"
#include "psx_dma.h"
#include "psx_timers.h"
#include "psx_sio.h"
#include "spu.h"
#include "gpu_state.h"
#include "mdec.h"
#include "config.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "HW"

/* PSX I/O register addresses */
#define PSX_MEM_CTRL_BASE  0x1F801000
#define PSX_MEM_CTRL_END   0x1F801024
#define PSX_SIO_BASE       0x1F801040
#define PSX_SIO_END        0x1F80105E
#define PSX_RAM_SIZE_REG   0x1F801060
#define PSX_I_STAT         0x1F801070
#define PSX_I_MASK         0x1F801074
#define PSX_DMA_BASE       0x1F801080
#define PSX_SPU_BASE       0x1F801C00
#define PSX_SPU_END        0x1F801E00
#define PSX_IRQ_VALID_MASK 0x7FF
#define PSX_I_MASK_VALID   0xFFFF07FF

/* Cap cycles_left to 0 when IRQ becomes pending during JIT block execution.
 * This mirrors DuckStation's downcount=0 pattern on IRQ assertion.
 * cycles_left_correction tracks the trimmed cycles so run_jit_chain stays accurate. */
static inline void cap_cycles_for_irq(void)
{
    int be = psx_block_exception;
    if (be && (int32_t)cpu.cycles_left > 0)
    {
        cpu.cycles_left_correction += (int32_t)cpu.cycles_left;
        cpu.cycles_left = 0;
    }
}

static inline uint32_t spu_register_offset(uint32_t phys)
{
    return (phys - PSX_SPU_BASE) >> 1;
}

/* Handle SIO IRQ acknowledgement when I_STAT bit 7 is cleared while
 * a deferred SIO IRQ is pending.  Fires the interrupt immediately.
 * Returns 1 if SIO IRQ was consumed (caller should early-return). */
static inline int handle_sio_irq_ack(uint32_t data)
{
    if (sio_irq_pending && !(data & (1 << 7)))
    {
        sio_irq_pending = 0;
        sio_irq_delay_cycle = 0;
        Sched_Remove(SCHED_EVENT_SIO_IRQ);
        SignalInterrupt(7);
        return 1;
    }
    return 0;
}

/* Memory Control */
static uint32_t mem_ctrl[9];           /* 0x1F801000-0x1F801020 */
static uint32_t ram_size = 0x00000B88; /* 0x1F801060 */

void Init_Interrupts(void)
{
    printf("PS2 VBlank Interrupt DISABLED (using cycle-accurate scheduler for PSX VBlank).\n");
}

/* Called by GPU when display resolution changes (GP1(08h)) */
void Timer0_RefreshDividerCache(void)
{
    Timer_RefreshDividerCache();
}

void SignalInterrupt(uint32_t irq)
{
    if (irq > 10)
        return;
    cpu.i_stat |= (1 << irq);
    cpu.irq_pending = (cpu.i_stat & cpu.i_mask & PSX_IRQ_VALID_MASK) != 0;
    cpu.irq_pending_fast = cpu.irq_pending & (cpu.cop0[PSX_COP0_SR] & 1);
    if (cpu.irq_pending)
    {
        sched_interrupt_chain = 1;
        cap_cycles_for_irq();
    }
}

uint32_t ReadHardware(uint32_t phys)
{
    /*
     * Two-level dispatch: switch on 256-byte block for O(1) coarse routing,
     * then fine-grained checks within each block.  The compiler generates a
     * compact jump table for the outer switch (32 entries max).
     */
    uint32_t off = phys - PSX_MEM_CTRL_BASE; /* 0x0000-0x1FFF */
    uint32_t result = 0;

    switch (off >> 8)
    {
    case 0x00: /* 0x1F801000-0x1F8010FF: memctrl, SIO, IRQ, DMA */
        if (phys == PSX_I_STAT)
            result = cpu.i_stat;
        else if (phys == PSX_I_MASK)
            result = cpu.i_mask;
        else if (phys < PSX_MEM_CTRL_END)
            result = mem_ctrl[(phys - PSX_MEM_CTRL_BASE) >> 2];
        else if (phys >= PSX_SIO_BASE && phys <= PSX_SIO_END)
            result = SIO_Read(phys);
        else if (phys == PSX_RAM_SIZE_REG)
            result = ram_size;
        else if (phys >= PSX_DMA_BASE)
            result = DMA_Read(phys);
        break;

    case 0x01: /* 0x1F801100-0x1F8011FF: Timers */
        result = Timers_Read(phys);
        break;

    case 0x08: /* 0x1F801800-0x1F8018FF: CDROM, GPU, MDEC */
        if (phys <= 0x1F801803)
        {
            uint32_t byte_val = CDROM_Read(phys) & 0xFF;
            result = byte_val | (byte_val << 8) | (byte_val << 16) | (byte_val << 24);
        }
        else if (phys == 0x1F801810)
            result = GPU_Read();
        else if (phys == 0x1F801814)
            result = GPU_ReadStatus();
        else if (phys == 0x1F801820)
            result = MDEC_ReadData();
        else if (phys == 0x1F801824)
            result = MDEC_ReadStatus();
        break;

    case 0x0C: /* 0x1F801C00-0x1F801CFF: SPU (low half) */
    case 0x0D: /* 0x1F801D00-0x1F801DFF: SPU (high half) */
    {
        uint32_t sreg = spu_register_offset(phys);
        uint32_t lo = SPU_ReadReg(sreg);
        uint32_t hi = ((phys + 2) < PSX_SPU_END) ? SPU_ReadReg(sreg + 1) : 0;
        result = lo | (hi << 16);
        break;
    }

    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: /* Expansion 2 */
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
        result = 0xFFFFFFFF;
        break;

    default:
        break;
    }

    /* PSX I/O bus penalty: hardware register reads are slower than RAM due
     * to bus wait states (COM_DELAY register).  Deduct from cpu.cycles_left
     * so the JIT's cycle tracking reflects the bus stall.  The mem_slow
     * trampoline reloads S2 from cpu.cycles_left after the C call.
     * Skip in interpreter mode: the interpreter manages global_cycles
     * directly and never initialises cpu.cycles_left, so decrementing it
     * would corrupt EFFECTIVE_CYCLES (which uses initial - current). */
    if (!psx_config.interpreter)
        cpu.cycles_left -= 1;

    return result;
}

void WriteHardware(uint32_t phys, uint32_t data, int size)
{
    uint32_t off = phys - PSX_MEM_CTRL_BASE;

    switch (off >> 8)
    {
    case 0x00: /* 0x1F801000-0x1F8010FF: memctrl, SIO, IRQ, DMA */
        if (phys < PSX_MEM_CTRL_END)
        {
            mem_ctrl[(phys - PSX_MEM_CTRL_BASE) >> 2] = data;
            return;
        }
        if (phys >= PSX_SIO_BASE && phys <= PSX_SIO_END)
        {
            SIO_Write(phys, data);
            return;
        }
        if (phys == PSX_RAM_SIZE_REG)
        {
            ram_size = data;
            return;
        }
        if (phys == PSX_I_STAT)
        {
            cpu.i_stat &= data;
            /* CD-ROM level-triggered re-assertion: if the game acknowledged
             * bit 2 but the CD-ROM IRQ condition is still active, re-set it
             * immediately (replaces per-loop polling). */
            if (cdrom_irq_active && !(cpu.i_stat & (1 << 2)))
                cpu.i_stat |= (1 << 2);
            if (handle_sio_irq_ack(data))
                return;           /* SignalInterrupt already updates irq_pending + irq_pending_fast */
            cpu.irq_pending = (cpu.i_stat & cpu.i_mask & PSX_IRQ_VALID_MASK) != 0;
            cpu.irq_pending_fast = cpu.irq_pending & (cpu.cop0[PSX_COP0_SR] & 1);
            if (cpu.irq_pending)
            {
                sched_interrupt_chain = 1;
                cap_cycles_for_irq();
            }
            return;
        }
        if (phys == PSX_I_MASK)
        {
            cpu.i_mask = data & PSX_I_MASK_VALID;
            cpu.irq_pending = (cpu.i_stat & cpu.i_mask & PSX_IRQ_VALID_MASK) != 0;
            cpu.irq_pending_fast = cpu.irq_pending & (cpu.cop0[PSX_COP0_SR] & 1);
            if (cpu.irq_pending)
            {
                sched_interrupt_chain = 1;
                cap_cycles_for_irq();
            }
            DLOG("I_MASK = %08X (VSync=%d CD=%d Timer0=%d Timer1=%d Timer2=%d)\n",
                 (unsigned)cpu.i_mask, (int)(cpu.i_mask & 1), (int)((cpu.i_mask >> 2) & 1),
                 (int)((cpu.i_mask >> 4) & 1), (int)((cpu.i_mask >> 5) & 1), (int)((cpu.i_mask >> 6) & 1));
            return;
        }
        if (phys >= PSX_DMA_BASE)
        {
#ifdef ENABLE_VRAM_DUMP
            if (phys >= 0x1F801090 && phys <= 0x1F80109F) {
                static int ch1_log = 0;
                if (ch1_log < 30) {
                    printf("[HW] DMA ch1 write: phys=%08X data=%08X size=%d\n",
                           (unsigned)phys, (unsigned)data, size);
                    ch1_log++;
                }
            }
#endif
            DMA_Write(phys, data);
            return;
        }
        return;

    case 0x01: /* 0x1F801100-0x1F8011FF: Timers */
        Timers_Write(phys, data);
        return;

    case 0x08: /* 0x1F801800-0x1F8018FF: CDROM, GPU, MDEC */
        if (phys <= 0x1F801803)
        {
            CDROM_Write(phys, data);
            return;
        }
        if (phys == 0x1F801810)
        {
            GPU_WriteGP0(data);
            return;
        }
        if (phys == 0x1F801814)
        {
            GPU_WriteGP1(data);
            return;
        }
        /* MDEC writes */
        if (phys == 0x1F801820) {
            MDEC_WriteCommand(data);
            return;
        }
        if (phys == 0x1F801824) {
            MDEC_WriteControl(data);
            return;
        }
        return;

    case 0x0C: /* 0x1F801C00-0x1F801CFF: SPU (low half) */
    case 0x0D: /* 0x1F801D00-0x1F801DFF: SPU (high half) */
    {
        uint32_t soff = spu_register_offset(phys);
        SPU_WriteReg(soff, (uint16_t)data);
        /* For 32-bit word writes, always write the upper halfword too even if
         * it is zero (a zero upper halfword is a valid register value). */
        if (size == 4 && (phys + 2) < PSX_SPU_END)
            SPU_WriteReg(soff + 1, (uint16_t)(data >> 16));
        return;
    }

    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: /* Expansion 2 */
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
        if (phys == 0x1F802002)
            printf("%c", (char)data);
        return;

    default:
        return;
    }
}

void UpdateTimers(uint32_t cycles)
{
    /* Driven by scheduler now */
    (void)cycles;
}
