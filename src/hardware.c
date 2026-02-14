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
static u32 mem_ctrl[9];           /* 0x1F801000-0x1F801020 */
static u32 ram_size = 0x00000B88; /* 0x1F801060 */

/* Interrupt Controller */
/* Interrupt Controller */
static volatile u32 i_stat = 0;
static u32 i_mask = 0;

static int VBlankHandler(int cause)
{
    GPU_VBlank();
    SignalInterrupt(0); /* PSX IRQ0 = VBLANK */
    return -1;          /* Call next handler */
}

void Init_Interrupts(void)
{
    int vblank_irq = INTC_VBLANK_S;
    /* Use Start or End VBlank? S is start. */
    AddIntcHandler(vblank_irq, VBlankHandler, 0);
    EnableIntc(vblank_irq);
    printf("Native PS2 VBlank Interrupt enabled.\n");
}

void SignalInterrupt(u32 irq)
{
    if (irq > 10)
        return;
    /*
       Do NOT use printf here! Called from ISR.
       We verified it works.
    */
    i_stat |= (1 << irq);
}

int CheckInterrupts(void)
{
    return (i_stat & i_mask);
}

/* DMA Controller */
static u32 dma_dpcr = 0x07654321; /* DMA priority control */
static u32 dma_dicr = 0;
typedef struct
{
    u32 madr; /* Base address */
    u32 bcr;  /* Block control */
    u32 chcr; /* Channel control */
} DmaChannel;
static DmaChannel dma_channels[7];

/* Timers */
typedef struct
{
    u32 value;
    u32 mode;
    u32 target;
} PsxTimer;
static PsxTimer timers[3];

/* SIO (Joypad/Memcard) - Minimal emulation for "no controller" */
static u32 sio_data = 0xFF;       /* RX Data (0xFF = no controller) */
static u32 sio_stat = 0x00000005; /* TX Ready 1+2 */
static u16 sio_mode = 0;
static u16 sio_ctrl = 0;
static u16 sio_baud = 0;
static int sio_tx_pending = 0; /* 1 = byte was sent, response pending */

/* SPU */
static u16 spu_regs[512]; /* 0x1F801C00-0x1F801DFF */

/* Cache control */
static u32 cache_control = 0;

/* ---- Read ---- */
u32 ReadHardware(u32 addr)
{
    u32 phys = addr & 0x1FFFFFFF;

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
    {
        return mem_ctrl[(phys - 0x1F801000) >> 2];
    }

    /* SIO (Joypad/Memcard) */
    if (phys == 0x1F801040)
    {
        /* JOY_DATA - Return RX data (0xFF = no controller) */
        sio_stat &= ~0x02; /* Clear RX Not Empty after read */
        sio_tx_pending = 0;
        return sio_data;
    }
    if (phys == 0x1F801044)
    {
        /* JOY_STAT: bit0=TX Ready1, bit1=RX Not Empty, bit2=TX Ready2, bit7=/ACK */
        u32 stat = 0x00000005; /* TX Ready 1 + 2 */
        if (sio_tx_pending)
        {
            /* After a byte was transmitted, set RX data available
             * This simulates the controller response (0xFF = no pad) */
            stat |= 0x02; /* RX Not Empty */
            sio_tx_pending = 0;
        }
        return stat;
    }
    if (phys == 0x1F801048)
        return sio_mode;
    if (phys == 0x1F80104A)
        return sio_ctrl;
    if (phys == 0x1F80104E)
        return sio_baud;

    /* RAM Size */
    if (phys == 0x1F801060)
        return ram_size;

    /* Interrupt Controller */
    if (phys == 0x1F801070)
        return i_stat;
    if (phys == 0x1F801074)
        return i_mask;

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
        return dma_dicr;

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
                return timers[t].value; // Return live value
            case 1:
                return timers[t].mode;
            case 2:
                return timers[t].target;
            default:
                return 0;
            }
        }
    }

    /* CD-ROM (0x1F801800-0x1F801803) */
    if (phys >= 0x1F801800 && phys <= 0x1F801803)
        return CDROM_Read(phys);

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
        int idx = (phys - 0x1F801C00) >> 1;
        return spu_regs[idx & 0x1FF];
    }

    /* Expansion 2 */
    if (phys >= 0x1F802000 && phys < 0x1F803000)
        return 0;

    /* Cache control */
    if (phys == 0x1FFE0130 || addr == 0xFFFE0130)
        return cache_control;

    return 0;
}

void UpdateTimers(u32 cycles)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        u32 mode = timers[i].mode;
        u32 target = timers[i].target;
        u32 val = timers[i].value;

        u32 inc = cycles;
        
        // Timer clock sources (bits 8-9):
        // Timer 0: 0/2=sysclk, 1=dotclock
        // Timer 1: 0/2=sysclk, 1=hblank
        // Timer 2: 0/1=sysclk, 2=sysclk/8
        
        // Timer 1 hblank mode (more accurate for boot logo timing)
        if (i == 1 && ((mode >> 8) & 3) == 1)
        {
            static u32 t1_accumulator = 0;
            t1_accumulator += cycles;
            // PSX CPU: 33.8688 MHz, NTSC hblank: ~15734 Hz
            // Cycles per hblank ≈ 33868800 / 15734 ≈ 2152
            inc = t1_accumulator / 2152;
            t1_accumulator %= 2152;
            if (inc == 0)
                continue;
        }
        // Timer 2 clock source (bits 8-9)
        // 2 = Sys/8.
        else if (i == 2 && ((mode >> 8) & 3) == 2)
        {
            static u32 t2_accumulator = 0;
            t2_accumulator += cycles;
            inc = t2_accumulator / 8;
            t2_accumulator %= 8; // Keep remainder
            if (inc == 0)
                continue;
        }

        val += inc;

        // Check Target
        if (val >= target && target > 0)
        { // Target match
            // Bit 4: IRQ on Target
            if (mode & (1 << 4))
            {
                // Bit 6: Toggle/One-shot?
                // Bit 10: IRQ Request Flag (we should set this in mode reg too?)
                timers[i].mode |= (1 << 10);

                // Signal CPU Interrupt (IRQ 4, 5, 6)
                SignalInterrupt(4 + i);
            }

            // Bit 3: Reset on Target
            if (mode & (1 << 3))
            {
                val = 0;
            }
        }

        // Check Overflow (FFFF)
        if (val >= 0xFFFF)
        {
            // Bit 5: IRQ on Overflow
            if (mode & (1 << 5))
            {
                timers[i].mode |= (1 << 11); // Flag? 11 or 12?
                SignalInterrupt(4 + i);
            }
            val &= 0xFFFF; // Wrap
        }

        timers[i].value = val;
    }
}

