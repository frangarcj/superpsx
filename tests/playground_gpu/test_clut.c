/*
 * GPU Playground — CLUT / Texture Cache Correctness Tests
 *
 * Tests for palette (CLUT) handling correctness:
 * - CLUT upload + invalidation on VRAM write
 * - prim_tex_cache hit/miss behavior
 * - CLD (CLUT Load Disable) correctness
 * - Round-robin CLUT allocator behavior
 *
 * All tests use the mock framework — GIF packets are measured but not
 * sent to real GS hardware.
 */
#include "playground_gpu.h"
#include <string.h>

/* Access internals for verification */
extern uint32_t vram_gen_counter;
extern void Tex_Cache_Init(void);

/* ================================================================
 *  Helper: Write a 4BPP CLUT (16 entries) to shadow VRAM
 * ================================================================ */
static void write_clut_4bpp(int clut_x, int clut_y, uint16_t base_color)
{
    if (!psx_vram_shadow) return;
    for (int i = 0; i < 16; i++)
        psx_vram_shadow[clut_y * 1024 + clut_x + i] = base_color + i;
}

/* ================================================================
 *  Helper: Write an 8BPP CLUT (256 entries) to shadow VRAM
 * ================================================================ */
static void write_clut_8bpp(int clut_x, int clut_y, uint16_t base_color)
{
    if (!psx_vram_shadow) return;
    for (int i = 0; i < 256; i++)
        psx_vram_shadow[clut_y * 1024 + clut_x + i] = base_color + (i & 0x7FFF);
}

/* ================================================================
 *  Helper: Write fake texture page data to shadow VRAM
 * ================================================================ */
static void write_texpage_data(int page_x, int page_y)
{
    if (!psx_vram_shadow) return;
    /* Fill 256x256 region with pattern data */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 128; x++)  /* 128 halfwords = 256 bytes = 256 4BPP texels */
            psx_vram_shadow[(page_y + y) * 1024 + page_x + x] = (uint16_t)(x + y * 128);
}

/* ================================================================
 *  Helper: Emit a textured quad (0x2C) with given CLUT
 *  Sets up E1 texpage, then emits the quad.
 * ================================================================ */
static void emit_textured_quad_4bpp(int page_tx, int page_ty, int clut_x, int clut_y)
{
    /* E1: Set texture page — format=0 (4BPP) */
    uint32_t e1_val = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7); /* 4BPP */
    EMIT_GP0(0xE1000000 | e1_val);

    /* 0x2C textured quad: color, (x0,y0), (u0,v0,clut), (x1,y1), (u1,v1,tpage), ... */
    uint32_t clut_word = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    uint32_t tpage_word = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7);

    EMIT_GP0(0x2C808080);               /* Neutral modulation color */
    EMIT_GP0(10 | (10 << 16));           /* v0: x=10, y=10 */
    EMIT_GP0(0 | (0 << 8) | (clut_word << 16));  /* u0=0, v0=0, clut */
    EMIT_GP0(50 | (10 << 16));           /* v1: x=50, y=10 */
    EMIT_GP0(32 | (0 << 8) | (tpage_word << 16)); /* u1=32, v1=0, tpage */
    EMIT_GP0(10 | (50 << 16));           /* v2: x=10, y=50 */
    EMIT_GP0(0 | (32 << 8));             /* u2=0, v2=32 */
    EMIT_GP0(50 | (50 << 16));           /* v3: x=50, y=50 */
    EMIT_GP0(32 | (32 << 8));            /* u3=32, v3=32 */
}

