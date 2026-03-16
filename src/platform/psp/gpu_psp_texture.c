/**
 * gpu_psp_texture.c — PSP texture / CLUT cache
 *
 * Page-level software texture cache with LRU eviction (T4/T8 in main RAM),
 * plus CLUT transform cache keyed by (clut_word, content_hash).
 * 15bpp textures bypass the cache — GE reads directly from EDRAM VRAM.
 *
 * Equivalent to gpu_ps2_texture.c on the PS2 platform.
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

/* ── EDRAM Texture Page Cache ────────────────────────────────────
 *  64 slots × 64KB in main RAM (static array).  GE reads from uncached
 *  main memory via TexImage.  Avoids PPSSPP EDRAM framebuffer tracking
 *  issues with sceGuCopyImage in SEND mode.
 *  15bpp reads directly from the PSX VRAM mirror (TBW=1024, no cache).
 * ─────────────────────────────────────────────────────────────────── */
#define TCACHE_SLOTS     64
#define TCACHE_SLOT_SIZE 0x10000  /* 64KB — fits T8 (64KB) or T4 (32KB) */

static uint8_t __attribute__((aligned(64)))
    tcache_data[TCACHE_SLOTS][TCACHE_SLOT_SIZE];

static struct {
    int tpx, tpy, fmt;   /* page key (-1 = empty) */
    int lru;              /* higher = more recent */
} tcache[TCACHE_SLOTS];

static int tcache_lru_tick = 0;

static uint32_t cached_clut_word = 0xFFFFFFFF;

/* ── CLUT Transform Cache ─────────────────────────────────────── */
#define CLUT_CACHE_SIZE 256

static struct {
    uint32_t clut_word;
    uint32_t src_hash;
    uint16_t __attribute__((aligned(16))) data[256];
} clut_cache[CLUT_CACHE_SIZE];
static int clut_cache_rr = 0;

static uint32_t clut_fast_hash(const uint16_t *src, int count)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < count; i++) {
        h ^= src[i];
        h *= 16777619u;
    }
    return h;
}

/* Quick scan: does the CLUT need STP fixup?
 * Returns 0 if all non-zero entries already have STP=1 (bit 15)
 * and no entry is exactly 0x8000 (black+STP needs color tweak).
 * When 0, the raw VRAM data can be used directly (zero-copy). */
static int clut_needs_fixup(const uint16_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t c = src[i];
        if (c == 0) continue;
        if (!(c & 0x8000)) return 1;     /* non-zero, missing STP */
        if ((c & 0x7FFF) == 0) return 1;  /* 0x8000: needs color tweak */
    }
    return 0;
}

static uint16_t *clut_cache_get(uint32_t cword, const uint16_t *src,
                                int count, int *hit)
{
    uint32_t hash = clut_fast_hash(src, count);
    for (int i = 0; i < CLUT_CACHE_SIZE; i++) {
        if (clut_cache[i].clut_word == cword &&
            clut_cache[i].src_hash == hash) {
            *hit = 1;
            return clut_cache[i].data;
        }
    }
    int slot = clut_cache_rr;
    clut_cache_rr = (clut_cache_rr + 1) & (CLUT_CACHE_SIZE - 1);
    clut_cache[slot].clut_word = cword;
    clut_cache[slot].src_hash = hash;
    *hit = 0;
    return clut_cache[slot].data;
}

/* ── Texture GE state tracking ──────────────────────────────────── */
static uint16_t *active_clut_ptr = NULL;
static const void *cached_tex_base = NULL;
static int cached_tex_func = -1;   /* 0=MODULATE, 1=REPLACE */
static int cached_tex_const = 0;
static int cached_ge_tex_mode = -1;
static const uint16_t *cached_ge_clut_ptr = NULL;
#ifdef ENABLE_PSP_STRIDE_HACK
float tex_v_scale = 1.0f;  /* V multiplier for stride hack (extern in state.h) */
static float cached_tex_v_scale = -1.0f;
#endif

/* Texture key — skip setup when unchanged */
static int cached_tex_tpx = -1, cached_tex_tpy = -1, cached_tex_fmt = -1;
static uint32_t cached_tex_clut_key = 0xFFFFFFFF;

/* ── Cache Lookup ───────────────────────────────────────────────── */

static inline void *tcache_slot_ptr(int slot)
{
    return (void *)tcache_data[slot];
}

