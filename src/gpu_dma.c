/**
 * gpu_dma.c — DMA Channel 2 (GPU) handling
 *
 * Processes PSX DMA channel 2 transfers: continuous / block mode writes,
 * GPU→CPU reads, and linked-list mode (the most common path used by
 * PSX games to submit display lists).
 */
#include "gpu_state.h"

/* ── Helper: read a 32-bit word from PSX RAM ─────────────────────── */

static uint32_t GPU_GetWord(uint32_t addr)
{
    addr &= 0x1FFFFC;
    return *(uint32_t *)&psx_ram[addr];
}

/* ── DMA Channel 2 entry point ───────────────────────────────────── */

void GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t addr = madr & 0x1FFFFC;
    if ((chcr & 0x01000000) == 0)
        return;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t direction = chcr & 1;

    // Flush any pending GIF data from direct GP0 writes before starting DMA
    Flush_GIF();

    // Sync Mode 0 (Continuous) and 1 (Block/Request): CPU -> GPU transfer
    if (sync_mode == 0 || sync_mode == 1)
    {
        if (direction == 1)
        { // CPU -> GPU
            uint32_t block_size = bcr & 0xFFFF;
            uint32_t block_count = (bcr >> 16) & 0xFFFF;
            uint32_t total_words;

            if (sync_mode == 0)
            {
                total_words = block_size;
                if (total_words == 0)
                    total_words = 0x10000;
            }
            else
            {
                if (block_count == 0)
                    block_count = 0x10000;
                if (block_size == 0)
                    block_size = 0x10000;
                total_words = block_size * block_count;
            }

            for (uint32_t i = 0; i < total_words; i++)
            {
                uint32_t word = *(uint32_t *)(psx_ram + (addr & 0x1FFFFC));
                GPU_WriteGP0(word);
                addr += 4;
            }
        }
        else
        {
            // GPU -> CPU (VRAM Read)
            uint32_t block_size = bcr & 0xFFFF;
            uint32_t block_count = (bcr >> 16) & 0xFFFF;
            uint32_t total_words;

            if (sync_mode == 0)
            {
                total_words = block_size;
                if (total_words == 0)
                    total_words = 0x10000;
            }
            else
            {
                if (block_count == 0)
                    block_count = 0x10000;
                if (block_size == 0)
                    block_size = 0x10000;
                total_words = block_size * block_count;
            }

            DLOG("DMA2 GPU->CPU Read: %" PRIu32 " words\n", total_words);

            for (uint32_t i = 0; i < total_words; i++)
            {
                uint32_t word = GPU_Read();
                *(uint32_t *)(psx_ram + (addr & 0x1FFFFC)) = word;
                addr += 4;
            }
        }
        return;
    }

    if (sync_mode == 2)
    {
        int packets = 0;
        int max_packets = 20000;

        while (packets < max_packets)
        {
            uint32_t packet_addr = addr;
            uint32_t header = GPU_GetWord(addr);
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;

            if (count > 256)
            {
                DLOG("ERROR: Packet count too large (%" PRIu32 "). Aborting chain.\n", count);
                break;
            }

            addr += 4;
            for (uint32_t i = 0; i < count;)
            {
                if (polyline_active)
                {
                    uint32_t word = GPU_GetWord(addr);
                    GPU_WriteGP0(word);
                    i++;
                    addr += 4;
                    continue;
                }

                uint32_t cmd_word = GPU_GetWord(addr);
                uint32_t cmd_byte = cmd_word >> 24;

                if ((cmd_byte & 0xE0) == 0x40)
                {
                    Flush_GIF();
                    while (i < count)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                        if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                            break;
                    }
                }
                else if (cmd_byte >= 0x20 && cmd_byte <= 0x7F)
                {
                    uint32_t *cmd_ptr = (uint32_t *)&psx_ram[addr];
                    Translate_GP0_to_GS(cmd_ptr);

                    int size = GPU_GetCommandSize(cmd_word >> 24);
                    i += size;
                    addr += (size * 4);
                }
                else if ((cmd_word >> 24) == 0x02)
                {
                    uint32_t *cmd_ptr = (uint32_t *)&psx_ram[addr];
                    Translate_GP0_to_GS(cmd_ptr);

                    int size = 3;
                    i += size;
                    addr += (size * 4);
                }
                else if ((cmd_word >> 24) == 0xA0)
                {
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;

                    while (i < count && gpu_cmd_remaining > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }

                    while (i < count && gpu_transfer_words > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }
                }
                else if (((cmd_word >> 24) & 0xE0) == 0x80)
                {
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;
                    while (i < count && gpu_cmd_remaining > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }
                }
                else
                {
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;
                }
            }

            packets++;

            if (next == 0xFFFFFF)
                break;

            if (next == packet_addr)
            {
                DLOG("Warning: Linked List Self-Reference %06" PRIX32 ". Breaking chain to allow CPU operation.\n", next);
                break;
            }

            if (next & 0x3)
            {
                DLOG("ERROR: Unaligned next pointer %06" PRIX32 "\n", next);
                break;
            }

            addr = next & 0x1FFFFC;
        }

        Flush_GIF();
    }
}
