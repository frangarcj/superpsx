/**
 * gpu_psp_core.c — PSP GPU core: state variables, lifecycle, display
 *
 * Owns all shared GPU state variable definitions (externed in gpu_state.h)
 * and the core lifecycle functions: Init, Flush, UpdateDisplay, VBlank,
 * Read/Status, and drawing environment (scissor, resolution, mask).
 *
 * Equivalent to gpu_ps2_core.c on the PS2 platform.
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

/* PSP-specific state */
#define DEBUG_SHOW_FULL_VRAM 0 /* 1 = show all 1024x512 PSX VRAM */
int psx_active_width = 320;
int psx_active_height = 240;
void *vram_mirror = (void *)PSP_VRAM_OFFSET;

/* Pre-calculated display blit parameters (recalc on resolution change) */
static struct
{
    int out_w, out_h, pad_x, pad_y;
    int src_w, src_h;
    int start_x, start_y;
    int valid;
} blit_cache;

/* Track which FB is the current back buffer for merged display blit */
static int back_fb_offset = PSP_FB0_OFFSET;

unsigned int __attribute__((aligned(16))) display_list[2][262144];

/* ── Vertex Pool (for GU_SEND mode) ─────────────────────────────── */
uint8_t __attribute__((aligned(64))) vpool_buf[2][VPOOL_SIZE];
int vpool_offset;
int dl_active = 0;
int sync_id[2] = {0, 0};

/* ── GPU Core Implementation ────────────────────────────────────── */

void GPU_Backend_Init(void)
{
    /* Allocate PSX VRAM shadow in main RAM (for CPU readback via C0).
     * 64-byte aligned for DCache writeback. */
    if (!psx_vram_shadow)
    {
        psx_vram_shadow = (uint16_t *)memalign(64, 1024 * 512 * sizeof(uint16_t));
        if (psx_vram_shadow)
            memset(psx_vram_shadow, 0, 1024 * 512 * sizeof(uint16_t));
    }

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list[0]);

    /* Double-buffer: draw→FB0, display→FB1.
     * sceGuSwapBuffers() exchanges them after each blit.
     * Blits are triggered by GP1(05h) display-start changes, not VBlank. */
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
            {-4, 0, -3, 1},
            {2, -2, 3, -1},
            {-3, 1, -4, 0},
            {3, -1, 2, -2}};
        sceGuSetDither(&dm);
    }

    sceGuFinish();
    sceGuSync(0, 0);

    /* Clear PSX VRAM region in EDRAM (uncached) */
    memset((void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000) + PSP_VRAM_OFFSET),
           0, 1024 * 512 * 2);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    /* Start first display list in SEND mode — render to PSX VRAM (offscreen RTT).
     * GU_SEND builds the list offline (no per-command stall updates).
     * Submitted as one batch via sceGuSendList at frame end / Flush. */
    vpool_offset = 0;
    dl_active = 0;
    sceGuStart(GU_SEND, display_list[dl_active]);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(0, 0, 1024, 512);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuOffset(0, 0);
    sceGuViewport(0, 0, 1024, 512);

    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 - draw_clip_x1 + 1, draw_clip_y2 - draw_clip_y1 + 1);
}

void GPU_Backend_Flush(void)
{
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf[dl_active], vpool_offset);
    sceGuFinish();

    sync_id[dl_active] = sceGuSendList(GU_TAIL, display_list[dl_active], NULL);
    /* We must sync here because most callers of Flush() follow with CPU VRAM access */
    sceGeListSync(sync_id[dl_active], 0);

    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list[dl_active]);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    Prim_InvalidateGSState();
}

void GPU_Backend_FlushSync(void)
{
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf[dl_active], vpool_offset);
    sceGuFinish();
    sceGuSendList(GU_TAIL, display_list[dl_active], NULL);
    sceGuSync(0, 0);
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list[dl_active]);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    Prim_InvalidateGSState();
}

