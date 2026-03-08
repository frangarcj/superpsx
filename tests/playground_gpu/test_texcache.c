/*
 * GPU Playground — TexCache Partial Upload Tests (TDD)
 *
 * Tests for the partial page upload optimization:
 * When only a subset of rows in a texture page are dirty,
 * upload only those rows instead of the full 256×256 page.
 *
 * The optimization is measured by comparing GIF QW output:
 *   Full 4BPP upload ≈ 2055 QWs (256×256 at 4BPP = 32KB)
 *   Partial 16-row upload ≈ 134 QWs (16×256 at 4BPP)
 *
 * We use a generous threshold (half of full) to be robust.
 */
#include "playground_gpu.h"
#include <string.h>
#include <gs_gp.h>

extern uint32_t vram_gen_counter;
extern void Tex_Cache_Init(void);

/* ── Helpers ─────────────────────────────────────────────────── */

/* Set up a texture page in shadow VRAM and dirty the page region.
 * format: 0=4BPP, 1=8BPP.  page_px, page_py in halfword coords. */
static void setup_page(int format, int page_px, int page_py,
                       int clut_x, int clut_y)
{
    /* Write CLUT entries */
    int entries = (format == 0) ? 16 : 256;
    for (int i = 0; i < entries; i++)
        psx_vram_shadow[clut_y * 1024 + clut_x + i] = (uint16_t)(0x1000 + i);

    /* Write test pattern to page data */
    int hw = (format == 0) ? 64 : 128;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < hw; x++)
            psx_vram_shadow[(page_py + y) * 1024 + page_px + x] = (uint16_t)(x + y);
}

/* Emit texpage E1 + textured quad (0x2C) at the given page/clut.
 * page_tx is in 64-halfword page units (page_px / 64). */
