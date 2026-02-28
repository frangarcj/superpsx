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
#include <kernel.h>
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <gs_psm.h>
#include <gs_gp.h>
#include <gif_tags.h>

#define LOG_TAG "GPU"

/* ── DMA Channel 1 (VIF1) registers ─────────────────────────────── */
#define D1_CHCR ((volatile uint32_t *)0x10009000)
#define D1_MADR ((volatile uint32_t *)0x10009010)
#define D1_QWC ((volatile uint32_t *)0x10009020)

/* ── PSX VRAM geometry ───────────────────────────────────────────── */
#define PSX_VRAM_WIDTH 1024
#define PSX_VRAM_HEIGHT 512
#define PSX_VRAM_FBW (PSX_VRAM_WIDTH / 64) /* =16 in 64-pixel units */

/* ── GIF packet buffer ───────────────────────────────────────────── */
#define GIF_BUFFER_SIZE 16384

/* ── GPU deferred IRQ ───────────────────────────────────────────── */
/* Cycles after GP0(1Fh) before IRQ1 fires into I_STAT (mirrors real PSX
 * async GPU FIFO processing latency). */
#define GPU_IRQ_DELAY 500

/* ── CLUT decoded texture temp area in GS VRAM ──────────────────── */
#define CLUT_DECODED_Y 512
#define CLUT_DECODED_X 0

/* ── GIF Tag structure ───────────────────────────────────────────── */
typedef struct
{
    uint64_t NLOOP : 15;
    uint64_t EOP : 1;
    uint64_t pad1 : 30;
    uint64_t PRE : 1;
    uint64_t PRIM : 11;
    uint64_t FLG : 2;
    uint64_t NREG : 4;
    uint64_t REGS;
} GifTag __attribute__((aligned(16)));

typedef struct __attribute__((aligned(16)))
{
    uint64_t d0;
    uint64_t d1;
} gif_qword_t;

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

/* GIF double-buffered packet buffers */
extern unsigned __int128 gif_packet_buf[2][GIF_BUFFER_SIZE];
extern gif_qword_t *fast_gif_ptr;
extern gif_qword_t *gif_buffer_end_safe;
extern int current_buffer;

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

/* Texture page state (from GP0 E1) */
extern int tex_page_x;
extern int tex_page_y;
extern int tex_page_format;
extern int semi_trans_mode;
extern int dither_enabled;

/* Shadow PSX VRAM for CLUT texture decode */
extern uint16_t *psx_vram_shadow;

/* Debug log file - removed */
/* extern FILE *gpu_debug_log; */

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

/* IMAGE transfer buffer */
extern unsigned __int128 buf_image[1024];
extern int buf_image_ptr;

/* ═══════════════════════════════════════════════════════════════════
 *  Internal functions (cross-module interface)
 * ═══════════════════════════════════════════════════════════════════ */

/* gpu_gif.c — GIF buffer management */
void Flush_GIF(void);
void Flush_GIF_Sync(void);

#define GIF_TAG_LO(nloop, eop, pre, prim, flg, nreg) \
    (((uint64_t)(nloop) & 0x7FFF) |                  \
     (((uint64_t)(eop) & 1) << 15) |                 \
     (((uint64_t)(pre) & 1) << 46) |                 \
     (((uint64_t)(prim) & 0x7FF) << 47) |            \
     (((uint64_t)(flg) & 3) << 58) |                 \
     (((uint64_t)(nreg) & 15) << 60))

static inline void Push_GIF_Tag(uint64_t tag_lo, uint64_t tag_hi)
{
    if (__builtin_expect(fast_gif_ptr >= gif_buffer_end_safe, 0))
        Flush_GIF();

    *(unsigned __int128 *)fast_gif_ptr = ((unsigned __int128)tag_hi << 64) | (unsigned __int128)tag_lo;
    fast_gif_ptr++;
}

static inline void Push_GIF_Data(uint64_t d0, uint64_t d1)
{
    *(unsigned __int128 *)fast_gif_ptr = ((unsigned __int128)d1 << 64) | (unsigned __int128)d0;
    fast_gif_ptr++;
}

