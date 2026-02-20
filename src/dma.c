#include <stdio.h>
#include <stdint.h>
#include "superpsx.h"
#include "psx_dma.h"
#include "spu.h"

#define LOG_TAG "DMA"

typedef struct
{
    uint32_t madr; /* Base address */
    uint32_t bcr;  /* Block control */
    uint32_t chcr; /* Channel control */
} DmaChannel;

static DmaChannel dma_channels[7];
static uint32_t dma_dpcr = 0x07654321;
static uint32_t dma_dicr = 0;

static void CDROM_DMA3(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t block_size_words = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    if (block_count == 0) block_count = 1;
    if (block_size_words == 0) block_size_words = 1;

    uint32_t total_bytes = block_size_words * block_count * 4;
    uint32_t phys_addr = madr & 0x1FFFFC;

    if (phys_addr + total_bytes > PSX_RAM_SIZE)
        total_bytes = PSX_RAM_SIZE - phys_addr;

    CDROM_ReadDataFIFO(psx_ram + phys_addr, total_bytes);
}

static void GPU_DMA6(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t addr = madr & 0x1FFFFC;
    uint32_t length = bcr;
    if (length == 0) length = 0x10000;

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
            case 0: return dma_channels[ch].madr;
            case 1: return dma_channels[ch].bcr;
            case 2: return dma_channels[ch].chcr & ~0x01000000;
            default: return 0;
            }
        }
    }
    if (phys == 0x1F8010F0) return dma_dpcr;
    if (phys == 0x1F8010F4)
    {
        uint32_t read_val = dma_dicr & 0x7F000000;
        read_val |= dma_dicr & 0x00FF803F;
        uint32_t force = (dma_dicr >> 15) & 1;
        uint32_t master_en = (dma_dicr >> 23) & 1;
        uint32_t en = (dma_dicr >> 16) & 0x7F;
        uint32_t flg = (dma_dicr >> 24) & 0x7F;
        if (force || (master_en && (en & flg))) read_val |= 0x80000000;
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
            case 0: dma_channels[ch].madr = data & 0x00FFFFFF; break;
            case 1: dma_channels[ch].bcr = data; break;
            case 2:
                dma_channels[ch].chcr = data;
                if (data & 0x01000000)
                {
                    if (ch == 2) GPU_DMA2(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    else if (ch == 3) CDROM_DMA3(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    else if (ch == 4) SPU_DMA4(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    else if (ch == 6) GPU_DMA6(dma_channels[ch].madr, dma_channels[ch].bcr, dma_channels[ch].chcr);
                    
                    dma_channels[ch].chcr &= ~0x01000000;
                    if (dma_dicr & (1 << (16 + ch)))
                    {
                        dma_dicr |= (1 << (24 + ch));
                        if (dma_dicr & 0x00800000) { dma_dicr |= 0x80000000; SignalInterrupt(3); }
                    }
                }
                break;
            }
        }
        return;
    }
    if (phys == 0x1F8010F0) { dma_dpcr = data; return; }
    if (phys == 0x1F8010F4)
    {
        uint32_t rw_mask = 0x00FF803F;
        uint32_t ack_bits = data & 0x7F000000;
        dma_dicr = (data & rw_mask) | ((dma_dicr & 0x7F000000) & ~ack_bits);
        return;
    }
}