void GPU_Backend_SetupEnvironment(void)
{
    sceGuStart(GU_SEND, display_list[dl_active]);
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
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_COLOR_TEST);

    /* Black letterbox borders */
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    /* Use offscreen PSX VRAM as texture source */
    sceGuTexFlush();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(psx_config.display_filter ? GU_LINEAR : GU_NEAREST,
                   psx_config.display_filter ? GU_LINEAR : GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

#if DEBUG_SHOW_FULL_VRAM
    /* Debug: show entire 1024x512 PSX VRAM scaled to PSP screen */
    {
        for (int strip = 0; strip < 2; strip++)
        {
            uintptr_t tex_base = (uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET + strip * 512 * 2;
            sceGuTexImage(0, 512, 512, 1024, (void *)tex_base);

            typedef struct
            {
                float u, v;
                int16_t x, y, z;
            } BlitVert;
            BlitVert *bv = (BlitVert *)vpool_alloc(2 * sizeof(BlitVert));
            bv[0].u = 0.0f;
            bv[0].v = 0.0f;
            bv[0].x = (int16_t)(strip * PSP_SCREEN_W / 2);
            bv[0].y = 0;
            bv[0].z = 0;
            bv[1].u = 512.0f;
            bv[1].v = 512.0f;
            bv[1].x = (int16_t)((strip + 1) * PSP_SCREEN_W / 2);
            bv[1].y = (int16_t)PSP_SCREEN_H;
            bv[1].z = 0;
            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           2, NULL, (void *)((uintptr_t)bv | 0x40000000));
        }
    }
#else
    /* Recalculate blit params only when resolution or display offset changed */
    if (blit_cache.src_w != psx_active_width ||
        blit_cache.src_h != psx_active_height ||
        blit_cache.start_x != display_start_x ||
        blit_cache.start_y != display_start_y)
    {
        int src_w = psx_active_width;
        int src_h = psx_active_height;

        if (src_w > 0 && src_h > 0)
        {
            if (psx_config.display_mode == 2)
            {
                int scale = 1;
                while ((src_w * (scale + 1)) <= PSP_SCREEN_W &&
                       (src_h * (scale + 1)) <= PSP_SCREEN_H)
                    scale++;
                blit_cache.out_w = src_w * scale;
                blit_cache.out_h = src_h * scale;
            }
            else if (psx_config.display_mode == 1)
            {
                blit_cache.out_w = PSP_SCREEN_W;
                blit_cache.out_h = PSP_SCREEN_H;
            }
            else
            {
                /* True 4:3 — PSX display is always 4:3 regardless of pixel
                 * resolution (320/512/640 all map to the same CRT width). */
                int out_h = PSP_SCREEN_H;
                int out_w = (out_h * 4) / 3;
                if (out_w > PSP_SCREEN_W)
                {
                    out_w = PSP_SCREEN_W;
                    out_h = (out_w * 3) / 4;
                }
                blit_cache.out_w = out_w;
                blit_cache.out_h = out_h;
            }
        }
        else
        {
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

    {
        int src_w = blit_cache.src_w;
        int src_h = blit_cache.src_h;
        int out_w = blit_cache.out_w;
        int out_h = blit_cache.out_h;
        int pad_x = blit_cache.pad_x;
        int pad_y = blit_cache.pad_y;

        int col = 0;
        while (col < src_w)
        {
            int strip_w = src_w - col;
            if (strip_w > 512)
                strip_w = 512;

            uintptr_t tex_base = (uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET + (display_start_x + col) * 2;
            int tex_pw = 1;
            while (tex_pw < strip_w)
                tex_pw <<= 1;
            int tex_ph = 1;
            while (tex_ph < (display_start_y + src_h))
                tex_ph <<= 1;
            if (tex_pw > 512)
                tex_pw = 512;
            if (tex_ph > 512)
                tex_ph = 512;
            sceGuTexImage(0, tex_pw, tex_ph, 1024, (void *)tex_base);

            int scr_x0 = pad_x + (col * out_w) / src_w;
            int scr_x1 = pad_x + ((col + strip_w) * out_w) / src_w;

            typedef struct
            {
                float u, v;
                int16_t x, y, z;
            } BlitVert;
            BlitVert *bv = (BlitVert *)vpool_alloc(2 * sizeof(BlitVert));
            bv[0].u = 0.0f;
            bv[0].v = (float)display_start_y;
            bv[0].x = (int16_t)scr_x0;
            bv[0].y = (int16_t)pad_y;
            bv[0].z = 0;
            bv[1].u = (float)strip_w;
            bv[1].v = (float)(display_start_y + src_h);
            bv[1].x = (int16_t)scr_x1;
            bv[1].y = (int16_t)(pad_y + out_h);
            bv[1].z = 0;
            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           2, NULL, (void *)((uintptr_t)bv | 0x40000000));
            col += strip_w;
        }
    }
#endif /* DEBUG_SHOW_FULL_VRAM */

    sceKernelDcacheWritebackRange(vpool_buf[dl_active], vpool_offset);
    sceGuFinish();

    sync_id[dl_active] = sceGuSendList(GU_TAIL, display_list[dl_active], NULL);
    /* NO SYNC HERE — GE processes blit while CPU starts next frame.
     * We will check for sync_id[new_dl] when we restart the list below. */
    sceGuSwapBuffers();

    /* Toggle back buffer for next frame */
    back_fb_offset = (back_fb_offset == PSP_FB0_OFFSET) ? PSP_FB1_OFFSET : PSP_FB0_OFFSET;

    /* ── Switch to other DL+vpool ────── */
    dl_active ^= 1;

    /* Only wait if the other buffer is STILL active in the GE (rare at 333MHz) */
    if (sync_id[dl_active] != 0)
    {
        sceGeListSync(sync_id[dl_active], 0);
    }

    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list[dl_active]);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 - draw_clip_x1 + 1, draw_clip_y2 - draw_clip_y1 + 1);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);

    Prim_InvalidateGSState();
}

