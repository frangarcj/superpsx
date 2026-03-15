/**
 * gpu_psp_backend.c — PSP GPU backend (GU-based rendering)
 *
 * Implements the GPU_Backend_* HAL interface using sceGu.
 * Also defines all shared GPU state variables (externed in gpu_state.h).
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "gpu_backend.h"
#include "osd.h"
#include "profiler.h"
#include "config.h"
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <psputils.h>
#include <malloc.h>
#include <string.h>

extern uint8_t *psx_ram;

/* ── Global variable definitions (externed in gpu_state.h) ──────────── */
uint32_t gpu_stat = 0x1C000000;
uint32_t gpu_read = 0;
volatile int gpu_pending_vblank_flush = 0;
uint64_t gpu_estimated_pixels = 0;

int fb_address = 0;
int fb_width = PSP_SCREEN_W;
int fb_height = PSP_SCREEN_H;
int fb_psm = 0;

int draw_offset_x = 0;
int draw_offset_y = 0;
int draw_clip_x1 = 0;
int draw_clip_y1 = 0;
int draw_clip_x2 = 1023;
int draw_clip_y2 = 511;

int disp_range_y1 = 0;
int disp_range_y2 = 240;

int display_start_x = 0;
int display_start_y = 0;

int tex_page_x = 0;
int tex_page_y = 0;
int tex_page_format = 0;
int semi_trans_mode = 0;
int dither_enabled = 0;

uint16_t *psx_vram_shadow = NULL;

int vram_tx_x = 0, vram_tx_y = 0, vram_tx_w = 0, vram_tx_h = 0, vram_tx_pixel = 0;
int vram_read_x = 0, vram_read_y = 0, vram_read_w = 0, vram_read_h = 0;
int vram_read_remaining = 0;
int vram_read_pixel = 0;

int polyline_active = 0;
int polyline_shaded = 0;
int polyline_semi_trans = 0;
uint32_t polyline_prev_color = 0;
uint32_t polyline_next_color = 0;
int16_t polyline_prev_x = 0;
int16_t polyline_prev_y = 0;
int polyline_expect_color = 0;

int tex_flip_x = 0;
int tex_flip_y = 0;

int mask_set_bit = 0;
int mask_check_bit = 0;
uint64_t cached_base_test = 0;

int gp1_allow_2mb = 0;

uint32_t tex_win_mask_x = 0;
uint32_t tex_win_mask_y = 0;
uint32_t tex_win_off_x = 0;
uint32_t tex_win_off_y = 0;

int gpu_cmd_remaining = 0;
uint32_t gpu_cmd_buffer[16];
int gpu_cmd_ptr = 0;
int gpu_transfer_words = 0;
int gpu_transfer_total = 0;

uint32_t vram_gen_counter = 0;
uint64_t gpu_busy_until = 0;
gs_state_t gs_state = {0};
gpu_frame_stats_t gpu_frame_stats = {0};

uint32_t raw_tex_window = 0;
uint32_t raw_draw_area_tl = 0;
uint32_t raw_draw_area_br = 0;
uint32_t raw_draw_offset = 0;

/* GIF stub variables (referenced by shared code but unused on PSP) */
unsigned char gif_packet_buf[2][16];
gif_qword_t *fast_gif_ptr = NULL;
gif_qword_t *gif_buffer_end_safe = NULL;
int current_buffer = 0;

/* PSP-specific state */
int psx_active_width = 320;
int psx_active_height = 240;
void *vram_mirror = (void *)PSP_VRAM_OFFSET;

/* Pre-calculated display blit parameters (recalc on resolution change) */
static struct {
    int out_w, out_h, pad_x, pad_y;
    int src_w, src_h;
    int start_x, start_y;
    int valid;
} blit_cache;

/* Track which FB is the current back buffer for merged display blit */
static int back_fb_offset = PSP_FB0_OFFSET;

static unsigned int __attribute__((aligned(16))) display_list[262144];

/* ── Vertex Pool (for GU_SEND mode) ─────────────────────────────── */
uint8_t __attribute__((aligned(64))) vpool_buf[VPOOL_SIZE];
int vpool_offset;

/* ── GPU Backend Implementation ─────────────────────────────────── */

