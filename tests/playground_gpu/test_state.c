/*
 * GPU Playground — GS State Transition & TEXFLUSH Tests
 *
 * Deep functional tests that verify:
 * 1. gs_state lazy tracking across primitive type transitions
 * 2. TEXFLUSH emission correctness (cache miss → flush, hit → skip)
 * 3. Semi-transparency mode switching
 * 4. Texture window / CLAMP state persistence
 *
 * These tests use the GIF register capture API to inspect WHICH registers
 * are emitted, not just how many QWs are generated.
 */
#include "playground_gpu.h"
#include <string.h>
#include <gs_gp.h>

/* Access internals */
extern uint32_t vram_gen_counter;
extern void Tex_Cache_Init(void);

/* ================================================================
 *  Helpers
 * ================================================================ */

/* Write CLUT + texture page data for textured-draw tests */
static void setup_texture_data(int page_tx, int page_ty,
                               int clut_x, int clut_y, int format)
{
    if (!psx_vram_shadow) return;
    int entries = (format == 0) ? 16 : 256;
    for (int i = 0; i < entries; i++)
        psx_vram_shadow[clut_y * 1024 + clut_x + i] = (uint16_t)(0x1000 + i);
    int hw = (format == 0) ? 64 : 128;
    int page_px = page_tx * 64;
    int page_py = page_ty * 256;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < hw; x++)
            psx_vram_shadow[(page_py + y) * 1024 + page_px + x] = (uint16_t)(x + y);
}

/* Emit a flat (untextured) triangle: GP0 cmd 0x20 */
static void emit_flat_tri(uint32_t color)
{
    EMIT_GP0(0x20000000 | (color & 0xFFFFFF));
    EMIT_GP0(10 | (10 << 16));   /* v0: 10,10 */
    EMIT_GP0(50 | (10 << 16));   /* v1: 50,10 */
    EMIT_GP0(30 | (50 << 16));   /* v2: 30,50 */
}

/* Emit a semi-transparent flat triangle: GP0 cmd 0x22 */
static void emit_semitrans_tri(uint32_t color)
{
    EMIT_GP0(0x22000000 | (color & 0xFFFFFF));
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(30 | (50 << 16));
}

/* Emit a textured quad (0x2C) with 4BPP CLUT */
static void emit_textured_quad(int page_tx, int page_ty,
                               int clut_x, int clut_y)
{
    uint32_t e1 = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7);
    EMIT_GP0(0xE1000000 | e1);

    uint32_t clut_w = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    uint32_t tp_w = (page_tx & 0xF) | ((page_ty & 1) << 4) | (0 << 7);

    EMIT_GP0(0x2C808080);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0 | (0 << 8) | (clut_w << 16));
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(32 | (0 << 8) | (tp_w << 16));
    EMIT_GP0(10 | (50 << 16));
    EMIT_GP0(0 | (32 << 8));
    EMIT_GP0(50 | (50 << 16));
    EMIT_GP0(32 | (32 << 8));
}

/* Emit a textured rect (0x64 = 1×1 sprite with tex) */
static void emit_textured_rect(int clut_x, int clut_y)
{
    /* Builds 0x64: textured rect, opaque, no blend */
    uint32_t clut_w = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    EMIT_GP0(0x64808080);               /* Textured rect cmd, neutral color */
    EMIT_GP0(10 | (10 << 16));          /* x=10, y=10 */
    EMIT_GP0(0 | (0 << 8) | (clut_w << 16)); /* u=0, v=0, clut */
    EMIT_GP0(32 | (32 << 16));          /* w=32, h=32 */
}

/* Emit a semi-transparent textured rect (0x66) */
static void emit_semitrans_textured_rect(int clut_x, int clut_y)
{
    uint32_t clut_w = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    EMIT_GP0(0x66808080);
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0 | (0 << 8) | (clut_w << 16));
    EMIT_GP0(32 | (32 << 16));
}

/* Emit a flat (untextured) rect: GP0 cmd 0x60 */
static void emit_flat_rect(uint32_t color)
{
    EMIT_GP0(0x60000000 | (color & 0xFFFFFF));
    EMIT_GP0(10 | (10 << 16));   /* x=10, y=10 */
    EMIT_GP0(32 | (32 << 16));   /* w=32, h=32 */
}