void Setup_GS_Environment(void);
/* ── Alpha blending register helpers ─────────────────────────────── */

// Compute GS ALPHA_1 register value from PSX semi-transparency mode
// GS formula: ((A-B)*C >> 7) + D  (C=FIX divides by 128, so FIX=128=1.0, 64=0.5, 32=0.25)
// Note: For mode 0, we use FIX=0x58 (88/128≈0.6875) instead of the standard 0x40 (64/128=0.5)
// to better match the reference test screenshots.  We use SDK helpers instead of
// hand-packed bitfields.
static inline uint64_t Get_Alpha_Reg(int mode)
{
    switch (mode & 3)
    {
    case 0:
        /* mode 0: ~0.69*Cs + 0.31*Cd */
        return GS_SET_ALPHA(0, 1, 2, 1, 0x58);
    case 1:
        /* mode 1: ~0.5*Cs + 0.5*Cd */
        return GS_SET_ALPHA(0, 2, 2, 1, 0x80);
    case 2:
        return GS_SET_ALPHA(1, 0, 2, 2, 0x80);
    default: 
        /* mode 3: Cd + 0.25*Cs */
        return GS_SET_ALPHA(0, 2, 2, 1, 0x20);
    }
}

static inline uint64_t Get_Base_TEST(void)
{
    /* Compose TEST register base using SDK macro, but only for invariant mask bits.
     * Use `mask_check_bit` to drive DATEN/DATMD (prevents writing to pixels that
     * already have bit 15 set in the framebuffer).
     * Alpha-test and Z-test fields are left clear so callers can safely OR their
     * own configuration without double-encoding ATEN/ATST or ZTST.
     * GS_SET_TEST params: ATEN, ATMETH, ATREF, ATFAIL, DATEN, DATMD, ZTEN, ZTMETH
     */
    return GS_SET_TEST(0, 0, 0, 0, mask_check_bit, 0, 0, 0);
}

/* Helper: pack an existing prim bitfield value into the GS_SET_PRIM macro
 * so callers that compute prim bits directly can still use the SDK helper. */
static inline uint64_t GS_PACK_PRIM_FROM_INT(uint64_t v)
{
    return GS_SET_PRIM((v) & 0x7, ((v) >> 3) & 0x1, ((v) >> 4) & 0x1, ((v) >> 5) & 0x1,
                       ((v) >> 6) & 0x1, ((v) >> 7) & 0x1, ((v) >> 8) & 0x1,
                       ((v) >> 9) & 0x1, ((v) >> 10) & 0x1);
}

/* gpu_vram.c — VRAM transfer operations */
void Start_VRAM_Transfer(int x, int y, int w, int h);
void Upload_Shadow_VRAM_Region(int x, int y, int w, int h);
uint16_t *GS_ReadbackRegion(int x, int y, int w_aligned, int h,
                            void *buf, int buf_qwc);
void GS_UploadRegion(int x, int y, int w, int h, const uint16_t *pixels);
void GS_UploadRegionFast(uint32_t coords, uint32_t dims, uint32_t *data_ptr, uint32_t word_count);
void DumpVRAM(const char *filename);

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
int Decode_CLUT4_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th);
int Decode_CLUT8_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th);
int Decode_TexWindow_Rect(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int u0_cmd, int v0_cmd, int w, int h,
                          int flip_x, int flip_y);
int Decode_TexPage_Cached(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y);
void Tex_Cache_DumpStats(void);
void Tex_Cache_ResetStats(void);
void Tex_Cache_DirtyRegion(int x, int y, int w, int h);

/* gpu_primitives.c — GP0 command translation to GS */
void Translate_GP0_to_GS(uint32_t *psx_cmd);
void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans);
void Prim_InvalidateGSState(void);
void Prim_InvalidateTexCache(void);

/* gpu_commands.c — GP0/GP1 command processing */
int GPU_GetCommandSize(uint32_t cmd);
void GPU_ProcessDmaBlock(uint32_t *data_ptr, uint32_t word_count);

/* gpu_core.c — Display update */
void Update_GS_Display(void);

#endif /* GPU_STATE_H */