void GPU_Backend_Init(void) {
    /* Allocate PSX VRAM shadow in main RAM (for CPU readback via C0).
     * 64-byte aligned for DCache writeback. */
    if (!psx_vram_shadow) {
        psx_vram_shadow = (uint16_t *)memalign(64, 1024 * 512 * sizeof(uint16_t));
        if (psx_vram_shadow)
            memset(psx_vram_shadow, 0, 1024 * 512 * sizeof(uint16_t));
    }

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);

    /* Standard double-buffer: draw→FB0, display→FB1.
     * sceGuSwapBuffers() exchanges them each frame.
     * PSX VRAM is an offscreen render target via DrawBufferList(). */
    sceGuDrawBuffer(GU_PSM_5551, (void *)PSP_FB0_OFFSET, PSP_BUF_W);
    sceGuDispBuffer(PSP_SCREEN_W, PSP_SCREEN_H, (void *)PSP_FB1_OFFSET, PSP_BUF_W);

    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_SMOOTH);

    /* PSX ordered dither matrix (-4..+3) */
    {
        ScePspIMatrix4 dm = {
            { -4,  0, -3,  1 },
            {  2, -2,  3, -1 },
            { -3,  1, -4,  0 },
            {  3, -1,  2, -2 }
        };
        sceGuSetDither(&dm);
    }

    sceGuFinish();
    sceGuSync(0, 0);

    /* Clear PSX VRAM region in EDRAM */
    memset((void *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET),
           0, 1024 * 512 * 2);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* Start first display list in SEND mode — render to PSX VRAM (offscreen RTT).
     * GU_SEND builds the list offline (no per-command stall updates).
     * Submitted as one batch via sceGuSendList at frame end / Flush. */
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 + 1, draw_clip_y2 + 1);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuOffset(0, 0);
    sceGuViewport(0, 0, 1024, 512);
}

void GPU_Backend_Flush(void) {
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf, vpool_offset);
    sceGuFinish();

    sceGuSendList(GU_TAIL, display_list, NULL);
    if (sceGuSync(0, 1))
        sceGuSync(0, 0);
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    Prim_InvalidateGSState();
}

void GPU_Backend_FlushSync(void) {
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf, vpool_offset);
    sceGuFinish();
    sceGuSendList(GU_TAIL, display_list, NULL);
    if (sceGuSync(0, 1))
        sceGuSync(0, 0);
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    Prim_InvalidateGSState();
}

void GPU_Backend_SetupEnvironment(void) {
    sceGuStart(GU_SEND, display_list);
}

