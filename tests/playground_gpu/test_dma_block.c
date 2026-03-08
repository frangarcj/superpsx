/*
 * GPU Playground — DMA Block Dispatch Tests
 *
 * Verifies that GPU_ProcessDmaBlock() produces identical GIF output
 * to word-at-a-time GPU_WriteGP0(), and measures block dispatch performance.
 */
#include "playground_gpu.h"

/* ================================================================
 *  Helper: EMIT_DMA_BLOCK — feed entire command array via block API
 * ================================================================ */
#define EMIT_DMA_BLOCK(arr) do { \
    uint32_t _cycles, _insns; \
    perf_start(); \
    GPU_ProcessDmaBlock((arr), sizeof(arr)/sizeof(arr[0])); \
    perf_stop(&_cycles, &_insns); \
    gp_ctx.eecycles_used += _cycles; \
    gp_ctx.eeinsns_used += _insns; \
} while(0)

/* ================================================================
 *  Test 1: Single flat triangle via DMA block
 *
 *  Verify GIF output matches word-at-a-time (9 QWORDs).
 * ================================================================ */
static void test_dma_block_flat_tri(void)
{
    /* --- Reference: word-at-a-time --- */
    BEGIN_GPU_TEST("blk_tri_ref");
    EMIT_GP0(0x200000FF);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    /* --- DMA block path --- */
    BEGIN_GPU_TEST("blk_tri");
    uint32_t block[] = {
        0x200000FF,
        10 | (10 << 16),
        50 | (10 << 16),
        10 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(800);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 2: Single flat quad via DMA block
 * ================================================================ */
static void test_dma_block_flat_quad(void)
{
    BEGIN_GPU_TEST("blk_quad_ref");
    EMIT_GP0(0x2800FF00);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(50 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_quad");
    uint32_t block[] = {
        0x2800FF00,
        10 | (10 << 16),
        50 | (10 << 16),
        10 | (50 << 16),
        50 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(950);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 3: Two back-to-back flat triangles in one DMA block
 *
 *  Second triangle should hit fast path (gs_state.valid).
 * ================================================================ */
static void test_dma_block_two_tris(void)
{
    /* Reference */
    BEGIN_GPU_TEST("blk_2tri_ref");
    EMIT_GP0(0x200000FF);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0x200000FF);
    EMIT_GP0(60 | (60 << 16));
    EMIT_GP0(90 | (60 << 16));
    EMIT_GP0(60 | (90 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_2tri");
    uint32_t block[] = {
        0x200000FF,
        10 | (10 << 16), 50 | (10 << 16), 10 | (50 << 16),
        0x200000FF,
        60 | (60 << 16), 90 | (60 << 16), 60 | (90 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    /* Two triangles via block should be faster than 2x word-at-a-time */
    EXPECT_CYCLES(1100);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 4: E1 + textured triangle in one DMA block
 *
 *  Tests mixed env+draw command sequence in a single block.
 * ================================================================ */
static void test_dma_block_e1_plus_tri(void)
{
    /* Reference */
    BEGIN_GPU_TEST("blk_e1tri_ref");
    EMIT_GP0(0xE1000200);  /* E1: set 15bpp mode */
    EMIT_GP0(0x200000FF);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_e1tri");
    uint32_t block[] = {
        0xE1000200,
        0x200000FF,
        10 | (10 << 16), 50 | (10 << 16), 10 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(1000);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 5: E5 (draw offset) + E3 + E4 + flat quad — env burst
 *
 *  Verify environment commands processed correctly in block mode.
 * ================================================================ */
static void test_dma_block_env_burst(void)
{
    BEGIN_GPU_TEST("blk_env_ref");
    EMIT_GP0(0xE5000000 | (10 << 0) | (20 << 11));  /* offset (10, 20) */
    EMIT_GP0(0xE3000000 | (0 << 0)  | (0  << 10));   /* clip TL (0,0) */
    EMIT_GP0(0xE4000000 | (256 << 0) | (240 << 10)); /* clip BR (256,240) */
    EMIT_GP0(0x2800FFFF);  /* flat quad yellow */
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(50 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_env");
    uint32_t block[] = {
        0xE5000000 | (10 << 0) | (20 << 11),
        0xE3000000 | (0 << 0) | (0 << 10),
        0xE4000000 | (256 << 0) | (240 << 10),
        0x2800FFFF,
        10 | (10 << 16), 50 | (10 << 16),
        10 | (50 << 16), 50 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(1500);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 6: Fill rect (0x02) via DMA block
 * ================================================================ */
static void test_dma_block_fill_rect(void)
{
    BEGIN_GPU_TEST("blk_fill_ref");
    EMIT_GP0(0x02FF0000);  /* fill red */
    EMIT_GP0(0x00000000);  /* (0,0) */
    EMIT_GP0(0x00F000F0);  /* 240x240 */
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_fill");
    uint32_t block[] = {
        0x02FF0000,
        0x00000000,
        0x00F000F0,
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(80000);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 7: Gouraud triangle (0x30) via DMA block — 7 words
 * ================================================================ */
static void test_dma_block_gouraud_tri(void)
{
    BEGIN_GPU_TEST("blk_gour_ref");
    EMIT_GP0(0x30FF0000);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0x3000FF00);
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(0x300000FF);
    EMIT_GP0(30 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_gour");
    uint32_t block[] = {
        0x30FF0000, 10 | (10 << 16),
        0x3000FF00, 50 | (10 << 16),
        0x300000FF, 30 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(850);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 8: NOP + flat tri — verify NOP (0x00) doesn't confuse block
 * ================================================================ */
static void test_dma_block_nop_plus_tri(void)
{
    BEGIN_GPU_TEST("blk_nop_ref");
    EMIT_GP0(0x00000000);
    EMIT_GP0(0x200000FF);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_nop");
    uint32_t block[] = {
        0x00000000,
        0x200000FF,
        10 | (10 << 16), 50 | (10 << 16), 10 | (50 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    EXPECT_CYCLES(950);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 9: mixed burst — 3 flat tris + E5 + 1 flat quad
 *
 *  Longer sequence to measure aggregate block dispatch benefit.
 * ================================================================ */
static void test_dma_block_mixed_burst(void)
{
    BEGIN_GPU_TEST("blk_mix_ref");
    EMIT_GP0(0x200000FF);
    EMIT_GP0(10 | (10 << 16)); EMIT_GP0(50 | (10 << 16)); EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0x2000FF00);
    EMIT_GP0(60 | (10 << 16)); EMIT_GP0(100 | (10 << 16)); EMIT_GP0(60 | (50 << 16));
    EMIT_GP0(0x20FF0000);
    EMIT_GP0(110 | (10 << 16)); EMIT_GP0(150 | (10 << 16)); EMIT_GP0(110 | (50 << 16));
    EMIT_GP0(0xE5000000 | (5 << 0) | (5 << 11));
    EMIT_GP0(0x28FFFFFF);
    EMIT_GP0(10 | (60 << 16)); EMIT_GP0(50 | (60 << 16));
    EMIT_GP0(10 | (100 << 16)); EMIT_GP0(50 | (100 << 16));
    Flush_GIF();
    uint32_t ref_qw = gp_ctx.qwords_generated;
    END_GPU_TEST();

    BEGIN_GPU_TEST("blk_mix");
    uint32_t block[] = {
        0x200000FF,
        10 | (10 << 16), 50 | (10 << 16), 10 | (50 << 16),
        0x2000FF00,
        60 | (10 << 16), 100 | (10 << 16), 60 | (50 << 16),
        0x20FF0000,
        110 | (10 << 16), 150 | (10 << 16), 110 | (50 << 16),
        0xE5000000 | (5 << 0) | (5 << 11),
        0x28FFFFFF,
        10 | (60 << 16), 50 | (60 << 16),
        10 | (100 << 16), 50 | (100 << 16),
    };
    EMIT_DMA_BLOCK(block);
    Flush_GIF();
    EXPECT_QWORDS(ref_qw);
    /* 5 primitives + 1 env via block should be significantly faster */
    EXPECT_CYCLES(2000);
    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_dma_block_tests(void)
{
    printf("\n--- DMA Block Dispatch Tests ---\n");
    test_dma_block_flat_tri();
    test_dma_block_flat_quad();
    test_dma_block_two_tris();
    test_dma_block_e1_plus_tri();
    test_dma_block_env_burst();
    test_dma_block_fill_rect();
    test_dma_block_gouraud_tri();
    test_dma_block_nop_plus_tri();
    test_dma_block_mixed_burst();
}
