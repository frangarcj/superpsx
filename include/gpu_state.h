/**
 * gpu_state.h — Shared GPU state, types, macros and constants
 *
 * This header is included by every gpu_*.c translation unit so they can
 * access the single set of GPU state variables that used to live as
 * file-scope statics inside graphics.c.
 */
#ifndef GPU_STATE_H
#define GPU_STATE_H

#include "superpsx.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define LOG_TAG "GPU"

/* ── PSX VRAM geometry ───────────────────────────────────────────── */
#define PSX_VRAM_WIDTH 1024
#define PSX_VRAM_HEIGHT 512
#define PSX_VRAM_FBW (PSX_VRAM_WIDTH / 64) /* =16 in 64-pixel units */

/* ── GPU deferred IRQ ───────────────────────────────────────────── */
/* Cycles after GP0(1Fh) before IRQ1 fires into I_STAT (mirrors real PSX
 * async GPU FIFO processing latency). */
#define GPU_IRQ_DELAY 500

/* ── GPUSTAT helper macros ───────────────────────────────────────── */
#define disp_hres368 ((gpu_stat >> 16) & 1)
#define disp_hres ((gpu_stat >> 17) & 3)
#define disp_vres ((gpu_stat >> 19) & 1)
#define disp_pal ((gpu_stat >> 20) & 1)
#define disp_interlace ((gpu_stat >> 22) & 1)

/* ═══════════════════════════════════════════════════════════════════
 *  Shared GPU state — defined in gpu_core.c, externed everywhere
 * ═══════════════════════════════════════════════════════════════════ */

/* GPU status / read registers */
extern uint32_t gpu_stat;
extern uint32_t gpu_read;
extern volatile int gpu_pending_vblank_flush;

/* GPU rendering cost estimation (accumulated pixel count for cycle accounting) */
extern uint64_t gpu_estimated_pixels;

/* Framebuffer configuration */
extern int fb_address;
extern int fb_width;
extern int fb_height;
extern int fb_psm;

/* GS shadow drawing state */
extern int draw_offset_x;
extern int draw_offset_y;
extern int draw_clip_x1;
extern int draw_clip_y1;
extern int draw_clip_x2;
extern int draw_clip_y2;

/* PSX Display Range */
extern int disp_range_y1;
extern int disp_range_y2;

/* Display start in VRAM (from GP1(05h)) */
extern int display_start_x;
extern int display_start_y;

/* Texture page state (from GP0 E1) */
extern int tex_page_x;
extern int tex_page_y;
extern int tex_page_format;
extern int semi_trans_mode;
extern int dither_enabled;

/* Shadow PSX VRAM for CLUT texture decode */
extern uint16_t *psx_vram_shadow;

/* VRAM transfer tracking for shadow writes */
extern int vram_tx_x, vram_tx_y, vram_tx_w, vram_tx_h, vram_tx_pixel;

/* VRAM read state (GP0 C0h) */
extern int vram_read_x, vram_read_y, vram_read_w, vram_read_h;
extern int vram_read_remaining;
extern int vram_read_pixel;

/* Polyline accumulation state (GP0 48h-5Fh polylines) */
extern int polyline_active;
extern int polyline_shaded;
extern int polyline_semi_trans;
extern uint32_t polyline_prev_color;
extern uint32_t polyline_next_color;
extern int16_t polyline_prev_x, polyline_prev_y;
extern int polyline_expect_color;

/* Texture flip bits from GP0(E1) bits 12-13 */
extern int tex_flip_x;
extern int tex_flip_y;

/* Mask bit state from GP0(E6) */
extern int mask_set_bit;
extern int mask_check_bit;
extern uint64_t cached_base_test; /* precomputed Get_Base_TEST() value */

/* GP1(09h) - Allow 2MB VRAM */
extern int gp1_allow_2mb;

/* Texture window from GP0(E2) */
extern uint32_t tex_win_mask_x;
extern uint32_t tex_win_mask_y;
extern uint32_t tex_win_off_x;
extern uint32_t tex_win_off_y;

/* Raw E-register values for GP1(10h) query responses */
extern uint32_t raw_tex_window;   /* E2: bits 0-19 */
extern uint32_t raw_draw_area_tl; /* E3: bits 0-19 */
extern uint32_t raw_draw_area_br; /* E4: bits 0-19 */
extern uint32_t raw_draw_offset;  /* E5: bits 0-21 */

