#include "psx_dma.h"
#include "scheduler.h"
#include "spu.h"
#include "superpsx.h"
#include <stdint.h>

typedef struct {
  uint32_t madr; /* Base address */
  uint32_t bcr;  /* Block control */
  uint32_t chcr; /* Channel control */
} DmaChannel;

static DmaChannel dma_channels[7];
static uint32_t dma_dpcr = 0x07654321;
static uint32_t dma_dicr = 0;

/* Pending deferred DMA completion: which channel and IRQ state */
static int dma_pending_channel = -1;

/* Cycles per word for SPU DMA (NoCash: ~4 cycles/word at CPU clock).
 * We use a conservative 16 cycles/word so tests see non-zero transfer time. */
#define SPU_DMA_CYCLES_PER_WORD 8U

static void DMA_FireCompletion(void);

static void dma_complete_channel(int ch) {
  dma_channels[ch].chcr &= ~0x01000000; /* Clear Transfer Start bit */
  if (dma_dicr & (1 << (16 + ch))) {
    dma_dicr |= (1 << (24 + ch));
    if (dma_dicr & 0x00800000) {
      dma_dicr |= 0x80000000;
      SignalInterrupt(3);
    }
  }
}

static void DMA_FireCompletion(void) {
  int ch = dma_pending_channel;
  if (ch >= 0 && ch < 7)
    dma_complete_channel(ch);
  dma_pending_channel = -1;
}

static void CDROM_DMA3(uint32_t madr, uint32_t bcr, uint32_t chcr) {
  uint32_t block_size_words = bcr & 0xFFFF;
  uint32_t block_count = (bcr >> 16) & 0xFFFF;
  if (block_count == 0)
    block_count = 1;
  if (block_size_words == 0)
    block_size_words = 1;

  uint32_t total_bytes = block_size_words * block_count * 4;
  uint32_t phys_addr = madr & 0x1FFFFC;

  if (phys_addr + total_bytes > PSX_RAM_SIZE)
    total_bytes = PSX_RAM_SIZE - phys_addr;

  CDROM_ReadDataFIFO(psx_ram + phys_addr, total_bytes);
}

static void GPU_DMA6(uint32_t madr, uint32_t bcr, uint32_t chcr) {
  uint32_t addr = madr & 0x1FFFFC;
  uint32_t length = bcr;
  if (length == 0)
    length = 0x10000;

  while (length > 0) {
    uint32_t next_addr = (addr - 4) & 0x1FFFFC;
    WriteWord(addr, next_addr);
    addr = next_addr;
    length--;
  }
  WriteWord((addr + 4) & 0x1FFFFC, 0xFFFFFF);
}

uint32_t DMA_Read(uint32_t addr) {
  uint32_t phys = addr & 0x1FFFFFFF;

  if (phys >= 0x1F801080 && phys < 0x1F801100) {
    int ch = (phys - 0x1F801080) / 0x10;
    int reg = ((phys - 0x1F801080) % 0x10) / 4;
    if (ch < 7) {
      switch (reg) {
      case 0:
        return dma_channels[ch].madr;
      case 1:
        return dma_channels[ch].bcr;
      case 2:
        /* DMA6/OTC: bit 1 hardwired to 1, only bits 24,28,30 exposed */
        if (ch == 6)
          return (dma_channels[ch].chcr & 0x51000000) | 0x00000002;
        return dma_channels[ch].chcr;
      default:
        return 0;
      }
    }
  }
  if (phys == 0x1F8010F0)
    return dma_dpcr;
  if (phys == 0x1F8010F4) {
    uint32_t read_val = dma_dicr & 0x7F000000;
    read_val |= dma_dicr & 0x00FF803F;
    uint32_t force = (dma_dicr >> 15) & 1;
    uint32_t master_en = (dma_dicr >> 23) & 1;
    uint32_t en = (dma_dicr >> 16) & 0x7F;
    uint32_t flg = (dma_dicr >> 24) & 0x7F;
    if (force || (master_en && (en & flg)))
      read_val |= 0x80000000;
    return read_val;
  }
  return 0;
}