static void tc_emit_textured_quad(int format, int page_tx, int page_ty,
                                  int clut_x, int clut_y)
{
    uint32_t e1 = (page_tx & 0xF) | ((page_ty & 1) << 4) | (format << 7);
    EMIT_GP0(0xE1000000 | e1);

    uint32_t clut_w = ((clut_x / 16) & 0x3F) | (((clut_y) & 0x1FF) << 6);
    uint32_t tp_w = (page_tx & 0xF) | ((page_ty & 1) << 4) | (format << 7);

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

/* ================================================================
 *  TC1: Partial upload — 4BPP, small dirty region
 *
 *  1. Full page upload (all dirty → miss)
 *  2. Dirty only 16 rows
 *  3. 2nd decode → partial upload → significantly fewer QWs
 * ================================================================ */
static void test_partial_4bpp_small(void)
{
    BEGIN_GPU_TEST("tc1_part_4bp");

    Tex_Cache_Init();
    int page_tx = 5;  /* page at halfword x=320, well away from FB */
    int page_px = page_tx * 64;

    setup_page(0, page_px, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 256);

    /* 1st draw: full upload (cold miss) */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t full_qws = gp_ctx.qwords_generated;
    gp_gif_reset_counter();

    /* Dirty only 16 rows at top of page (1 block-row) */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 16);

    /* 2nd draw: should detect partial dirty → upload only 16 rows */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t partial_qws = gp_ctx.qwords_generated;

    /* Validation: partial should be significantly less than full.
     * Full 4BPP ≈ 2055 QWs.  Partial 16 rows ≈ 134 QWs + primitive state.
     * Use threshold: partial < full / 4 */
    if (partial_qws > 0 && partial_qws < full_qws / 4) {
        printf("    %-16s: partial=%u full=%u (%.0f%% reduction) OK\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws,
               (1.0f - (float)partial_qws / (float)full_qws) * 100.0f);
    } else {
        printf("  [FAIL] %-16s: partial=%u NOT < full/4=%u\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws / 4);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  TC2: Partial upload — 8BPP, small dirty region
 * ================================================================ */
static void test_partial_8bpp_small(void)
{
    BEGIN_GPU_TEST("tc2_part_8bp");

    Tex_Cache_Init();
    int page_tx = 5;
    int page_px = page_tx * 64;

    setup_page(1, page_px, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 128, 256);

    /* 1st draw: full upload */
    tc_emit_textured_quad(1, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t full_qws = gp_ctx.qwords_generated;
    gp_gif_reset_counter();

    /* Dirty only 16 rows at bottom of page */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 240, 128, 16);

    /* 2nd draw: partial upload */
    tc_emit_textured_quad(1, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t partial_qws = gp_ctx.qwords_generated;

    if (partial_qws > 0 && partial_qws < full_qws / 4) {
        printf("    %-16s: partial=%u full=%u (%.0f%% reduction) OK\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws,
               (1.0f - (float)partial_qws / (float)full_qws) * 100.0f);
    } else {
        printf("  [FAIL] %-16s: partial=%u NOT < full/4=%u\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws / 4);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  TC3: FillRect to FB does NOT invalidate non-overlapping tex page
 *
 *  FB at (0,0,320,240).  Tex page at (320,0) = column 5.
 *  FillRect only touches columns 0-4, rows 0-14.
 *  Tex page column 5 should NOT be dirtied → cache HIT.
 * ================================================================ */
static void test_fillrect_nonoverlap(void)
{
    BEGIN_GPU_TEST("tc3_fill_noovl");

    Tex_Cache_Init();
    int page_tx = 5;
    int page_px = page_tx * 64;

    setup_page(0, page_px, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 256);

    /* 1st draw: populate cache */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    gp_gif_reset_counter();

    /* FillRect to FB area (0,0)(320,240) — does NOT overlap tex at (320,0) */
    EMIT_GP0(0x02000000);
    EMIT_GP0(0 | (0 << 16));
    EMIT_GP0(320 | (240 << 16));
    Flush_GIF();
    gp_gif_reset_counter();

    /* 2nd draw: should be cache HIT → no TEXFLUSH */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  TC4: FillRect overlapping tex page → NO re-upload (FillRect safe)
 *
 *  FillRect writes to GS framebuffer (FBP) via sprite draw; it does
 *  NOT modify the GS texture page at TBP0.  Shadow VRAM is updated
 *  for CPU readback, but the texture gen should NOT be bumped.
 *  Tex page at (0,0) = column 0.  FillRect at (0,0,64,16).
 *  This writes to FB area → tex page SHOULD remain clean → HIT.
 * ================================================================ */
static void test_fillrect_overlap(void)
{
    BEGIN_GPU_TEST("tc4_fill_ovlp");

    Tex_Cache_Init();
    int page_tx = 0;

    setup_page(0, 0, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256);

    /* 1st draw: populate cache */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    gp_gif_reset_counter();

    /* FillRect overlapping the texture page at (0,0) —
     * this should NOT dirty the texture page gen */
    EMIT_GP0(0x02FF0000);          /* Red fill */
    EMIT_GP0(0 | (0 << 16));      /* x=0, y=0 */
    EMIT_GP0(64 | (16 << 16));    /* w=64, h=16 */
    Flush_GIF();
    gp_gif_reset_counter();

    /* 2nd draw: HIT expected → no TEXFLUSH (FillRect doesn't dirty tex) */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  TC6: DMA / LoadImage overlapping tex page → correct MISS
 *
 *  Unlike FillRect, DMA uploads (GP0.A0h LoadImage) write actual
 *  texture data that MUST trigger re-upload.  We simulate this by
 *  calling Tex_Cache_DirtyRegion directly (which is what the DMA
 *  path does) after the initial cache populate.
 * ================================================================ */
static void test_dma_overlap(void)
{
    BEGIN_GPU_TEST("tc6_dma_ovlp");

    Tex_Cache_Init();
    int page_tx = 0;

    setup_page(0, 0, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 256);

    /* 1st draw: populate cache */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    gp_gif_reset_counter();

    /* Simulate DMA upload that overlaps page at (0,0) */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(0, 0, 64, 16);

    /* 2nd draw: MISS expected → TEXFLUSH present */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    gp_gif_scan();
    EXPECT_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  TC5: Multiple dirty ranges → only dirty rows uploaded
 *
 *  Dirty rows 0-15 and 240-255 (2 ranges, 32 rows total out of 256).
 *  Upload should be << full upload.
 * ================================================================ */
static void test_partial_multi_range(void)
{
    BEGIN_GPU_TEST("tc5_multi_rng");

    Tex_Cache_Init();
    int page_tx = 5;
    int page_px = page_tx * 64;

    setup_page(0, page_px, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 256);

    /* 1st draw: full upload */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t full_qws = gp_ctx.qwords_generated;
    gp_gif_reset_counter();

    /* Dirty 2 non-contiguous ranges: rows 0-15 and 240-255 */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 16);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 240, 64, 16);

    /* 2nd draw: partial upload of 2 ranges (32 rows out of 256) */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    uint32_t partial_qws = gp_ctx.qwords_generated;

    if (partial_qws > 0 && partial_qws < full_qws / 4) {
        printf("    %-16s: partial=%u full=%u (%.0f%% reduction) OK\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws,
               (1.0f - (float)partial_qws / (float)full_qws) * 100.0f);
    } else {
        printf("  [FAIL] %-16s: partial=%u NOT < full/4=%u\n",
               gp_ctx.name, (unsigned)partial_qws, (unsigned)full_qws / 4);
        gp_ctx.fail_count++;
    }

    END_GPU_TEST();
}

/* ================================================================
 *  TC7: Multi-page no thrashing (direct-mapped cache)
 *
 *  Use 12 different texture pages (> old FIFO size of 8).
 *  All pages are populated, then re-accessed → ALL must HIT.
 *  Old FIFO would evict early pages; direct-mapped never does.
 * ================================================================ */
static void test_multipage_no_thrash(void)
{
    BEGIN_GPU_TEST("tc7_no_thrash");

    Tex_Cache_Init();

    /* Set up 12 texture pages at different X positions (page_tx 2..13) */
    for (int i = 0; i < 12; i++)
    {
        int page_tx = i + 2;
        int page_px = page_tx * 64;
        setup_page(0, page_px, 0, 0, 480);
    }
    vram_gen_counter++;
    for (int i = 0; i < 12; i++)
    {
        int page_tx = i + 2;
        int page_px = page_tx * 64;
        Tex_Cache_DirtyRegion(page_px, 0, 64, 256);
    }

    /* 1st pass: populate cache (all misses → TEXFLUSH) */
    for (int i = 0; i < 12; i++)
    {
        tc_emit_textured_quad(0, i + 2, 0, 0, 480);
        Flush_GIF();
    }
    gp_gif_reset_counter();

    /* 2nd pass: re-access ALL 12 pages — ALL must HIT (no TEXFLUSH) */
    for (int i = 0; i < 12; i++)
    {
        tc_emit_textured_quad(0, i + 2, 0, 0, 480);
    }
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  TC8: Global gen fast-path — no VRAM writes → O(1) lookup
 *
 *  After populating a page, access it multiple times without any
 *  VRAM writes.  The lookup should HIT via global_gen check only
 *  (no Tex_Cache_GetPageGen call needed).  Verified by lack of
 *  TEXFLUSH on repeated draws.
 * ================================================================ */
static void test_global_gen_fastpath(void)
{
    BEGIN_GPU_TEST("tc8_ggen_fast");

    Tex_Cache_Init();
    int page_tx = 5;
    int page_px = page_tx * 64;

    setup_page(0, page_px, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(page_px, 0, 64, 256);

    /* 1st draw: populate cache */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    Flush_GIF();
    gp_gif_reset_counter();

    /* NO VRAM writes — vram_gen_counter stays the same */

    /* 2nd draw: global_gen matches → instant HIT */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    gp_gif_reset_counter();

    /* 3rd draw: still no writes → still instant HIT */
    tc_emit_textured_quad(0, page_tx, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  TC9: VRAM write to OTHER page → current page still HITs
 *
 *  Two pages at different positions. Write to page B, then access
 *  page A → should still HIT (global_gen differs but page gen same).
 * ================================================================ */
static void test_other_page_write(void)
{
    BEGIN_GPU_TEST("tc9_other_wr");

    Tex_Cache_Init();

    /* Page A at page_tx=3, Page B at page_tx=7 */
    setup_page(0, 3 * 64, 0, 0, 480);
    setup_page(0, 7 * 64, 0, 0, 480);
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(3 * 64, 0, 64, 256);
    Tex_Cache_DirtyRegion(7 * 64, 0, 64, 256);

    /* Populate both in cache */
    tc_emit_textured_quad(0, 3, 0, 0, 480);
    tc_emit_textured_quad(0, 7, 0, 0, 480);
    Flush_GIF();
    gp_gif_reset_counter();

    /* Dirty ONLY page B */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(7 * 64, 0, 64, 16);

    /* Access page A → global_gen differs, but page A's gen unchanged → HIT */
    tc_emit_textured_quad(0, 3, 0, 0, 480);
    gp_gif_scan();
    EXPECT_NO_GIF_REG("TEXFLUSH", GS_REG_TEXFLUSH);

    END_GPU_TEST();
}

/* ================================================================
 *  Runner
 * ================================================================ */
void gp_run_texcache_tests(void)
{
    printf("\n--- TexCache Partial Upload Tests ---\n");

    test_partial_4bpp_small();       /* TC1 */
    test_partial_8bpp_small();       /* TC2 */
    test_fillrect_nonoverlap();      /* TC3 */
    test_fillrect_overlap();         /* TC4 — FillRect doesn't dirty tex */
    test_partial_multi_range();      /* TC5 */
    test_dma_overlap();              /* TC6 — DMA DOES dirty tex */
    test_multipage_no_thrash();      /* TC7 — 12 pages, no FIFO eviction */
    test_global_gen_fastpath();      /* TC8 — O(1) fast path */
    test_other_page_write();         /* TC9 — write to other page → HIT */
}
