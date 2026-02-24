/**
 * gpu_texture.c — CLUT texture decode with HW CLUT + page-level cache
 *
 * Two rendering paths for indexed (4BPP/8BPP) textures:
 *
 * 1. HW CLUT (primary): Upload raw PSMT8/4 indices + CT16 CLUT palette.
 *    GS hardware performs per-pixel CLUT lookup — zero CPU decode.
 *    Requires CSM1 entry shuffle for 8BPP (256-entry) CLUTs.
 *
 * 2. SW decode (fallback): Full 256×256 CPU decode to CT16S when texture
 *    window is active (GS indexed formats cannot apply PSX tex window).
 *
 * 15BPP textures always use SW decode (direct color, no CLUT).
 *
 * Page-Level Cache: 16 entries with LRU eviction, keyed by
 * (format, page, clut, texwindow, vram_gen).  Per-VRAM-page dirty
 * tracking avoids false invalidations.
 */
#include "gpu_state.h"
#include <string.h> /* memcpy, memset */

/* Static decode buffer — avoids memalign/free per call (max 256×256 texels) */
static uint16_t decode_buf[256 * 256] __attribute__((aligned(64)));

/* VRAM write generation counter — bumped on every shadow VRAM modification */
uint32_t vram_gen_counter = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Per-VRAM-Page Dirty Tracking
 *
 *  VRAM split into 64×256 blocks (matching 4BPP texture page width).
 *  16 columns × 2 rows = 32 blocks.  Each block has a generation
 *  counter.  Only bumped when a VRAM write actually touches that block.
 *  Cache entries store the max gen of overlapping blocks at decode time.
 * ═══════════════════════════════════════════════════════════════════ */
#define VRAM_DIRTY_COLS 16
#define VRAM_DIRTY_ROWS 2
static uint32_t vram_page_gen[VRAM_DIRTY_COLS * VRAM_DIRTY_ROWS];

/* Bump generation for all VRAM blocks overlapping a pixel region */
void Tex_Cache_DirtyRegion(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    /* Use unsigned shifts — avoids GCC sign-extension fixup for signed / */
    unsigned int col_start = (unsigned)x >> 6;
    unsigned int col_end   = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> 8;
    unsigned int row_end   = (unsigned)(y + h - 1) >> 8;
    if (col_end >= VRAM_DIRTY_COLS)
        col_end = VRAM_DIRTY_COLS - 1;
    if (row_end >= VRAM_DIRTY_ROWS)
        row_end = VRAM_DIRTY_ROWS - 1;
    for (unsigned int r = row_start; r <= row_end; r++)
        for (unsigned int c = col_start; c <= col_end; c++)
            vram_page_gen[r * VRAM_DIRTY_COLS + c]++;
}