static int tcache_lookup(int tpx, int tpy, int fmt, int *hit)
{
    int best = 0, best_lru = tcache[0].lru;

    for (int i = 0; i < TCACHE_SLOTS; i++) {
        if (tcache[i].tpx == tpx && tcache[i].tpy == tpy &&
            tcache[i].fmt == fmt) {
            tcache[i].lru = ++tcache_lru_tick;
            *hit = 1;
            return i;
        }
        if (tcache[i].lru < best_lru) {
            best_lru = tcache[i].lru;
            best = i;
        }
    }

    tcache[best].tpx = tpx;
    tcache[best].tpy = tpy;
    tcache[best].fmt = fmt;
    tcache[best].lru = ++tcache_lru_tick;
    *hit = 0;
    return best;
}

/* ── Texture Setup — configure GE from PSX VRAM ────────────────── */

static void setup_psx_texture(uint32_t clut_word)
{
    int tpx = tex_page_x;
    int tpy = tex_page_y;
    int need_flush = 0;

    if (tex_page_format == 0)
    {
#ifdef ENABLE_PSP_STRIDE_HACK
        /* 4bpp T4 — zero-copy from EDRAM VRAM with stride hack (V×4) */
        uint8_t *edram = (uint8_t *)sceGeEdramGetAddr() + PSP_VRAM_OFFSET;
        const void *tex_ptr = edram + (tpy * 1024 + tpx) * 2;
        if (tex_ptr != cached_tex_base)
            need_flush = 1;
#else
        /* 4bpp T4 — copy texpage to main RAM tcache (256×256, tbw=256) */
        int hit;
        int slot = tcache_lookup(tpx, tpy, 0, &hit);
        void *slot_ptr = tcache_slot_ptr(slot);
        if (hit) gpu_frame_stats.texcache_hit++;
        else     gpu_frame_stats.texcache_miss++;
        if (!hit) {
            int copy_h = 256;
            if (tpy + 256 > 512) copy_h = 512 - tpy;
            for (int row = 0; row < copy_h; row++)
                memcpy((uint8_t *)slot_ptr + row * 128,
                       &psx_vram_shadow[(tpy + row) * 1024 + tpx], 128);
            if (copy_h < 256) {
                int rem = 256 - copy_h;
                for (int row = 0; row < rem; row++)
                    memcpy((uint8_t *)slot_ptr + (copy_h + row) * 128,
                           &psx_vram_shadow[row * 1024 + tpx], 128);
            }
            sceKernelDcacheWritebackRange(slot_ptr, 256 * 128);
            need_flush = 1;
        } else if (slot_ptr != cached_tex_base) {
            need_flush = 1;
        }
#endif

        if (clut_word != cached_clut_word) {
            int clut_x = ((clut_word >> 16) & 0x3F) * 16;
            int clut_y = (clut_word >> 22) & 0x1FF;
            uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
            gpu_frame_stats.clut_change++;
            if (!clut_needs_fixup(csrc, 16)) {
                /* Zero-copy: CLUT in EDRAM already correct */
                uint16_t *edram_vram = (uint16_t *)((uintptr_t)sceGeEdramGetAddr()
                                                    + PSP_VRAM_OFFSET);
                active_clut_ptr = &edram_vram[clut_y * 1024 + clut_x];
                gpu_frame_stats.clut_cache_hit++;
            } else {
                int clut_hit;
                uint16_t *cd = clut_cache_get(clut_word, csrc, 16, &clut_hit);
                if (clut_hit) gpu_frame_stats.clut_cache_hit++;
                else          gpu_frame_stats.clut_cache_miss++;
                if (!clut_hit) {
                    for (int i = 0; i < 16; i++) {
                        uint16_t c = csrc[i];
                        if (c == 0) { cd[i] = 0; continue; }
                        c |= 0x8000;
                        if ((c & 0x7FFF) == 0) c |= 0x0001;
                        cd[i] = c;
                    }
                }
                sceKernelDcacheWritebackRange(cd, 32);
                active_clut_ptr = cd;
            }
            cached_clut_word = clut_word;
        }

        if (cached_ge_clut_ptr != active_clut_ptr) {
            sceGuClutMode(GU_PSM_5551, 0, 0x0F, 0);
            sceGuClutLoad(2, active_clut_ptr);
            cached_ge_clut_ptr = active_clut_ptr;
        }
        if (need_flush) sceGuTexFlush();
        if (cached_ge_tex_mode != GU_PSM_T4) {
            sceGuTexMode(GU_PSM_T4, 0, 0, 0);
            cached_ge_tex_mode = GU_PSM_T4;
        }
#ifdef ENABLE_PSP_STRIDE_HACK
        sceGuTexImage(0, 256, 512, 1024, tex_ptr);
        cached_tex_base = tex_ptr;
        tex_v_scale = 4.0f;
#else
        sceGuTexImage(0, 256, 256, 256, slot_ptr);
        cached_tex_base = slot_ptr;
#endif
    }
    else if (tex_page_format == 1)
    {
#ifdef ENABLE_PSP_STRIDE_HACK
        /* 8bpp T8 — zero-copy from EDRAM VRAM with stride hack (V×2) */
        uint8_t *edram = (uint8_t *)sceGeEdramGetAddr() + PSP_VRAM_OFFSET;
        const void *tex_ptr = edram + (tpy * 1024 + tpx) * 2;
        if (tex_ptr != cached_tex_base)
            need_flush = 1;
#else
        /* 8bpp T8 — copy texpage to main RAM tcache (256×256, tbw=256) */
        int hit;
        int slot = tcache_lookup(tpx, tpy, 1, &hit);
        void *slot_ptr = tcache_slot_ptr(slot);
        if (hit) gpu_frame_stats.texcache_hit++;
        else     gpu_frame_stats.texcache_miss++;
        if (!hit) {
            int copy_h = 256;
            if (tpy + 256 > 512) copy_h = 512 - tpy;
            for (int row = 0; row < copy_h; row++)
                memcpy((uint8_t *)slot_ptr + row * 256,
                       &psx_vram_shadow[(tpy + row) * 1024 + tpx], 256);
            if (copy_h < 256) {
                int rem = 256 - copy_h;
                for (int row = 0; row < rem; row++)
                    memcpy((uint8_t *)slot_ptr + (copy_h + row) * 256,
                           &psx_vram_shadow[row * 1024 + tpx], 256);
            }
            sceKernelDcacheWritebackRange(slot_ptr, 256 * 256);
            need_flush = 1;
        } else if (slot_ptr != cached_tex_base) {
            need_flush = 1;
        }
#endif

        if (clut_word != cached_clut_word) {
            int clut_x = ((clut_word >> 16) & 0x3F) * 16;
            int clut_y = (clut_word >> 22) & 0x1FF;
            uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
            gpu_frame_stats.clut_change++;
            if (!clut_needs_fixup(csrc, 256)) {
                /* Zero-copy: CLUT in EDRAM already correct */
                uint16_t *edram_vram = (uint16_t *)((uintptr_t)sceGeEdramGetAddr()
                                                    + PSP_VRAM_OFFSET);
                active_clut_ptr = &edram_vram[clut_y * 1024 + clut_x];
                gpu_frame_stats.clut_cache_hit++;
            } else {
                int clut_hit;
                uint16_t *cd = clut_cache_get(clut_word, csrc, 256, &clut_hit);
                if (clut_hit) gpu_frame_stats.clut_cache_hit++;
                else          gpu_frame_stats.clut_cache_miss++;
                if (!clut_hit) {
                    for (int i = 0; i < 256; i++) {
                        uint16_t c = csrc[i];
                        if (c == 0) { cd[i] = 0; continue; }
                        c |= 0x8000;
                        if ((c & 0x7FFF) == 0) c |= 0x0001;
                        cd[i] = c;
                    }
                }
                sceKernelDcacheWritebackRange(cd, 512);
                active_clut_ptr = cd;
            }
            cached_clut_word = clut_word;
        }

        if (cached_ge_clut_ptr != active_clut_ptr) {
            sceGuClutMode(GU_PSM_5551, 0, 0xFF, 0);
            sceGuClutLoad(32, active_clut_ptr);
            cached_ge_clut_ptr = active_clut_ptr;
        }
        if (need_flush) sceGuTexFlush();
        if (cached_ge_tex_mode != GU_PSM_T8) {
            sceGuTexMode(GU_PSM_T8, 0, 0, 0);
            cached_ge_tex_mode = GU_PSM_T8;
        }
#ifdef ENABLE_PSP_STRIDE_HACK
        sceGuTexImage(0, 256, 512, 1024, tex_ptr);
        cached_tex_base = tex_ptr;
        tex_v_scale = 2.0f;
#else
        sceGuTexImage(0, 256, 256, 256, slot_ptr);
        cached_tex_base = slot_ptr;
#endif
    }
    else
    {
        /* 15bpp: GE reads directly from EDRAM VRAM (stride = 1024). */
        uint16_t *edram_vram = (uint16_t *)((uintptr_t)sceGeEdramGetAddr()
                                            + PSP_VRAM_OFFSET);
        const void *tex_ptr = &edram_vram[tpy * 1024 + tpx];
        if (tex_ptr != cached_tex_base) {
            sceGuTexFlush();
            cached_tex_base = tex_ptr;
        }
        if (cached_ge_tex_mode != GU_PSM_5551) {
            sceGuTexMode(GU_PSM_5551, 0, 0, 0);
            cached_ge_tex_mode = GU_PSM_5551;
        }
        cached_ge_clut_ptr = NULL;
        sceGuTexImage(0, 256, 256, 1024, tex_ptr);
#ifdef ENABLE_PSP_STRIDE_HACK
        tex_v_scale = 1.0f;
#endif
    }

    if (cached_tex_func != 0) {
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        cached_tex_func = 0;
    }
    if (!cached_tex_const
#ifdef ENABLE_PSP_STRIDE_HACK
        || cached_tex_v_scale != tex_v_scale
#endif
    ) {
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        cached_tex_const = 1;
#ifdef ENABLE_PSP_STRIDE_HACK
        cached_tex_v_scale = tex_v_scale;
#endif
    }
}

