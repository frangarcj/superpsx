/*
 * GPU Playground — GP1 Expansion Tests
 *
 * Measures the code expansion ratio (EE CPU cycles and GS QWORDs)
 * per PSX GP1 instruction (Display, Reset, Env).
 */
#include "playground_gpu.h"

/* ================================================================
 *  Test 1: GP1(00) - Reset GPU
 * ================================================================ */
static void test_gp1_reset_gpu(void)
{
    BEGIN_GPU_TEST("gp1_reset_gpu");

    /* 0x00000000: Reset GPU (clears state, VRAM shadow, sets default disp) */
    EMIT_GP1(0x00000000); 

    /* Expect 8 QWORDs (clearing GS state) and high cycles due to memset of shadow vram */
    EXPECT_QWORDS(8); 
    EXPECT_CYCLES(656500);

    END_GPU_TEST();
}

/* ================================================================
 *  Test 2: GP1(01) - Reset Command Buffer
 * ================================================================ */
static void test_gp1_reset_cmd_buf(void)
{
    BEGIN_GPU_TEST("gp1_reset_cmdb");

    /* Send some partial command to ensure it resets it */
    SETUP_GP0(0x280000FF);
    SETUP_GP0(0 | (0<<16)); 

    EMIT_GP1(0x01000000);

    /* Nothing sent to GS */
    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(47); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 3: GP1(02) - Acknowledge IRQ (GPUSTAT.24)
 * ================================================================ */
static void test_gp1_ack_irq(void)
{
    BEGIN_GPU_TEST("gp1_ack_irq");

    /* First, trigger an IRQ */
    SETUP_GP0(0x1F000000);

    /* Now Ack it */
    EMIT_GP1(0x02000000);

    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(48); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 4: GP1(03) - Display Enable
 * ================================================================ */
static void test_gp1_display_enable(void)
{
    BEGIN_GPU_TEST("gp1_disp_en");

    EMIT_GP1(0x03000000); // Enable

    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(50); 

    BEGIN_GPU_TEST("gp1_disp_dis");

    EMIT_GP1(0x03000001); // Disable

    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(50); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 5: GP1(04) - DMA Direction
 * ================================================================ */
static void test_gp1_dma_dir(void)
{
    BEGIN_GPU_TEST("gp1_dma_dir");

    EMIT_GP1(0x04000002); // DMA 2 (CPU to VRAM)

    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(53); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 6: GP1(08) - Display Mode
 * ================================================================ */
static void test_gp1_disp_mode(void)
{
    BEGIN_GPU_TEST("gp1_disp_mode");

    EMIT_GP1(0x08000000); // NTSC, 256x240, etc

    EXPECT_QWORDS(0); 
    EXPECT_CYCLES(79); 

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_expansion_gp1_tests(void)
{
    test_gp1_reset_gpu();
    test_gp1_reset_cmd_buf();
    test_gp1_ack_irq();
    test_gp1_display_enable();
    test_gp1_dma_dir();
    test_gp1_disp_mode();
}