void GPU_Backend_UpdateDisplay(void)
{
    /* Merged GP0 draws + display blit in same GE list (no mid-frame sync).
     * The GE executes commands in order, so the blit sees completed GP0 draws. */
    Prim_FlushBatch();

    /* Switch draw target from PSX VRAM to screen back buffer */
    sceGuDrawBufferList(GU_PSM_5551, (void *)(uintptr_t)back_fb_offset, PSP_BUF_W);

    sceGuScissor(0, 0, PSP_SCREEN_W, PSP_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_STENCIL_TEST);

    /* Black letterbox borders */
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    /* Use offscreen PSX VRAM as texture source (render-to-texture) */
    sceGuTexFlush();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(psx_config.display_filter ? GU_LINEAR : GU_NEAREST,
                   psx_config.display_filter ? GU_LINEAR : GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    /* Recalculate blit params only when resolution or display offset changed */
    if (!blit_cache.valid ||
        blit_cache.src_w != psx_active_width ||
        blit_cache.src_h != psx_active_height ||
        blit_cache.start_x != display_start_x ||
        blit_cache.start_y != display_start_y) {

        int src_w = psx_active_width;
        int src_h = psx_active_height;

        if (src_w > 0 && src_h > 0) {
            if (psx_config.display_mode == 2) {
                /* Integer scaling: NxN factor */
                if (src_w <= PSP_SCREEN_W && src_h <= PSP_SCREEN_H) {
                    int scale = 1;
                    while ((src_w * (scale + 1)) <= PSP_SCREEN_W &&
                           (src_h * (scale + 1)) <= PSP_SCREEN_H)
                        scale++;
                    blit_cache.out_w = src_w * scale;
                    blit_cache.out_h = src_h * scale;
                } else {
                    int div = 2;
                    while ((src_w / div) > PSP_SCREEN_W || (src_h / div) > PSP_SCREEN_H)
                        div++;
                    blit_cache.out_w = src_w / div;
                    blit_cache.out_h = src_h / div;
                }
            } else if (psx_config.display_mode == 1) {
                /* Stretch to fill — ignore aspect ratio */
                blit_cache.out_w = PSP_SCREEN_W;
                blit_cache.out_h = PSP_SCREEN_H;
            } else {
                /* 4:3 aspect-ratio preserving stretch (default) */
                int out_w = PSP_SCREEN_W;
                int out_h = (src_h * PSP_SCREEN_W) / src_w;
                if (out_h > PSP_SCREEN_H) {
                    out_h = PSP_SCREEN_H;
                    out_w = (src_w * PSP_SCREEN_H) / src_h;
                }
                blit_cache.out_w = out_w;
                blit_cache.out_h = out_h;
            }
        } else {
            blit_cache.out_w = PSP_SCREEN_W;
            blit_cache.out_h = PSP_SCREEN_H;
        }
        blit_cache.pad_x = (PSP_SCREEN_W - blit_cache.out_w) / 2;
        blit_cache.pad_y = (PSP_SCREEN_H - blit_cache.out_h) / 2;
        blit_cache.src_w = src_w;
        blit_cache.src_h = src_h;
        blit_cache.start_x = display_start_x;
        blit_cache.start_y = display_start_y;
        blit_cache.valid = 1;
    }

    int src_w  = blit_cache.src_w;
    int src_h  = blit_cache.src_h;
    int out_w  = blit_cache.out_w;
    int out_h  = blit_cache.out_h;
    int pad_x  = blit_cache.pad_x;
    int pad_y  = blit_cache.pad_y;

    /* Textured sprite strips (tiled for modes > 512 wide) */
    int col = 0;
    while (col < src_w) {
        int strip_w = src_w - col;
        if (strip_w > 512) strip_w = 512;

        uintptr_t tex_base = (uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET
                           + (display_start_x + col) * 2;
        int tex_pw = 1; while (tex_pw < strip_w) tex_pw <<= 1;
        int tex_ph = 1; while (tex_ph < (display_start_y + src_h)) tex_ph <<= 1;
        if (tex_pw > 512) tex_pw = 512;
        if (tex_ph > 512) tex_ph = 512;
        sceGuTexImage(0, tex_pw, tex_ph, 1024, (void *)tex_base);

        int scr_x0 = pad_x + (col * out_w) / src_w;
        int scr_x1 = pad_x + ((col + strip_w) * out_w) / src_w;

        typedef struct { float u, v; int16_t x, y, z; } BlitVert;
        BlitVert *bv = (BlitVert *)vpool_alloc(2 * sizeof(BlitVert));
        bv[0].u = 0.0f;                         bv[0].v = (float)display_start_y;
        bv[0].x = (int16_t)scr_x0;              bv[0].y = (int16_t)pad_y;         bv[0].z = 0;
        bv[1].u = (float)strip_w;               bv[1].v = (float)(display_start_y + src_h);
        bv[1].x = (int16_t)scr_x1;              bv[1].y = (int16_t)(pad_y + out_h); bv[1].z = 0;
        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, (void *)((uintptr_t)bv | 0x40000000));
        col += strip_w;
    }

    /* Writeback vertex pool so GE can read it, then submit entire list */
    sceKernelDcacheWritebackRange(vpool_buf, vpool_offset);
    sceGuFinish();

    sceGuSendList(GU_TAIL, display_list, NULL);
    if (sceGuSync(0, 1))
        sceGuSync(0, 0);
    sceGuSwapBuffers();

    /* Toggle back buffer for next frame */
    back_fb_offset = (back_fb_offset == PSP_FB0_OFFSET) ? PSP_FB1_OFFSET : PSP_FB0_OFFSET;

    /* ── Restart offscreen PSX VRAM draw list (SEND mode) ──────── */
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 + 1, draw_clip_y2 + 1);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);

    /* Blit changed GE state — invalidate all cached state so next GP0
     * primitives re-apply blend/dither/texture/color_test properly. */
    Prim_InvalidateGSState();

    /* gpu_frame_stats accumulates over profiler interval (60 frames).
     * Reset is handled by profiler_frame_end, not here. */
}

void GPU_Backend_VBlank(void) {
    gpu_stat ^= 0x80000000;   /* Toggle GPUSTAT bit 31 (LCF) — BIOS polls this for VSync */
    GPU_Backend_UpdateDisplay();
    osd_vblank_count++;
}