/* ── Public Texture API (called from primitives.c) ─────────────── */

void Tex_SetupIfChanged(uint32_t clut_word)
{
    if (tex_page_x != cached_tex_tpx || tex_page_y != cached_tex_tpy ||
        tex_page_format != cached_tex_fmt || clut_word != cached_tex_clut_key) {
        gpu_frame_stats.tex_key_change++;
        Prim_FlushBatch();
        setup_psx_texture(clut_word);

        cached_tex_tpx = tex_page_x;
        cached_tex_tpy = tex_page_y;
        cached_tex_fmt = tex_page_format;
        cached_tex_clut_key = clut_word;
    }
}

void Tex_ApplyFuncReplace(void)
{
    if (cached_tex_func != 1) {
        Prim_FlushBatch();
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        cached_tex_func = 1;
    }
}

/* ── Texture State Invalidation ────────────────────────────────── */

void Tex_InvalidateState(void)
{
    cached_tex_base = NULL;
    cached_tex_func = -1;
    cached_tex_const = 0;
#ifdef ENABLE_PSP_STRIDE_HACK
    cached_tex_v_scale = -1.0f;
    tex_v_scale = 1.0f;
#endif
    cached_tex_tpx = -1;
    cached_tex_tpy = -1;
    cached_tex_fmt = -1;
    cached_tex_clut_key = 0xFFFFFFFF;
    cached_ge_tex_mode = -1;
    cached_ge_clut_ptr = NULL;
    cached_clut_word = 0xFFFFFFFF;
    active_clut_ptr = NULL;
}

