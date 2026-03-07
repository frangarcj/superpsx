/*
 * GPU Playground — Expansion Ratio Tests
 *
 * Measures the code expansion ratio (EE CPU cycles and GS QWORDs)
 * per PSX GP0 instruction.
 */
#include "playground_gpu.h"

/* ================================================================
 *  Test 1: Flat Triangle (0x20)
 * ================================================================ */
static void test_expansion_flat_triangle(void)
{
    BEGIN_GPU_TEST("flat_triangle");

    /* 0x20RRGGBB, x0, y0, x1, y1, x2, y2 */
    EMIT_GP0(0x200000FF); /* Red triangle */
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));

    /* Ensure we flush any batched commands to measure final qwords */
    Flush_GIF();

    /* Target baselines based on current performance: 
     * Expect 37 cycles per triangle, and 9 GS QWORDs */
    EXPECT_QWORDS(9); 
    EXPECT_CYCLES(900); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 1b: Flat Triangle Fast Path (0x20) consecutive
 * ================================================================ */
static void test_expansion_flat_tri_fast(void)
{
    BEGIN_GPU_TEST("flat_tri_fast");

    /* Triangle 1: Takes slow path to establish lazy state (valid=1, dthe=0) */
    EMIT_GP0(0x200000FF); 
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(10 | (50 << 16));

    /* Triangle 2: Takes inline fast path avoiding Translate_GP0_to_GS entirely */
    EMIT_GP0(0x200000FF);
    EMIT_GP0(60 | (60 << 16));
    EMIT_GP0(90 | (60 << 16));
    EMIT_GP0(60 | (90 << 16));

    Flush_GIF();

    /* Expected QWORDs: 9 for the first triangle + 8 for the second (A+D) = 17 */
    EXPECT_QWORDS(17); 
    /* Expected Cycles: ~770 for the first + ~520 for the second = ~1300 */
    EXPECT_CYCLES(1400); 

    END_GPU_TEST();
}

/* ================================================================
 *  Test 2: Textured Quad (0x2C)
 * ================================================================ */
static void test_expansion_textured_quad(void)
{
    BEGIN_GPU_TEST("textured_quad");

    /* Requires valid texpage setup first so TexCache lookup doesn't abort */
    SETUP_GP1(0x00000000); // GP1(00) Reset GPU
    SETUP_GP0(0xE1000000 | (0 << 0) | (2 << 7)); // TP(0,0), 15-bit (direct mapped, no clut needed)

    /* 0x2CRRGGBB, x0,y0, u0,v0,clut, x1,y1, u1,v1,tpage, x2,y2, u2,v2, x3,y3, u3,v3 */
    EMIT_GP0(0x2C808080); /* Neutral color */
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0  | (0  << 8) | (0 << 16)); /* u0, v0, clut */
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(32 | (0  << 8) | ((2 << 7) << 16)); /* u1, v1, tpage (format=2=15BPP) */
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0  | (32 << 8)); /* u2, v2 */
    EMIT_GP0(50 | (50 << 16));
    EMIT_GP0(32 | (32 << 8)); /* u3, v3 */

    Flush_GIF();

    /* Textured quad max expected: Generous 2000 cycles for new run baseline */
    EXPECT_QWORDS(38); 
    EXPECT_CYCLES(2000);

    END_GPU_TEST();
}

/* ================================================================
 *  Test 3: GP0(E1) Texpage State Change
 * ================================================================ */
static void test_expansion_e1_texpage(void)
{
    BEGIN_GPU_TEST("e1_texpage");

    /* Initial state */
    SETUP_GP1(0x00000000); 

    /* Send E1 twice: 
     * 1st time should do a full setup.
     * 2nd time should hit the identical parameter cache and skip emitting. */
    EMIT_GP0(0xE1000000 | (1 << 0) | (2 << 7)); /* Change to TP=(64,0) */
    
    Flush_GIF();

    EXPECT_QWORDS(13);
    EXPECT_CYCLES(200);

    END_GPU_TEST();
}

/* ================================================================
 *  Test 4: NOP / System (0x00, 0x01, 0x02)
 * ================================================================ */