void DMA_Write(uint32_t addr, uint32_t data) {
  uint32_t phys = addr & 0x1FFFFFFF;

  if (phys >= 0x1F801080 && phys < 0x1F8010F0) {
    int ch = (phys - 0x1F801080) / 0x10;
    int reg = ((phys - 0x1F801080) % 0x10) / 4;
    if (ch < 7) {
      switch (reg) {
      case 0:
        dma_channels[ch].madr = data & 0x00FFFFFF;
        break;
      case 1:
        dma_channels[ch].bcr = data;
        break;
      case 2:
        /* DMA6/OTC: only bits 24,28,30 are writable; bit 1 hardwired to 1. */
        if (ch == 6)
          dma_channels[ch].chcr = (data & 0x51000000) | 0x00000002;
        else
          dma_channels[ch].chcr = data;

        if (data & 0x01000000) {
          /* Check DPCR master enable for this channel */
          if (!((dma_dpcr >> (ch * 4 + 3)) & 1)) {
            dma_channels[ch].chcr &= ~0x01000000;
            break;
          }

          /* SyncMode=0 channels (incl. DMA6) require bit28 (Start/Trigger) */
          uint32_t sync_mode = (dma_channels[ch].chcr >> 9) & 3;
          if (sync_mode == 0 && !(dma_channels[ch].chcr & 0x10000000)) {
            dma_channels[ch].chcr &= ~0x01000000;
            break;
          }

          /* Execute the actual data transfer */
          if (ch == 2)
            GPU_DMA2(dma_channels[ch].madr, dma_channels[ch].bcr,
                     dma_channels[ch].chcr);
          else if (ch == 3)
            CDROM_DMA3(dma_channels[ch].madr, dma_channels[ch].bcr,
                       dma_channels[ch].chcr);
          else if (ch == 4)
            SPU_DMA4(dma_channels[ch].madr, dma_channels[ch].bcr,
                     dma_channels[ch].chcr);
          else if (ch == 6)
            GPU_DMA6(dma_channels[ch].madr, dma_channels[ch].bcr,
                     dma_channels[ch].chcr);

          if (ch == 4 && sync_mode == 1) {
            /* SPU DMA mode 1 (sync block): defer completion so that test
             * code can observe bit24 still set (DMA not yet complete).
             * Transfer time ≈ block_size * block_count *
             * SPU_DMA_CYCLES_PER_WORD */
            uint32_t block_size = dma_channels[ch].bcr & 0xFFFF;
            uint32_t block_count = (dma_channels[ch].bcr >> 16) & 0xFFFF;
            if (block_size == 0)
              block_size = 1;
            if (block_count == 0)
              block_count = 1;
            uint32_t total_words = block_size * block_count;
            uint64_t delay_cycles =
                (uint64_t)total_words * SPU_DMA_CYCLES_PER_WORD;
            if (delay_cycles < 32)
              delay_cycles = 32; /* minimum visible delay */

            dma_pending_channel = ch;
            /* chcr bit24 stays set (busy) until the deferred event fires */
            uint64_t dma_deadline = global_cycles + delay_cycles;
            Scheduler_ScheduleEvent(SCHED_EVENT_DMA, dma_deadline,
                                    DMA_FireCompletion);
          } else {
            /* All other channels / modes: clear bit24 and bit28 immediately */
            dma_channels[ch].chcr &= ~0x11000000;
            if (dma_dicr & (1 << (16 + ch))) {
              dma_dicr |= (1 << (24 + ch));
              if (dma_dicr & 0x00800000) {
                dma_dicr |= 0x80000000;
                SignalInterrupt(3);
              }
            }
          }
        }
        break;
      }
    }
    return;
  }
  if (phys == 0x1F8010F0) {
    dma_dpcr = data;
    return;
  }
  if (phys == 0x1F8010F4) {
    uint32_t rw_mask = 0x00FF803F;
    uint32_t ack_bits = data & 0x7F000000;
    dma_dicr = (data & rw_mask) | ((dma_dicr & 0x7F000000) & ~ack_bits);
    return;
  }
}
