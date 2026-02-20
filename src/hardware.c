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

/* Cache control */
static uint32_t cache_control = 0;

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

uint32_t ReadHardware(uint32_t addr)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
        return mem_ctrl[(phys - 0x1F801000) >> 2];

    /* SIO (Joypad/Memcard) and Serial */
    if (phys >= 0x1F801040 && phys <= 0x1F80105E)
        return SIO_Read(phys);

    /* RAM Size */
    if (phys == 0x1F801060)
        return ram_size;

    /* Interrupt Controller */
    if (phys == 0x1F801070)
        return cpu.i_stat;
    if (phys == 0x1F801074)
        return cpu.i_mask;

    /* DMA registers */
    if (phys >= 0x1F801080 && phys <= 0x1F8010F4)
        return DMA_Read(phys);

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130)
        return Timers_Read(phys);

    /* CD-ROM */
    if (phys >= 0x1F801800 && phys <= 0x1F801803)
    {
        uint32_t byte_val = CDROM_Read(phys) & 0xFF;
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
        return 0x80040000;

    /* SPU */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00)
        return SPU_ReadReg((phys - 0x1F801C00) >> 1);

    /* Expansion 2 */
    if (phys >= 0x1F802000 && phys < 0x1F803000)
        return 0xFFFFFFFF;

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130)
        return cache_control;

    return 0;
}

void WriteHardware(uint32_t addr, uint32_t data)
{
    uint32_t phys = addr & 0x1FFFFFFF;

    /* SIO and Serial Port */
    if (phys >= 0x1F801040 && phys <= 0x1F80105E)
    {
        SIO_Write(phys, data);
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
        cpu.i_stat &= data;
        /* SIO IRQ handling based on I_STAT write */
        extern void SIO_CheckIRQ(uint32_t data);
        SIO_CheckIRQ(data); 
        return;
    }
    if (phys == 0x1F801074)
    {
        cpu.i_mask = data & 0xFFFF07FF;
        DLOG("I_MASK = %08X (VSync=%d CD=%d Timer0=%d Timer1=%d Timer2=%d)\n",
             (unsigned)cpu.i_mask, (int)(cpu.i_mask & 1), (int)((cpu.i_mask >> 2) & 1),
             (int)((cpu.i_mask >> 4) & 1), (int)((cpu.i_mask >> 5) & 1), (int)((cpu.i_mask >> 6) & 1));
        return;
    }

    /* DMA channel registers */
    if (phys >= 0x1F801080 && phys <= 0x1F8010F4)
    {
        DMA_Write(phys, data);
        return;
    }

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130)
    {
        Timers_Write(phys, data);
        return;
    }

    /* CD-ROM */
    if (phys >= 0x1F801800 && phys <= 0x1F801803)
    {
        CDROM_Write(phys, data);
        return;
    }

    /* GPU */
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

    /* MDEC */
    if (phys == 0x1F801820 || phys == 0x1F801824)
        return;

    /* SPU */
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

void UpdateTimers(uint32_t cycles)
{
    /* Driven by scheduler now */
    (void)cycles;
}
