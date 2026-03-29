#include "psx_dma.h"
#include "scheduler.h"
#include "spu.h"
#include "superpsx.h"
#include "dynarec.h" /* for jit_invalidate_page */
#include "mdec.h"
#include <stdint.h>

typedef struct
{
  uint32_t madr; /* Base address */
  uint32_t bcr;  /* Block control */
  uint32_t chcr; /* Channel control */
} DmaChannel;

static DmaChannel dma_channels[7];
static uint32_t dma_dpcr = 0x07654321;
static uint32_t dma_dicr = 0;

/* Pending deferred DMA completion: which channel and deadline */
static int dma_pending_channel = -1;
static uint64_t dma_pending_deadline = 0;

/* Cycles per word for SPU DMA (NoCash: ~4 cycles/word at CPU clock).
 * We use a conservative 16 cycles/word so tests see non-zero transfer time. */
#define SPU_DMA_CYCLES_PER_WORD 8U

static void DMA_FireCompletion(int ticks_late);

static void dma_complete_channel(int ch)
{
  dma_channels[ch].chcr &= ~0x01000000; /* Clear Transfer Start bit */
  if (dma_dicr & (1 << (16 + ch)))
  {
    dma_dicr |= (1 << (24 + ch));
    if (dma_dicr & 0x00800000)
    {
      dma_dicr |= 0x80000000;
      SignalInterrupt(3);
    }
  }
}

static void DMA_FireCompletion(int ticks_late)
{
  (void)ticks_late;
  int ch = dma_pending_channel;
  if (ch >= 0 && ch < 7)
    dma_complete_channel(ch);
  dma_pending_channel = -1;
}

/* Effective cycles accounting for mid-block consumption in JIT */
#define DMA_EFFECTIVE_CYCLES \
  (global_cycles + (cpu.initial_cycles_left - cpu.cycles_left) + partial_block_cycles)

/* Inline check for CHCR polling: complete DMA early if deadline reached,
 * cancel the scheduler event (which would be a no-op duplicate). */
static inline void dma_check_pending(void)
{
  if (dma_pending_channel >= 0 && DMA_EFFECTIVE_CYCLES >= dma_pending_deadline)
  {
    int ch = dma_pending_channel;
    dma_pending_channel = -1;
    Scheduler_RemoveEvent(SCHED_EVENT_DMA);
    dma_complete_channel(ch);
  }
}

int DMA_IsPending(void)
{
  return dma_pending_channel >= 0;
}

static void CDROM_DMA3(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
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

  /* Invalidate all pages touched by this DMA transfer */
  for (uint32_t off = 0; off < total_bytes; off += 4096)
    jit_invalidate_page(phys_addr + off);
  if (total_bytes > 0)
    jit_invalidate_page(phys_addr + total_bytes - 1);
}

static void GPU_DMA6(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
  uint32_t addr = madr & 0x1FFFFC;
  uint32_t length = bcr;
  if (length == 0)
    length = 0x10000;

  while (length > 0)
  {
    uint32_t next_addr = (addr - 4) & 0x1FFFFC;
    WriteWord(addr, next_addr);
    addr = next_addr;
    length--;
  }
  WriteWord((addr + 4) & 0x1FFFFC, 0xFFFFFF);
}

