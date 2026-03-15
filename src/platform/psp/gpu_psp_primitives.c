/**
 * gpu_psp_primitives.c — PSX GP0 primitive → PSP GU translation
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "profiler.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

extern uint64_t gpu_estimated_pixels;

/* ── Coordinate Transformation ──────────────────────────────────── */
/* PSX_to_PSP is now in gpu_psp_state.h */

static inline uint32_t PSX_to_ABGR(uint32_t c)
{
    /* Alpha = 0x00: non-textured PSX pixels have STP=0 by default.
     * In GU_PSM_5551, alpha >= 0x80 → bit15=1 (STP set).
     * mask_set_bit forces STP=1 via stencil, not vertex alpha. */
    return (c & 0x00FFFFFF);
}

/* PSX texture blending: output = tex * vc / 128 (0x80 = 1.0)
 * PSP GU_TFX_MODULATE: output = tex * vc / 255 (0xFF = 1.0)
 * Scale vertex colors by 2x to match PSX brightness. */
static inline uint32_t PSX_to_ABGR_texblend(uint32_t c)
{
    uint32_t r = (c >> 0) & 0xFF;
    uint32_t g = (c >> 8) & 0xFF;
    uint32_t b = (c >> 16) & 0xFF;
    r = r < 128 ? r * 2 : 255;
    g = g < 128 ? g * 2 : 255;
    b = b < 128 ? b * 2 : 255;
    return r | (g << 8) | (b << 16) | 0xFF000000;
}

/* ── Texture Setup ──────────────────────────────────────────────── */

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

/* ── CLUT Transform Cache ───────────────────────────────────────
 *  Caches STP-transformed CLUT data keyed by (clut_word, content_hash).
 *  Avoids re-running the transform loop + dcache writeback when the
 *  same palette is reused (common in fight scenes with multiple sprites).
 * ─────────────────────────────────────────────────────────────────── */
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

/* Look up CLUT in cache. Returns pointer to (possibly cached) transform buffer.
 * On hit (*hit=1): buffer already contains valid STP-transformed + dcache-flushed data.
 * On miss (*hit=0): caller must fill buffer, then flush dcache. */
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

static uint16_t *active_clut_ptr = NULL;
static const void *cached_tex_base = NULL;  /* last tex ptr for sceGuTexFlush skip */
static int cached_tex_func = -1;  /* 0=MODULATE, 1=REPLACE */
static int cached_tex_const = 0;  /* 1=filter/scale/offset already set */
static int cached_ge_tex_mode = -1; /* last sceGuTexMode psm (-1=unknown) */
static const uint16_t *cached_ge_clut_ptr = NULL; /* last sceGuClutLoad pointer */

/* Texture key — skip setup_psx_texture when unchanged */
static int cached_tex_tpx = -1, cached_tex_tpy = -1, cached_tex_fmt = -1;
static uint32_t cached_tex_clut_key = 0xFFFFFFFF;

/* Return RAM pointer for cache slot N */
static inline void *tcache_slot_ptr(int slot)
{
    return (void *)tcache_data[slot];
}

/* Find or allocate a cache slot for (tpx, tpy, fmt).
 * Returns slot index.  Sets *hit = 1 on cache hit. */
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

    /* Miss — evict LRU slot */
    tcache[best].tpx = tpx;
    tcache[best].tpy = tpy;
    tcache[best].fmt = fmt;
    tcache[best].lru = ++tcache_lru_tick;
    *hit = 0;
    return best;
}

/* Forward declarations — needed by setup_psx_texture's flush-on-overflow */
static void vbatch_flush(void);

/* Set up GE texture from PSX VRAM.
 * T4/T8: raw index data in EDRAM cache slot + GE CLUT hardware.
 * 15bpp:  GE reads directly from EDRAM VRAM (zero CPU work). */
