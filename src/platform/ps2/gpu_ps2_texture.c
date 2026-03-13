/**
 * gpu_texture.c — CLUT texture decode with HW CLUT + page-level cache
 *
 * Two rendering paths for indexed (4BPP/8BPP) textures:
 *
 * 1. HW CLUT CSM2 (primary): Upload raw PSMT8/4 indices to GS.
 *    GS reads CLUT directly from PSX VRAM mirror (CT16S at FBP=0)
 *    via TEXCLUT register — zero CLUT upload, zero CPU swizzle.
 *    Texture windows are handled by GS CLAMP_1 REGION_REPEAT mode.
 *
 * 2. SW decode (fallback): Full 256×256 CPU decode to CT16S.
 *    Only used for 15BPP (direct color, no CLUT) textures.
 *
 * Page-Level Cache: 16 entries with LRU eviction, keyed by
 * (format, page, clut, texwindow, vram_gen).  Per-VRAM-page dirty
 * tracking avoids false invalidations.
 */
#include "gpu_state.h"
#include "fast_copy.h"
#include <string.h> /* memcpy, memset */

/* VRAM write generation counter — bumped on every shadow VRAM modification */
uint32_t vram_gen_counter = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Per-VRAM-Page Dirty Tracking
 *
 *  VRAM split into 64×16 blocks (halfwords × scanlines).
 *  16 columns × 32 rows = 512 blocks.  Each block has a generation
 *  counter.  Only bumped when a VRAM write actually touches that block.
 *
 *  The fine 16-line vertical granularity is critical: it separates
 *  framebuffer writes (y=0..239, rows 0-14) from CLUT data (y=240+,
 *  row 15+).  With coarse 256-line rows, framebuffer draws would
 *  falsely invalidate CLUT generation counters, causing ~10% of
 *  texture lookups to miss and re-upload at 249μs each.
 * ═══════════════════════════════════════════════════════════════════ */
#define VRAM_DIRTY_COLS 16
#define VRAM_DIRTY_ROWS 32   /* 512 lines / 16 = 32 rows */
#define VRAM_DIRTY_ROW_SHIFT 4  /* each row = 16 scanlines (was 256) */
static uint32_t vram_page_gen[VRAM_DIRTY_COLS * VRAM_DIRTY_ROWS];

/* Bump generation for all VRAM blocks overlapping a pixel region.
 * Uses the global vram_gen_counter (already incremented by the caller)
 * rather than simple ++ to ensure the MAX-based get_region_gen() always
 * detects the change — even when the dirtied block's old gen was below
 * the page-region maximum. */
void Tex_Cache_DirtyRegion(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    /* Use unsigned shifts — avoids GCC sign-extension fixup for signed / */
    unsigned int col_start = (unsigned)x >> 6;
    unsigned int col_end = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> VRAM_DIRTY_ROW_SHIFT;
    unsigned int row_end = (unsigned)(y + h - 1) >> VRAM_DIRTY_ROW_SHIFT;
    if (col_end >= VRAM_DIRTY_COLS)
        col_end = VRAM_DIRTY_COLS - 1;
    if (row_end >= VRAM_DIRTY_ROWS)
        row_end = VRAM_DIRTY_ROWS - 1;
    for (unsigned int r = row_start; r <= row_end; r++)
        for (unsigned int c = col_start; c <= col_end; c++)
            vram_page_gen[r * VRAM_DIRTY_COLS + c] = vram_gen_counter;
}

/* Get max generation across all VRAM blocks overlapping a pixel region */
static inline uint32_t get_region_gen(int x, int y, int w, int h)
{
    unsigned int col_start = (unsigned)x >> 6;
    unsigned int col_end = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> VRAM_DIRTY_ROW_SHIFT;
    unsigned int row_end = (unsigned)(y + h - 1) >> VRAM_DIRTY_ROW_SHIFT;
    if (col_end >= VRAM_DIRTY_COLS)
        col_end = VRAM_DIRTY_COLS - 1;
    if (row_end >= VRAM_DIRTY_ROWS)
        row_end = VRAM_DIRTY_ROWS - 1;
    uint32_t max_gen = 0;
    for (unsigned int r = row_start; r <= row_end; r++)
        for (unsigned int c = col_start; c <= col_end; c++)
        {
            uint32_t g = vram_page_gen[r * VRAM_DIRTY_COLS + c];
            if (g > max_gen)
                max_gen = g;
        }
    return max_gen;
}

