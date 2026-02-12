#include "superpsx.h"
#include "superpsx.h"
#include <stdio.h>
#include <kernel.h>

/*
 * PSX Hardware Register Emulation
 *
 * Physical addresses 0x1F801000-0x1F802FFF
 */

/* Memory Control */
static u32 mem_ctrl[9];    /* 0x1F801000-0x1F801020 */
static u32 ram_size = 0x00000B88; /* 0x1F801060 */

/* Interrupt Controller */
/* Interrupt Controller */
static volatile u32 i_stat = 0;
static u32 i_mask = 0;

static int VBlankHandler(int cause) {
    SignalInterrupt(0); /* PSX IRQ0 = VBLANK */
    return -1; /* Call next handler */
}

void Init_Interrupts(void) {
    int vblank_irq = INTC_VBLANK_S; 
    /* Use Start or End VBlank? S is start. */
    AddIntcHandler(vblank_irq, VBlankHandler, 0);
    EnableIntc(vblank_irq);
    printf("Native PS2 VBlank Interrupt enabled.\n");
}

void SignalInterrupt(u32 irq) {
    i_stat |= (1 << irq);
}

int CheckInterrupts(void) {
    return (i_stat & i_mask);
}

/* DMA Controller */
static u32 dma_dpcr = 0x07654321; /* DMA priority control */
static u32 dma_dicr = 0;
typedef struct {
    u32 madr;  /* Base address */
    u32 bcr;   /* Block control */
    u32 chcr;  /* Channel control */
} DmaChannel;
static DmaChannel dma_channels[7];

/* Timers */
typedef struct {
    u32 value;
    u32 mode;
    u32 target;
} PsxTimer;
static PsxTimer timers[3];

/* GPU */
/*
 * GPUSTAT:
 * Bit 31: Generic interlace bit (toggles)
 * Bit 28: Ready to receive DMA (1=Ready)
 * Bit 27: Ready to send Data (1=Ready)
 * Bit 26: Ready to receive Cmd (1=Ready)
 * Bit 25: DMA / Data Request (1=Ready)
 * Bit 24: Interrupt Request (0=Ready)
 * Bit 23: Display Enable (0=My guess, 0=On?) - No wait, 1=Off
 * Bit 19: Vertical Resolution (0=240, 1=480)
 * Bit 17-18: Horizontal Resolution
 * Bit 29-30: DMA Direction
 */
static u32 gpu_stat = 0x14802000;
static u32 gpu_read = 0;
static u32 gpu_cmd_count = 0;

/* SPU */
static u16 spu_regs[512]; /* 0x1F801C00-0x1F801DFF */

/* Cache control */
static u32 cache_control = 0;

/* ---- Read ---- */
u32 ReadHardware(u32 addr) {
    u32 phys = addr & 0x1FFFFFFF;
    
    /* Log all hardware accesses for debugging */
    printf("[HW] Read 0x%08X (phys 0x%08X)\n", addr, phys);

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024) {
        return mem_ctrl[(phys - 0x1F801000) >> 2];
    }

    /* RAM Size */
    if (phys == 0x1F801060) return ram_size;

    /* Interrupt Controller */
    if (phys == 0x1F801070) return i_stat;
    if (phys == 0x1F801074) return i_mask;

    /* DMA registers */
    if (phys >= 0x1F801080 && phys < 0x1F801100) {
        int ch = (phys - 0x1F801080) / 0x10;
        int reg = ((phys - 0x1F801080) % 0x10) / 4;
        if (ch < 7) {
            switch (reg) {
                case 0: return dma_channels[ch].madr;
                case 1: return dma_channels[ch].bcr;
                case 2: return dma_channels[ch].chcr & ~0x01000000; /* Clear busy */
                default: return 0;
            }
        }
    }
    if (phys == 0x1F8010F0) return dma_dpcr;
    if (phys == 0x1F8010F4) return dma_dicr;

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130) {
        int t = (phys - 0x1F801100) / 0x10;
        int reg = ((phys - 0x1F801100) % 0x10) / 4;
        if (t < 3) {
            switch (reg) {
                case 0: return timers[t].value++;
                case 1: return timers[t].mode;
                case 2: return timers[t].target;
                default: return 0;
            }
        }
    }

    /* GPU */
    if (phys == 0x1F801810) return gpu_read;
    if (phys == 0x1F801814) {
        /* GPUSTAT: toggle bit 31 (interlace signal) on read */
        gpu_stat ^= 0x80000000;
        return gpu_stat;
    }

    /* MDEC */
    if (phys == 0x1F801820) return 0;
    if (phys == 0x1F801824) return 0x80040000; /* MDEC status: ready */

    /* SPU */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00) {
        int idx = (phys - 0x1F801C00) >> 1;
        return spu_regs[idx & 0x1FF];
    }

    /* Expansion 2 */
    if (phys >= 0x1F802000 && phys < 0x1F803000) return 0;

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130) return cache_control;

    return 0;
}