static void setup_psx_texture(uint32_t clut_word)
{
    int tpx = tex_page_x;
    int tpy = tex_page_y;

    int need_flush = 0;

    if (tex_page_format == 0)
    {
        /* 4bpp T4: 256×256 texels = 64 16-bit pixels × 256 rows */
        int hit;
        int slot = tcache_lookup(tpx, tpy, 0, &hit);
        void *slot_ptr = tcache_slot_ptr(slot);
        if (hit) gpu_frame_stats.texcache_hit++;
        else     gpu_frame_stats.texcache_miss++;

        if (!hit) {
            /* CPU copy: shadow VRAM → EDRAM cache slot.
             * Avoids sceGuCopyImage which reads from the DrawBufferList region —
             * PPSSPP's framebuffer tracking may serve stale GPU-cached data
             * instead of CPU-written VRAM data in SEND mode. */
            int copy_h = 256;
            if (tpy + 256 > 512) copy_h = 512 - tpy;
            for (int row = 0; row < copy_h; row++) {
                memcpy((uint8_t *)slot_ptr + row * 128,
                       &psx_vram_shadow[(tpy + row) * 1024 + tpx],
                       128);  /* 64 halfwords = 128 bytes */
            }
            if (copy_h < 256) {
                /* Wrap: remaining rows from top of VRAM */
                int rem = 256 - copy_h;
                for (int row = 0; row < rem; row++) {
                    memcpy((uint8_t *)slot_ptr + (copy_h + row) * 128,
                           &psx_vram_shadow[row * 1024 + tpx],
                           128);
                }
            }
            sceKernelDcacheWritebackRange(slot_ptr, 256 * 128);
            need_flush = 1;
        } else if (slot_ptr != cached_tex_base) {
            need_flush = 1;
        }

        /* CLUT: update if clut_word changed */
        if (clut_word != cached_clut_word) {
            int clut_x = ((clut_word >> 16) & 0x3F) * 16;
            int clut_y = (clut_word >> 22) & 0x1FF;
            uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
            int clut_hit;
            uint16_t *cd = clut_cache_get(clut_word, csrc, 16, &clut_hit);
            if (clut_hit) gpu_frame_stats.clut_cache_hit++;
            else          gpu_frame_stats.clut_cache_miss++;
            gpu_frame_stats.clut_change++;
            if (!clut_hit) {
                for (int i = 0; i < 16; i++) {
                    uint16_t c = csrc[i];
                    if (c == 0) { cd[i] = 0; continue; }
                    c |= 0x8000;
                    if ((c & 0x7FFF) == 0) c |= 0x0001;
                    cd[i] = c;
                }
            }
            /* Point directly at clut_cache — 256 slots is enough to
             * avoid recycling within a single GE list. */
            sceKernelDcacheWritebackRange(cd, 32);
            active_clut_ptr = cd;
            cached_clut_word = clut_word;
        }

        /* Emit GE commands — skip if state already matches */
        if (cached_ge_clut_ptr != active_clut_ptr) {
            sceGuClutMode(GU_PSM_5551, 0, 0xFF, 0);
            sceGuClutLoad(2, active_clut_ptr);
            cached_ge_clut_ptr = active_clut_ptr;
        }
        if (need_flush) sceGuTexFlush();
        if (cached_ge_tex_mode != GU_PSM_T4) {
            sceGuTexMode(GU_PSM_T4, 0, 0, 0);
            cached_ge_tex_mode = GU_PSM_T4;
        }
        sceGuTexImage(0, 256, 256, 256, slot_ptr);
        cached_tex_base = slot_ptr;
    }
    else if (tex_page_format == 1)
    {
        /* 8bpp T8: 256×256 = 64KB in cache slot */
        int hit;
        int slot = tcache_lookup(tpx, tpy, 1, &hit);
        void *slot_ptr = tcache_slot_ptr(slot);
        if (hit) gpu_frame_stats.texcache_hit++;
        else     gpu_frame_stats.texcache_miss++;

        if (!hit) {
            /* CPU copy: shadow VRAM → EDRAM cache slot (T8).
             * Same PPSSPP workaround as T4 path above. */
            int copy_h = 256;
            if (tpy + 256 > 512) copy_h = 512 - tpy;
            for (int row = 0; row < copy_h; row++) {
                memcpy((uint8_t *)slot_ptr + row * 256,
                       &psx_vram_shadow[(tpy + row) * 1024 + tpx],
                       256);  /* 128 halfwords = 256 bytes */
            }
            if (copy_h < 256) {
                int rem = 256 - copy_h;
                for (int row = 0; row < rem; row++) {
                    memcpy((uint8_t *)slot_ptr + (copy_h + row) * 256,
                           &psx_vram_shadow[row * 1024 + tpx],
                           256);
                }
            }
            sceKernelDcacheWritebackRange(slot_ptr, 256 * 256);
            need_flush = 1;
        } else if (slot_ptr != cached_tex_base) {
            need_flush = 1;
        }

        /* CLUT: update if clut_word changed */
        if (clut_word != cached_clut_word) {
            int clut_x = ((clut_word >> 16) & 0x3F) * 16;
            int clut_y = (clut_word >> 22) & 0x1FF;
            uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
            int clut_hit;
            uint16_t *cd = clut_cache_get(clut_word, csrc, 256, &clut_hit);
            if (clut_hit) gpu_frame_stats.clut_cache_hit++;
            else          gpu_frame_stats.clut_cache_miss++;
            gpu_frame_stats.clut_change++;
            if (!clut_hit) {
                for (int i = 0; i < 256; i++) {
                    uint16_t c = csrc[i];
                    if (c == 0) { cd[i] = 0; continue; }
                    c |= 0x8000;
                    if ((c & 0x7FFF) == 0) c |= 0x0001;
                    cd[i] = c;
                }
            }
            /* Point directly at clut_cache — same logic as T4 path. */
            sceKernelDcacheWritebackRange(cd, 512);
            active_clut_ptr = cd;
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
        sceGuTexImage(0, 256, 256, 256, slot_ptr);
        cached_tex_base = slot_ptr;
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
    }

    if (cached_tex_func != 0) {
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        cached_tex_func = 0;
    }
    if (!cached_tex_const) {
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        cached_tex_const = 1;
    }

}

/* Batch-aware texture setup: only call setup_psx_texture if tex key changed.
 * When it changes, flush pending batch first. */
static void setup_texture_if_changed(uint32_t clut_word)
{
    if (tex_page_x != cached_tex_tpx || tex_page_y != cached_tex_tpy ||
        tex_page_format != cached_tex_fmt || clut_word != cached_tex_clut_key) {
        gpu_frame_stats.tex_key_change++;
        vbatch_flush();
        setup_psx_texture(clut_word);

        cached_tex_tpx = tex_page_x;
        cached_tex_tpy = tex_page_y;
        cached_tex_fmt = tex_page_format;
        cached_tex_clut_key = clut_word;
    }
}

/* Batch-aware raw_tex func switch */
static inline void apply_tex_func_replace(void)
{
    if (cached_tex_func != 1) {
        vbatch_flush();
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        cached_tex_func = 1;
    }
}

/* ── State Management ───────────────────────────────────────────── */

/* ── Vertex Batch ────────────────────────────────────────────────── */
#define VBATCH_MAX 256  /* max verts per batch */

static struct {
    union {
        PspVertFlat flat[VBATCH_MAX];
        PspVertTex  tex[VBATCH_MAX];
    } v;
    int count;
    int vsize;  /* sizeof per vertex */
    int vfmt;   /* GU format flags (without GU_TRANSFORM_2D) */
    int prim;   /* GU_TRIANGLES, GU_LINES, GU_SPRITES */
} vbatch;

static void vbatch_flush(void)
{
    if (vbatch.count == 0) return;
    gpu_frame_stats.vbatch_flushes++;
    gpu_frame_stats.vbatch_verts += vbatch.count;
    int bytes = vbatch.count * vbatch.vsize;
    void *dst = vpool_alloc(bytes);
    memcpy(dst, &vbatch.v, bytes);
    sceGuDrawArray(vbatch.prim, vbatch.vfmt | GU_TRANSFORM_2D,
                   vbatch.count, NULL, (void *)((uintptr_t)dst | 0x40000000));
    vbatch.count = 0;
}

/* Ensure batch is compatible; flush if not. Returns write offset. */
static inline void vbatch_prepare(int prim, int vfmt, int vsize, int nverts)
{
    if (vbatch.count > 0 &&
        (vbatch.prim != prim || vbatch.vfmt != vfmt ||
         vbatch.count + nverts > VBATCH_MAX))
        vbatch_flush();
    vbatch.prim = prim;
    vbatch.vfmt = vfmt;
    vbatch.vsize = vsize;
}

void Prim_FlushBatch(void) { vbatch_flush(); }

/* Lazy GU state cache — avoid redundant sceGu calls.
 * Reset to -1 (unknown) by Prim_InvalidateGSState().
 * Each helper flushes the batch when it actually emits a GU command,
 * ensuring all batched verts are drawn with the correct prior state. */
static int cached_blend_on = -1;   /* 0=off, 1=on */
static int cached_blend_mode = -1; /* semi_trans_mode 0-3 */
static int cached_dither_on = -1;  /* 0=off, 1=on */
static int cached_tex_on = -1;     /* 0=off, 1=on */
static int cached_color_test_on = -1;

static void apply_dither(int is_shaded, int is_textured, int is_raw_tex)
{
    int want = dither_enabled && (is_shaded || (is_textured && !is_raw_tex));
    if (want != cached_dither_on) {
        vbatch_flush();
        if (want) sceGuEnable(GU_DITHER);
        else      sceGuDisable(GU_DITHER);
        cached_dither_on = want;
    }
}

static void apply_blend(int is_semi_trans)
{
    if (is_semi_trans)
    {
        if (cached_blend_on != 1) {
            vbatch_flush();
            sceGuEnable(GU_BLEND);
            cached_blend_on = 1;
        }
        if (semi_trans_mode != cached_blend_mode) {
            vbatch_flush();
            switch (semi_trans_mode)
            {
            case 0:
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x80808080, 0x80808080);
                break;
            case 1:
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
                break;
            case 2:
                sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
                break;
            case 3:
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x40404040, 0xFFFFFFFF);
                break;
            }
            cached_blend_mode = semi_trans_mode;
        }
    }
    else
    {
        if (cached_blend_on != 0) {
            vbatch_flush();
            sceGuDisable(GU_BLEND);
            cached_blend_on = 0;
        }
    }
}