/* Page-only gen: skip CLUT region scan (used by CSM2 cache path) */
uint32_t Tex_Cache_GetPageGen(int tex_format, int tex_page_x, int tex_page_y)
{
    int tex_hw_w = (tex_format == 0) ? 64 : (tex_format == 1) ? 128 : 256;
    return get_region_gen(tex_page_x, tex_page_y, tex_hw_w, 256);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VRAM 1:1 Direct-Mapped Texture Pages (Dual-Format)
 *
 *  Each of the 32 possible PSX texture pages has TWO fixed slots in
 *  GS VRAM — one for 4BPP (PSMT4) and one for 8BPP (PSMT8).  This
 *  eliminates re-uploads when the game alternates between 4BPP and
 *  8BPP interpretations of the same VRAM region (common: Crash
 *  Bandicoot uploads 45 pages/frame, ALL due to format switching).
 *
 *  CLUTs are read via CSM2 — the GS reads palette data directly from
 *  the PSX VRAM mirror (CT16S at block 0) via the TEXCLUT register.
 *  No CLUT upload or round-robin allocator needed.
 *
 *  GS VRAM layout (in 256-byte blocks, TBP0 units):
 *    [0..4095]         PSX VRAM (CT16S, 1MB) — display + 15BPP
 *    [4096..12287]     32 × 256 = 8BPP page slots (PSMT8 256×256) = 2MB
 *    [12288..16383]    32 × 128 = 4BPP page slots (PSMT4 256×256) = 1MB
 *    Total: 16384 / 16384 blocks used (100%)
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Dual-format direct-mapped page slots ────────────────────────── */
#define PAGE_LOCS        32    /* 16 X positions × 2 Y positions */
#define PAGE_SLOTS       64    /* 32 per format (4BPP + 8BPP) */
#define PAGE8_TBP_BASE   4096
#define PAGE8_TBP_STRIDE 256   /* PSMT8 256×256 = 64KB = 256 blocks */
#define PAGE4_TBP_BASE   12288 /* immediately after 8BPP region */
#define PAGE4_TBP_STRIDE 128   /* PSMT4 256×256 = 32KB = 128 blocks */

/* ── Compute page_id from PSX texture page coordinates ───────────── */
/* page_x: 0,64,128..960 (halfword coords), page_y: 0 or 256 */
static inline int compute_page_id(int page_x, int page_y)
{
    return (page_x >> 6) + ((page_y >> 8) << 4);  /* 0..31 */
}

/* ── Compute page slot index (format-specific) ───────────────────── */
static inline int page_slot(int page_id, int tex_format)
{
    return tex_format * PAGE_LOCS + page_id;  /* 0..63 */
}

/* ── Compute deterministic TBP0 for a page (format-specific) ──── */
static inline int page_tbp0(int page_id, int tex_format)
{
    return (tex_format == 0)
        ? PAGE4_TBP_BASE + page_id * PAGE4_TBP_STRIDE   /* 4BPP */
        : PAGE8_TBP_BASE + page_id * PAGE8_TBP_STRIDE;  /* 8BPP */
}

/* ── Per-page dirty tracking ───────────────────────────────────── */
/* One gen per slot (64 slots: 32 4BPP + 32 8BPP).
 * No format field needed — format is implicit in the slot index. */
static uint32_t page_gen[PAGE_SLOTS];

/* ── Statistics ───────────────────────────────────────────────────── */
static struct
{
    uint32_t total_requests;
    uint32_t page_hits;       /* page data already in GS VRAM (no re-upload) */
    uint32_t page_uploads;    /* page data re-uploaded (dirty) */
    uint32_t partial_uploads; /* page re-uploaded (partial rows only) */
    uint32_t full_uploads;    /* page re-uploaded (full 256 rows) */
    uint32_t rect_fallbacks;
    uint64_t pixels_saved;
    uint32_t vram_gen_at_start;
} tex_stats;

/* ── Initialization ──────────────────────────────────────────────── */
void Tex_Cache_Init(void)
{
    memset(page_gen, 0, sizeof(page_gen));
    memset(&tex_stats, 0, sizeof(tex_stats));
    tex_stats.vram_gen_at_start = vram_gen_counter;
}

/* ═══════════════════════════════════════════════════════════════════
 *  HW CLUT Upload Functions
 *
 *  Upload raw indexed texture data (PSMT8/PSMT4) and CLUT palette
 *  to GS VRAM via GIF IMAGE transfer.  The GS hardware then performs
 *  the CLUT lookup per-pixel during rasterization — no CPU decode.
 * ═══════════════════════════════════════════════════════════════════ */

/* Upload 256×256 raw 8-bit indices to GS VRAM in PSMT8 format */
static void Upload_Indexed_8BPP(int tbp0, int tex_page_x, int tex_page_y)
{
    /* BITBLTBUF: DBP=tbp0, DBW=4 (256/64), DPSM=PSMT8 */
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, tbp0, 4, GS_PSM_8), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(256, 256), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* 256×256 8BPP = 64KB = 4096 QWs.  Split into 4 chunks of 1024 QWs
     * (64 rows each).  Uses LQ/SQ for 4× fewer instructions vs memcpy. */
    for (int chunk = 0; chunk < 4; chunk++)
    {
        int eop = (chunk == 3) ? 1 : 0;
        int base_row = chunk * 64;
        Push_GIF_Tag(GIF_TAG_LO(1024, eop, 0, 0, 2, 0), 0);
        for (int row = base_row; row < base_row + 64; row++)
        {
            const void *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
            const void *next = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row + 1) * 1024 + tex_page_x];
            fast_copy_256(fast_gif_ptr, src, next);
            fast_gif_ptr += 16;
        }
    }
}