uint32_t DMA_Read(uint32_t addr)
{
  uint32_t phys = addr & 0x1FFFFFFF;

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
        /* DMA6/OTC: bit 1 hardwired to 1, only bits 24,28,30 exposed */
        if (ch == 6)
          return (dma_channels[ch].chcr & 0x51000000) | 0x00000002;
        /* Check deferred DMA completion inline when polling CHCR */
        if (ch == dma_pending_channel)
          dma_check_pending();
        return dma_channels[ch].chcr;
      default:
        return 0;
      }
    }
  }
  if (phys == 0x1F8010F0)
    return dma_dpcr;
  if (phys == 0x1F8010F4)
  {
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

void DMA_Write(uint32_t addr, uint32_t data)
{
  uint32_t phys = addr & 0x1FFFFFFF;

  if (phys >= 0x1F801080 && phys < 0x1F8010F0)
  {
    int ch = (phys - 0x1F801080) / 0x10;
    int reg = ((phys - 0x1F801080) % 0x10) / 4;
    if (ch < 7)
    {
      switch (reg)
      {
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

#ifdef ENABLE_VRAM_DUMP
        if (ch == 1)
          printf("[DMA] ch1 CHCR ANY write: data=%08X madr=%08X bcr=%08X dpcr=%08X\n",
                 (unsigned)data, (unsigned)dma_channels[ch].madr,
                 (unsigned)dma_channels[ch].bcr, (unsigned)dma_dpcr);
#endif

        if (data & 0x01000000)
        {
#ifdef ENABLE_VRAM_DUMP
          if (ch == 1)
            printf("[DMA] ch1 CHCR write: data=%08X madr=%08X bcr=%08X dpcr=%08X\n",
                   (unsigned)data, (unsigned)dma_channels[ch].madr,
                   (unsigned)dma_channels[ch].bcr, (unsigned)dma_dpcr);
#endif
          /* Check DPCR master enable for this channel.
           * If master is disabled, leave bit24 SET (DMA stuck/pending) —
           * hardware never clears it when the channel can't run. */
          if (!((dma_dpcr >> (ch * 4 + 3)) & 1))
            break;

          /* DMA6/OTC SyncMode=0 requires bit28 (manual Start/Trigger).
           * Other channels start with bit24 alone. */
          uint32_t sync_mode = (dma_channels[ch].chcr >> 9) & 3;
          if (ch == 6 && sync_mode == 0 &&
              !(dma_channels[ch].chcr & 0x10000000))
          {
            dma_channels[ch].chcr &= ~0x01000000;
            break;
          }

          /* Execute the actual data transfer */
          int dma_stalled = 0;
          if (ch == 0)
            MDEC_DMA0(dma_channels[ch].madr, dma_channels[ch].bcr,
                      dma_channels[ch].chcr);
          else if (ch == 1)
            MDEC_DMA1(dma_channels[ch].madr, dma_channels[ch].bcr,
                      dma_channels[ch].chcr);
          else if (ch == 2)
            dma_stalled = GPU_DMA2(dma_channels[ch].madr, dma_channels[ch].bcr,
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

          if (ch == 4)
          {
            /* SPU DMA: defer completion so the transfer takes visible
             * time.  The polling check (dma_check_pending) completes
             * the DMA inline when EFFECTIVE_CYCLES reaches the
             * deadline; the scheduler is a fallback for IRQ-based
             * games that don't poll CHCR. */
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
              delay_cycles = 32;

            dma_pending_channel = ch;
            dma_pending_deadline = DMA_EFFECTIVE_CYCLES + delay_cycles;

            /* Scheduler fallback for IRQ-based completion */
            Scheduler_ScheduleEvent(SCHED_EVENT_DMA,
                                    global_cycles + delay_cycles,
                                    DMA_FireCompletion);
          }
          else if (dma_stalled)
          {
            /* Linked-list DMA stalled (loop detected) — leave bit24 set,
             * the transfer never completes on real hardware either. */
          }
          else
          {
            /* All other channels / modes: clear bit24 and bit28 immediately */
            dma_channels[ch].chcr &= ~0x11000000;
            if (dma_dicr & (1 << (16 + ch)))
            {
              dma_dicr |= (1 << (24 + ch));
              if (dma_dicr & 0x00800000)
              {
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
  if (phys == 0x1F8010F0)
  {
    dma_dpcr = data;
    return;
  }
  if (phys == 0x1F8010F4)
  {
    uint32_t rw_mask = 0x00FF803F;
    uint32_t ack_bits = data & 0x7F000000;
    uint32_t old_master = dma_dicr & 0x80000000;
    dma_dicr = (data & rw_mask) | ((dma_dicr & 0x7F000000) & ~ack_bits);
    /* Recalculate bit31 (Master IRQ Flag) — per psx-spx, this is
     * read-only and reflects: Force_IRQ OR (Master_Enable AND
     * (Enable AND Flags) != 0). Must recalculate after every write. */
    uint32_t force = (dma_dicr >> 15) & 1;
    uint32_t master_en = (dma_dicr >> 23) & 1;
    uint32_t en = (dma_dicr >> 16) & 0x7F;
    uint32_t flg = (dma_dicr >> 24) & 0x7F;
    if (force || (master_en && (en & flg)))
      dma_dicr |= 0x80000000;
    /* Rising edge on bit31 → fire DMA master interrupt */
    if (!old_master && (dma_dicr & 0x80000000))
      SignalInterrupt(3);
    return;
  }
}