/* Get max generation across all VRAM blocks overlapping a pixel region */
static inline uint32_t get_region_gen(int x, int y, int w, int h)
{
    unsigned int col_start = (unsigned)x >> 6;
    unsigned int col_end   = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> 8;
    unsigned int row_end   = (unsigned)(y + h - 1) >> 8;
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

/* Compute combined generation for a texture page + CLUT region.
 * tex_format: 0=4BPP (64hw wide), 1=8BPP (128hw), 2+=15BPP (256hw) */
static inline uint32_t get_tex_combined_gen(int tex_format,
                                            int tex_page_x, int tex_page_y,
                                            int clut_x, int clut_y)
{
    /* Texture data region width in halfwords */
    int tex_hw_w = (tex_format == 0) ? 64 : (tex_format == 1) ? 128
                                                              : 256;
    uint32_t tex_gen = get_region_gen(tex_page_x, tex_page_y, tex_hw_w, 256);

    /* CLUT region (only for indexed formats) */
    if (tex_format <= 1)
    {
        int clut_entries = (tex_format == 0) ? 16 : 256;
        uint32_t clut_gen = get_region_gen(clut_x, clut_y, clut_entries, 1);
        if (clut_gen > tex_gen)
            tex_gen = clut_gen;
    }

    return tex_gen;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Page-Level Texture Cache — 16 entries, LRU eviction
 *
 *  TWO cache modes depending on texture window:
 *
 *  1. HW CLUT (no texwindow): raw indexed data uploaded as PSMT8/4,
 *     CLUT palette uploaded separately.  GS hardware does CLUT lookup.
 *     → Zero CPU decode.  Half the upload bandwidth (8-bit vs 16-bit).
 *
 *  2. SW decode (texwindow active): full 256×256 decode to CT16S
 *     as before (fallback path).
 *
 *  GS VRAM layout (in 256-byte blocks, TBP0 units):
 *    [0..4095]       PSX VRAM (CT16S, 1MB)
 *    [4096..8191]    PSMT8/4 indexed texture cache (16 slots × 256 blocks)
 *    [8192..8703]    CT16 CLUT storage (16 slots × 32 blocks)
 *    [8704..12799]   CT16S SW decode cache (16 slots × 256 blocks)
 * ═══════════════════════════════════════════════════════════════════ */
#define TEX_CACHE_SLOTS 16

/* HW CLUT texture slots (PSMT8/4 format) */
#define HW_TEX_TBP_BASE 4096
#define HW_TEX_TBP_STRIDE 256 /* 256×256 PSMT8 = 64KB = 256 blocks */

/* CLUT palette slots — one CT16 page per CLUT to avoid swizzle overlap */
#define HW_CLUT_CBP_BASE 8192
#define HW_CLUT_CBP_STRIDE 32 /* 1 CT16 page = 32 blocks (64×64 pixels) */

/* SW decode slots (CT16S format, for texwindow fallback) */
#define SW_TEX_TBP_BASE (HW_CLUT_CBP_BASE + TEX_CACHE_SLOTS * HW_CLUT_CBP_STRIDE)
#define SW_TEX_TBP_STRIDE 256 /* 256×256 CT16S decoded = same block count */

typedef struct
{
    int valid;
    int tex_format;
    int tex_page_x, tex_page_y;
    int clut_x, clut_y;
    uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    uint32_t combined_gen; /* max gen of tex data + CLUT page blocks */
    int is_hw_clut;        /* 1 = PSMT8/4 + CLUT (HW), 0 = CT16S (SW) */
    int hw_tbp0;           /* HW CLUT: TBP0 for indexed texture data */
    int hw_cbp;            /* HW CLUT: CBP for CLUT palette */
    int slot_x, slot_y;    /* SW decode: position in GS VRAM */
    uint32_t lru_tick;
} TexPageCacheEntry;

static TexPageCacheEntry tex_page_cache[TEX_CACHE_SLOTS];
static uint32_t tex_cache_tick = 0;
static int last_hit_slot = 0;  /* MRU shortcut — last cache hit index */
static uint32_t last_mru_vram_gen = 0; /* vram_gen_counter at last MRU hit */

/* SW decode slot layout — Y=512+ (v4 layout) */
static inline void tex_cache_get_sw_slot_pos(int slot, int *x, int *y)
{
    *x = (slot % 4) * 256;
    *y = CLUT_DECODED_Y + (slot / 4) * 256; /* Y=512..1792 */
}

/* ── Statistics ───────────────────────────────────────────────────── */
static struct
{
    uint32_t total_requests;
    uint32_t page_hits;
    uint32_t page_misses;
    uint32_t evictions;
    uint32_t dirty_invalidations; /* misses due to dirty page */
    uint32_t rect_fallbacks;
    uint32_t hw_clut_uploads;   /* HW CLUT path cache misses */
    uint32_t sw_decode_uploads; /* SW decode path cache misses */
    uint64_t pixels_decoded;
    uint64_t pixels_saved;
    uint32_t vram_gen_at_start;
} tex_stats;

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
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);                           /* TRXPOS: (0,0)→(0,0) */
    Push_GIF_Data(GS_SET_TRXREG(256, 256), GS_REG_TRXREG); /* TRXREG: 256×256 */
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);                           /* TRXDIR: Host→Local */

    /* Pack raw bytes into IMAGE quadwords.
     * Little-endian MIPS: psx_vram_shadow halfwords contain [lo=texel_even, hi=texel_odd].
     * Reinterpreting as uint8_t* gives texels in correct order for PSMT8. */
    buf_image_ptr = 0;
    for (int row = 0; row < 256; row++)
    {
        const uint8_t *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
        /* 256 bytes per row, 16 bytes per quadword = 16 QWs per row */
        for (int qw = 0; qw < 16; qw++)
        {
            uint64_t lo, hi;
            memcpy(&lo, &src[qw * 16], 8);
            memcpy(&hi, &src[qw * 16 + 8], 8);
            buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);

            if (buf_image_ptr >= 1000)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
                for (int j = 0; j < buf_image_ptr; j++)
                {
                    uint64_t *pp = (uint64_t *)&buf_image[j];
                    Push_GIF_Data(pp[0], pp[1]);
                }
                buf_image_ptr = 0;
            }
        }
    }
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
        for (int j = 0; j < buf_image_ptr; j++)
        {
            uint64_t *pp = (uint64_t *)&buf_image[j];
            Push_GIF_Data(pp[0], pp[1]);
        }
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

    /* 4BPP: 128 bytes per row (256 texels / 2). PSX nibble layout matches PSMT4. */
    buf_image_ptr = 0;
    for (int row = 0; row < 256; row++)
    {
        const uint8_t *src = (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
        /* 128 bytes per row, 16 bytes per QW = 8 QWs per row */
        for (int qw = 0; qw < 8; qw++)
        {
            uint64_t lo, hi;
            memcpy(&lo, &src[qw * 16], 8);
            memcpy(&hi, &src[qw * 16 + 8], 8);
            buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);

            if (buf_image_ptr >= 1000)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
                for (int j = 0; j < buf_image_ptr; j++)
                {
                    uint64_t *pp = (uint64_t *)&buf_image[j];
                    Push_GIF_Data(pp[0], pp[1]);
                }
                buf_image_ptr = 0;
            }
        }
    }
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
        for (int j = 0; j < buf_image_ptr; j++)
        {
            uint64_t *pp = (uint64_t *)&buf_image[j];
            Push_GIF_Data(pp[0], pp[1]);
        }
    }
}