/* Upload a contiguous range of rows of an 8BPP page to GS VRAM.
 * Partial upload: only rows [start_row, start_row+num_rows) are sent.
 * DY offset ensures data lands at the correct row within the GS texture page. */
static void Upload_Indexed_8BPP_Partial(int tbp0, int tex_page_x, int tex_page_y,
                                        int start_row, int num_rows)
{
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, tbp0, 4, GS_PSM_8), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0, 0, start_row, 0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(256, num_rows), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* 16 QWs per row (256 bytes).  Split into chunks of up to 1024 QWs. */
    int total_qws = num_rows * 16;
    int qws_sent = 0;
    int row = start_row;
    while (qws_sent < total_qws)
    {
        int chunk_qws = total_qws - qws_sent;
        if (chunk_qws > 1024) chunk_qws = 1024;
        int chunk_rows = chunk_qws / 16;
        int eop = (qws_sent + chunk_qws >= total_qws) ? 1 : 0;
        Push_GIF_Tag(GIF_TAG_LO(chunk_qws, eop, 0, 0, 2, 0), 0);
        for (int r = 0; r < chunk_rows; r++, row++)
        {
            const void *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
            const void *next = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row + 1) * 1024 + tex_page_x];
            fast_copy_256(fast_gif_ptr, src, next);
            fast_gif_ptr += 16;
        }
        qws_sent += chunk_qws;
    }
}

/* Upload 256×256 raw 4-bit indices to GS VRAM in PSMT4 format */
static void Upload_Indexed_4BPP(int tbp0, int tex_page_x, int tex_page_y)
{
    /* BITBLTBUF: DBP=tbp0, DBW=4 (256/64), DPSM=PSMT4 */
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, tbp0, 4, GS_PSM_4), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(256, 256), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* 256×256 4BPP = 32KB = 2048 QWs.  Split into 2 chunks of 1024 QWs
     * (128 rows each).  Uses LQ/SQ for 4× fewer instructions vs memcpy. */
    for (int chunk = 0; chunk < 2; chunk++)
    {
        int eop = (chunk == 1) ? 1 : 0;
        int base_row = chunk * 128;
        Push_GIF_Tag(GIF_TAG_LO(1024, eop, 0, 0, 2, 0), 0);
        for (int row = base_row; row < base_row + 128; row++)
        {
            const void *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
            const void *next = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row + 1) * 1024 + tex_page_x];
            fast_copy_128(fast_gif_ptr, src, next);
            fast_gif_ptr += 8;
        }
    }
}

/* Upload a contiguous range of rows of a 4BPP page to GS VRAM.
 * Partial upload: only rows [start_row, start_row+num_rows) are sent. */
static void Upload_Indexed_4BPP_Partial(int tbp0, int tex_page_x, int tex_page_y,
                                        int start_row, int num_rows)
{
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, tbp0, 4, GS_PSM_4), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0, 0, start_row, 0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(256, num_rows), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* 8 QWs per row (128 bytes).  Split into chunks of up to 1024 QWs. */
    int total_qws = num_rows * 8;
    int qws_sent = 0;
    int row = start_row;
    while (qws_sent < total_qws)
    {
        int chunk_qws = total_qws - qws_sent;
        if (chunk_qws > 1024) chunk_qws = 1024;
        int chunk_rows = chunk_qws / 8;
        int eop = (qws_sent + chunk_qws >= total_qws) ? 1 : 0;
        Push_GIF_Tag(GIF_TAG_LO(chunk_qws, eop, 0, 0, 2, 0), 0);
        for (int r = 0; r < chunk_rows; r++, row++)
        {
            const void *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
            const void *next = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row + 1) * 1024 + tex_page_x];
            fast_copy_128(fast_gif_ptr, src, next);
            fast_gif_ptr += 8;
        }
        qws_sent += chunk_qws;
    }
}