static void test_expansion_nop_fill(void)
{
    BEGIN_GPU_TEST("nop");
    EMIT_GP0(0x00000000); // NOP
    Flush_GIF();
    EXPECT_QWORDS(0); EXPECT_CYCLES(80);
    END_GPU_TEST();

    BEGIN_GPU_TEST("clear_cache");
    EMIT_GP0(0x01000000); // Clear cache
    Flush_GIF();
    EXPECT_QWORDS(0); EXPECT_CYCLES(80);
    END_GPU_TEST();

    BEGIN_GPU_TEST("fill_rect");
    EMIT_GP0(0x02808080); // Fill rect with gray
    EMIT_GP0(0 | (0 << 16)); // x, y
    EMIT_GP0(10 | (10 << 16)); // w, h
    Flush_GIF();
    EXPECT_QWORDS(8); EXPECT_CYCLES(900);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 5: Primitive Geometry (0x28, 0x30, 0x34, 0x38, 0x3C)
 * ================================================================ */
static void test_expansion_shaded_geom(void)
{
    BEGIN_GPU_TEST("flat_quad");
    EMIT_GP0(0x280000FF);
    EMIT_GP0(0 | (0<<16)); EMIT_GP0(10 | (0<<16)); EMIT_GP0(0 | (10<<16)); EMIT_GP0(10 | (10<<16));
    Flush_GIF();
    EXPECT_QWORDS(17); EXPECT_CYCLES(1131);
    END_GPU_TEST();

    BEGIN_GPU_TEST("shaded_tri");
    EMIT_GP0(0x300000FF); EMIT_GP0(0|(0<<16)); 
    EMIT_GP0(0x0000FF00); EMIT_GP0(10|(0<<16));
    EMIT_GP0(0x00FF0000); EMIT_GP0(0|(10<<16));
    Flush_GIF();
    EXPECT_QWORDS(9); EXPECT_CYCLES(1050);
    END_GPU_TEST();

    BEGIN_GPU_TEST("shaded_quad");
    EMIT_GP0(0x380000FF); EMIT_GP0(0|(0<<16)); 
    EMIT_GP0(0x0000FF00); EMIT_GP0(10|(0<<16));
    EMIT_GP0(0x00FF0000); EMIT_GP0(0|(10<<16));
    EMIT_GP0(0x00FFFFFF); EMIT_GP0(10|(10<<16));
    Flush_GIF();
    EXPECT_QWORDS(17); EXPECT_CYCLES(1331);
    END_GPU_TEST();

    
    BEGIN_GPU_TEST("shaded_tex_quad");
    /* Needs texpage to not crash */
    SETUP_GP1(0x00000000); SETUP_GP0(0xE1000000 | (0 << 0) | (2 << 7));
    EMIT_GP0(0x3C0000FF); EMIT_GP0(0|(0<<16));  EMIT_GP0(0|(0<<8));
    EMIT_GP0(0x0000FF00); EMIT_GP0(10|(0<<16)); EMIT_GP0(32|(0<<8)|((2<<7)<<16));
    EMIT_GP0(0x00FF0000); EMIT_GP0(0|(10<<16)); EMIT_GP0(0|(32<<8));
    EMIT_GP0(0x00FFFFFF); EMIT_GP0(10|(10<<16)); EMIT_GP0(32|(32<<8));
    Flush_GIF();
    EXPECT_QWORDS(30); EXPECT_CYCLES(1999);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 6: Lines and Polylines (0x40, 0x48, 0x50, 0x58)
 * ================================================================ */
static void test_expansion_lines(void)
{
    BEGIN_GPU_TEST("flat_line");
    EMIT_GP0(0x400000FF); EMIT_GP0(0|(0<<16)); EMIT_GP0(10|(10<<16));
    Flush_GIF();
    EXPECT_QWORDS(6); EXPECT_CYCLES(510);
    END_GPU_TEST();

    BEGIN_GPU_TEST("poly_line");
    EMIT_GP0(0x480000FF); EMIT_GP0(0|(0<<16)); EMIT_GP0(10|(0<<16));
    EMIT_GP0(10|(10<<16)); EMIT_GP0(0x55555555); // Termination
    Flush_GIF();
    EXPECT_QWORDS(12); EXPECT_CYCLES(830);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 7: Rectangles (0x60, 0x64, 0x68, 0x70, 0x74, 0x78)
 * ================================================================ */
static void test_expansion_rects(void)
{
    BEGIN_GPU_TEST("flat_rect_var"); // Variable size
    EMIT_GP0(0x600000FF); EMIT_GP0(0|(0<<16)); EMIT_GP0(10|(10<<16));
    Flush_GIF();
    EXPECT_QWORDS(7); EXPECT_CYCLES(520);
    END_GPU_TEST();

    BEGIN_GPU_TEST("flat_rect_8x8");
    EMIT_GP0(0x680000FF); EMIT_GP0(0|(0<<16));
    Flush_GIF();
    EXPECT_QWORDS(7); EXPECT_CYCLES(460);
    END_GPU_TEST();

    BEGIN_GPU_TEST("tex_rect_dummy");
    SETUP_GP1(0x00000000); SETUP_GP0(0xE1000000 | (0 << 0) | (1 << 7));
    EMIT_GP0(0x74808080); EMIT_GP0(0); EMIT_GP0(0); EMIT_GP0(1);
    Flush_GIF();
    EXPECT_QWORDS(4200); EXPECT_CYCLES(200000); /* initial page upload: ~4100 QWORDs */
    END_GPU_TEST();

    BEGIN_GPU_TEST("tex_rect_var");
    /* 8BPP textured rect with NON-aligned CLUT.
     * clut_x=17*16=272, byte_offset=544, 544%256=32 -> CSM1 CLUT upload */
    SETUP_GP1(0x00000000); SETUP_GP0(0xE1000000 | (0 << 0) | (1 << 7));
    EMIT_GP0(0x74808080);
    EMIT_GP0(0 | (0 << 16));
    EMIT_GP0(0 | (0 << 8) | (17 << 16)); /* pal=(17*16, 0) -> (272,0) */
    EMIT_GP0(10 | (10 << 16));
    Flush_GIF();
    EXPECT_QWORDS(64); EXPECT_CYCLES(200000); /* page hit + CSM1 CLUT upload */
    END_GPU_TEST();
    
    BEGIN_GPU_TEST("tex_rect_aligned");
    /* 8BPP textured rect with ALIGNED CLUT (CSM2 bypass).
     * clut_x=128, byte_offset=256, 256%256=0 -> CSM2: no CLUT upload! */
    SETUP_GP1(0x00000000); SETUP_GP0(0xE1000000 | (0 << 0) | (1 << 7));
    EMIT_GP0(0x74808080);
    EMIT_GP0(0 | (0 << 16));
    EMIT_GP0(0 | (0 << 8) | (8 << 16)); /* pal=(8*16, 0) -> (128,0) */
    EMIT_GP0(10 | (10 << 16));
    Flush_GIF();
    EXPECT_QWORDS(64); /* CSM2 bypass: HW CLUT path via CSM1 + page upload */
    EXPECT_CYCLES(200000);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 8: VRAM Transfers (0x80, 0xA0, 0xC0)
 * ================================================================ */
static void test_expansion_vram_transfers(void)
{
    BEGIN_GPU_TEST("vram_copy");
    EMIT_GP0(0x80000000); EMIT_GP0(0|(0<<16)); /* src */ EMIT_GP0(10|(10<<16)); /* dst */ EMIT_GP0(5|(5<<16));
    Flush_GIF();
    EXPECT_QWORDS(0); EXPECT_CYCLES(1100);
    END_GPU_TEST();

    BEGIN_GPU_TEST("cpu_to_vram"); // Image Load
    EMIT_GP0(0xA0000000); EMIT_GP0(0|(0<<16)); EMIT_GP0(2|(1<<16)); /* 2x1 = 2 words */
    EMIT_GP0(0x11111111); EMIT_GP0(0x22222222);
    Flush_GIF();
    EXPECT_QWORDS(4); EXPECT_CYCLES(900);
    END_GPU_TEST();

    BEGIN_GPU_TEST("vram_to_cpu"); // Image Store
    EMIT_GP0(0xC0000000); EMIT_GP0(0|(0<<16)); EMIT_GP0(2|(1<<16));
    // The emulator would now wait for GPU_Read(), let's not block but test parsing cycles
    Flush_GIF();
    EXPECT_QWORDS(0); EXPECT_CYCLES(4500);
    END_GPU_TEST();
}

/* ================================================================
 *  Test 9: Environment/State (0xE2, 0xE3, 0xE4, 0xE5, 0xE6)
 * ================================================================ */
static void test_expansion_env(void)
{
    BEGIN_GPU_TEST("e2_tex_window");
    EMIT_GP0(0xE2000000 | (15 << 0) | (15 << 5) | (0 << 10) | (0 << 15));
    Flush_GIF();
    EXPECT_QWORDS(0); EXPECT_CYCLES(130);
    END_GPU_TEST();

    BEGIN_GPU_TEST("e3_draw_area_tl");
    EMIT_GP0(0xE3000000 | (0 << 0) | (0 << 10));
    EXPECT_QWORDS(0); EXPECT_CYCLES(170);
    END_GPU_TEST();

    BEGIN_GPU_TEST("e4_draw_area_br");
    EMIT_GP0(0xE4000000 | (256 << 0) | (240 << 10));
    EXPECT_QWORDS(0); EXPECT_CYCLES(170);
    END_GPU_TEST();

    BEGIN_GPU_TEST("e5_draw_offset");
    EMIT_GP0(0xE5000000 | (0 << 0) | (0 << 11));
    Flush_GIF(); // Environment commands usually trigger flushes
    EXPECT_QWORDS(0); EXPECT_CYCLES(125);
    END_GPU_TEST();

    BEGIN_GPU_TEST("e6_mask_bit");
    EMIT_GP0(0xE6000000 | (1 << 0) | (0 << 1));
    Flush_GIF();
    EXPECT_QWORDS(3); EXPECT_CYCLES(160);
    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_expansion_tests(void)
{
    test_expansion_flat_triangle();
    test_expansion_flat_tri_fast();
    test_expansion_textured_quad();
    test_expansion_e1_texpage();
    
    test_expansion_nop_fill();
    test_expansion_shaded_geom();
    test_expansion_lines();
    test_expansion_rects();
    test_expansion_vram_transfers();
    test_expansion_env();
}