/* Upload CLUT palette to GS VRAM (CSM1, PSMCT16).
 * Pre-processes PSX palette: set STP bit (bit 15) for non-zero entries.
 * Applies CSM1 entry shuffle (swap entries where (i & 0x18) == 8 with i+8).
 * For 8BPP: 256 entries uploaded as 16×16 rectangle.
 * For 4BPP: 16 entries uploaded as 16×1 rectangle. */
static void Upload_CLUT_CSM1(int cbp, int clut_x, int clut_y, int tex_format)
{
    const uint16_t *raw_clut = &psx_vram_shadow[clut_y * 1024 + clut_x];
    int num_entries = (tex_format == 0) ? 16 : 256;

    /* Pre-process: STP bit + CSM1 shuffle into temp buffer */
    uint16_t clut_buf[256];
    for (int i = 0; i < num_entries; i++)
    {
        uint16_t c = raw_clut[i];
        if (c != 0)
            c |= 0x8000;
        clut_buf[i] = c;
    }

    /* CSM1 shuffle: swap entries where (i & 0x18) == 8 with i+8
     * This reorders each 32-entry group: [0-7, 8-15, 16-23, 24-31]
     *  becomes [0-7, 16-23, 8-15, 24-31] in the upload buffer */
    if (num_entries == 256)
    {
        for (int i = 0; i < 256; i++)
        {
            if ((i & 0x18) == 8)
            {
                uint16_t tmp = clut_buf[i];
                clut_buf[i] = clut_buf[i + 8];
                clut_buf[i + 8] = tmp;
            }
        }
    }

    int upload_w = (num_entries == 256) ? 16 : 8;
    int upload_h = (num_entries == 256) ? 16 : 2;

    /* BITBLTBUF: DBP=cbp, DBW=1 (64px), DPSM=CT16 (matches CSM1 standard) */
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, cbp, 1, GS_PSM_16), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(upload_w, upload_h), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* Pack shuffled CLUT entries into IMAGE quadwords */
    buf_image_ptr = 0;
    uint32_t pend[4];
    int pc = 0;
    for (int i = 0; i < num_entries; i += 2)
    {
        uint16_t p0 = clut_buf[i];
        uint16_t p1 = (i + 1 < num_entries) ? clut_buf[i + 1] : 0;
        pend[pc++] = (uint32_t)p0 | ((uint32_t)p1 << 16);
        if (pc >= 4)
        {
            uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
            uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
            buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
            pc = 0;
        }
    }
    if (pc > 0)
    {
        while (pc < 4)
            pend[pc++] = 0;
        uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
        uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
        buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
    }
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
        for (int j = 0; j < buf_image_ptr; j++)
        {
            uint64_t *pp = (uint64_t *)&buf_image[j];
            Push_GIF_Data(pp[0], pp[1]);
        }
    }
}

/* ── Texture window coordinate transform ─────────────────────────── */

// Apply PSX texture window formula to a texture coordinate
// texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
uint32_t Apply_Tex_Window_U(uint32_t u)
{
    if (tex_win_mask_x == 0)
        return u;
    uint32_t mask = tex_win_mask_x * 8;
    uint32_t off = (tex_win_off_x & tex_win_mask_x) * 8;
    return (u & ~mask) | off;
}

uint32_t Apply_Tex_Window_V(uint32_t v)
{
    if (tex_win_mask_y == 0)
        return v;
    uint32_t mask = tex_win_mask_y * 8;
    uint32_t off = (tex_win_off_y & tex_win_mask_y) * 8;
    return (v & ~mask) | off;
}

/* ── Internal: Decode full 256×256 texture page (optimized) ────────── */

