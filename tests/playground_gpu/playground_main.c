/*
 * GPU Playground — Main Entry
 */
#include "playground_gpu.h"
#include <kernel.h>
#include <sifrpc.h>
#include <malloc.h>

/* Global results */
GPUPlaygroundResults gp_results;
GPTestCtx gp_ctx;
uint32_t mock_gif_qwords_written = 0;

/* Dummy RAM buffers */
static uint8_t dummy_vram[1024 * 512 * 2];

extern void Tex_Cache_Init(void);

void gp_reset_state(void)
{
    /* Reset our test metrics */
    memset(&gp_ctx, 0, sizeof(gp_ctx));
    mock_gif_qwords_written = 0;

    /* Reset global emulator mock state */
    gpu_stat = 0x14802000;
    gpu_read = 0;
    
    current_buffer = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;
    gif_buffer_end_safe = (gif_qword_t *)(MOCK_GIF_BUFFER_START + 2000);

    /* Point VRAM to our dummy allocation */
    psx_vram_shadow = (uint16_t *)dummy_vram;

    /* PSX RAM mock for gpu_dma.c */
    static uint8_t dummy_psx_ram[1024 * 1024 * 2];
    extern uint8_t *psx_ram;
    psx_ram = dummy_psx_ram;

    /* Invalidate GP0/GP1 caches to ensure full translation paths */
    Prim_InvalidateGSState();
    Prim_InvalidateTexCache();
    
    gpu_cmd_remaining = 0;
    gpu_transfer_words = 0;
    buf_image_ptr = 0;

    /* Reset texture state to 15BPP (no CLUT) so tests start clean */
    tex_page_format = 2;
    tex_page_x = 0;
    tex_page_y = 0;
}

int main(void)
{
    SifInitRpc(0);

    printf("====================================================================\n");
    printf("                  SuperPSX GPU Expansion Playground                 \n");
    printf("====================================================================\n\n");

    /* Initialize texture cache structure since gpu_commands/texture rely on it */
    Tex_Cache_Init();

    /* Performance counters need to be cleared */
    perf_start();
    uint32_t dummy_c, dummy_i;
    perf_stop(&dummy_c, &dummy_i);

    /* Run tests */
    gp_run_expansion_tests();
    gp_run_expansion_gp1_tests();
    gp_run_clut_tests();

    printf("\n====================================================================\n");
    printf("Test Results: %d passed, %d failed (Total %d)\n",
           gp_results.passed, gp_results.failed, gp_results.total);
    printf("====================================================================\n");

    SleepThread();
    return gp_results.failed > 0 ? 1 : 0;
}