/* ── VRAM Operations ───────────────────────────────────────────── */

void GPU_Backend_StartVRAMTransfer(int x, int y, int w, int h) {
    vram_tx_x = x;
    vram_tx_y = y;
    vram_tx_w = w;
    vram_tx_h = h;
    vram_tx_pixel = 0;
}

#define VRAM_BASE_PTR ((void *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET))

void GPU_Backend_UploadShadowVRAM(int x, int y, int w, int h) {
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;
    /* In SEND mode, flush pending GE draws before CPU EDRAM write.
     * Otherwise deferred GE draws overwrite this data when executed. */
    GPU_Backend_Flush();
    for (int row = 0; row < h; row++) {
        uint16_t *src = &psx_vram_shadow[(y + row) * 1024 + x];
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + (y + row) * 1024 + x;
        memcpy(dst, src, w * 2);
    }
    /* Flush dcache so GE sees shadow→EDRAM copy */
    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    sceKernelDcacheWritebackRange(
        &edram_vram[y * 1024 + x],
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    /* Invalidate texture cache — shadow data changed in this region */
    Prim_InvalidateTexCache_Region(x, y, w, h);
}

void GPU_Backend_UploadRegionFast(uint32_t coords, uint32_t dims,
                                  uint32_t *data_ptr, uint32_t word_count) {
    int x = coords & 0xFFFF;
    int y = coords >> 16;
    int w = dims & 0xFFFF;
    int h = dims >> 16;
    if (w <= 0 || h <= 0) return;
    (void)word_count;
    GPU_Backend_Flush();

    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < h; row++) {
        int ry = (y + row) & 511;
        memcpy(&edram_vram[ry * 1024 + x], (uint16_t *)data_ptr + row * w, w * 2);
        if (psx_vram_shadow)
            memcpy(&psx_vram_shadow[ry * 1024 + x], (uint16_t *)data_ptr + row * w, w * 2);
    }

    /* Flush dcache so GE can see CPU-written texture/CLUT data */
    sceKernelDcacheWritebackRange(
        &edram_vram[(y & 511) * 1024 + x],
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    /* Invalidate software texture cache — VRAM content changed */
    Prim_InvalidateTexCache_Region(x, y, w, h);
}

void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    GPU_Backend_Flush();
    /* Copy within EDRAM PSX VRAM (GE render target) */
    for (int row = 0; row < h; row++) {
        uint16_t *src = (uint16_t *)VRAM_BASE_PTR + ((sy + row) & 511) * 1024 + (sx & 1023);
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + ((dy + row) & 511) * 1024 + (dx & 1023);
        memmove(dst, src, w * 2);
    }
    /* Keep shadow in sync */
    if (psx_vram_shadow) {
        for (int row = 0; row < h; row++) {
            uint16_t *src = &psx_vram_shadow[((sy + row) & 511) * 1024 + (sx & 1023)];
            uint16_t *dst = &psx_vram_shadow[((dy + row) & 511) * 1024 + (dx & 1023)];
            memmove(dst, src, w * 2);
        }
    }
    /* Flush dcache for destination region */
    uint16_t *edram_dst = (uint16_t *)VRAM_BASE_PTR + ((dy & 511) * 1024 + (dx & 1023));
    sceKernelDcacheWritebackRange(edram_dst,
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    Prim_InvalidateTexCache_Region(dx, dy, w, h);
}

void GPU_Backend_VRAMWrite(uint32_t word) {
    /* No-op: gpu_commands.c already writes to psx_vram_shadow and increments
     * vram_tx_pixel.  GPU_Backend_VRAMFlush() will copy shadow → EDRAM. */
    (void)word;
}

void GPU_Backend_VRAMFlush(void) {
    /* After A0 VRAM transfer completes, sync shadow RAM → EDRAM
     * so the data is available as a texture source for GE draws. */
    if (!psx_vram_shadow || vram_tx_w <= 0 || vram_tx_h <= 0) return;

    GPU_Backend_Flush();

    int tx = vram_tx_x, ty = vram_tx_y;
    int tw = vram_tx_w, th = vram_tx_h;

    /* Copy dirty region from shadow to EDRAM VRAM */
    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < th; row++) {
        int y = (ty + row) & 511;
        memcpy(&edram_vram[y * 1024 + tx],
               &psx_vram_shadow[y * 1024 + tx],
               tw * 2);
    }

    /* Flush dcache so GE can read the updated data */
    sceKernelDcacheWritebackRange(
        &edram_vram[ty * 1024 + tx],
        (uint32_t)(tw * 2 + (th - 1) * 1024 * 2));

    Prim_InvalidateTexCache_Region(tx, ty, tw, th);
}