static void GPU_DMA6(u32 madr, u32 bcr, u32 chcr)
{
    // OTC - Clear Ordering Table (Reverse Clear)
    // Writes pointers to form a linked list from Top to Bottom

    u32 addr = madr & 0x1FFFFC;
    u32 length = bcr;
    if (length == 0)
        length = 0x10000; // 0 means max words? usually on PSX DMA

    // "Write value of MADR (current address - 4) to the current address."
    // "Decrement MADR in 4."
    // "Repeat BCR times."

    // printf("[DMA] Starting DMA6 (OTC) Addr=%08X Count=%d\n", addr, length);

    while (length > 0)
    {
        u32 next_addr = (addr - 4) & 0x1FFFFC;

        // Write pointer to previous entry
        WriteWord(addr, next_addr);

        addr = next_addr;
        length--;
    }

    // "At the end, must write terminator 0xFFFFFF in the last processed address."
    // The last address we WROTE to was 'addr + 4'
    u32 last_written = (addr + 4) & 0x1FFFFC;
    WriteWord(last_written, 0xFFFFFF);

    // printf("[DMA] DMA6 Complete. Terminator at %08X\n", last_written);
}

/* ---- Write ---- */
void WriteHardware(u32 addr, u32 data)
{
    u32 phys = addr & 0x1FFFFFFF;

    /* Memory Control */
    if (phys >= 0x1F801000 && phys < 0x1F801024)
    {
        mem_ctrl[(phys - 0x1F801000) >> 2] = data;
        return;
    }

    /* SIO (Joypad/Memcard) */
    if (phys == 0x1F801040)
    {
        /* JOY_DATA - TX byte. After sending, a response will be available.
         * With no controller: response is 0xFF */
        sio_data = 0xFF;    /* No controller connected */
        sio_tx_pending = 1; /* Mark that RX data will be available */
        return;
    }
    if (phys == 0x1F801048)
    {
        sio_mode = (u16)data;
        return;
    }
    if (phys == 0x1F80104A)
    {
        sio_ctrl = (u16)data;
        if (data & 0x40)
        {
            /* Reset - clear status */
            sio_tx_pending = 0;
        }
        if (data & 0x10)
        {
            /* ACK - acknowledge interrupt */
            sio_stat &= ~(1 << 9); /* Clear IRQ flag */
        }
        return;
    }
    if (phys == 0x1F80104E)
    {
        sio_baud = (u16)data;
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
        /* Log CD-ROM IRQ ack */
        if (!(data & 0x04) && (i_stat & 0x04))
        {
            //            printf("[IRQ] Acknowledging CD-ROM IRQ (I_STAT=%08X, write=%08X)\n",
            //                   (unsigned)i_stat, (unsigned)data);
        }
        i_stat &= data;
        return;
    }
    if (phys == 0x1F801074)
    {
        if (data != i_mask)
        {
            //            printf("[IRQ] I_MASK changed: %08X -> %08X\n",
            //                   (unsigned)i_mask, (unsigned)data);
        }
        i_mask = data;
        return;
    }

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
                dma_channels[ch].madr = data;
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
                    else if (ch == 6)
                    {
                        /* OTC DMA */
                        GPU_DMA6(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    }
                    dma_channels[ch].chcr &= ~0x01000000;

                    /* Handle DMA Interrupts */
                    /* DICR (0x1F8010F4) */
                    /* Bits 16-22: IM (Interrupt Mask) for Ch 0-6 */
                    /* Bits 23: Master Enable */
                    /* Bits 24-30: IP (Interrupt Pending) for Ch 0-6 */

                    /* Set IP bit for this channel */
                    dma_dicr |= (1 << (24 + ch));

                    /* Check for IRQ generation */
                    /* IRQ triggered if (IP & IM) is non-zero OR force bit?
                       Actually: MasterEnable && ((IP & IM) != 0) */
                    if ((dma_dicr & 0x00800000) && (dma_dicr & (1 << (16 + ch))))
                    {
                        // Bit 31 is "IRQ Master Flag". It reflects the IRQ line state?
                        dma_dicr |= 0x80000000;
                        SignalInterrupt(3); /* DMA IRQ */
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
        dma_dicr = data;
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
                timers[t].value = data & 0xFFFF;
                break;
            case 1:
                timers[t].mode = data;
                break;
            case 2:
                timers[t].target = data & 0xFFFF;
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

    /* SPU */
    if (phys >= 0x1F801C00 && phys < 0x1F801E00)
    {
        int idx = (phys - 0x1F801C00) >> 1;
        spu_regs[idx & 0x1FF] = (u16)data;
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