/* ================================================================
 *  S1: TEXFLUSH emitted on first textured draw (cold start)
 * ================================================================ */
static void test_texflush_cold_start(void)
{
    BEGIN_GPU_TEST("texfl_cold");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEST_1", GS_REG_TEST_1);
    END_GPU_TEST();
}

/* ================================================================
 *  S2: TEXFLUSH suppressed on cache hit (same texture twice)
 * ================================================================ */
static void test_texflush_cache_hit(void)
{
    BEGIN_GPU_TEST("texfl_hit");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* First draw — warms caches */
    emit_textured_quad(0, 0, 0, 256);

    /* Reset GIF capture but keep GPU state warm */
    gp_gif_reset_counter();

    /* Second draw — same texture, should be cache hit */
    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    /* TEXFLUSH should NOT be emitted on cache hit */
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    END_GPU_TEST();
}

/* ================================================================
 *  S3: TEXFLUSH emitted on VRAM dirty (page re-upload)
 * ================================================================ */
static void test_texflush_vram_dirty(void)
{
    BEGIN_GPU_TEST("texfl_dirty");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* First draw — warm caches */
    emit_textured_quad(0, 0, 0, 256);
    gp_gif_reset_counter();

    /* Dirty the texture region */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256); /* mark tex page blocks */

    /* Second draw — page must re-upload, TEXFLUSH required */
    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    END_GPU_TEST();
}

/* ================================================================
 *  S4: Rect path TEXFLUSH on cache miss (regression for the
 *      stretching bug we just fixed)
 * ================================================================ */
static void test_texflush_rect_miss(void)
{
    BEGIN_GPU_TEST("texfl_rect");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* Set E1 for 4BPP texture page 0 */
    EMIT_GP0(0xE1000000 | (0 << 7));

    /* First rect — cold start, must TEXFLUSH */
    emit_textured_rect(0, 256);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    /* Reset capture, keep state */
    gp_gif_reset_counter();

    /* Dirty VRAM so prim_tex_cache misses */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256); /* mark tex page blocks */

    /* Second rect — cache miss due to dirty, must TEXFLUSH */
    emit_textured_rect(0, 256);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  S5: Rect path TEXFLUSH suppressed on cache hit
 * ================================================================ */