/* Immediate mode command buffer */
extern int gpu_cmd_remaining;
extern uint32_t gpu_cmd_buffer[16];
extern int gpu_cmd_ptr;
extern int gpu_transfer_words;
extern int gpu_transfer_total;

/* ═══════════════════════════════════════════════════════════════════
 *  Internal functions (cross-module interface)
 * ═══════════════════════════════════════════════════════════════════ */

/* gpu_texture.c — CLUT texture decode + page-level cache */
static inline uint32_t Apply_Tex_Window_U(uint32_t u)
{
    if (tex_win_mask_x == 0)
        return u;
    uint32_t mask = tex_win_mask_x * 8;
    uint32_t off = (tex_win_off_x & tex_win_mask_x) * 8;
    return (u & ~mask) | off;
}
static inline uint32_t Apply_Tex_Window_V(uint32_t v)
{
    if (tex_win_mask_y == 0)
        return v;
    uint32_t mask = tex_win_mask_y * 8;
    uint32_t off = (tex_win_off_y & tex_win_mask_y) * 8;
    return (v & ~mask) | off;
}
extern uint32_t vram_gen_counter;

/* ── Per-frame GPU command counters (profiler visibility) ──────── */
typedef struct {
    uint32_t poly_tex;      /* textured polygon (0x24-0x2F, 0x34-0x3F) */
    uint32_t poly_flat;     /* non-textured polygon */
    uint32_t rect_tex;      /* textured rectangle/sprite */
    uint32_t rect_flat;     /* non-textured rectangle */
    uint32_t line;          /* line/polyline */
    uint32_t fill;          /* FillRect (GP0.02) */
    uint32_t vram_load;     /* CPU→VRAM transfer (GP0.A0) */
    uint32_t vram_store;    /* VRAM→CPU transfer (GP0.C0) */
    uint32_t vram_copy;     /* VRAM→VRAM copy (GP0.80) */
    uint32_t texcache_hit;  /* prim_tex_cache hits */
    uint32_t texcache_miss; /* prim_tex_cache misses → Decode_TexPage_Cached */
    uint32_t tex_upload_full;    /* full page uploads (all 256 rows) */
    uint32_t tex_upload_partial; /* partial page uploads (subset of rows) */
    uint32_t tex_upload_4bpp;    /* 4BPP page uploads */
    uint32_t tex_upload_8bpp;    /* 8BPP page uploads */
    uint32_t tex_upload_rows;    /* total rows actually uploaded */
    /* PSP-specific counters */
    uint32_t clut_change;       /* CLUT word changes (triggers cache lookup) */
    uint32_t clut_cache_hit;    /* CLUT cache hits (skip transform+dcache) */
    uint32_t clut_cache_miss;   /* CLUT cache misses (full transform) */
    uint32_t tex_key_change;    /* texture key changes (triggers setup) */
    uint32_t vbatch_flushes;    /* vertex batch flushes (sceGuDrawArray calls) */
    uint32_t vbatch_verts;      /* total vertices submitted */
} gpu_frame_stats_t;
extern gpu_frame_stats_t gpu_frame_stats;
int Decode_TexPage_Cached(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y);
void Tex_Cache_DumpStats(void);
void Tex_Cache_ResetStats(void);
void Tex_Cache_DirtyRegion(int x, int y, int w, int h);
uint32_t Tex_Cache_GetPageGen(int tex_format, int tex_page_x, int tex_page_y);

/* gpu_primitives.c — GP0 command translation to GS */
int Translate_GP0_to_GS(uint32_t *psx_cmd);
int GPU_TryFastEmit(uint32_t *psx_cmd);
void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans);
void Prim_InvalidateGSState(void);
void Prim_InvalidateTexCache(void);
void Prim_InvalidateTexCache_Page(int tex_page_x, int tex_page_y);
void Prim_InvalidateTexCache_Region(int x, int y, int w, int h);
void Prim_FlushBatch(void);

/* gpu_commands.c — GP0/GP1 command processing */
extern const uint8_t gpu_cmd_size[256]; /* O(1) command size lookup */
int GPU_GetCommandSize(uint32_t cmd);
void GPU_ProcessDmaBlock(uint32_t *data_ptr, uint32_t word_count);

#endif /* GPU_STATE_H */