void GPU_Backend_VRAMReadback(int x, int y, int w, int h) {
    /* Read from EDRAM PSX VRAM back to shadow for CPU access (C0 StoreImage).
     * GE draws go to EDRAM, so shadow may be stale for rendered pixels. */
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;

    /* Need to finish pending GE draws first */
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf, vpool_offset);
    sceGuFinish();
    sceGuSendList(GU_TAIL, display_list, NULL);
    sceGuSync(0, 0);

    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < h; row++) {
        int ry = (y + row) & 511;
        int rx = x & 1023;
        memcpy(&psx_vram_shadow[ry * 1024 + rx],
               &edram_vram[ry * 1024 + rx],
               w * 2);
    }

    /* Re-open display list for further draws (SEND mode) */
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 + 1, draw_clip_y2 + 1);
    sceGuEnable(GU_SCISSOR_TEST);
}

/* ── Drawing Environment ───────────────────────────────────────── */

void GPU_Backend_SetScissor(int x1, int y1, int x2, int y2) {
    /* Scissor in PSX VRAM coordinates — GE renders directly to 1024×512 EDRAM */
    sceGuScissor(x1, y1, x2 + 1, y2 + 1);
}

void GPU_Backend_SetDisplayFB(int x, int y) {
    display_start_x = x;
    display_start_y = y;
}

void GPU_Backend_SetResolution(int interlace, int mode) {
    /* Decode PSX display resolution from gpu_stat (set by GP1(08h)).
     * Parameters match PS2's SetGsCrt(interlace, mode, 0):
     *   interlace: 0=progressive, 1=interlaced
     *   mode: 2=NTSC, 3=PAL */
    static const int hres_table[] = {256, 320, 512, 640};
    uint32_t hres1 = (gpu_stat >> 17) & 3;
    uint32_t hres2 = (gpu_stat >> 16) & 1;
    uint32_t vres  = (gpu_stat >> 19) & 1;

    psx_active_width = hres2 ? 368 : hres_table[hres1];
    if (mode == 3) /* PAL */
        psx_active_height = vres ? 512 : 256;
    else /* NTSC */
        psx_active_height = vres ? 480 : 240;
    (void)interlace;
}

void GPU_Backend_SetMaskBit(int set, int check) {
    /* PSP GE stencil on GU_PSM_5551: bit 15 (alpha) = stencil bit.
     * mask_set_bit:   force STP=1 on written pixels  → stencil REPLACE ref=1
     * mask_check_bit: skip pixels where STP=1        → stencil NOTEQUAL ref=1 */
    if (set || check) {
        sceGuEnable(GU_STENCIL_TEST);
        if (check) {
            /* Block writes where dest STP=1 */
            sceGuStencilFunc(GU_NOTEQUAL, 1, 0xFF);
            /* On pass: force STP=1 if set, else keep dest STP (which is 0) */
            sceGuStencilOp(GU_KEEP, GU_KEEP,
                           set ? GU_REPLACE : GU_KEEP);
        } else {
            /* set only: always write, force STP=1 */
            sceGuStencilFunc(GU_ALWAYS, 1, 0xFF);
            sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
        }
    } else {
        sceGuDisable(GU_STENCIL_TEST);
    }
}

void GPU_Backend_ClearVRAM(int clip_x1, int clip_y1, int clip_x2, int clip_y2) {
    (void)clip_x1; (void)clip_y1; (void)clip_x2; (void)clip_y2;
    /* Clear PSX VRAM region in EDRAM */
    memset((void *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET),
           0, 1024 * 512 * 2);
    if (psx_vram_shadow)
        memset(psx_vram_shadow, 0, 1024 * 512 * 2);
}

void GPU_Backend_InvalidateState(void) {
    Prim_InvalidateGSState();
}

int GPU_Backend_TryFastPoly(uint32_t *cmd_buffer) {
    (void)cmd_buffer;
    return 0; /* No fast path on PSP yet */
}