static void test_texflush_rect_hit(void)
{
    BEGIN_GPU_TEST("texfl_rect_hit");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    EMIT_GP0(0xE1000000 | (0 << 7));

    /* First rect — cold */
    emit_textured_rect(0, 256);
    gp_gif_reset_counter();

    /* Second rect — same texture, cache hit — no TEXFLUSH */
    emit_textured_rect(0, 256);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  S6: Transition flat → textured: TEX0 + TEST must appear
 * ================================================================ */
static void test_flat_to_textured(void)
{
    BEGIN_GPU_TEST("flat→tex");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* First draw: flat triangle warms gs_state with untextured state */
    emit_flat_tri(0x808080);
    gp_gif_reset_counter();

    /* Second draw: textured quad — must emit TEX0 + TEST_1 + TEXFLUSH */
    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEST_1", GS_REG_TEST_1);
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  S7: Transition textured → flat: TEX0/TEST should NOT appear
 *      (flat prims don't need texture registers)
 * ================================================================ */
static void test_textured_to_flat(void)
{
    BEGIN_GPU_TEST("tex→flat");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* First: textured quad */
    emit_textured_quad(0, 0, 0, 256);
    gp_gif_reset_counter();

    /* Second: flat triangle */
    emit_flat_tri(0x808080);

    gp_gif_scan();
    /* Flat prim should NOT emit TEX0 or TEXFLUSH */
    EXPECT_NO_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    /* But should have PRIM */
    EXPECT_GIF_REG("PRIM", GS_REG_PRIM);

    END_GPU_TEST();
}

/* ================================================================
 *  S8: Transition textured quad → textured rect (same texture)
 *      If gs_state matches, no TEX0/TEXFLUSH needed.
 * ================================================================ */
static void test_quad_to_rect_same_tex(void)
{
    BEGIN_GPU_TEST("quad→rect");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    EMIT_GP0(0xE1000000 | (0 << 7));

    /* Textured quad first */
    emit_textured_quad(0, 0, 0, 256);
    gp_gif_reset_counter();

    /* Same-texture rect — should NOT emit TEX0/TEXFLUSH */
    emit_textured_rect(0, 256);

    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    END_GPU_TEST();
}

/* ================================================================
 *  S9: Transition textured rect → textured quad after dirty
 *      Must TEXFLUSH because prim_tex_cache invalidated
 * ================================================================ */
static void test_rect_to_quad_dirty(void)
{
    BEGIN_GPU_TEST("rect→quad_d");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    EMIT_GP0(0xE1000000 | (0 << 7));

    /* Textured rect first */
    emit_textured_rect(0, 256);
    gp_gif_reset_counter();

    /* Dirty VRAM */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256); /* mark tex page blocks */

    /* Textured quad — cache miss — must TEXFLUSH */
    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    END_GPU_TEST();
}

/* ================================================================
 *  S10: Semi-transparency mode 0 → ALPHA_1 emitted
 * ================================================================ */
static void test_semitrans_mode0(void)
{
    BEGIN_GPU_TEST("alpha_mode0");
    /* Set E1 with semi_trans_mode = 0 */
    EMIT_GP0(0xE1000000 | (0 << 5)); /* mode 0 */

    emit_semitrans_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(0));
    END_GPU_TEST();
}

/* ================================================================
 *  S11: Semi-transparency mode switching 0→1→0
 * ================================================================ */
static void test_semitrans_switch(void)
{
    BEGIN_GPU_TEST("alpha_switch");

    /* Mode 0 */
    EMIT_GP0(0xE1000000 | (0 << 5));
    emit_semitrans_tri(0xFF0000);
    gp_gif_reset_counter();

    /* Mode 1 — ALPHA_1 must be re-emitted */
    EMIT_GP0(0xE1000000 | (1 << 5));
    emit_semitrans_tri(0x00FF00);

    gp_gif_scan();
    EXPECT_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(1));

    /* Reset captures */
    gp_gif_reset_counter();

    /* Mode 0 again — should re-emit ALPHA_1 with mode 0 value */
    EMIT_GP0(0xE1000000 | (0 << 5));
    emit_semitrans_tri(0x0000FF);

    gp_gif_scan();
    EXPECT_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(0));

    END_GPU_TEST();
}

/* ================================================================
 *  S12: Opaque between semi-trans — lazy alpha tracking
 *  Opaque draws don't touch gs_state.alpha (optimization: GS register
 *  retains valid value). Same-mode semi-trans after opaque → no re-emit.
 *  Different-mode semi-trans after opaque → must re-emit.
 * ================================================================ */
static void test_opaque_between_semitrans(void)
{
    BEGIN_GPU_TEST("alpha_opaque");

    /* Semi-trans mode 0 first */
    EMIT_GP0(0xE1000000 | (0 << 5));
    emit_semitrans_tri(0x808080);
    gp_gif_reset_counter();

    /* Opaque tri — should NOT emit ALPHA_1 (not needed for opaque) */
    emit_flat_tri(0x808080);

    gp_gif_scan();
    EXPECT_NO_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);

    gp_gif_reset_counter();

    /* Semi-trans mode 1 — ALPHA_1 must re-emit because gs_state.alpha
     * still has mode 0's value (opaque didn't change it) */
    EMIT_GP0(0xE1000000 | (1 << 5));
    emit_semitrans_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);

    END_GPU_TEST();
}

/* ================================================================
 *  S13: All 4 semi-trans modes produce correct ALPHA_1 values
 * ================================================================ */
static void test_all_alpha_modes(void)
{
    for (int mode = 0; mode < 4; mode++) {
        char name[20];
        snprintf(name, sizeof(name), "alpha_m%d", mode);
        BEGIN_GPU_TEST(name);

        EMIT_GP0(0xE1000000 | (mode << 5));
        emit_semitrans_tri(0x808080);

        gp_gif_scan();
        EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(mode));

        END_GPU_TEST();
    }
}

/* ================================================================
 *  S14: Flat rect → textured rect transition
 * ================================================================ */
