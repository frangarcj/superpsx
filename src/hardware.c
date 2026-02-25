#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <kernel.h>
#include "superpsx.h"
#include "scheduler.h"
#include "psx_dma.h"
#include "psx_timers.h"
#include "psx_sio.h"
#include "spu.h"
#include "gpu_state.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "HW"

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
}

uint32_t ReadHardware(uint32_t phys)
{
    /*
     * Two-level dispatch: switch on 256-byte block for O(1) coarse routing,
     * then fine-grained checks within each block.  The compiler generates a
     * compact jump table for the outer switch (32 entries max).
     */
    uint32_t off = phys - 0x1F801000;  /* 0x0000-0x1FFF */

    switch (off >> 8) {
    case 0x00: /* 0x1F801000-0x1F8010FF: memctrl, SIO, IRQ, DMA */
        if (phys < 0x1F801024)
            return mem_ctrl[(phys - 0x1F801000) >> 2];
        if (phys >= 0x1F801040 && phys <= 0x1F80105E)
            return SIO_Read(phys);
        if (phys == 0x1F801060)
            return ram_size;
        if (phys == 0x1F801070)
            return cpu.i_stat;
        if (phys == 0x1F801074)
            return cpu.i_mask;
        if (phys >= 0x1F801080)
            return DMA_Read(phys);
        return 0;

    case 0x01: /* 0x1F801100-0x1F8011FF: Timers */
        return Timers_Read(phys);

    case 0x08: /* 0x1F801800-0x1F8018FF: CDROM, GPU, MDEC */
        if (phys <= 0x1F801803) {
            uint32_t byte_val = CDROM_Read(phys) & 0xFF;
            return byte_val | (byte_val << 8) | (byte_val << 16) | (byte_val << 24);
        }
        if (phys == 0x1F801810)
        {
            uint32_t val = GPU_Read();
            return val;
        }
        if (phys == 0x1F801814)
            return GPU_ReadStatus();
        if (phys == 0x1F801820)
            return 0;
        if (phys == 0x1F801824)
            return 0x80040000;
        return 0;

    case 0x0C: /* 0x1F801C00-0x1F801CFF: SPU (low half) */
    case 0x0D: /* 0x1F801D00-0x1F801DFF: SPU (high half) */
    {
        uint32_t sreg = (phys - 0x1F801C00) >> 1;
        uint32_t lo = SPU_ReadReg(sreg);
        uint32_t hi = ((phys + 2) < 0x1F801E00) ? SPU_ReadReg(sreg + 1) : 0;
        return lo | (hi << 16);
    }

    case 0x10: case 0x11: case 0x12: case 0x13: /* Expansion 2 */
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        return 0xFFFFFFFF;

    default:
        return 0;
    }
}

void WriteHardware(uint32_t phys, uint32_t data, int size)
{
    uint32_t off = phys - 0x1F801000;

    switch (off >> 8) {
    case 0x00: /* 0x1F801000-0x1F8010FF: memctrl, SIO, IRQ, DMA */
        if (phys < 0x1F801024) {
            mem_ctrl[(phys - 0x1F801000) >> 2] = data;
            return;
        }
        if (phys >= 0x1F801040 && phys <= 0x1F80105E) {
            SIO_Write(phys, data);
            return;
        }
        if (phys == 0x1F801060) {
            ram_size = data;
            return;
        }
        if (phys == 0x1F801070) {
            cpu.i_stat &= data;
            /* Inline SIO IRQ check: if SIO IRQ was pending and bit 7 is now
             * cleared, fire the SIO interrupt immediately. */
            extern int sio_irq_pending;
            extern volatile uint64_t sio_irq_delay_cycle;
            if (sio_irq_pending && !(data & (1 << 7)))
            {
                sio_irq_pending = 0;
                sio_irq_delay_cycle = 0;
                SignalInterrupt(7);
            }
            return;
        }
        if (phys == 0x1F801074) {
            cpu.i_mask = data & 0xFFFF07FF;
            DLOG("I_MASK = %08X (VSync=%d CD=%d Timer0=%d Timer1=%d Timer2=%d)\n",
                 (unsigned)cpu.i_mask, (int)(cpu.i_mask & 1), (int)((cpu.i_mask >> 2) & 1),
                 (int)((cpu.i_mask >> 4) & 1), (int)((cpu.i_mask >> 5) & 1), (int)((cpu.i_mask >> 6) & 1));
            return;
        }
        if (phys >= 0x1F801080) {
            DMA_Write(phys, data);
            return;
        }
        return;

    case 0x01: /* 0x1F801100-0x1F8011FF: Timers */
        Timers_Write(phys, data);
        return;

    case 0x08: /* 0x1F801800-0x1F8018FF: CDROM, GPU, MDEC */
        if (phys <= 0x1F801803) {
            CDROM_Write(phys, data);
            return;
        }
        if (phys == 0x1F801810) {
            GPU_WriteGP0(data);
            return;
        }
        if (phys == 0x1F801814) {
            GPU_WriteGP1(data);
            return;
        }
        /* MDEC writes (0x1F801820, 0x1F801824) — stub */
        return;

    case 0x0C: /* 0x1F801C00-0x1F801CFF: SPU (low half) */
    case 0x0D: /* 0x1F801D00-0x1F801DFF: SPU (high half) */
    {
        uint32_t soff = (phys - 0x1F801C00) >> 1;
        SPU_WriteReg(soff, (uint16_t)data);
        /* For 32-bit word writes, always write the upper halfword too even if
         * it is zero (a zero upper halfword is a valid register value). */
        if (size == 4 && (phys + 2) < 0x1F801E00)
            SPU_WriteReg(soff + 1, (uint16_t)(data >> 16));
        return;
    }

    case 0x10: case 0x11: case 0x12: case 0x13: /* Expansion 2 */
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
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