/* ── Texture window coordinate transform ─────────────────────────── */

/* Apply_Tex_Window_U/V are now static inline in gpu_state.h */

/* ═══════════════════════════════════════════════════════════════════
 *  Decode_TexPage_Cached — VRAM 1:1 Direct-Mapped Pages + CLUT Robin
 *
 *  O(1) lookup: page_id computed from coordinates, TBP0 is fixed.
 *  Page data only re-uploaded if dirty (gen mismatch).
 *  CLUT always uploaded to a fresh round-robin CBP.
 *
 *  Returns:
 *    0 = 15BPP — caller should use direct PSX VRAM (CT16S).
 *    3 = HW CLUT CSM2 (PSMT8/4).  out_slot_x = TBP0, out_slot_y = 0.
 *        GS reads CLUT directly from PSX VRAM mirror via TEXCLUT.
 * ═══════════════════════════════════════════════════════════════════ */

int Decode_TexPage_Cached(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y)
{
    tex_stats.total_requests++;

    /* 15BPP textures bypass — direct PSX VRAM as CT16S */
    if (tex_format > 1)
    {
#ifdef TEX_DEBUG_OVERLAY
            printf("[DECODE] #%lu fmt=15BPP BYPASS pg=(%d,%d)\n",
                   (unsigned long)tex_stats.total_requests,
                   tex_page_x, tex_page_y);
#endif
        return 0;
    }

    int page_id = compute_page_id(tex_page_x, tex_page_y);
    int slot = page_slot(page_id, tex_format);
    int tbp0 = page_tbp0(page_id, tex_format);

    /* ── Check if page needs re-upload (VRAM dirty) ────────────────── */
    int tex_hw_w = (tex_format == 0) ? 64 : 128;  /* halfword width of page data */
    uint32_t current_page_gen = get_region_gen(tex_page_x, tex_page_y, tex_hw_w, 256);

    int page_dirty = (page_gen[slot] != current_page_gen);
    if (page_dirty)
    {
        uint32_t old_gen = page_gen[slot];

        /* Determine which block-rows within the page are dirty.
         * Each block-row covers 16 scanlines.  A page has 16 block-rows.
         * If page was never uploaded (old_gen==0), upload everything. */
        int use_partial = (old_gen != 0);
        uint16_t dirty_mask = 0; /* bit i = block-row i is dirty */

        if (use_partial)
        {
            unsigned int col_start = (unsigned)tex_page_x >> 6;
            unsigned int col_end   = (unsigned)(tex_page_x + tex_hw_w - 1) >> 6;
            unsigned int row_base  = (unsigned)tex_page_y >> VRAM_DIRTY_ROW_SHIFT;
            if (col_end >= VRAM_DIRTY_COLS) col_end = VRAM_DIRTY_COLS - 1;

            for (int br = 0; br < 16; br++)
            {
                unsigned int gr = row_base + br;
                if (gr >= VRAM_DIRTY_ROWS) break;
                for (unsigned int c = col_start; c <= col_end; c++)
                {
                    if (vram_page_gen[gr * VRAM_DIRTY_COLS + c] > old_gen)
                    {
                        dirty_mask |= (1 << br);
                        break; /* this block-row is dirty, no need to check more cols */
                    }
                }
            }
            /* If all 16 block-rows dirty, fall back to full upload (no benefit) */
            if (dirty_mask == 0xFFFF)
                use_partial = 0;
        }

        if (!use_partial)
        {
            /* Full page upload */
            tex_stats.full_uploads++;
#ifdef ENABLE_SUBSYSTEM_PROFILER
            gpu_frame_stats.tex_upload_full++;
            gpu_frame_stats.tex_upload_rows += 256;
            if (tex_format == 1) gpu_frame_stats.tex_upload_8bpp++;
            else                 gpu_frame_stats.tex_upload_4bpp++;
#endif
            if (tex_format == 1)
                Upload_Indexed_8BPP(tbp0, tex_page_x, tex_page_y);
            else
                Upload_Indexed_4BPP(tbp0, tex_page_x, tex_page_y);
        }
        else
        {
            tex_stats.partial_uploads++;
#ifdef ENABLE_SUBSYSTEM_PROFILER
            gpu_frame_stats.tex_upload_partial++;
            if (tex_format == 1) gpu_frame_stats.tex_upload_8bpp++;
            else                 gpu_frame_stats.tex_upload_4bpp++;
#endif
            /* Partial upload: emit one transfer per contiguous dirty range */
            int br = 0;
            while (br < 16)
            {
                if (!(dirty_mask & (1 << br))) { br++; continue; }
                int start = br;
                while (br < 16 && (dirty_mask & (1 << br))) br++;
                int start_row = start * 16;
                int num_rows  = (br - start) * 16;
#ifdef ENABLE_SUBSYSTEM_PROFILER
                gpu_frame_stats.tex_upload_rows += num_rows;
#endif
                if (tex_format == 1)
                    Upload_Indexed_8BPP_Partial(tbp0, tex_page_x, tex_page_y, start_row, num_rows);
                else
                    Upload_Indexed_4BPP_Partial(tbp0, tex_page_x, tex_page_y, start_row, num_rows);
            }
        }

        page_gen[slot] = current_page_gen;
        tex_stats.page_uploads++;
        /* Invalidate prim_tex_cache entries referencing this page.
         * With dual-format slots, only THIS format's entry is affected,
         * but we invalidate all formats for safety (3 array writes). */
        Prim_InvalidateTexCache_Page(tex_page_x, tex_page_y);
    }
    else
    {
        tex_stats.page_hits++;
        tex_stats.pixels_saved += 256 * 256;
    }

    /* ── CSM2: CLUT read directly from PSX VRAM mirror ────────────── */
    /* No CLUT upload needed.  The GS reads the CLUT directly from the
     * PSX VRAM mirror (CT16S at FBP=0) via the TEXCLUT register.
     * The mirror is already up-to-date — all VRAM writes go through
     * Upload_Shadow_VRAM_Region / GS_UploadRegionFast which apply STP
     * fixup and upload to GS VRAM block 0 as CT16S.
     *
     * Savings vs CSM1:
     *   - Eliminates 256-entry swizzle table lookup (csm1_order_256)
     *   - Eliminates CLUT GIF upload (37 QW for 8BPP, 7 QW for 4BPP)
     *   - Eliminates CLUT content cache + round-robin CBP allocator
     */

#ifdef TEX_DEBUG_OVERLAY
    printf("[DECODE] #%lu fmt=%d pg=%d(%d,%d) clut=(%d,%d) tbp=%d CSM2 %s\n",
           (unsigned long)tex_stats.total_requests,
           tex_format, page_id, tex_page_x, tex_page_y,
           clut_x, clut_y, tbp0,
           page_dirty ? "UPLOAD" : "HIT");
#endif

    *out_slot_x = tbp0;
    *out_slot_y = 0; /* unused for CSM2; TEX0.CBA=0 */
    return 3;
}