void GPU_Backend_VBlank(void)
{
    gpu_pending_vblank_flush = 1;
    gpu_stat ^= 0x80000000; /* Toggle GPUSTAT bit 31 (LCF) */
    osd_vblank_count++;

    /* PSP cannot scan EDRAM directly like the PS2 GS — we must actively
     * blit the PSX VRAM region to the screen framebuffer every VBlank. */
    GPU_Backend_UpdateDisplay();
}

/* ── Drawing Environment ───────────────────────────────────────── */

void GPU_Backend_SetScissor(int x1, int y1, int x2, int y2)
{
    /* Scissor in PSX VRAM coordinates — GE renders directly to 1024×512 EDRAM.
     * sceGuScissor takes (x, y, width, height). */
    sceGuScissor(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

void GPU_Backend_SetDisplayFB(int x, int y)
{
    /* Just invalidate blit cache — VBlank will pick up new coordinates.
     * No immediate blit needed since VBlank drives display on PSP. */
    blit_cache.valid = 0;
}

void GPU_Backend_SetResolution(int interlace, int mode)
{
    static const int hres_table[] = {256, 320, 512, 640};
    uint32_t hres1 = (gpu_stat >> 17) & 3;
    uint32_t hres2 = (gpu_stat >> 16) & 1;
    uint32_t vres = (gpu_stat >> 19) & 1;

    psx_active_width = hres2 ? 368 : hres_table[hres1];
    if (mode == 3) /* PAL */
        psx_active_height = vres ? 512 : 256;
    else /* NTSC */
        psx_active_height = vres ? 480 : 240;
    (void)interlace;
}

void GPU_Backend_SetMaskBit(int set, int check)
{
    if (set || check)
    {
        sceGuEnable(GU_STENCIL_TEST);
        if (check)
        {
            sceGuStencilFunc(GU_NOTEQUAL, 1, 0xFF);
            sceGuStencilOp(GU_KEEP, GU_KEEP,
                           set ? GU_REPLACE : GU_KEEP);
        }
        else
        {
            sceGuStencilFunc(GU_ALWAYS, 1, 0xFF);
            sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
        }
    }
    else
    {
        sceGuDisable(GU_STENCIL_TEST);
    }
}

void GPU_Backend_ClearVRAM(int clip_x1, int clip_y1, int clip_x2, int clip_y2)
{
    (void)clip_x1;
    (void)clip_y1;
    (void)clip_x2;
    (void)clip_y2;
    memset((void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000) + PSP_VRAM_OFFSET),
           0, 1024 * 512 * 2);
    if (psx_vram_shadow)
        memset(psx_vram_shadow, 0, 1024 * 512 * 2);
}

void GPU_Backend_InvalidateState(void)
{
    Prim_InvalidateGSState();
}

int GPU_Backend_TryFastPoly(uint32_t *cmd_buffer)
{
    (void)cmd_buffer;
    return 0;
}

/* ── GPU Read / Status ─────────────────────────────────────────── */

uint32_t GPU_Read(void)
{
    if (vram_read_remaining > 0)
    {
        uint16_t p0 = 0, p1 = 0;
        if (psx_vram_shadow && vram_read_w > 0)
        {
            int px0 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py0 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px0 < 1024 && py0 < 512)
                p0 = psx_vram_shadow[py0 * 1024 + px0];
            vram_read_pixel++;
            int px1 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py1 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px1 < 1024 && py1 < 512)
                p1 = psx_vram_shadow[py1 * 1024 + px1];
            vram_read_pixel++;
        }
        vram_read_remaining--;
        if (vram_read_remaining == 0)
            gpu_stat &= ~0x08000000;
        gpu_read = (uint32_t)p0 | ((uint32_t)p1 << 16);
    }
    return gpu_read;
}

uint32_t GPU_ReadStatus(void)
{
    extern uint64_t global_cycles;
    extern uint64_t scheduler_cached_earliest;
    extern void Scheduler_DispatchEvents(uint64_t now);

    if (global_cycles < gpu_busy_until)
    {
        global_cycles = gpu_busy_until;
        gpu_busy_until = 0;
        while (global_cycles >= scheduler_cached_earliest)
            Scheduler_DispatchEvents(global_cycles);
    }

    uint32_t final_stat = gpu_stat | 0x14002000;

    uint32_t dma_dir = (final_stat >> 29) & 3;
    if (dma_dir == 1)
        final_stat |= 0x02000000;
    else if (dma_dir == 2)
        final_stat |= 0x02000000;
    else if (dma_dir == 3 && (final_stat & 0x08000000))
        final_stat |= 0x02000000;

    return final_stat;
}
