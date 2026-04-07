/**
 * gpu_dma.c — DMA Channel 2 (GPU) handling
 *
 * Processes PSX DMA channel 2 transfers: continuous / block mode writes,
 * GPU→CPU reads, and linked-list mode (the most common path used by
 * PSX games to submit display lists).
 */
#include "gpu_ps2_state.h"
#include "gpu_trace.h"
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
/* Returns 0 if transfer completed normally, 1 if stalled (e.g. linked list loop). */
int GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t addr = madr & 0x1FFFFC;
    if ((chcr & 0x01000000) == 0)
        return 0;

    PROF_PUSH(PROF_GPU_DMA);

    /* Reset pixel accumulator for this DMA batch */
    gpu_estimated_pixels = 0;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t direction = chcr & 1;

    // Flush any pending GIF data from direct GP0 writes before starting DMA
    if (fast_gif_ptr != (gif_qword_t *)&gif_packet_buf[current_buffer][0])
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
                if (global_cycles >= sched_cached_earliest)
                    Sched_Tick(global_cycles);
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
                if (global_cycles >= sched_cached_earliest)
                    Sched_Tick(global_cycles);
            }
        }
        PROF_POP(PROF_GPU_DMA);
        return 0;
    }

    if (sync_mode == 2)
    {
        int packets = 0;
        int max_packets = 20000;
        uint32_t total_dma_words = 0; /* track total data words for cycle cost */
        int chain_completed = 0; /* set to 1 when 0xFFFFFF terminator reached */
        uint32_t start_addr = addr; /* for loop detection */

        while (packets < max_packets)
        {
            uint32_t packet_addr = addr;
            uint32_t header = *(uint32_t *)&psx_ram[addr];
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;
            total_dma_words += count + 1; /* +1 for the header word */

            /* Prefetch next packet header while we process the current one.
             * Linked-list traversal is pointer-chasing — each header read
             * is a cache miss on the R5900's 8KB L1 dcache. Issuing pref
             * here gives ~50-100 cycles for the load to complete in the
             * background while we process the current packet. */
            if (next != 0xFFFFFF && !(next & 0x3))
                __builtin_prefetch(&psx_ram[next & 0x1FFFFC], 0, 3);

            if (count > 256)
            {
                DLOG("ERROR: Packet count too large (%" PRIu32 "). Aborting chain.\n", count);
                break;
            }

            /* Record raw GP0 words for offline trace analysis */
            if (count > 0)
                gpu_trace_record((uint32_t *)&psx_ram[(addr + 4) & 0x1FFFFC], count);

            addr = (addr + 4) & 0x1FFFFC;

            /* ── Polyline-active: rare slow path, word-by-word ── */
            if (polyline_active)
            {
                Prim_FlushBatch();
                for (uint32_t i = 0; i < count; i++)
                {
                    GPU_WriteGP0(*(uint32_t *)&psx_ram[addr]);
                    addr = (addr + 4) & 0x1FFFFC;
                }
            }
            else
            {
                /* ── Fast inner loop: table-driven dispatch ── */
                for (uint32_t i = 0; i < count;)
                {
                    uint32_t *cmd_ptr = (uint32_t *)&psx_ram[addr];
                    uint32_t cmd_word = cmd_ptr[0];
                    uint32_t cmd_byte = cmd_word >> 24;
                    uint8_t cmd_size = gpu_cmd_size[cmd_byte];

                    /* ── Variable-length commands (polylines, LoadImage, StoreImage) ── */
                    if (cmd_size == 0)
                    {
                        Prim_FlushBatch();
                        if (cmd_byte == 0xA0)
                        {
                            /* LoadImage: fast path if entire block fits in packet */
                            uint32_t dims = cmd_ptr[2];
                            uint32_t image_words = ((dims & 0xFFFF) * (dims >> 16)) / 2;
                            if (3 + image_words <= count - i)
                            {
                                PROF_PUSH(PROF_GPU_UPLOAD);
                                GS_UploadRegionFast(cmd_ptr[1], dims,
                                                    (uint32_t *)(psx_ram + ((addr + 12) & 0x1FFFFC)),
                                                    image_words);
                                PROF_POP(PROF_GPU_UPLOAD);
                                uint32_t skip = 3 + image_words;
                                i += skip;
                                addr = (addr + skip * 4) & 0x1FFFFC;
                                continue;
                            }
                        }
                        /* Polylines / fragmented LoadImage / StoreImage → word-by-word */
                        while (i < count)
                        {
                            GPU_WriteGP0(*(uint32_t *)&psx_ram[addr]);
                            i++;
                            addr = (addr + 4) & 0x1FFFFC;
                            if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                                break;
                        }
                        continue;
                    }

                    /* ── Draw commands: polys, rects, lines, fill-rect (0x02-0x7F) ── */
                    if (cmd_byte <= 0x7F)
                    {
                        int size = GPU_TryBatchAdd(cmd_ptr);
                        if (size > 0)
                        {
                            i += size;
                            addr = (addr + size * 4) & 0x1FFFFC;
                            continue;
                        }
                        Prim_FlushBatch();
                        size = GPU_TryFastEmit(cmd_ptr);
                        if (size <= 0)
                        {
                            PROF_PUSH(PROF_GPU_PRIM);
                            size = Translate_GP0_to_GS(cmd_ptr);
                            PROF_POP(PROF_GPU_PRIM);
                        }
                        i += size;
                        addr = (addr + size * 4) & 0x1FFFFC;
                        continue;
                    }

                    /* ── VRAM-to-VRAM copy (0x80-0x9F): 4 words ── */
                    if ((cmd_byte & 0xE0) == 0x80)
                    {
                        Prim_FlushBatch();
                        if (i + 4 <= count)
                        {
                            GPU_WriteGP0(cmd_ptr[0]);
                            GPU_WriteGP0(cmd_ptr[1]);
                            GPU_WriteGP0(cmd_ptr[2]);
                            GPU_WriteGP0(cmd_ptr[3]);
                            i += 4;
                            addr = (addr + 16) & 0x1FFFFC;
                        }
                        else
                        {
                            GPU_WriteGP0(cmd_word);
                            i++;
                            addr = (addr + 4) & 0x1FFFFC;
                        }
                        continue;
                    }

                    /* ── E1-E6 env commands, NOP, etc. ── */
                    Prim_FlushBatch();
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr = (addr + 4) & 0x1FFFFC;
                }
            }

            packets++;

            if (next == 0xFFFFFF) {
                chain_completed = 1;
                break;
            }

            if (next == packet_addr || (next & 0x1FFFFC) == start_addr)
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

        Prim_FlushBatch();

        if (fast_gif_ptr != (gif_qword_t *)&gif_packet_buf[current_buffer][0])
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
            while (global_cycles >= sched_cached_earliest)
                Sched_Tick(global_cycles);
        }

        PROF_POP(PROF_GPU_DMA);
        return chain_completed ? 0 : 1;
    }
    PROF_POP(PROF_GPU_DMA);
    return 0;
}