static void Decode_FullPage(int tex_format,
                            int tex_page_x, int tex_page_y,
                            int clut_x, int clut_y)
{
    uint16_t *decoded = decode_buf;

    /* Pre-expand CLUT with STP bit applied (eliminates branch per pixel) */
    const uint16_t *raw_clut = &psx_vram_shadow[clut_y * 1024 + clut_x];

    int no_texwin = (tex_win_mask_x == 0 && tex_win_mask_y == 0);

    if (tex_format == 0) /* 4BPP CLUT */
    {
        uint16_t exp_clut[16];
        for (int i = 0; i < 16; i++)
        {
            exp_clut[i] = raw_clut[i];
            if (exp_clut[i] != 0)
                exp_clut[i] |= 0x8000;
        }

        if (no_texwin)
        {
            /* Fast path: no texture window — process 4 texels per halfword */
            for (int row = 0; row < 256; row++)
            {
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col += 4)
                {
                    uint16_t packed = tex_row[col >> 2];
                    dst[col + 0] = exp_clut[(packed >> 0) & 0xF];
                    dst[col + 1] = exp_clut[(packed >> 4) & 0xF];
                    dst[col + 2] = exp_clut[(packed >> 8) & 0xF];
                    dst[col + 3] = exp_clut[(packed >> 12) & 0xF];
                }
            }
        }
        else
        {
            register uint32_t m_x = ~(tex_win_mask_x * 8) & 0xFF;
            register uint32_t o_x = (tex_win_off_x & tex_win_mask_x) * 8;
            register uint32_t m_y = ~(tex_win_mask_y * 8) & 0xFF;
            register uint32_t o_y = (tex_win_off_y & tex_win_mask_y) * 8;

            for (int row = 0; row < 256; row++)
            {
                uint32_t v_win = ((uint32_t)row & m_y) | o_y;
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col++)
                {
                    uint32_t u_win = ((uint32_t)col & m_x) | o_x;
                    uint16_t packed = tex_row[tex_page_x + (u_win >> 2)];
                    dst[col] = exp_clut[(packed >> ((u_win & 3) * 4)) & 0xF];
                }
            }
        }
    }
    else if (tex_format == 1) /* 8BPP CLUT */
    {
        uint16_t exp_clut[256];
        for (int i = 0; i < 256; i++)
        {
            exp_clut[i] = raw_clut[i];
            if (exp_clut[i] != 0)
                exp_clut[i] |= 0x8000;
        }

        if (no_texwin)
        {
            /* Fast path: no texture window — process 2 texels per halfword */
            for (int row = 0; row < 256; row++)
            {
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col += 2)
                {
                    uint16_t packed = tex_row[col >> 1];
                    dst[col + 0] = exp_clut[packed & 0xFF];
                    dst[col + 1] = exp_clut[(packed >> 8) & 0xFF];
                }
            }
        }
        else
        {
            register uint32_t m_x = ~(tex_win_mask_x * 8) & 0xFF;
            register uint32_t o_x = (tex_win_off_x & tex_win_mask_x) * 8;
            register uint32_t m_y = ~(tex_win_mask_y * 8) & 0xFF;
            register uint32_t o_y = (tex_win_off_y & tex_win_mask_y) * 8;

            for (int row = 0; row < 256; row++)
            {
                uint32_t v_win = ((uint32_t)row & m_y) | o_y;
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col++)
                {
                    uint32_t u_win = ((uint32_t)col & m_x) | o_x;
                    uint16_t packed = tex_row[tex_page_x + (u_win >> 1)];
                    dst[col] = exp_clut[(packed >> ((u_win & 1) * 8)) & 0xFF];
                }
            }
        }
    }
    else /* 15BPP (format 2 or 3) */
    {
        if (no_texwin)
        {
            /* Fast path: direct memcpy with STP fixup */
            for (int row = 0; row < 256; row++)
            {
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col++)
                {
                    uint16_t pixel = tex_row[col];
                    if (pixel != 0)
                        pixel |= 0x8000;
                    dst[col] = pixel;
                }
            }
        }
        else
        {
            register uint32_t m_x = ~(tex_win_mask_x * 8) & 0xFF;
            register uint32_t o_x = (tex_win_off_x & tex_win_mask_x) * 8;
            register uint32_t m_y = ~(tex_win_mask_y * 8) & 0xFF;
            register uint32_t o_y = (tex_win_off_y & tex_win_mask_y) * 8;

            for (int row = 0; row < 256; row++)
            {
                uint32_t v_win = ((uint32_t)row & m_y) | o_y;
                const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024 + tex_page_x];
                uint16_t *dst = &decoded[row * 256];

                for (int col = 0; col < 256; col++)
                {
                    uint32_t u_win = ((uint32_t)col & m_x) | o_x;
                    uint16_t pixel = tex_row[u_win];
                    if (pixel != 0)
                        pixel |= 0x8000;
                    dst[col] = pixel;
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Decode_TexPage_Cached — Page-level texture cache with LRU
 *
 *  Returns:
 *    0 = failure
 *    1 = SW decode (CT16S).  out_slot_x/out_slot_y = UV offsets.
 *    2 = HW CLUT (PSMT8/4).  out_slot_x = TBP0, out_slot_y = CBP.
 *        Caller must set TEX0 for indexed texture mode.
 * ═══════════════════════════════════════════════════════════════════ */

int Decode_TexPage_Cached(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y)
{
    tex_stats.total_requests++;
    tex_cache_tick++;

    /* Determine if HW CLUT path is usable:
     * - Must be indexed format (4BPP or 8BPP)
     * - Must NOT have texture window active (mask=0)
     * 15BPP always uses SW path. */
    int use_hw_clut = (tex_format <= 1 && tex_win_mask_x == 0 && tex_win_mask_y == 0);

    /* ── MRU shortcut: check last-hit slot before full scan ──── */
    /* Fast path: if vram_gen_counter hasn't changed since last MRU hit,
     * no VRAM was modified → combined_gen is unchanged, skip the
     * expensive get_tex_combined_gen() multi-block scan (~365K calls). */
    int vram_unchanged = (vram_gen_counter == last_mru_vram_gen);
    uint32_t current_gen;

    {
        TexPageCacheEntry *e = &tex_page_cache[last_hit_slot];
        if (e->valid &&
            e->tex_format == tex_format &&
            e->tex_page_x == tex_page_x &&
            e->tex_page_y == tex_page_y &&
            e->clut_x == clut_x &&
            e->clut_y == clut_y &&
            e->tw_mask_x == tex_win_mask_x &&
            e->tw_mask_y == tex_win_mask_y &&
            e->tw_off_x == tex_win_off_x &&
            e->tw_off_y == tex_win_off_y)
        {
            /* Parameters match — check generation validity */
            if (vram_unchanged)
            {
                /* No VRAM changes → cached gen still correct */
                tex_stats.page_hits++;
                tex_stats.pixels_saved += 256 * 256;
                e->lru_tick = tex_cache_tick;
                if (e->is_hw_clut)
                {
                    *out_slot_x = e->hw_tbp0;
                    *out_slot_y = e->hw_cbp;
                    return 2;
                }
                *out_slot_x = e->slot_x;
                *out_slot_y = e->slot_y - CLUT_DECODED_Y;
                return 1;
            }
            /* VRAM changed — recompute gen and compare */
            current_gen = get_tex_combined_gen(tex_format, tex_page_x, tex_page_y, clut_x, clut_y);
            if (e->combined_gen == current_gen)
            {
                tex_stats.page_hits++;
                tex_stats.pixels_saved += 256 * 256;
                e->lru_tick = tex_cache_tick;
                last_mru_vram_gen = vram_gen_counter;
                if (e->is_hw_clut)
                {
                    *out_slot_x = e->hw_tbp0;
                    *out_slot_y = e->hw_cbp;
                    return 2;
                }
                *out_slot_x = e->slot_x;
                *out_slot_y = e->slot_y - CLUT_DECODED_Y;
                return 1;
            }
        }
    }

    /* current_gen is needed for the linear scan below.  The fast path
     * (unchanged VRAM + MRU param match) already returned above.
     * It may have been computed in the MRU block (vram changed + params
     * matched); recompute unconditionally to cover all fall-through cases. */
    current_gen = get_tex_combined_gen(tex_format, tex_page_x, tex_page_y, clut_x, clut_y);

    /* ── Search for matching entry ─────────────────────────────── */
    for (int i = 0; i < TEX_CACHE_SLOTS; i++)
    {
        TexPageCacheEntry *e = &tex_page_cache[i];
        if (e->valid &&
            e->combined_gen == current_gen &&
            e->tex_format == tex_format &&
            e->tex_page_x == tex_page_x &&
            e->tex_page_y == tex_page_y &&
            e->clut_x == clut_x &&
            e->clut_y == clut_y &&
            e->tw_mask_x == tex_win_mask_x &&
            e->tw_mask_y == tex_win_mask_y &&
            e->tw_off_x == tex_win_off_x &&
            e->tw_off_y == tex_win_off_y)
        {
            /* Cache HIT */
            tex_stats.page_hits++;
            tex_stats.pixels_saved += 256 * 256;
            e->lru_tick = tex_cache_tick;
            last_hit_slot = i;
            last_mru_vram_gen = vram_gen_counter;

            if (e->is_hw_clut)
            {
                *out_slot_x = e->hw_tbp0;
                *out_slot_y = e->hw_cbp;
                return 2;
            }
            else
            {
                *out_slot_x = e->slot_x;
                *out_slot_y = e->slot_y - CLUT_DECODED_Y; /* relative to TBP0=4096 */
                return 1;
            }
        }
    }

    /* ── Cache MISS — find slot via LRU eviction ───────────────── */
    tex_stats.page_misses++;

    int evict_idx = 0;
    uint32_t oldest_tick = 0xFFFFFFFF;
    for (int i = 0; i < TEX_CACHE_SLOTS; i++)
    {
        if (!tex_page_cache[i].valid)
        {
            evict_idx = i;
            break;
        }
        if (tex_page_cache[i].lru_tick < oldest_tick)
        {
            oldest_tick = tex_page_cache[i].lru_tick;
            evict_idx = i;
        }
    }

    if (tex_page_cache[evict_idx].valid)
        tex_stats.evictions++;

    /* ── Upload to GS VRAM ─────────────────────────────────────── */
    TexPageCacheEntry *e = &tex_page_cache[evict_idx];

    if (use_hw_clut)
    {
        /* === HW CLUT path: upload raw indexed data + CLUT palette === */
        int tbp0 = HW_TEX_TBP_BASE + evict_idx * HW_TEX_TBP_STRIDE;
        int cbp = HW_CLUT_CBP_BASE + evict_idx * HW_CLUT_CBP_STRIDE;

        if (tex_format == 1)
            Upload_Indexed_8BPP(tbp0, tex_page_x, tex_page_y);
        else
            Upload_Indexed_4BPP(tbp0, tex_page_x, tex_page_y);

        Upload_CLUT_CSM1(cbp, clut_x, clut_y, tex_format);
        tex_stats.hw_clut_uploads++;

        e->is_hw_clut = 1;
        e->hw_tbp0 = tbp0;
        e->hw_cbp = cbp;
        e->slot_x = 0;
        e->slot_y = 0;

        *out_slot_x = tbp0;
        *out_slot_y = cbp;
    }
    else
    {
        /* === SW decode path: full 256×256 CLUT decode + CT16S upload === */
        Decode_FullPage(tex_format, tex_page_x, tex_page_y, clut_x, clut_y);
        tex_stats.pixels_decoded += 256 * 256;
        tex_stats.sw_decode_uploads++;

        int slot_x, slot_y;
        tex_cache_get_sw_slot_pos(evict_idx, &slot_x, &slot_y);
        GS_UploadRegion(slot_x, slot_y, 256, 256, decode_buf);

        e->is_hw_clut = 0;
        e->hw_tbp0 = 0;
        e->hw_cbp = 0;
        e->slot_x = slot_x;
        e->slot_y = slot_y;

        *out_slot_x = slot_x;
        *out_slot_y = slot_y - CLUT_DECODED_Y; /* relative to TBP0=4096 base (Y=512) */
    }

    /* Update common cache entry fields */
    e->valid = 1;
    e->tex_format = tex_format;
    e->tex_page_x = tex_page_x;
    e->tex_page_y = tex_page_y;
    e->clut_x = clut_x;
    e->clut_y = clut_y;
    e->tw_mask_x = tex_win_mask_x;
    e->tw_mask_y = tex_win_mask_y;
    e->tw_off_x = tex_win_off_x;
    e->tw_off_y = tex_win_off_y;
    e->combined_gen = current_gen;
    e->lru_tick = tex_cache_tick;
    last_hit_slot = evict_idx;
    last_mru_vram_gen = vram_gen_counter;

    return use_hw_clut ? 2 : 1;
}

/* ── Statistics dump (called on triangle button press) ────────────── */

void Tex_Cache_DumpStats(void)
{
    printf("\n");
    printf("============================================\n");
    printf("   TEXTURE PAGE CACHE STATISTICS\n");
    printf("============================================\n");
    printf("Total requests:     %lu\n", (unsigned long)tex_stats.total_requests);
    printf("Page cache hits:    %lu", (unsigned long)tex_stats.page_hits);
    if (tex_stats.total_requests > 0)
        printf(" (%.1f%%)", (float)tex_stats.page_hits * 100.0f / tex_stats.total_requests);
    printf("\n");
    printf("Page cache misses:  %lu", (unsigned long)tex_stats.page_misses);
    if (tex_stats.total_requests > 0)
        printf(" (%.1f%%)", (float)tex_stats.page_misses * 100.0f / tex_stats.total_requests);
    printf("\n");
    printf("Evictions (LRU):    %lu\n", (unsigned long)tex_stats.evictions);
    printf("Dirty invalidations:%lu\n", (unsigned long)tex_stats.dirty_invalidations);
    printf("Rect fallbacks:     %lu\n", (unsigned long)tex_stats.rect_fallbacks);
    printf("HW CLUT uploads:    %lu\n", (unsigned long)tex_stats.hw_clut_uploads);
    printf("SW decode uploads:  %lu\n", (unsigned long)tex_stats.sw_decode_uploads);
    printf("Pixels decoded(SW): %llu\n", (unsigned long long)tex_stats.pixels_decoded);
    printf("Pixels saved:       %llu\n", (unsigned long long)tex_stats.pixels_saved);
    printf("VRAM gen counter:   %lu (delta=%lu)\n",
           (unsigned long)vram_gen_counter,
           (unsigned long)(vram_gen_counter - tex_stats.vram_gen_at_start));
    printf("--------------------------------------------\n");
    printf("Active cache entries:\n");
    for (int i = 0; i < TEX_CACHE_SLOTS; i++)
    {
        TexPageCacheEntry *e = &tex_page_cache[i];
        if (e->valid)
        {
            const char *fmt_str = e->tex_format == 0 ? "4BPP" : e->tex_format == 1 ? "8BPP"
                                                                                   : "15BPP";
            printf("  [%d] %s page=(%d,%d) clut=(%d,%d) gen=%lu lru=%lu %s %s\n",
                   i, fmt_str,
                   e->tex_page_x, e->tex_page_y,
                   e->clut_x, e->clut_y,
                   (unsigned long)e->combined_gen,
                   (unsigned long)e->lru_tick,
                   e->is_hw_clut ? "[HW_CLUT]" : "[SW_DEC]",
                   (e->combined_gen == get_tex_combined_gen(e->tex_format, e->tex_page_x, e->tex_page_y, e->clut_x, e->clut_y)) ? "[VALID]" : "[STALE]");
        }
        else
        {
            printf("  [%d] (empty)\n", i);
        }
    }
    printf("============================================\n\n");
}

void Tex_Cache_ResetStats(void)
{
    memset(&tex_stats, 0, sizeof(tex_stats));
    tex_stats.vram_gen_at_start = vram_gen_counter;
}

/* ── Per-pixel texture window decode (legacy, used as fallback) ───── */

// Decode a textured rect region with per-pixel texture window masking.
// This is the FALLBACK path — only used when page-level cache cannot be used
// (e.g., oversized rects or direct callers needing exact UV region).
// tex_format: 0=4BPP, 1=8BPP, 2=15BPP
// Reads from psx_vram_shadow (CPU shadow copy).
// Uploads result to CLUT_DECODED area.

int Decode_TexWindow_Rect(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int u0_cmd, int v0_cmd, int w, int h,
                          int flip_x, int flip_y)
{
    /* Clamp to static buffer capacity */
    if (w <= 0 || h <= 0 || w > 256 || h > 256)
        return 0;

    tex_stats.rect_fallbacks++;

    uint16_t *decoded = decode_buf;

    /* ── Pre-compute texture window masks inline (register optimized) ── */
    register uint32_t m_x = ~(tex_win_mask_x * 8) & 0xFF;
    register uint32_t o_x = (tex_win_off_x & tex_win_mask_x) * 8;
    register uint32_t m_y = ~(tex_win_mask_y * 8) & 0xFF;
    register uint32_t o_y = (tex_win_off_y & tex_win_mask_y) * 8;

    /* ── Pre-compute CLUT base pointer for indexed modes ───────── */
    const uint16_t *clut_ptr = &psx_vram_shadow[clut_y * 1024 + clut_x];

    /* ── Format-specialised decode loops ───────────────────────── */
    if (tex_format == 0) /* 4BPP CLUT */
    {
        for (int row = 0; row < h; row++)
        {
            uint32_t v_iter = flip_y ? (uint32_t)(v0_cmd - row) : (uint32_t)(v0_cmd + row);
            uint32_t v_win = (v_iter & m_y) | o_y;
            const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024];
            uint16_t *dst = &decoded[row * w];

            for (int col = 0; col < w; col++)
            {
                uint32_t u_iter = flip_x ? (uint32_t)(u0_cmd - col) : (uint32_t)(u0_cmd + col);
                uint32_t u_win = (u_iter & m_x) | o_x;

                uint16_t packed = tex_row[tex_page_x + (u_win >> 2)];
                int idx = (packed >> ((u_win & 3) * 4)) & 0xF;
                uint16_t pixel = clut_ptr[idx];
                if (pixel != 0)
                    pixel |= 0x8000;
                dst[col] = pixel;
            }
        }
    }
    else if (tex_format == 1) /* 8BPP CLUT */
    {
        for (int row = 0; row < h; row++)
        {
            uint32_t v_iter = flip_y ? (uint32_t)(v0_cmd - row) : (uint32_t)(v0_cmd + row);
            uint32_t v_win = (v_iter & m_y) | o_y;
            const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024];
            uint16_t *dst = &decoded[row * w];

            for (int col = 0; col < w; col++)
            {
                uint32_t u_iter = flip_x ? (uint32_t)(u0_cmd - col) : (uint32_t)(u0_cmd + col);
                uint32_t u_win = (u_iter & m_x) | o_x;

                uint16_t packed = tex_row[tex_page_x + (u_win >> 1)];
                int idx = (packed >> ((u_win & 1) * 8)) & 0xFF;
                uint16_t pixel = clut_ptr[idx];
                if (pixel != 0)
                    pixel |= 0x8000;
                dst[col] = pixel;
            }
        }
    }
    else /* 15BPP (format 2 or 3) */
    {
        for (int row = 0; row < h; row++)
        {
            uint32_t v_iter = flip_y ? (uint32_t)(v0_cmd - row) : (uint32_t)(v0_cmd + row);
            uint32_t v_win = (v_iter & m_y) | o_y;
            const uint16_t *tex_row = &psx_vram_shadow[(tex_page_y + v_win) * 1024 + tex_page_x];
            uint16_t *dst = &decoded[row * w];

            for (int col = 0; col < w; col++)
            {
                uint32_t u_iter = flip_x ? (uint32_t)(u0_cmd - col) : (uint32_t)(u0_cmd + col);
                uint32_t u_win = (u_iter & m_x) | o_x;

                uint16_t pixel = tex_row[u_win];
                if (pixel != 0)
                    pixel |= 0x8000;
                dst[col] = pixel;
            }
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, w, h, decoded);

    return 1;
}

/* ── 4-bit CLUT texture decode ───────────────────────────────────── */

// Decode a 4-bit CLUT texture region and upload to GS VRAM at CLUT_DECODED_Y.
// clut_x, clut_y: CLUT position in PSX VRAM (16 entries for 4-bit)
// tex_x, tex_y: texture page position in PSX VRAM (in halfword coords)
// u0, v0: start UV, tw, th: size to decode (in texel coords)
// Returns 1 on success, 0 on failure.
int Decode_CLUT4_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    // 4-bit mode: each halfword at (tex_x + u/4, tex_y + v) holds 4 nibbles
    // Nibble index = u % 4, from LSB: bits [3:0],[7:4],[11:8],[15:12]
    int hw_x0 = u0 / 4;
    int hw_x1 = (u0 + tw + 3) / 4;
    int hw_w = hw_x1 - hw_x0;
    int rb_x = tex_x + hw_x0;
    int rb_y = tex_y + v0;
    int rb_w = hw_w;
    int rb_h = th;

    int clut_rb_w = 16;

    // Align widths to 8 for qword boundary
    int rb_w_aligned = (rb_w + 7) & ~7;
    int clut_w_aligned = (clut_rb_w + 7) & ~7;

    int tex_bytes = rb_w_aligned * rb_h * 2;
    int clut_bytes = clut_w_aligned * 1 * 2;
    int tex_qwc = (tex_bytes + 15) / 16;
    int clut_qwc = (clut_bytes + 15) / 16;

    void *tex_buf = memalign(64, tex_qwc * 16);
    void *clut_buf = memalign(64, clut_qwc * 16);
    if (!tex_buf || !clut_buf)
    {
        if (tex_buf)
            free(tex_buf);
        if (clut_buf)
            free(clut_buf);
        return 0;
    }

    uint16_t *clut_uc = GS_ReadbackRegion(clut_x, clut_y, clut_w_aligned, 1, clut_buf, clut_qwc);
    uint16_t *tex_uc = GS_ReadbackRegion(rb_x, rb_y, rb_w_aligned, rb_h, tex_buf, tex_qwc);

    uint16_t *decoded = (uint16_t *)memalign(64, tw * th * 2);
    if (!decoded)
    {
        free(tex_buf);
        free(clut_buf);
        return 0;
    }

    for (int row = 0; row < th; row++)
    {
        for (int col = 0; col < tw; col++)
        {
            int texel_u = u0 + col;
            int hw_col = texel_u / 4 - hw_x0;
            int nibble = texel_u % 4;
            uint16_t packed = tex_uc[row * rb_w_aligned + hw_col];
            int idx = (packed >> (nibble * 4)) & 0xF;
            uint16_t cv = clut_uc[idx];
            if (cv != 0)
                cv |= 0x8000;
            decoded[row * tw + col] = cv;
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, tw, th, decoded);

    free(decoded);
    free(tex_buf);
    free(clut_buf);
    return 1;
}

/* ── 8-bit CLUT texture decode ───────────────────────────────────── */

int Decode_CLUT8_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    int hw_x0 = u0 / 2;
    int hw_x1 = (u0 + tw + 1) / 2;
    int hw_w = hw_x1 - hw_x0;
    int rb_x = tex_x + hw_x0;
    int rb_y = tex_y + v0;
    int rb_w = hw_w;
    int rb_h = th;

    int clut_rb_w = 256;
    int rb_w_aligned = (rb_w + 7) & ~7;
    int clut_w_aligned = (clut_rb_w + 7) & ~7;

    int tex_bytes = rb_w_aligned * rb_h * 2;
    int clut_bytes = clut_w_aligned * 1 * 2;
    int tex_qwc = (tex_bytes + 15) / 16;
    int clut_qwc = (clut_bytes + 15) / 16;

    void *tex_buf = memalign(64, tex_qwc * 16);
    void *clut_buf = memalign(64, clut_qwc * 16);
    if (!tex_buf || !clut_buf)
    {
        if (tex_buf)
            free(tex_buf);
        if (clut_buf)
            free(clut_buf);
        return 0;
    }

    uint16_t *clut_uc = GS_ReadbackRegion(clut_x, clut_y, clut_w_aligned, 1, clut_buf, clut_qwc);
    uint16_t *tex_uc = GS_ReadbackRegion(rb_x, rb_y, rb_w_aligned, rb_h, tex_buf, tex_qwc);

    uint16_t *decoded = (uint16_t *)memalign(64, tw * th * 2);
    if (!decoded)
    {
        free(tex_buf);
        free(clut_buf);
        return 0;
    }

    for (int row = 0; row < th; row++)
    {
        for (int col = 0; col < tw; col++)
        {
            int texel_u = u0 + col;
            int hw_col = texel_u / 2 - hw_x0;
            int byte_idx = texel_u % 2;
            uint16_t packed = tex_uc[row * rb_w_aligned + hw_col];
            int idx = (packed >> (byte_idx * 8)) & 0xFF;
            uint16_t cv = clut_uc[idx];
            if (cv != 0)
                cv |= 0x8000;
            decoded[row * tw + col] = cv;
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, tw, th, decoded);

    free(decoded);
    free(tex_buf);
    free(clut_buf);
    return 1;
}
