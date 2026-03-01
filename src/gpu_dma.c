/**
 * gpu_dma.c — DMA Channel 2 (GPU) handling
 *
 * Processes PSX DMA channel 2 transfers: continuous / block mode writes,
 * GPU→CPU reads, and linked-list mode (the most common path used by
 * PSX games to submit display lists).
 */
#include "gpu_state.h"
#include "scheduler.h"
#include "profiler.h"

/* ── GPU rendering busy tracking ─────────────────────────────────────── */
/* gpu_busy_until: global_cycles value until which the GPU is "busy".
 * GPU_ReadStatus() checks this to clear the "ready for commands" bits,
 * making DrawSync(0) poll loops consume real emulated time.             */
uint64_t gpu_busy_until = 0;

/* DMA bus cost: ~1 CPU cycle per word (psx-spx: 0110h clks per 100h words).
 * Linked-list header read adds ~1 cycle per node. */
#define DMA_CYCLES_PER_WORD 1
#define DMA_CYCLES_PER_PACKET 1

/* Approximate GPU rendering cost per primitive pixel-clock.
 * Real PSX GPU fills ~2 cycles/pixel (flat), ~3 (gouraud), ~4 (textured).
 * We use a uniform ~2 CPU cycles per pixel as a rough average.             */
#define GPU_CYCLES_PER_PIXEL 2

/* ── DMA Channel 2 entry point ───────────────────────────────────── */

void GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t addr = madr & 0x1FFFFC;
    if ((chcr & 0x01000000) == 0)
        return;

    PROF_PUSH(PROF_GPU_DMA);

    /* Reset pixel accumulator for this DMA batch */
    gpu_estimated_pixels = 0;
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

            PROF_PUSH(PROF_GPU_PRIM);
            GPU_ProcessDmaBlock((uint32_t *)(psx_ram + (addr & 0x1FFFFC)), total_words);
            PROF_POP(PROF_GPU_PRIM);
            addr += (total_words * 4);

            /* ── DMA bus + GPU processing cycle cost ── */
            {
                uint64_t dma_cost = (uint64_t)total_words * DMA_CYCLES_PER_WORD;
                uint64_t gpu_cost = gpu_estimated_pixels * GPU_CYCLES_PER_PIXEL;
                gpu_estimated_pixels = 0;
                global_cycles += dma_cost;
                gpu_busy_until = global_cycles + gpu_cost;
                /* Dispatch pending scheduler events (HBlank, timers) */
                if (global_cycles >= scheduler_cached_earliest)
                    Scheduler_DispatchEvents(global_cycles);
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

            /* ── DMA bus cycle cost for GPU→CPU read ── */
            {
                uint64_t dma_cost = (uint64_t)total_words * DMA_CYCLES_PER_WORD;
                global_cycles += dma_cost;
                if (global_cycles >= scheduler_cached_earliest)
                    Scheduler_DispatchEvents(global_cycles);
            }
        }
        PROF_POP(PROF_GPU_DMA);
        return;
    }

    if (sync_mode == 2)
    {
        int packets = 0;
        int max_packets = 20000;
        uint32_t total_dma_words = 0; /* track total data words for cycle cost */

        while (packets < max_packets)
        {
            uint32_t packet_addr = addr;
            uint32_t header = *(uint32_t *)&psx_ram[addr];
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;
            total_dma_words += count + 1; /* +1 for the header word */

            if (count > 256)
            {
                DLOG("ERROR: Packet count too large (%" PRIu32 "). Aborting chain.\n", count);
                break;
            }

            addr = (addr + 4) & 0x1FFFFC;

            /* ── Polyline-active: rare slow path, word-by-word ── */
            if (polyline_active)
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    GPU_WriteGP0(*(uint32_t *)&psx_ram[addr]);
                    addr = (addr + 4) & 0x1FFFFC;
                }
            }
            else
            {
                /* ── Fast inner loop: direct pointer reads, no polyline check ── */
                for (uint32_t i = 0; i < count;)
                {
                    uint32_t *cmd_ptr = (uint32_t *)&psx_ram[addr];
                    uint32_t cmd_word = cmd_ptr[0];
                    uint32_t cmd_byte = cmd_word >> 24;

                    if (cmd_byte >= 0x20 && cmd_byte <= 0x7F)
                    {
                        /* Polyline lines: must go word-by-word for state machine */
                        if ((cmd_byte & 0xE8) == 0x48)
                        {
                            while (i < count)
                            {
                                GPU_WriteGP0(*(uint32_t *)&psx_ram[addr]);
                                i++;
                                addr = (addr + 4) & 0x1FFFFC;
                                if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                                    break;
                            }
                        }
                        else
                        {
                            /* Polygons, rects, non-polyline lines → fast translate */
                            PROF_PUSH(PROF_GPU_PRIM);
                            int size = Translate_GP0_to_GS(cmd_ptr);
                            PROF_POP(PROF_GPU_PRIM);
                            i += size;
                            addr = (addr + size * 4) & 0x1FFFFC;
                        }
                    }
                    else if (cmd_byte == 0x02)
                    {
                        /* Fill-rect → fast translate */
                        PROF_PUSH(PROF_GPU_PRIM);
                        int size = Translate_GP0_to_GS(cmd_ptr);
                        PROF_POP(PROF_GPU_PRIM);
                        i += size;
                        addr = (addr + size * 4) & 0x1FFFFC;
                    }
                    else if (cmd_byte == 0xA0)
                    {
                        uint32_t coords = cmd_ptr[1];
                        uint32_t dims = cmd_ptr[2];
                        uint32_t image_words = ((dims & 0xFFFF) * (dims >> 16)) / 2;

                        /* Fast path only if the entire image block fits in this packet */
                        if (3 + image_words <= count - i)
                        {
                            PROF_PUSH(PROF_GPU_UPLOAD);
                            GS_UploadRegionFast(coords, dims, (uint32_t *)(psx_ram + ((addr + 12) & 0x1FFFFC)), image_words);
                            PROF_POP(PROF_GPU_UPLOAD);
                            uint32_t skip = 3 + image_words;
                            i += skip;
                            addr = (addr + skip * 4) & 0x1FFFFC;
                        }
                        else
                        {
                            /* Fallback: fragmented upload (uncommon) */
                            GPU_WriteGP0(cmd_word);
                            i++;
                            addr = (addr + 4) & 0x1FFFFC;
                        }
                    }
                    else if ((cmd_byte & 0xE0) == 0x80)
                    {
                        /* VRAM-to-VRAM copy: fast path if all 4 words available */
                        if (i + 3 < count)
                        {
                            /* Feed all 4 words directly without looping */
                            GPU_WriteGP0(cmd_ptr[0]);
                            GPU_WriteGP0(cmd_ptr[1]);
                            GPU_WriteGP0(cmd_ptr[2]);
                            GPU_WriteGP0(cmd_ptr[3]);
                            i += 4;
                            addr = (addr + 16) & 0x1FFFFC;
                        }
                        else
                        {
                            /* Fallback: word-by-word */
                            GPU_WriteGP0(cmd_word);
                            i++;
                            addr = (addr + 4) & 0x1FFFFC;
                            while (i < count && gpu_cmd_remaining > 0)
                            {
                                GPU_WriteGP0(*(uint32_t *)&psx_ram[addr]);
                                i++;
                                addr = (addr + 4) & 0x1FFFFC;
                            }
                        }
                    }
                    else
                    {
                        /* E1-E6 env commands, NOP, etc. */
                        GPU_WriteGP0(cmd_word);
                        i++;
                        addr = (addr + 4) & 0x1FFFFC;
                    }
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

        /* ── DMA bus + GPU processing cycle cost for linked-list ── */
        {
            uint64_t dma_cost = (uint64_t)total_dma_words * DMA_CYCLES_PER_WORD + (uint64_t)packets * DMA_CYCLES_PER_PACKET;
            uint64_t gpu_cost = gpu_estimated_pixels * GPU_CYCLES_PER_PIXEL;
            gpu_estimated_pixels = 0;
            global_cycles += dma_cost;
            gpu_busy_until = global_cycles + gpu_cost;

            DLOG("DMA2 linked-list: %d packets, %lu words, %llu pixels, dma=%llu gpu=%llu\n",
                 packets, (unsigned long)total_dma_words,
                 (unsigned long long)(gpu_cost / GPU_CYCLES_PER_PIXEL),
                 (unsigned long long)dma_cost, (unsigned long long)gpu_cost);

            /* Dispatch pending scheduler events so HBlank/timers fire during
             * the DMA window — this is what makes Timer1 (HBlank) advance
             * while the GPU is working, enabling the benchmark to measure FPS. */
            while (global_cycles >= scheduler_cached_earliest)
                Scheduler_DispatchEvents(global_cycles);
        }
    }
    PROF_POP(PROF_GPU_DMA);
}