static void emit_textured_quad_8bpp(int page_tx, int page_ty, int clut_x, int clut_y)
{
    /* E1: Set texture page — format=1 (8BPP) */
    uint32_t e1_val = (page_tx & 0xF) | ((page_ty & 1) << 4) | (1 << 7); /* 8BPP */
    EMIT_GP0(0xE1000000 | e1_val);

    uint32_t clut_word = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    uint32_t tpage_word = (page_tx & 0xF) | ((page_ty & 1) << 4) | (1 << 7);

    EMIT_GP0(0x2C808080);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0 | (0 << 8) | (clut_word << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(32 | (0 << 8) | (tpage_word << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0 | (32 << 8));
    EMIT_GP0(50 | (50 << 16));
    EMIT_GP0(32 | (32 << 8));
}

/* ================================================================
 *  Test C1: CLUT 4BPP basic — textured draw produces GIF output
 * ================================================================ */
static void test_clut_4bpp_basic(void)
{
    BEGIN_GPU_TEST("clut4_basic");

    /* Prepare: Reset GPU, init texcache, write CLUT + texpage data */
    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_4bpp(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();

    /* Page upload (~2048 QWs) + CLUT upload (~16 QWs) + prim + env regs */
    EXPECT_QWORDS(2200);    /* Baseline ~2095 QWs */
    EXPECT_CYCLES(50000);   /* Baseline ~37K cycles */

    END_GPU_TEST();
}

/* ================================================================
 *  Test C2: CLUT 8BPP basic — larger palette upload
 * ================================================================ */
static void test_clut_8bpp_basic(void)
{
    BEGIN_GPU_TEST("clut8_basic");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_8bpp(0, 240, 0x2000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 256, 1);

    emit_textured_quad_8bpp(0, 0, 0, 240);
    Flush_GIF();

    /* 8BPP: larger page (256 texels wide) + CLUT upload (256 entries) */
    EXPECT_QWORDS(4300);    /* Baseline ~4175 QWs */
    EXPECT_CYCLES(80000);   /* Baseline ~73K cycles */

    END_GPU_TEST();
}

/* ================================================================
 *  Test C3: prim_tex_cache HIT — consecutive same draw skips upload
 * ================================================================ */
static void test_prim_cache_hit(void)
{
    BEGIN_GPU_TEST("cache_hit");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_4bpp(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* First draw: cache miss → full page + CLUT upload */
    SETUP_GP0(0xE1000000 | (0 << 0) | (0 << 7)); /* 4BPP texpage(0,0) */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_first = gp_ctx.qwords_generated;

    /* Reset GIF counter for second draw (keep cache state) */
    gp_ctx.qwords_generated = 0;
    mock_gif_qwords_written = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;

    /* Second draw: same texture + same CLUT → prim_tex_cache HIT */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_second = gp_ctx.qwords_generated;

    /* Second draw should produce MUCH less GIF (no page/CLUT upload) */
    if (qw_second < qw_first / 2) {
        printf("    %-16s: 1st=%u 2nd=%u QWs (cache hit) OK\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_second);
    } else {
        printf("  [FAIL] %-16s: 1st=%u 2nd=%u QWs (expected 2nd << 1st)\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_second);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  Test C4: prim_tex_cache MISS after CLUT region dirty
 *  Write new CLUT data → vram_gen bumps → cache misses
 * ================================================================ */
static void test_prim_cache_miss_clut_dirty(void)
{
    BEGIN_GPU_TEST("cache_miss_clut");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_4bpp(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* First draw: populate cache */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_first = gp_ctx.qwords_generated;

    /* Simulate: game uploads new CLUT data to same location */
    write_clut_4bpp(0, 240, 0x3000); /* Different colors! */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* Reset counter */
    gp_ctx.qwords_generated = 0;
    mock_gif_qwords_written = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;

    /* Second draw: CLUT dirty → cache miss → re-upload */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_second = gp_ctx.qwords_generated;

    /* Second draw should upload CLUT (but page may be cached if only CLUT dirty) */
    if (qw_second > 10) {
        printf("    %-16s: 1st=%u 2nd=%u QWs (re-uploaded) OK\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_second);
    } else {
        printf("  [FAIL] %-16s: 1st=%u 2nd=%u QWs (expected CLUT re-upload)\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_second);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  Test C5: prim_tex_cache invalidation due to FB FillRect
 *  FillRect to framebuffer area → vram_gen bumps → cache misses
 *  (This tests the current behavior; after G2 optimization this
 *   should become a HIT since FillRect is in FB, not tex/clut area)
 * ================================================================ */
static void test_prim_cache_fb_fillrect(void)
{
    BEGIN_GPU_TEST("cache_fb_fill");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_4bpp(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* First draw: populate cache */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_first = gp_ctx.qwords_generated;

    /* FillRect to framebuffer (y < 240, should not affect tex/clut) */
    SETUP_GP0(0x02000000); /* FillRect black */
    SETUP_GP0(0 | (0 << 16)); /* x=0, y=0 */
    SETUP_GP0(640 | (240 << 16)); /* w=640, h=240 */
    Flush_GIF(); /* FillRect also goes through GIF */

    /* Reset counter */
    gp_ctx.qwords_generated = 0;
    mock_gif_qwords_written = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;

    /* Second draw: currently MISS (vram_gen changed globally).
     * After G2 optimization: should be HIT (FB dirty, not tex/clut) */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_second = gp_ctx.qwords_generated;

    /* Log the result for baseline tracking */
    printf("    %-16s: 1st=%u 2nd=%u QWs (fb_fill → %s)\n",
           gp_ctx.name, (unsigned)qw_first, (unsigned)qw_second,
           qw_second < qw_first / 2 ? "HIT" : "MISS");
    /* Currently: MISS expected (global vram_gen). Not a failure. */

    END_GPU_TEST();
}

/* ================================================================
 *  Test C6: Round-robin CLUT CBP advances
 *  Draw 65 different CLUTs → verify round-robin wraps
 * ================================================================ */
static void test_clut_round_robin(void)
{
    BEGIN_GPU_TEST("clut_robin");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);

    int draws_ok = 0;
    /* Draw 65 times with different CLUTs (force round-robin wrap at 64) */
    for (int i = 0; i < 65; i++) {
        int cy = 240 + (i % 16); /* Different CLUT rows */
        write_clut_4bpp(0, cy, (uint16_t)(0x1000 + i * 0x100));
        vram_gen_counter++;
        Tex_Cache_DirtyRegion(0, cy, 16, 1);
        Prim_InvalidateTexCache(); /* Force fresh decode each time */

        gp_ctx.qwords_generated = 0;
        mock_gif_qwords_written = 0;
        fast_gif_ptr = MOCK_GIF_BUFFER_START;

        emit_textured_quad_4bpp(0, 0, 0, cy);
        Flush_GIF();

        /* Each draw should emit CLUT upload GIF data (> 0 QWs for CLUT) */
        if (gp_ctx.qwords_generated > 5)
            draws_ok++;
    }

    if (draws_ok == 65) {
        printf("    %-16s: %d/65 draws with CLUT upload OK\n",
               gp_ctx.name, draws_ok);
    } else {
        printf("  [FAIL] %-16s: %d/65 draws with CLUT upload (expected 65)\n",
               gp_ctx.name, draws_ok);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  Test C7: Two alternating textures — single-entry cache misses
 * ================================================================ */
static void test_alternating_textures(void)
{
    BEGIN_GPU_TEST("alt_tex");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();

    /* Two separate texture pages with different CLUTs */
    write_texpage_data(0, 0);     /* Page 0 at (0,0) */
    write_texpage_data(64, 0);    /* Page 1 at (64,0) */
    write_clut_4bpp(0, 240, 0x1000);   /* CLUT A */
    write_clut_4bpp(16, 240, 0x2000);  /* CLUT B */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 192, 256);
    Tex_Cache_DirtyRegion(0, 240, 32, 1);

    int miss_count = 0;
    for (int i = 0; i < 10; i++) {
        gp_ctx.qwords_generated = 0;
        mock_gif_qwords_written = 0;
        fast_gif_ptr = MOCK_GIF_BUFFER_START;

        if (i % 2 == 0)
            emit_textured_quad_4bpp(0, 0, 0, 240);   /* Tex A */
        else
            emit_textured_quad_4bpp(1, 0, 16, 240);  /* Tex B */
        Flush_GIF();

        /* Count draws that trigger CLUT upload (cache miss) */
        if (gp_ctx.qwords_generated > 30)
            miss_count++;
    }

    /* With single-entry cache + alternating textures: ALL should miss
     * (except possibly the 2nd draw of same texture if consecutive).
     * Currently expected: most or all miss. */
    printf("    %-16s: %d/10 cache misses with alternating textures\n",
           gp_ctx.name, miss_count);
    /* After G3 (multi-entry cache): miss count should drop to 2 (initial misses) */

    END_GPU_TEST();
}

/* ================================================================
 *  Test C8: Round-robin overwrite → CLUT content cache invalidation
 *  Draw CLUT A, then exhaust 64 round-robin slots with different CLUTs,
 *  then draw CLUT A again.  The CLUT content cache must NOT return a
 *  stale CBP — it should detect that the CBP was overwritten and
 *  re-upload.  (Regression test for stale-CBP visual glitch.)
 * ================================================================ */
static void test_clut_robin_overwrite(void)
{
    BEGIN_GPU_TEST("robin_overwrite");

    SETUP_GP1(0x00000000);
    Tex_Cache_Init();
    write_texpage_data(0, 0);
    write_clut_4bpp(0, 240, 0x1000);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 128, 256);
    Tex_Cache_DirtyRegion(0, 240, 16, 1);

    /* First draw of CLUT A — populates CLUT content cache */
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_first = gp_ctx.qwords_generated;

    /* Exhaust all 64 round-robin slots with unique CLUTs.
     * This will overwrite the CBP slot used for CLUT A. */
    for (int i = 0; i < 64; i++) {
        int cy = 230 + (i % 10);  /* Different CLUT rows */
        write_clut_4bpp(32 + (i * 16) % 512, cy, (uint16_t)(0x2000 + i * 0x10));
        vram_gen_counter++;
        Tex_Cache_DirtyRegion(32 + (i * 16) % 512, cy, 16, 1);
        Prim_InvalidateTexCache();

        gp_ctx.qwords_generated = 0;
        mock_gif_qwords_written = 0;
        fast_gif_ptr = MOCK_GIF_BUFFER_START;
        emit_textured_quad_4bpp(0, 0, 32 + (i * 16) % 512, cy);
        Flush_GIF();
    }

    /* Reset counter for final draw */
    gp_ctx.qwords_generated = 0;
    mock_gif_qwords_written = 0;
    fast_gif_ptr = MOCK_GIF_BUFFER_START;

    /* Draw CLUT A again — the CLUT content cache should have been
     * invalidated when the round-robin overwrote its CBP slot.
     * So this MUST re-upload (not use stale CBP). */
    Prim_InvalidateTexCache();
    emit_textured_quad_4bpp(0, 0, 0, 240);
    Flush_GIF();
    uint32_t qw_final = gp_ctx.qwords_generated;

    /* Expect a CLUT re-upload (not a cache hit with 0 CLUT QWs) */
    if (qw_final > 5) {
        printf("    %-16s: 1st=%u final=%u QWs (re-uploaded after overwrite) OK\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_final);
    } else {
        printf("  [FAIL] %-16s: 1st=%u final=%u QWs (stale CBP from cache!)\n",
               gp_ctx.name, (unsigned)qw_first, (unsigned)qw_final);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_clut_tests(void)
{
    printf("─── CLUT / Texture Cache Tests ────────────────────────────\n");
    test_clut_4bpp_basic();
    test_clut_8bpp_basic();
    test_prim_cache_hit();
    test_prim_cache_miss_clut_dirty();
    test_prim_cache_fb_fillrect();
    test_clut_round_robin();
    test_alternating_textures();
    test_clut_robin_overwrite();
    printf("\n");
}