/* ── Statistics dump (called on triangle button press) ────────────── */

void Tex_Cache_DumpStats(void)
{
    printf("\n");
    printf("============================================\n");
    printf("   TEXTURE DIRECT-MAP STATISTICS\n");
    printf("============================================\n");
    printf("Total requests:     %lu\n", (unsigned long)tex_stats.total_requests);
    printf("Page hits (skip):   %lu", (unsigned long)tex_stats.page_hits);
    if (tex_stats.total_requests > 0)
        printf(" (%.1f%%)", (float)tex_stats.page_hits * 100.0f / tex_stats.total_requests);
    printf("\n");
    printf("Page uploads:       %lu", (unsigned long)tex_stats.page_uploads);
    if (tex_stats.total_requests > 0)
        printf(" (%.1f%%)", (float)tex_stats.page_uploads * 100.0f / tex_stats.total_requests);
    printf("\n");
    printf("  Full uploads:     %lu\n", (unsigned long)tex_stats.full_uploads);
    printf("  Partial uploads:  %lu\n", (unsigned long)tex_stats.partial_uploads);
    printf("Rect fallbacks:     %lu\n", (unsigned long)tex_stats.rect_fallbacks);
    printf("Pixels saved:       %llu\n", (unsigned long long)tex_stats.pixels_saved);
    printf("VRAM gen counter:   %lu (delta=%lu)\n",
           (unsigned long)vram_gen_counter,
           (unsigned long)(vram_gen_counter - tex_stats.vram_gen_at_start));
    printf("--------------------------------------------\n");
    printf("Page gen (slot): ");
    for (int i = 0; i < PAGE_SLOTS; i++)
        if (page_gen[i])
            printf("[%d]=%lu ", i, (unsigned long)page_gen[i]);
    printf("\n");
    printf("============================================\n\n");
}

void Tex_Cache_ResetStats(void)
{
    memset(&tex_stats, 0, sizeof(tex_stats));
    tex_stats.vram_gen_at_start = vram_gen_counter;
    /* Reset page dirty tracking to force re-upload on next access */
    memset(page_gen, 0, sizeof(page_gen));
}