static void test_flat_rect_to_textured_rect(void)
{
    BEGIN_GPU_TEST("frect→trect");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    EMIT_GP0(0xE1000000 | (0 << 7));

    /* Flat rect */
    emit_flat_rect(0x808080);
    gp_gif_reset_counter();

    /* Textured rect — must emit TEX0 + TEST */
    emit_textured_rect(0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEST_1", GS_REG_TEST_1);
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  S15: Semi-transparent textured rect — both ALPHA and TEX0
 * ================================================================ */
static void test_semitrans_textured_rect(void)
{
    BEGIN_GPU_TEST("st_tex_rect");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    EMIT_GP0(0xE1000000 | (2 << 5) | (0 << 7)); /* mode 2, 4BPP */

    emit_semitrans_textured_rect(0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("ALPHA_1", GS_REG_ALPHA_1);
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEST_1", GS_REG_TEST_1);
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(2));

    END_GPU_TEST();
}

/* ================================================================
 *  S16: DTHE register — dither on shaded, off on flat
 * ================================================================ */
static void test_dthe_shaded_vs_flat(void)
{
    BEGIN_GPU_TEST("dthe_toggle");

    /* Enable dithering */
    EMIT_GP0(0xE1000000 | (1 << 9)); /* dither enabled in E1 */

    /* Shaded tri (0x30) — should emit DTHE=1 */
    EMIT_GP0(0x30FF0000);   /* cmd, color0 */
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0x3000FF00);   /* color1 */
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(0x300000FF);   /* color2 */
    EMIT_GP0(30 | (50 << 16));

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("DTHE", GS_REG_DTHE, 1);

    gp_gif_reset_counter();

    /* Flat tri (0x20) — should emit DTHE=0 */
    emit_flat_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("DTHE", GS_REG_DTHE, 0);

    END_GPU_TEST();
}

/* ================================================================
 *  S17: Consecutive same-state draws skip all state registers
 * ================================================================ */
static void test_same_state_no_reemit(void)
{
    BEGIN_GPU_TEST("no_reemit");

    /* Two identical flat tris — second should NOT re-emit DTHE */
    emit_flat_tri(0x808080);
    gp_gif_reset_counter();
    emit_flat_tri(0x808080);

    gp_gif_scan();
    EXPECT_NO_GIF_REG("DTHE", GS_REG_DTHE);
    /* But PRIM should be present */
    EXPECT_GIF_REG("PRIM", GS_REG_PRIM);

    END_GPU_TEST();
}

/* ================================================================
 *  S18: Different texture pages → different TEX0 → re-emit
 * ================================================================ */
static void test_different_texpage(void)
{
    BEGIN_GPU_TEST("diff_texpg");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    setup_texture_data(1, 0, 0, 257, 0); /* page 1 */
    vram_gen_counter++;

    /* Draw with page 0 */
    emit_textured_quad(0, 0, 0, 256);
    gp_gif_reset_counter();

    /* Draw with page 1 → different TEX0, must re-emit */
    emit_textured_quad(1, 0, 0, 257);

    gp_gif_scan();
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  G5: E1 Handler Optimization Tests
 *
 *  E1 (Draw Mode) should NOT emit GIF data directly. All GS register
 *  writes (TEX0, TEXFLUSH, DTHE, ALPHA_1) should be deferred to
 *  the next primitive, which always re-evaluates via gs_state tracking.
 * ================================================================ */

/* G5a: E1 alone should NOT emit GIF registers (after optimization).
 * Currently this test FAILS (E1 emits TEX0+TEXFLUSH+DTHE+ALPHA_1). */
static void test_e1_no_gif(void)
{
    BEGIN_GPU_TEST("e1_no_gif");

    /* Warm up with a flat tri so gs_state.valid = 1 */
    emit_flat_tri(0x808080);
    gp_gif_reset_counter();

    /* E1 change: switch to 4BPP, page 0, semi-trans mode 2, dither on */
    EMIT_GP0(0xE1000000 | (0 << 0) | (0 << 4) | (0 << 7) | (2 << 5) | (1 << 9));

    gp_gif_scan();
    /* After optimization: E1 should produce zero TEX0 writes */
    EXPECT_GIF_REG_COUNT("TEX0", GS_REG_TEX0, 0);

    END_GPU_TEST();
}

/* G5b: E1 + textured quad — primitive emits correct TEX0 */
static void test_e1_deferred_tex0(void)
{
    BEGIN_GPU_TEST("e1_def_tex0");
    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 256, 0);
    vram_gen_counter++;

    /* E1: set 4BPP page 0 */
    EMIT_GP0(0xE1000000 | (0 << 7));

    /* Textured quad — must emit TEX0 + TEXFLUSH */
    emit_textured_quad(0, 0, 0, 256);

    gp_gif_scan();
    EXPECT_GIF_REG("TEX0", GS_REG_TEX0);
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* G5c: E1 dither change reflected in next draw */
static void test_e1_dither_deferred(void)
{
    BEGIN_GPU_TEST("e1_dither");

    /* Initial draw: default dither (off or on depending on E1) */
    EMIT_GP0(0xE1000000 | (0 << 9)); /* dither=0 */
    emit_flat_tri(0x808080);

    /* Check first draw has DTHE=0 */
    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("DTHE", GS_REG_DTHE, 0);
    gp_gif_reset_counter();

    /* Change dither via E1 */
    EMIT_GP0(0xE1000000 | (1 << 9)); /* dither=1 */

    /* Shaded tri (uses dither) — should pick up dither=1 from E1 */
    EMIT_GP0(0x30808080);   /* shaded tri */
    EMIT_GP0(10 | (10 << 16));
    EMIT_GP0(0x00FF0000);
    EMIT_GP0(50 | (10 << 16));
    EMIT_GP0(0x0000FF00);
    EMIT_GP0(30 | (50 << 16));

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("DTHE", GS_REG_DTHE, 1);

    END_GPU_TEST();
}

/* G5d: E1 alpha mode change reflected in next semi-trans draw */
static void test_e1_alpha_deferred(void)
{
    BEGIN_GPU_TEST("e1_alpha");

    /* E1: semi-trans mode 0 */
    EMIT_GP0(0xE1000000 | (0 << 5));
    emit_semitrans_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(0));
    gp_gif_reset_counter();

    /* E1: switch to semi-trans mode 2 */
    EMIT_GP0(0xE1000000 | (2 << 5));

    emit_semitrans_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(2));

    END_GPU_TEST();
}