static inline void gu_enable_texture(void)
{
    if (cached_tex_on != 1) {
        vbatch_flush();
        sceGuEnable(GU_TEXTURE_2D);
        cached_tex_on = 1;
    }
}

static inline void gu_disable_texture(void)
{
    if (cached_tex_on != 0) {
        vbatch_flush();
        sceGuDisable(GU_TEXTURE_2D);
        cached_tex_on = 0;
    }
}

static inline void gu_enable_color_test(void)
{
    if (cached_color_test_on != 1) {
        vbatch_flush();
        sceGuEnable(GU_COLOR_TEST);
        sceGuColorFunc(GU_NOTEQUAL, 0x00000000, 0x00FFFFFF);
        cached_color_test_on = 1;
    }
}

static inline void gu_disable_color_test(void)
{
    if (cached_color_test_on != 0) {
        vbatch_flush();
        sceGuDisable(GU_COLOR_TEST);
        cached_color_test_on = 0;
    }
}

void Prim_InvalidateGSState(void)
{
    vbatch_flush();
    gs_state.valid = 0;
    cached_blend_on = -1;
    cached_blend_mode = -1;
    cached_dither_on = -1;
    cached_tex_on = -1;
    cached_color_test_on = -1;
    cached_tex_base = NULL;
    cached_tex_func = -1;
    cached_tex_const = 0;
    cached_tex_tpx = -1;
    cached_tex_tpy = -1;
    cached_tex_fmt = -1;
    cached_tex_clut_key = 0xFFFFFFFF;
    cached_ge_tex_mode = -1;
    cached_ge_clut_ptr = NULL;
    /* Must reset CLUT word so setup_psx_texture re-copies CLUT data
     * to the new vpool epoch (vpool resets after each list submit). */
    cached_clut_word = 0xFFFFFFFF;
    active_clut_ptr = NULL;
}

