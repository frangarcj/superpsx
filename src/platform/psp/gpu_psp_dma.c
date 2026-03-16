/**
 * gpu_psp_dma.c — PSP GPU DMA handler
 *
 * Equivalent to gpu_ps2_dma.c on the PS2 platform.
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "profiler.h"

extern uint8_t *psx_ram;

/* Forward declarations from shared code */
extern void GPU_ProcessDmaBlock(uint32_t *data, uint32_t words);
extern uint32_t GPU_Read(void);

int GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t addr = madr & 0x1FFFFC;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t direction = chcr & 1;

    PROF_PUSH(PROF_GPU_DMA);

    if (sync_mode == 0 || sync_mode == 1) {
        if (direction == 1) { /* CPU → GPU */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            PROF_PUSH(PROF_GPU_PRIM);
            GPU_ProcessDmaBlock((uint32_t *)(psx_ram + addr), words);
            PROF_POP(PROF_GPU_PRIM);
        } else { /* GPU → CPU (VRAM Read) */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            for (uint32_t i = 0; i < words; i++) {
                uint32_t word = GPU_Read();
                *(uint32_t *)(psx_ram + ((addr + i * 4) & 0x1FFFFC)) = word;
            }
        }
    } else if (sync_mode == 2) { /* Linked List */
        uint32_t max_packets = 20000;
        uint32_t packets = 0;

        while (packets < max_packets) {
            uint32_t header = *(uint32_t *)&psx_ram[addr];
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;

            if (count > 0) {
                PROF_PUSH(PROF_GPU_PRIM);
                GPU_ProcessDmaBlock((uint32_t *)&psx_ram[(addr + 4) & 0x1FFFFC], count);
                PROF_POP(PROF_GPU_PRIM);
            }

            if (next == 0xFFFFFF) break;
            addr = next & 0x1FFFFC;
            packets++;
        }
    }

    PROF_POP(PROF_GPU_DMA);
    return 0;
}