/* G5e: Multiple E1 before draw — only last state matters */
static void test_e1_multiple_before_draw(void)
{
    BEGIN_GPU_TEST("e1_multi");

    /* Rapid E1 changes — only the last should take effect */
    EMIT_GP0(0xE1000000 | (0 << 5) | (0 << 9)); /* mode 0, dither off */
    EMIT_GP0(0xE1000000 | (1 << 5) | (1 << 9)); /* mode 1, dither on */
    EMIT_GP0(0xE1000000 | (3 << 5) | (0 << 9)); /* mode 3, dither off */

    /* Reset captures: drop E1 intermediate GIF data, keep GPU state */
    gp_gif_reset_counter();

    /* Semi-trans tri — should use mode 3 from the last E1 */
    emit_semitrans_tri(0x808080);

    gp_gif_scan();
    EXPECT_GIF_REG_VALUE("ALPHA_1", GS_REG_ALPHA_1, Get_Alpha_Reg(3));

    END_GPU_TEST();
}

/* ================================================================
 *  G2: Region-based tex cache — FillRect to unrelated VRAM should
 *      NOT invalidate prim_tex_cache.
 *
 *  Key observable: prim_tex_cache miss → need_texflush=1 → TEXFLUSH
 *  emitted.  Cache hit → need_texflush=0 → no TEXFLUSH.
 *
 *  Dirty tracking granularity: 64hw × 16 scanlines per block.
 *  Texture page (4BPP): 64hw wide.
 * ================================================================ */

/* FillRect helper: GP0(02) */
static void emit_fillrect(int x, int y, int w, int h, uint32_t color)
{
    EMIT_GP0(0x02000000 | (color & 0xFFFFFF));
    EMIT_GP0((uint32_t)(x & 0xFFFF) | ((uint32_t)(y & 0xFFFF) << 16));
    EMIT_GP0((uint32_t)(w & 0xFFFF) | ((uint32_t)(h & 0xFFFF) << 16));
}

