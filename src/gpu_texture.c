/**
 * gpu_texture.c — CLUT texture decode with HW CLUT + page-level cache
 *
 * Two rendering paths for indexed (4BPP/8BPP) textures:
 *
 * 1. HW CLUT (primary): Upload raw PSMT8/4 indices + CT16 CLUT palette.
 *    GS hardware performs per-pixel CLUT lookup — zero CPU decode.
 *    Requires CSM1 entry shuffle for 8BPP (256-entry) CLUTs.
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
    unsigned int col_end = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> 8;
    unsigned int row_end = (unsigned)(y + h - 1) >> 8;
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
    unsigned int col_end = (unsigned)(x + w - 1) >> 6;
    unsigned int row_start = (unsigned)y >> 8;
    unsigned int row_end = (unsigned)(y + h - 1) >> 8;
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
 *  Page-Level Texture Cache — 32 entries, LRU eviction
 *
 *  HW CLUT only (indexed 4BPP/8BPP): raw indexed data uploaded as PSMT8/4,
 *  CLUT palette uploaded separately.  GS hardware does CLUT lookup.
 *  Texture windows handled by GS CLAMP_1 REGION_REPEAT mode.
 *  → Zero CPU decode.  Half the upload bandwidth (8-bit vs 16-bit).
 *
 *  15BPP textures bypass this cache entirely — they reference PSX VRAM
 *  directly as CT16S with per-vertex Apply_Tex_Window.
 *
 *  GS VRAM layout (in 256-byte blocks, TBP0 units):
 *    [0..4095]         PSX VRAM (CT16S, 1MB)
 *    [4096..12287]     PSMT8/4 indexed texture cache (32 slots × 256 blocks)
 *    [12288..13311]    CT16 CLUT storage (32 slots × 32 blocks)
 *    Total: 13312 / 16384 blocks used (81.2%)
 * ═══════════════════════════════════════════════════════════════════ */
#define TEX_CACHE_SLOTS 32

/* HW CLUT texture slots (PSMT8/4 format) */
#define HW_TEX_TBP_BASE 4096
#define HW_TEX_TBP_STRIDE 256 /* 256×256 PSMT8 = 64KB = 256 blocks */

/* CLUT palette slots — one CT16 page per CLUT to avoid swizzle overlap */
#define HW_CLUT_CBP_BASE (HW_TEX_TBP_BASE + TEX_CACHE_SLOTS * HW_TEX_TBP_STRIDE)
#define HW_CLUT_CBP_STRIDE 32 /* 1 CT16 page = 32 blocks (64×64 pixels) */

typedef struct
{
    int valid;
    int tex_format;
    int tex_page_x, tex_page_y;
    int clut_x, clut_y;
    uint32_t combined_gen; /* max gen of tex data + CLUT page blocks */
    int is_hw_clut;        /* always 1 for HW CLUT path */
    int hw_tbp0;           /* HW CLUT: TBP0 for indexed texture data */
    int hw_cbp;            /* HW CLUT: CBP for CLUT palette */
    uint32_t lru_tick;
} TexPageCacheEntry;

