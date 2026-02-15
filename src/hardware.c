#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <kernel.h>
#include "superpsx.h"
#include "joystick.h"
#include "superpsx.h"

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

/* SIO (Joypad/Memcard) - PSX controller protocol state machine */
static u32 sio_data = 0xFF;       /* RX Data register */
static u32 sio_stat = 0x00000005; /* TX Ready 1+2 */
static u16 sio_mode = 0;
static u16 sio_ctrl = 0;
static u16 sio_baud = 0;
static int sio_tx_pending = 0;    /* 1 = RX data available */

/* Controller protocol state machine */
static int sio_state = 0;         /* Current byte index in protocol */
static uint8_t sio_response[5];   /* Pre-built response buffer */
static int sio_selected = 0;      /* 1 = JOY SELECT is asserted */

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
        /* JOY_DATA - Return RX data from controller */
        u32 val = sio_data;
        sio_tx_pending = 0;
        return val;
    }
    if (phys == 0x1F801044)
    {
        /* JOY_STAT: bit0=TX Ready1, bit1=RX Not Empty, bit2=TX Ready2, bit7=/ACK */
        u32 stat = 0x00000005; /* TX Ready 1 + 2 always set */
        if (sio_tx_pending)
            stat |= 0x02;     /* RX Not Empty */
        /* Report /ACK low (active) during active transfer (bytes 0-3) */
        if (sio_selected && sio_state > 0 && sio_state < 5)
            stat |= 0x80;     /* /ACK input level */
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
                return timers[t].value & 0xFFFF;
            case 1:
            {
                u32 val = timers[t].mode;
                /* Bits 11-12 are cleared after reading */
                timers[t].mode &= ~((1 << 11) | (1 << 12));
                return val;
            }
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

/* Scanline/frame tracking for timer sync modes */
/* PSX NTSC: 263 scanlines per frame, ~2152 CPU cycles per scanline */
/* Hblank occurs at the end of each scanline for ~538 CPU cycles */
#define CYCLES_PER_SCANLINE   2152
#define HBLANK_START_CYCLE    1614  /* Active display ends, hblank begins */
#define SCANLINES_PER_FRAME   263
#define VBLANK_START_LINE     240   /* Vblank starts after line 240 */

static u32 scanline_cycle = 0;     /* Current cycle within the scanline */
static u32 current_scanline = 0;   /* Current scanline number */
static int in_hblank = 0;          /* Whether we're in hblank */
static int in_vblank = 0;          /* Whether we're in vblank */
static int prev_hblank = 0;        /* Previous hblank state (for edge detect) */
static int prev_vblank = 0;        /* Previous vblank state (for edge detect) */

/* Per-timer sync state */
static int timer0_sync3_started = 0; /* Timer0 sync mode 3: after first hblank */
static int timer1_sync3_started = 0; /* Timer1 sync mode 3: after first vblank */

void UpdateTimers(u32 cycles)
{
    /* Update scanline position */
    prev_hblank = in_hblank;
    prev_vblank = in_vblank;
    
    scanline_cycle += cycles;
    while (scanline_cycle >= CYCLES_PER_SCANLINE)
    {
        scanline_cycle -= CYCLES_PER_SCANLINE;
        current_scanline++;
        if (current_scanline >= SCANLINES_PER_FRAME)
            current_scanline = 0;
    }
    
    in_hblank = (scanline_cycle >= HBLANK_START_CYCLE) ? 1 : 0;
    in_vblank = (current_scanline >= VBLANK_START_LINE) ? 1 : 0;
    
    int hblank_edge = (in_hblank && !prev_hblank); /* Rising edge of hblank */
    int vblank_edge = (in_vblank && !prev_vblank); /* Rising edge of vblank */

    int i;
    for (i = 0; i < 3; i++)
    {
        u32 mode = timers[i].mode;
        u32 target = timers[i].target;
        u32 val = timers[i].value;

        /* Check sync mode (bit 0 = sync enable, bits 1-2 = sync type) */
        int sync_enabled = mode & 1;
        int sync_type = (mode >> 1) & 3;
        
        if (sync_enabled)
        {
            if (i == 0)
            {
                /* Timer0 sync modes use Hblank */
                switch (sync_type)
                {
                case 0: /* Pause counter during Hblank */
                    if (in_hblank)
                        continue; /* Don't count during hblank */
                    break;
                case 1: /* Reset counter to 0 at Hblank */
                    if (hblank_edge)
                        val = 0;
                    break;
                case 2: /* Reset at Hblank, pause outside Hblank */
                    if (hblank_edge)
                        val = 0;
                    if (!in_hblank)
                        continue; /* Only count during hblank */
                    break;
                case 3: /* Pause until Hblank once, then Free Run */
                    if (!timer0_sync3_started)
                    {
                        if (hblank_edge)
                            timer0_sync3_started = 1;
                        else
                            continue; /* Paused until first hblank */
                    }
                    break;
                }
            }
            else if (i == 1)
            {
                /* Timer1 sync modes use Vblank */
                switch (sync_type)
                {
                case 0: /* Pause counter during Vblank */
                    if (in_vblank)
                        continue;
                    break;
                case 1: /* Reset counter to 0 at Vblank */
                    if (vblank_edge)
                        val = 0;
                    break;
                case 2: /* Reset at Vblank, pause outside Vblank */
                    if (vblank_edge)
                        val = 0;
                    if (!in_vblank)
                        continue;
                    break;
                case 3: /* Pause until Vblank once, then Free Run */
                    if (!timer1_sync3_started)
                    {
                        if (vblank_edge)
                            timer1_sync3_started = 1;
                        else
                            continue;
                    }
                    break;
                }
            }
            else if (i == 2)
            {
                /* Timer2: sync modes 0,3 = stop counter; 1,2 = free run */
                if (sync_type == 0 || sync_type == 3)
                    continue;
                /* sync_type 1 or 2: fall through to free run */
            }
        }

        u32 inc = cycles;
        
        // Timer clock sources (bits 8-9):
        // Timer 0: 0/2=sysclk, 1/3=dotclock
        // Timer 1: 0/2=sysclk, 1/3=hblank
        // Timer 2: 0/1=sysclk, 2/3=sysclk/8
        
        // Timer 0 dot clock mode
        if (i == 0 && ((mode >> 8) & 1) == 1)
        {
            static u32 t0_accumulator = 0;
            t0_accumulator += cycles;
            // Dot clock varies with resolution. Default 320px = 53.2224 MHz / 10 = 5.32224 MHz
            // Ratio: CPU 33.8688 MHz / dotclock 5.32224 MHz ≈ 6.3636
            // But for 320x mode the dot clock is ~6.65 MHz, ratio ~5.09
            // Use a reasonable average: CPU/dotclock ≈ 5 for 320x
            // A simpler approach: dotclock ≈ sysclk * 7 / 33.8688 ≈ sysclk / 5
            // Actually GPU pixels per scanline is 3413 GPU clocks at 53.69 MHz
            // For 320px: 53.693175 / 8 = 6.7116 MHz. CPU ratio = 33.8688/6.7116 ≈ 5.046
            inc = t0_accumulator / 5;
            t0_accumulator %= 5;
            if (inc == 0)
                continue;
        }
        // Timer 1 hblank mode (more accurate for boot logo timing)
        else if (i == 1 && ((mode >> 8) & 1) == 1)
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
        // 2 or 3 = Sys/8.
        else if (i == 2 && ((mode >> 8) & 2) == 2)
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
        {
            // Set bit 11: Reached Target Value flag
            timers[i].mode |= (1 << 11);

            // Bit 4: IRQ on Target
            if (mode & (1 << 4))
            {
                // Bit 10: IRQ Request flag (0=Yes, 1=No)
                // Pulse mode (bit 7=0): briefly set bit 10 to 0
                // Toggle mode (bit 7=1): toggle bit 10
                if (mode & (1 << 7))
                    timers[i].mode ^= (1 << 10);
                else
                    timers[i].mode &= ~(1 << 10);

                // Signal CPU Interrupt (IRQ 4, 5, 6)
                SignalInterrupt(4 + i);
            }

            // Bit 3: Reset on Target
            if (mode & (1 << 3))
            {
                val %= (target + 1); // Handle overshoot properly
            }
        }

        // Check Overflow (FFFF)
        if (val >= 0xFFFF)
        {
            // Set bit 12: Reached FFFFh Value flag
            timers[i].mode |= (1 << 12);

            // Bit 5: IRQ on Overflow
            if (mode & (1 << 5))
            {
                if (mode & (1 << 7))
                    timers[i].mode ^= (1 << 10);
                else
                    timers[i].mode &= ~(1 << 10);

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
        /* JOY_DATA - TX byte: PSX controller protocol state machine */
        uint8_t tx = (uint8_t)(data & 0xFF);

        if (!sio_selected) {
            /* No device selected, respond with 0xFF (hi-z) */
            sio_data = 0xFF;
            sio_tx_pending = 1;
            return;
        }

        switch (sio_state) {
        case 0:
            /* Byte 0: Host sends 0x01 to select controller */
            if (tx == 0x01) {
                /* Snapshot the buttons now for this entire transfer */
                Joystick_GetPSXDigitalResponse(sio_response + 1);
                sio_response[0] = 0xFF; /* hi-z during address byte */
                /* response[1]=0x41  response[2]=0x5A  put button bytes after */
                /* Rearrange: [0]=0xFF [1]=0x41 [2]=0x5A [3]=btnLo [4]=btnHi */
                {
                    uint8_t id  = sio_response[1]; /* 0x41 */
                    uint8_t lo  = sio_response[2]; /* buttons low */
                    uint8_t hi  = sio_response[3]; /* buttons high */
                    sio_response[1] = id;   /* 0x41 */
                    sio_response[2] = 0x5A; /* data-start marker */
                    sio_response[3] = lo;
                    sio_response[4] = hi;
                }
                sio_data = sio_response[0]; /* 0xFF */
                sio_state = 1;
                sio_tx_pending = 1;
                SignalInterrupt(7); /* IRQ7 - controller */
            } else {
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
            SignalInterrupt(7);
            break;
        case 2:
            /* Byte 2: Host sends 0x00, controller responds 0x5A */
            sio_data = sio_response[2]; /* 0x5A */
            sio_state = 3;
            sio_tx_pending = 1;
            SignalInterrupt(7);
            break;
        case 3:
            /* Byte 3: Host sends 0x00, controller responds button low byte */
            sio_data = sio_response[3];
            sio_state = 4;
            sio_tx_pending = 1;
            SignalInterrupt(7);
            break;
        case 4:
            /* Byte 4: Host sends 0x00, controller responds button high byte */
            sio_data = sio_response[4];
            sio_state = 5; /* transfer complete */
            sio_tx_pending = 1;
            SignalInterrupt(7);
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
        sio_mode = (u16)data;
        return;
    }
    if (phys == 0x1F80104A)
    {
        sio_ctrl = (u16)data;
        if (data & 0x40)
        {
            /* Reset - clear all state */
            sio_tx_pending = 0;
            sio_state = 0;
            sio_selected = 0;
            sio_data = 0xFF;
        }
        if (data & 0x10)
        {
            /* ACK - acknowledge interrupt */
            sio_stat &= ~(1 << 9); /* Clear IRQ flag */
        }
        /* Bit 1: JOYn output select - directly controls /SEL line */
        if (data & 0x02) {
            if (!sio_selected) {
                /* Freshly asserted: reset protocol state */
                sio_state = 0;
            }
            sio_selected = 1;
        } else {
            sio_selected = 0;
            sio_state = 0;
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
                /* Writing mode register:
                 * - Resets counter to 0
                 * - Clears bits 11-12 (reached flags)
                 * - Sets bit 10 (IRQ request = No/1) */
                timers[t].mode = (data & 0x03FF) | (1 << 10);
                timers[t].value = 0;
                /* Reset sync mode 3 state for this timer */
                if (t == 0)
                    timer0_sync3_started = 0;
                else if (t == 1)
                    timer1_sync3_started = 0;
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