void Prim_InvalidateTexCache(void)
{
    vbatch_flush();
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
/* Invalidate only cache slots whose page overlaps (x, y, w, h) in VRAM */
void Prim_InvalidateTexCache_Region(int rx, int ry, int rw, int rh)
{
    vbatch_flush();
    int rx2 = rx + rw, ry2 = ry + rh;
    for (int i = 0; i < TCACHE_SLOTS; i++) {
        if (tcache[i].tpx < 0) continue;
        int pw = (tcache[i].fmt == 0) ? 64 : 128; /* halfword width */
        int px1 = tcache[i].tpx, py1 = tcache[i].tpy;
        int px2 = px1 + pw, py2 = py1 + 256;
        if (rx < px2 && rx2 > px1 && ry < py2 && ry2 > py1)
            tcache[i].tpx = -1;
    }
    cached_clut_word = 0xFFFFFFFF;
    cached_tex_base = NULL;
    cached_tex_tpx = -1;
}

/* ── Primary Dispatch ───────────────────────────────────────────── */

int GPU_TryFastEmit(uint32_t *psx_cmd)
{
    (void)psx_cmd;
    return 0;
}

int Translate_GP0_to_GS(uint32_t *psx_cmd)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    /* FillRect (GP0 0x02) — special case: absolute coords, changes scissor.
     * Must flush batch before drawing since scissor changes. */
    if (cmd == 0x02)
    {
        vbatch_flush();
        /* Flush pending GE list before CPU EDRAM write (SEND mode). */
        GPU_Backend_Flush();
        uint32_t color = PSX_to_ABGR(psx_cmd[0]);
        uint32_t xy = psx_cmd[1];
        uint32_t wh = psx_cmd[2];
        int16_t fx = (int16_t)(xy & 0x3F0);
        int16_t fy = (int16_t)((xy >> 16) & 0x1FF);
        int16_t fw = (int16_t)(((wh & 0x3FF) + 0xF) & ~0xF);
        int16_t fh = (int16_t)((wh >> 16) & 0x1FF);
        if (fw == 0 || fh == 0)
            return 3;

        /* Update shadow VRAM + EDRAM directly.
         * EDRAM write needed because 15bpp textures read from EDRAM.
         * GPU sprite also draws to EDRAM but PPSSPP may not propagate it
         * correctly for subsequent texture reads from the same region. */
        {
            uint16_t r5 = (uint16_t)((cmd_word >> 3) & 0x1F);
            uint16_t g5 = (uint16_t)((cmd_word >> 11) & 0x1F);
            uint16_t b5 = (uint16_t)((cmd_word >> 19) & 0x1F);
            uint16_t col16 = r5 | (g5 << 5) | (b5 << 10);
            if (mask_set_bit) col16 |= 0x8000;
            uint16_t *edram = (uint16_t *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET);
            int ey = (fy + fh > 512) ? 512 : fy + fh;
            int ex = (fx + fw > 1024) ? 1024 : fx + fw;
            if (!mask_check_bit) {
                /* Fast path: no per-pixel mask check */
                int w = ex - fx;
                for (int y = fy; y < ey; y++) {
                    uint16_t *er = &edram[y * 1024 + fx];
                    for (int x = 0; x < w; x++) er[x] = col16;
                    if (psx_vram_shadow) {
                        uint16_t *sr = &psx_vram_shadow[y * 1024 + fx];
                        for (int x = 0; x < w; x++) sr[x] = col16;
                    }
                }
            } else {
                for (int y = fy; y < ey; y++)
                    for (int x = fx; x < ex; x++) {
                        int idx = y * 1024 + x;
                        if (edram[idx] & 0x8000) continue;
                        edram[idx] = col16;
                        if (psx_vram_shadow) psx_vram_shadow[idx] = col16;
                    }
            }
            sceKernelDcacheWritebackRange(
                &edram[fy * 1024 + fx],
                (uint32_t)(fw * 2 + (fh - 1) * 1024 * 2));
        }

        /* Draw via sceGu — direct DrawArray (not batched) */
        gu_disable_texture();
        gu_disable_color_test();
        apply_blend(0);
        PspVertFlat *v = (PspVertFlat *)vpool_alloc(2 * sizeof(PspVertFlat));
        v[0].color = color; v[0].x = fx;      v[0].y = fy;      v[0].z = 0;
        v[1].color = color; v[1].x = fx + fw; v[1].y = fy + fh; v[1].z = 0;
        sceGuScissor(0, 0, 1024, 512);
        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, (void *)((uintptr_t)v | 0x40000000));
        sceGuScissor(draw_clip_x1, draw_clip_y1,
                     draw_clip_x2 + 1, draw_clip_y2 + 1);
        gpu_frame_stats.fill++;

        return 3;
    }

    switch (cmd & 0xE0)
    {
    case 0x20:
    { /* Polygons */
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        int is_raw_tex = is_textured && (cmd & 0x01);
        int nv = is_quad ? 4 : 3;

        apply_blend(is_semi);
        apply_dither(is_shaded, is_textured, is_raw_tex);

        if (!is_textured)
        {
            gu_disable_texture();
            gu_disable_color_test();
            /* Triangles: 3 verts. Quads: decompose to 2 triangles = 6 verts. */
            int batch_nv = is_quad ? 6 : 3;
            vbatch_prepare(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertFlat), batch_nv);

            PspVertFlat tmp[4];
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                tmp[i].color = color; tmp[i].x = x; tmp[i].y = y; tmp[i].z = 0;
            }

            PspVertFlat *dst = &vbatch.v.flat[vbatch.count];
            dst[0] = tmp[0]; dst[1] = tmp[1]; dst[2] = tmp[2];
            if (is_quad) {
                dst[3] = tmp[2]; dst[4] = tmp[1]; dst[5] = tmp[3];
            }
            vbatch.count += batch_nv;
            gpu_frame_stats.poly_flat++;
            return p;
        }
        else
        {
            gu_enable_texture();
            gu_enable_color_test();
            /* Extract texpage from vertex 1's UV word */
            {
                int tpage_idx = is_shaded ? 5 : 4;
                uint32_t tpage = psx_cmd[tpage_idx] >> 16;
                tex_page_x = (tpage & 0xF) * 64;
                tex_page_y = ((tpage >> 4) & 0x1) * 256;
                tex_page_format = (tpage >> 7) & 3;
                semi_trans_mode = (tpage >> 5) & 3;
            }
            int clut_src_p = 2;
            uint32_t clut_word = psx_cmd[clut_src_p] & 0xFFFF0000;
            setup_texture_if_changed(clut_word);

            if (is_raw_tex)
                apply_tex_func_replace();

            int batch_nv = is_quad ? 6 : 3;
            vbatch_prepare(GU_TRIANGLES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertTex), batch_nv);

            PspVertTex tmp[4];
            uint32_t base_color = is_raw_tex ? PSX_to_ABGR(psx_cmd[0])
                                             : PSX_to_ABGR_texblend(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? (is_raw_tex ? PSX_to_ABGR(psx_cmd[p++])
                                              : PSX_to_ABGR_texblend(psx_cmd[p++]))
                    : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                uint32_t uv_clut = psx_cmd[p++];
                tmp[i].u = (float)Apply_Tex_Window_U(uv_clut & 0xFF);
                tmp[i].v = (float)Apply_Tex_Window_V((uv_clut >> 8) & 0xFF);
                tmp[i].color = color; tmp[i].x = x; tmp[i].y = y; tmp[i].z = 0;
            }

            PspVertTex *dst = &vbatch.v.tex[vbatch.count];
            dst[0] = tmp[0]; dst[1] = tmp[1]; dst[2] = tmp[2];
            if (is_quad) {
                dst[3] = tmp[2]; dst[4] = tmp[1]; dst[5] = tmp[3];
            }
            vbatch.count += batch_nv;
            gpu_frame_stats.poly_tex++;

            return p;
        }
    }

    case 0x40:
    { /* Lines */
        int is_semi = (cmd & 0x02) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        gu_disable_texture();
        gu_disable_color_test();
        apply_blend(is_semi);
        apply_dither(is_shaded, 0, 0);

        vbatch_prepare(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertFlat), 2);

        uint32_t c0 = PSX_to_ABGR(psx_cmd[0]);
        int p = 1;
        int16_t x0, y0, x1, y1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;
        uint32_t c1 = is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : c0;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x1, &y1);
        p++;

        PspVertFlat *v = &vbatch.v.flat[vbatch.count];
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        vbatch.count += 2;
        gpu_frame_stats.line++;
        return p;
    }

    case 0x60:
    { /* Rectangles / Sprites */
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        int is_raw_rect = is_textured && (cmd & 0x01);
        uint32_t color = (is_textured && !is_raw_rect)
                             ? PSX_to_ABGR_texblend(psx_cmd[0])
                             : PSX_to_ABGR(psx_cmd[0]);

        int16_t x0, y0;
        int p = 1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;

        uint8_t u0 = 0, v0 = 0;
        uint32_t clut_word = 0;
        if (is_textured) {
            uint32_t uv_clut = psx_cmd[p];
            clut_word = uv_clut & 0xFFFF0000;
            u0 = (uint8_t)(uv_clut & 0xFF);
            v0 = (uint8_t)((uv_clut >> 8) & 0xFF);
            p++;
        }

        int16_t w, h;
        int size_code = (cmd >> 3) & 3;
        if (size_code == 0) {
            w = (int16_t)(psx_cmd[p] & 0xFFFF);
            h = (int16_t)(psx_cmd[p] >> 16);
            p++;
        } else {
            static const int16_t sz[] = {0, 1, 8, 16};
            w = sz[size_code]; h = sz[size_code];
        }

        apply_blend(is_semi);
        apply_dither(0, is_textured, is_raw_rect);

        if (!is_textured)
        {
            gu_disable_texture();
            gu_disable_color_test();
            vbatch_prepare(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertFlat), 2);
            PspVertFlat *v = &vbatch.v.flat[vbatch.count];
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_flat++;
        }
        else
        {
            gu_enable_texture();
            gu_enable_color_test();
            setup_texture_if_changed(clut_word);

            if (is_raw_rect)
                apply_tex_func_replace();

            vbatch_prepare(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertTex), 2);
            PspVertTex *v = &vbatch.v.tex[vbatch.count];
            v[0].u = (float)Apply_Tex_Window_U(u0);
            v[0].v = (float)Apply_Tex_Window_V(v0);
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].u = (float)Apply_Tex_Window_U(u0 + w);
            v[1].v = (float)Apply_Tex_Window_V(v0 + h);
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_tex++;
        }
        return p;
    }

    default:
        return 0;
    }
}

void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans)
{
    gu_disable_texture();
    apply_blend(is_semi_trans);
    apply_dither(is_shaded, 0, 0);

    vbatch_prepare(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                   sizeof(PspVertFlat), 2);
    int16_t sx0, sy0, sx1, sy1;
    PSX_to_PSP(x0, y0, &sx0, &sy0);
    PSX_to_PSP(x1, y1, &sx1, &sy1);
    PspVertFlat *v = &vbatch.v.flat[vbatch.count];
    v[0].color = PSX_to_ABGR(color0);
    v[0].x = sx0; v[0].y = sy0; v[0].z = 0;
    v[1].color = is_shaded ? PSX_to_ABGR(color1) : PSX_to_ABGR(color0);
    v[1].x = sx1; v[1].y = sy1; v[1].z = 0;
    vbatch.count += 2;
}