static TexPageCacheEntry tex_page_cache[TEX_CACHE_SLOTS];
static uint32_t tex_cache_tick = 0;
static int last_hit_slot = 0;          /* MRU shortcut — last cache hit index */
static uint32_t last_mru_vram_gen = 0; /* vram_gen_counter at last MRU hit */

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
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(256, 256), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* 256×256 8BPP = 64KB = 4096 QWs.  Split into 4 chunks of 1024 QWs
     * (64 rows each) for GIF buffer safety.  Direct memcpy from VRAM
     * shadow eliminates per-QW loop overhead of the old buf_image path. */
    for (int chunk = 0; chunk < 4; chunk++)
    {
        int eop = (chunk == 3) ? 1 : 0;
        Push_GIF_Tag(GIF_TAG_LO(1024, eop, 0, 0, 2, 0), 0);
        for (int row = chunk * 64; row < (chunk + 1) * 64; row++)
        {
            /* 256 bytes/row = 16 QWs.  Source is uint16_t* reinterpreted
             * as raw bytes (little-endian, natural PSMT8 order). */
            memcpy(fast_gif_ptr,
                   (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x],
                   256);
            fast_gif_ptr += 16;
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

    /* 256×256 4BPP = 32KB = 2048 QWs.  Split into 2 chunks of 1024 QWs
     * (128 rows each) for GIF buffer safety.  Direct memcpy from VRAM
     * shadow eliminates per-QW loop overhead of the old buf_image path. */
    for (int chunk = 0; chunk < 2; chunk++)
    {
        int eop = (chunk == 1) ? 1 : 0;
        Push_GIF_Tag(GIF_TAG_LO(1024, eop, 0, 0, 2, 0), 0);
        for (int row = chunk * 128; row < (chunk + 1) * 128; row++)
        {
            /* 128 bytes/row = 8 QWs.  Source is uint16_t* reinterpreted
             * as raw bytes (little-endian, natural PSMT4 nibble order). */
            memcpy(fast_gif_ptr,
                   (const uint8_t *)&psx_vram_shadow[(tex_page_y + row) * 1024 + tex_page_x],
                   128);
            fast_gif_ptr += 8;
        }
    }
}

/* Upload CLUT palette to GS VRAM (CSM1, PSMCT16).
 * Pre-processes PSX palette: set STP bit (bit 15) for non-zero entries.
 * Applies CSM1 entry shuffle (swap entries where (i & 0x18) == 8 with i+8).
 * For 8BPP: 256 entries uploaded as 16×16 rectangle.
 * For 4BPP: 16 entries uploaded as 16×1 rectangle.
 *
 * Single-pass: read in CSM1 order, apply STP, pack directly into QWs. */

/* CSM1 reorder table for 256-entry CLUT: for each group of 32,
 * [0-7, 8-15, 16-23, 24-31] → [0-7, 16-23, 8-15, 24-31].
 * Index i maps to source index csm1_order_256[i]. */
static const uint8_t csm1_order_256[256] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    24,
    25,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    48,
    49,
    50,
    51,
    52,
    53,
    54,
    55,
    40,
    41,
    42,
    43,
    44,
    45,
    46,
    47,
    56,
    57,
    58,
    59,
    60,
    61,
    62,
    63,
    64,
    65,
    66,
    67,
    68,
    69,
    70,
    71,
    80,
    81,
    82,
    83,
    84,
    85,
    86,
    87,
    72,
    73,
    74,
    75,
    76,
    77,
    78,
    79,
    88,
    89,
    90,
    91,
    92,
    93,
    94,
    95,
    96,
    97,
    98,
    99,
    100,
    101,
    102,
    103,
    112,
    113,
    114,
    115,
    116,
    117,
    118,
    119,
    104,
    105,
    106,
    107,
    108,
    109,
    110,
    111,
    120,
    121,
    122,
    123,
    124,
    125,
    126,
    127,
    128,
    129,
    130,
    131,
    132,
    133,
    134,
    135,
    144,
    145,
    146,
    147,
    148,
    149,
    150,
    151,
    136,
    137,
    138,
    139,
    140,
    141,
    142,
    143,
    152,
    153,
    154,
    155,
    156,
    157,
    158,
    159,
    160,
    161,
    162,
    163,
    164,
    165,
    166,
    167,
    176,
    177,
    178,
    179,
    180,
    181,
    182,
    183,
    168,
    169,
    170,
    171,
    172,
    173,
    174,
    175,
    184,
    185,
    186,
    187,
    188,
    189,
    190,
    191,
    192,
    193,
    194,
    195,
    196,
    197,
    198,
    199,
    208,
    209,
    210,
    211,
    212,
    213,
    214,
    215,
    200,
    201,
    202,
    203,
    204,
    205,
    206,
    207,
    216,
    217,
    218,
    219,
    220,
    221,
    222,
    223,
    224,
    225,
    226,
    227,
    228,
    229,
    230,
    231,
    240,
    241,
    242,
    243,
    244,
    245,
    246,
    247,
    232,
    233,
    234,
    235,
    236,
    237,
    238,
    239,
    248,
    249,
    250,
    251,
    252,
    253,
    254,
    255,
};