/* G2a: FillRect to non-overlapping VRAM → prim_tex_cache should HIT
 *
 * Texture at page_tx=2 (hw 128-191, column 2), CLUT at (0,480, col 0 row 30).
 * FillRect at (0,0,16,16) → column 0, row 0.  No overlap with tex or CLUT.
 *
 * PRE-G2: MISS → 2nd draw emits TEXFLUSH (FAIL)
 * POST-G2: HIT → no TEXFLUSH on 2nd draw (PASS)
 */
static void test_g2a_fillrect_no_overlap(void)
{
    BEGIN_GPU_TEST("g2a_no_ovlap");

    Tex_Cache_Init();
    setup_texture_data(2, 0, 0, 480, 0); /* page_tx=2, clut at (0,480), 4BPP */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(128, 0, 64, 256); /* mark tex page dirty */
    Tex_Cache_DirtyRegion(0, 480, 16, 1);   /* mark CLUT dirty */

    /* 1st draw: populate prim_tex_cache (MISS) */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH); /* 1st draw → always MISS */
    gp_gif_reset_counter();

    /* FillRect: column 0, row 0 — NOT overlapping tex (col 2) or CLUT (row 30) */
    emit_fillrect(0, 0, 16, 16, 0x000000);
    gp_gif_reset_counter();

    /* 2nd draw: same texture params → should be cache HIT after G2 */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH); /* G2: HIT → no TEXFLUSH */

    END_GPU_TEST();
}

/* G2b: FillRect overlapping texture page → must MISS (correctness guard)
 *
 * Texture at page_tx=2 (hw 128-191, column 2).
 * FillRect at (128,0,64,16) → column 2, row 0. Overlaps texture page!
 */
static void test_g2b_fillrect_tex_overlap(void)
{
    BEGIN_GPU_TEST("g2b_tex_ovlap");

    Tex_Cache_Init();
    setup_texture_data(2, 0, 0, 480, 0);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(128, 0, 64, 256);
    Tex_Cache_DirtyRegion(0, 480, 16, 1);

    /* 1st draw: populate cache */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    gp_gif_reset_counter();

    /* FillRect: column 2, row 0 — OVERLAPS texture page */
    emit_fillrect(128, 0, 64, 16, 0xFF0000);
    gp_gif_reset_counter();

    /* 2nd draw: cache MISS (region gen bumped) → TEXFLUSH */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH); /* must MISS */

    END_GPU_TEST();
}

/* G2c: FillRect overlapping CLUT area → must MISS (correctness guard)
 *
 * Texture at page_tx=2, CLUT at (0,480) → column 0, row 30.
 * FillRect at (0,480,16,16) → column 0, row 30. Overlaps CLUT!
 */
static void test_g2c_fillrect_clut_overlap(void)
{
    BEGIN_GPU_TEST("g2c_clut_ovlp");

    Tex_Cache_Init();
    setup_texture_data(2, 0, 0, 480, 0);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(128, 0, 64, 256);
    Tex_Cache_DirtyRegion(0, 480, 16, 1);

    /* 1st draw: populate cache */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    gp_gif_reset_counter();

    /* FillRect: column 0, row 30 — OVERLAPS CLUT */
    emit_fillrect(0, 480, 16, 16, 0x00FF00);
    gp_gif_reset_counter();

    /* 2nd draw: cache MISS (CLUT region gen bumped) → TEXFLUSH */
    emit_textured_quad(2, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH); /* must MISS */

    END_GPU_TEST();
}

/* ================================================================
 *  G3: Multi-entry prim_tex_cache (1→4 entries)
 *
 *  With a single-entry cache, alternating A→B→A causes 3 misses.
 *  With 4 entries, A stays cached when B fills the next slot,
 *  so the return-to-A is a HIT.
 *
 *  Observable: TEXFLUSH on 3rd draw (A→B→A) should be absent.
 * ================================================================ */

/* G3a: A→B→A pattern — 3rd draw should HIT (A still in multi-entry cache)
 *
 * Tex A = page 0, CLUT at (0, 480)
 * Tex B = page 0, CLUT at (16, 480)  — different CLUT → different cache key
 */