/* ── PSP-specific stubs for shared code ──────────────────────────── */

VU0JITCache vu0_jit_cache;
void vu0_prepare_mvmva(R3000CPU *cpu, uint32_t mx_cv) {
    (void)cpu; (void)mx_cv;
}

/* ── GPU Read/Write/DMA (shared interface from superpsx.h) ─────── */

uint32_t GPU_Read(void) {
    if (vram_read_remaining > 0) {
        uint16_t p0 = 0, p1 = 0;
        if (psx_vram_shadow && vram_read_w > 0) {
            int px0 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py0 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px0 < 1024 && py0 < 512) p0 = psx_vram_shadow[py0 * 1024 + px0];
            vram_read_pixel++;
            int px1 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py1 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px1 < 1024 && py1 < 512) p1 = psx_vram_shadow[py1 * 1024 + px1];
            vram_read_pixel++;
        }
        vram_read_remaining--;
        if (vram_read_remaining == 0) gpu_stat &= ~0x08000000;
        gpu_read = (uint32_t)p0 | ((uint32_t)p1 << 16);
    }
    return gpu_read;
}

uint32_t GPU_ReadStatus(void) {
    /* Fast-forward cycles when GPU is "busy" — needed for DrawSync loops
     * that poll GPUSTAT in tight loops without advancing time. */
    extern uint64_t global_cycles;
    extern uint64_t scheduler_cached_earliest;
    extern void Scheduler_DispatchEvents(uint64_t now);

    if (global_cycles < gpu_busy_until) {
        global_cycles = gpu_busy_until;
        gpu_busy_until = 0;
        while (global_cycles >= scheduler_cached_earliest)
            Scheduler_DispatchEvents(global_cycles);
    }

    uint32_t final_stat = gpu_stat | 0x14002000;

    /* Bit 25: DMA data request — depends on GP1(04h) direction.
     * Dir=0→0, Dir=1→1 (FIFO ready), Dir=2→same as bit28, Dir=3→same as bit27.
     * BIOS gpu_send_dma polls this before starting GPU DMA. */
    uint32_t dma_dir = (final_stat >> 29) & 3;
    if (dma_dir == 1)
        final_stat |= 0x02000000;
    else if (dma_dir == 2)
        final_stat |= 0x02000000;
    else if (dma_dir == 3 && (final_stat & 0x08000000))
        final_stat |= 0x02000000;

    return final_stat;
}

int GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t addr = madr & 0x1FFFFC;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t direction = chcr & 1;

    PROF_PUSH(PROF_GPU_DMA);

    if (sync_mode == 0 || sync_mode == 1) {
        if (direction == 1) { /* CPU → GPU */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            PROF_PUSH(PROF_GPU_PRIM);
            GPU_ProcessDmaBlock((uint32_t *)(psx_ram + addr), words);
            PROF_POP(PROF_GPU_PRIM);
        } else { /* GPU → CPU (VRAM Read) */
            uint32_t words = (sync_mode == 0) ? (bcr & 0xFFFF) : ((bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF));
            if (words == 0) words = 0x10000;
            for (uint32_t i = 0; i < words; i++) {
                uint32_t word = GPU_Read();
                *(uint32_t *)(psx_ram + ((addr + i * 4) & 0x1FFFFC)) = word;
            }
        }
    } else if (sync_mode == 2) { /* Linked List */
        uint32_t max_packets = 20000;
        uint32_t packets = 0;

        while (packets < max_packets) {
            uint32_t header = *(uint32_t *)&psx_ram[addr];
            uint32_t count = header >> 24;
            uint32_t next = header & 0xFFFFFF;

            if (count > 0) {
                PROF_PUSH(PROF_GPU_PRIM);
                GPU_ProcessDmaBlock((uint32_t *)&psx_ram[(addr + 4) & 0x1FFFFC], count);
                PROF_POP(PROF_GPU_PRIM);
            }

            if (next == 0xFFFFFF) break;
            addr = next & 0x1FFFFC;
            packets++;
        }
    }

    PROF_POP(PROF_GPU_DMA);
    return 0;
}

void DumpVRAM(const char *filename) { (void)filename; }
void DumpShadowVRAM(const char *filename) { (void)filename; }

/* ── Texture cache stubs (PSP version) ─────────────────────────── */

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