static void Upload_CLUT_CSM1(int cbp, int clut_x, int clut_y, int tex_format)
{
    const uint16_t *raw_clut = &psx_vram_shadow[clut_y * 1024 + clut_x];
    int num_entries = (tex_format == 0) ? 16 : 256;

    int upload_w = (num_entries == 256) ? 16 : 8;
    int upload_h = (num_entries == 256) ? 16 : 2;

    /* BITBLTBUF: DBP=cbp, DBW=1 (64px), DPSM=CT16 (matches CSM1 standard) */
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, cbp, 1, GS_PSM_16), GS_REG_BITBLTBUF);
    Push_GIF_Data(GS_SET_TRXPOS(0,0,0,0,0), GS_REG_TRXPOS);
    Push_GIF_Data(GS_SET_TRXREG(upload_w, upload_h), GS_REG_TRXREG);
    Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);

    /* Single-pass: read in CSM1 order, apply STP, pack directly into QWs.
     * 8 entries per QW (16 bytes / 2 bytes per entry).
     * 256 entries → 32 QWs, 16 entries → 2 QWs. */
    int total_qw = num_entries >> 3; /* /8 */
    Push_GIF_Tag(GIF_TAG_LO(total_qw, 1, 0, 0, 2, 0), 0);

    if (num_entries == 256)
    {
        /* 8BPP: CSM1 reorder via lookup table */
        for (int qw = 0; qw < 32; qw++)
        {
            int base = qw * 8;
            uint64_t lo = 0, hi = 0;
            for (int j = 0; j < 4; j++)
            {
                uint16_t c0 = raw_clut[csm1_order_256[base + j * 2]];
                uint16_t c1 = raw_clut[csm1_order_256[base + j * 2 + 1]];
                if (c0 != 0)
                    c0 |= 0x8000;
                if (c1 != 0)
                    c1 |= 0x8000;
                uint32_t pair = (uint32_t)c0 | ((uint32_t)c1 << 16);
                if (j < 2)
                    lo |= (uint64_t)pair << (j * 32);
                else
                    hi |= (uint64_t)pair << ((j - 2) * 32);
            }
            Push_GIF_Data(lo, hi);
        }
    }
    else
    {
        /* 4BPP: 16 entries, no CSM1 shuffle needed, 2 QWs */
        for (int qw = 0; qw < 2; qw++)
        {
            int base = qw * 8;
            uint64_t lo = 0, hi = 0;
            for (int j = 0; j < 4; j++)
            {
                uint16_t c0 = raw_clut[base + j * 2];
                uint16_t c1 = (base + j * 2 + 1 < 16) ? raw_clut[base + j * 2 + 1] : 0;
                if (c0 != 0)
                    c0 |= 0x8000;
                if (c1 != 0)
                    c1 |= 0x8000;
                uint32_t pair = (uint32_t)c0 | ((uint32_t)c1 << 16);
                if (j < 2)
                    lo |= (uint64_t)pair << (j * 32);
                else
                    hi |= (uint64_t)pair << ((j - 2) * 32);
            }
            Push_GIF_Data(lo, hi);
        }
    }
}

/* ── Texture window coordinate transform ─────────────────────────── */

/* Apply_Tex_Window_U/V are now static inline in gpu_state.h */

/* ═══════════════════════════════════════════════════════════════════
 *  Decode_TexPage_Cached — Page-level texture cache with LRU (32 slots)
 *
 *  Returns:
 *    0 = not cached (15BPP — caller should use direct PSX VRAM).
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

    /* 15BPP textures bypass the cache entirely — they reference PSX VRAM
     * directly as CT16S with per-vertex Apply_Tex_Window in the callers.
     * Only indexed formats (4BPP/8BPP) use the HW CLUT cache. */
    if (tex_format > 1)
        return 0;

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
            e->clut_y == clut_y)
        {
            /* Parameters match — check generation validity */
            if (vram_unchanged)
            {
                /* No VRAM changes → cached gen still correct */
                tex_stats.page_hits++;
                tex_stats.pixels_saved += 256 * 256;
                e->lru_tick = tex_cache_tick;
                *out_slot_x = e->hw_tbp0;
                *out_slot_y = e->hw_cbp;
                return 2;
            }
            /* VRAM changed — recompute gen and compare */
            current_gen = get_tex_combined_gen(tex_format, tex_page_x, tex_page_y, clut_x, clut_y);
            if (e->combined_gen == current_gen)
            {
                tex_stats.page_hits++;
                tex_stats.pixels_saved += 256 * 256;
                e->lru_tick = tex_cache_tick;
                last_mru_vram_gen = vram_gen_counter;
                *out_slot_x = e->hw_tbp0;
                *out_slot_y = e->hw_cbp;
                return 2;
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
            e->clut_y == clut_y)
        {
            /* Cache HIT */
            tex_stats.page_hits++;
            tex_stats.pixels_saved += 256 * 256;
            e->lru_tick = tex_cache_tick;
            last_hit_slot = i;
            last_mru_vram_gen = vram_gen_counter;

            *out_slot_x = e->hw_tbp0;
            *out_slot_y = e->hw_cbp;
            return 2;
        }
    }

    /* ── Cache MISS — find slot via LRU eviction ───────────────── */
    tex_stats.page_misses++;
    /* Invalidate primitive-level decode cache: the TBP0/CBP it cached
     * may point to a GS VRAM slot about to be overwritten by eviction. */
    Prim_InvalidateTexCache();

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

    /* HW CLUT path: upload raw indexed data + CLUT palette */
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

    *out_slot_x = tbp0;
    *out_slot_y = cbp;

    /* Update common cache entry fields */
    e->valid = 1;
    e->tex_format = tex_format;
    e->tex_page_x = tex_page_x;
    e->tex_page_y = tex_page_y;
    e->clut_x = clut_x;
    e->clut_y = clut_y;
    e->combined_gen = current_gen;
    e->lru_tick = tex_cache_tick;
    last_hit_slot = evict_idx;
    last_mru_vram_gen = vram_gen_counter;

    return 2;
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