static void test_g3a_aba_pattern(void)
{
    BEGIN_GPU_TEST("g3a_aba");

    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 480, 0);  /* Tex page 0, CLUT A at (0,480) */
    /* Write second CLUT B at (16,480) */
    if (psx_vram_shadow) {
        for (int i = 0; i < 16; i++)
            psx_vram_shadow[480 * 1024 + 16 + i] = (uint16_t)(0x2000 + i);
    }
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256);  /* tex page */
    Tex_Cache_DirtyRegion(0, 480, 32, 1);  /* both CLUTs */

    /* Draw A (miss) */
    emit_textured_quad(0, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    gp_gif_reset_counter();

    /* Draw B (miss — different CLUT) */
    emit_textured_quad(0, 0, 16, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);
    gp_gif_reset_counter();

    /* Draw A again — with multi-entry cache: HIT → no TEXFLUSH */
    emit_textured_quad(0, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* G3b: A→B→C→D→E pattern — 5 different textures, 4-entry cache.
 *       5th draw evicts oldest. Return to A = MISS.
 */
static void test_g3b_eviction(void)
{
    BEGIN_GPU_TEST("g3b_evict");

    Tex_Cache_Init();
    setup_texture_data(0, 0, 0, 480, 0);
    /* Write 5 different CLUTs at y=480, x=0,16,32,48,64 */
    if (psx_vram_shadow) {
        for (int k = 0; k < 5; k++)
            for (int i = 0; i < 16; i++)
                psx_vram_shadow[480 * 1024 + k * 16 + i] = (uint16_t)(0x1000 + k * 0x100 + i);
    }
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256);
    Tex_Cache_DirtyRegion(0, 480, 80, 1);

    /* Draw A through E (5 misses) */
    for (int k = 0; k < 5; k++) {
        emit_textured_quad(0, 0, k * 16, 480);
        gp_gif_reset_counter();
    }

    /* Return to A — should be evicted from 4-entry cache → MISS */
    emit_textured_quad(0, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH); /* MISS: A was evicted */

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_state_tests(void)
{
    printf("\n--- GS State Transition & TEXFLUSH Tests ---\n");

    test_texflush_cold_start();      /* S1 */
    test_texflush_cache_hit();       /* S2 */
    test_texflush_vram_dirty();      /* S3 */
    test_texflush_rect_miss();       /* S4 - rect TEXFLUSH regression */
    test_texflush_rect_hit();        /* S5 */
    test_flat_to_textured();         /* S6 */
    test_textured_to_flat();         /* S7 */
    test_quad_to_rect_same_tex();    /* S8 */
    test_rect_to_quad_dirty();       /* S9 */
    test_semitrans_mode0();          /* S10 */
    test_semitrans_switch();         /* S11 */
    test_opaque_between_semitrans(); /* S12 */
    test_all_alpha_modes();          /* S13 - 4 sub-tests */
    test_flat_rect_to_textured_rect(); /* S14 */
    test_semitrans_textured_rect();  /* S15 */
    test_dthe_shaded_vs_flat();      /* S16 */
    test_same_state_no_reemit();     /* S17 */
    test_different_texpage();        /* S18 */

    printf("\n--- G5: E1 Handler Optimization Tests ---\n");
    test_e1_no_gif();               /* G5a */
    test_e1_deferred_tex0();        /* G5b */
    test_e1_dither_deferred();      /* G5c */
    test_e1_alpha_deferred();       /* G5d */
    test_e1_multiple_before_draw(); /* G5e */

    printf("\n--- G2: Region-based Tex Cache Tests ---\n");
    test_g2a_fillrect_no_overlap();     /* G2a — FillRect far → HIT */
    test_g2b_fillrect_tex_overlap();    /* G2b — FillRect on texpage → MISS */
    test_g2c_fillrect_clut_overlap();   /* G2c — FillRect on CLUT → MISS */

    printf("\n--- G3: Multi-entry Tex Cache Tests ---\n");
    test_g3a_aba_pattern();              /* G3a — A→B→A HIT */
    test_g3b_eviction();                 /* G3b — 5 textures, A evicted */
}