/* ── Cache Invalidation ────────────────────────────────────────── */

void Prim_InvalidateTexCache(void)
{
    Prim_FlushBatch();
    for (int i = 0; i < TCACHE_SLOTS; i++)
        tcache[i].tpx = -1;
    cached_clut_word = 0xFFFFFFFF;
    cached_tex_base = NULL;
    cached_tex_tpx = -1;
}

void Prim_InvalidateTexCache_Page(int tpx, int tpy)
{
    for (int i = 0; i < TCACHE_SLOTS; i++)
        if (tcache[i].tpx == tpx && tcache[i].tpy == tpy)
            tcache[i].tpx = -1;
}

void Prim_InvalidateTexCache_Region(int rx, int ry, int rw, int rh)
{
    Prim_FlushBatch();
    int rx2 = rx + rw, ry2 = ry + rh;
    for (int i = 0; i < TCACHE_SLOTS; i++) {
        if (tcache[i].tpx < 0) continue;
        int pw = (tcache[i].fmt == 0) ? 64 : 128;
        int px1 = tcache[i].tpx, py1 = tcache[i].tpy;
        int px2 = px1 + pw, py2 = py1 + 256;
        if (rx < px2 && rx2 > px1 && ry < py2 && ry2 > py1)
            tcache[i].tpx = -1;
    }
    cached_clut_word = 0xFFFFFFFF;
    cached_tex_base = NULL;
    cached_tex_tpx = -1;
}

/* ── Texture Cache Stubs (PS2 compatibility interface) ─────────── */

int Decode_TexPage_Cached(int tex_format, int tpx, int tpy,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y) {
    (void)tex_format; (void)tpx; (void)tpy;
    (void)clut_x; (void)clut_y;
    if (out_slot_x) *out_slot_x = 0;
    if (out_slot_y) *out_slot_y = 0;
    return 0;
}

uint32_t Tex_Cache_GetPageGen(int tex_format, int tex_page_x, int tex_page_y) {
    (void)tex_format; (void)tex_page_x; (void)tex_page_y;
    return vram_gen_counter;
}

void Tex_Cache_DumpStats(void) {}
void Tex_Cache_ResetStats(void) {}
void Tex_Cache_DirtyRegion(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