/* ---- Write ---- */
void WriteHardware(u32 addr, u32 data) {
    u32 phys = addr & 0x1FFFFFFF;

    /* Log all hardware accesses for debugging */
    printf("[HW] Write 0x%08X = 0x%08X\n", addr, data);

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024) {
        mem_ctrl[(phys - 0x1F801000) >> 2] = data;
        return;
    }

    /* RAM Size */
    if (phys == 0x1F801060) { ram_size = data; return; }

    /* Interrupt Controller */
    if (phys == 0x1F801070) { i_stat &= data; return; } /* Write 0 to ack */
    if (phys == 0x1F801074) { i_mask = data; return; }

    /* DMA registers */
    if (phys >= 0x1F801080 && phys < 0x1F801100) {
        int ch = (phys - 0x1F801080) / 0x10;
        int reg = ((phys - 0x1F801080) % 0x10) / 4;
        if (ch < 7) {
            switch (reg) {
                case 0: dma_channels[ch].madr = data; break;
                case 1: dma_channels[ch].bcr = data; break;
                case 2:
                    dma_channels[ch].chcr = data;
                    /* If starting a DMA transfer, just mark it done immediately */
                    if (data & 0x01000000) {
                        dma_channels[ch].chcr &= ~0x01000000;
                    }
                    break;
            }
        }
        return;
    }
    if (phys == 0x1F8010F0) { dma_dpcr = data; return; }
    if (phys == 0x1F8010F4) { dma_dicr = data; return; }

    /* Timers */
    if (phys >= 0x1F801100 && phys < 0x1F801130) {
        int t = (phys - 0x1F801100) / 0x10;
        int reg = ((phys - 0x1F801100) % 0x10) / 4;
        if (t < 3) {
            switch (reg) {
                case 0: timers[t].value = data; break;
                case 1: timers[t].mode = data; break;
                case 2: timers[t].target = data; break;
            }
        }
        return;
    }

    /* GPU */
    if (phys == 0x1F801810) {
        /* GP0 command */
        gpu_cmd_count++;
        return;
    }
    if (phys == 0x1F801814) {
        /* GP1 command */
        u32 cmd = (data >> 24) & 0xFF;
        switch (cmd) {
            case 0x00: /* Reset GPU */
                gpu_stat = 0x14802000;
                break;
            case 0x01: /* Reset command buffer */
                break;
            case 0x03: /* Display enable/disable */
                if (data & 1) gpu_stat |= 0x00800000;
                else gpu_stat &= ~0x00800000;
                break;
            case 0x04: /* DMA direction */
                gpu_stat = (gpu_stat & ~0x60000000) | ((data & 3) << 29);
                break;
            case 0x05: /* Start of display area */
                break;
            case 0x06: /* Horizontal display range */
                break;
            case 0x07: /* Vertical display range */
                break;
            case 0x08: /* Display mode */
                break;
            case 0x10: /* GPU info */
                gpu_read = 2; /* GPU version */
                break;
        }
        return;
    }

    /* MDEC */
    if (phys == 0x1F801820 || phys == 0x1F801824) return;

    /* SPU */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00) {
        int idx = (phys - 0x1F801C00) >> 1;
        spu_regs[idx & 0x1FF] = (u16)data;
        return;
    }

    /* Expansion 2 */
    if (phys >= 0x1F802000 && phys < 0x1F803000) return;

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130) {
        cache_control = data;
        return;
    }
}
