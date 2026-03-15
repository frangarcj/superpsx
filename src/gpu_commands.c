/**
 * gpu_commands.c — GP0 / GP1 command processing
 *
 * Handles the PSX GPU command FIFO: accumulating multi-word commands,
 * VRAM-to-CPU/CPU-to-VRAM transfers, polyline state machine, rendering
 * environment registers (E1-E6), and GP1 display/status commands.
 *
 * This file is PLATFORM-AGNOSTIC. All hardware specifics are handled
 * via the GPU_Backend_* interface defined in gpu_backend.h.
 */
#include "gpu_state.h"
#include "gpu_backend.h"
#include "osd.h"
#include "config.h"
#include <time.h>

/* ── Command size lookup table (256 entries, O(1)) ───────────────── */
/*
 * gpu_cmd_size[cmd_byte] = number of PSX words for that GP0 command.
 * 0 means "variable/special" (polylines, LoadImage, StoreImage).
 * Indexed by the top byte of the GP0 command word.
 *
 * Layout:
 *   0x00-0x01  NOP (1)
 *   0x02       Fill rect (3)
 *   0x03-0x1E  Reserved/NOP (1)
 *   0x1F       IRQ (1)
 *   0x20-0x3F  Polygons (4-12 depending on flags)
 *   0x40-0x47  Non-poly lines: flat=3, shaded=4
 *   0x48-0x5F  Polylines: 0 (variable length — use word-at-a-time)
 *   0x60-0x7F  Rectangles (2-4 depending on flags)
 *   0x80-0x9F  VRAM-to-VRAM copy (4)
 *   0xA0       LoadImage (0 — variable: 3 header + N data words)
 *   0xC0       StoreImage (0 — 3 words, but triggers read mode)
 *   0xE1-0xE6  Environment commands (1)
 *   Everything else: 1 (NOP/unknown)
 */
const uint8_t gpu_cmd_size[256] = {
    /* 0x00-0x0F: NOP / fill / reserved */
    1,
    1,
    3,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    /* 0x10-0x1F: reserved, 0x1F = IRQ */
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,

    /* 0x20-0x2F: Polygons (tri/quad × flat/shaded × untex/tex) */
    /*         base  +tex  +semi +tex+semi  +quad +quad+tex +quad+semi +quad+tex+semi */
    /* 0x20 */ 4,
    4,
    4,
    4,
    7,
    7,
    7,
    7,
    5,
    5,
    5,
    5,
    9,
    9,
    9,
    9,
    /* 0x30: +shaded */
    /* 0x30 */ 6,
    6,
    6,
    6,
    9,
    9,
    9,
    9,
    8,
    8,
    8,
    8,
    12,
    12,
    12,
    12,

    /* 0x40-0x4F: Lines (0x40-0x47 single, 0x48-0x4F polyline=variable) */
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* 0x50-0x5F: Shaded lines (0x50-0x57 single, 0x58-0x5F polyline=variable) */
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,

    /* 0x60-0x6F: Rectangles */
    /* size=var: 0x60-0x63 untex(3), 0x64-0x67 tex(4) */
    3,
    3,
    3,
    3,
    4,
    4,
    4,
    4,
    /* size=1x1: 0x68-0x6B untex(2), 0x6C-0x6F tex(3) */
    2,
    2,
    2,
    2,
    3,
    3,
    3,
    3,
    /* 0x70-0x7F: Rectangles (cont.) */
    /* size=8x8: 0x70-0x73 untex(2), 0x74-0x77 tex(3) */
    2,
    2,
    2,
    2,
    3,
    3,
    3,
    3,
    /* size=16x16: 0x78-0x7B untex(2), 0x7C-0x7F tex(3) */
    2,
    2,
    2,
    2,
    3,
    3,
    3,
    3,

    /* 0x80-0x9F: VRAM-to-VRAM copy (4 words) */
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,
    4,

    /* 0xA0-0xAF: LoadImage = 0 (variable) */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* 0xB0-0xBF: reserved (1) */
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,

    /* 0xC0-0xCF: StoreImage = 0 (special) */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* 0xD0-0xDF: reserved (1) */
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,

    /* 0xE0-0xEF: Environment commands (1 word each) */
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    /* 0xF0-0xFF: reserved / NOP (1) */
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
};

/* Compatibility wrapper (for callers outside the hot path) */
int GPU_GetCommandSize(uint32_t cmd)
{
    uint8_t s = gpu_cmd_size[cmd & 0xFF];
    return s ? s : 1;
}
static uint32_t cache_e1 = 0xFFFFFFFF;
static uint32_t cache_e3 = 0xFFFFFFFF;
static uint32_t cache_e4 = 0xFFFFFFFF;
static uint32_t cache_e5 = 0xFFFFFFFF;
static uint32_t cache_gp1_05 = 0xFFFFFFFF;
static uint32_t cache_gp1_06 = 0xFFFFFFFF;
static uint32_t cache_gp1_07 = 0xFFFFFFFF;
static uint32_t cache_gp1_08 = 0xFFFFFFFF;

void clear_gpu_param_cache(void)
{
    cache_e1 = 0xFFFFFFFF;
    cache_e3 = 0xFFFFFFFF;
    cache_e4 = 0xFFFFFFFF;
    cache_e5 = 0xFFFFFFFF;
    cache_gp1_05 = 0xFFFFFFFF;
    cache_gp1_06 = 0xFFFFFFFF;
    cache_gp1_07 = 0xFFFFFFFF;
    cache_gp1_08 = 0xFFFFFFFF;
}

/* ── Frame counter state ─────────────────────────────────────────── */
static uint32_t frame_count = 0;
static uint32_t fps_display = 0;
static uint32_t speed_display = 0;
static clock_t fps_clock_start = 0;

/* ── GP0 Write ───────────────────────────────────────────────────── */

void GPU_WriteGP0(uint32_t data)
{
    // Deferred flush from VBlank ISR
    if (gpu_pending_vblank_flush)
    {
        if (psx_config.show_fps)
        {
            frame_count++;
            clock_t now = clock();
            if (now - fps_clock_start >= CLOCKS_PER_SEC)
            {
                fps_display = frame_count;
                /* Speed %: vblanks/sec relative to target (60 NTSC, 50 PAL) */
                uint32_t target = psx_config.region_pal ? 50 : 60;
                uint32_t vblanks = osd_vblank_count;
                osd_vblank_count = 0;
                speed_display = (vblanks * 100 + target / 2) / target;
                frame_count = 0;
                fps_clock_start = now;
            }
            osd_printf(display_start_x + 4, display_start_y + 4,
                       OSD_COLOR_WHITE, "%luFPS %lu%%  ",
                       (unsigned long)fps_display,
                       (unsigned long)speed_display);
        }
        osd_draw(); /* render OSD overlay — stays in current GE list */
        gpu_pending_vblank_flush = 0;
    }

    // If transferring data (A0/C0)
    if (gpu_transfer_words > 0)
    {
        // Write to shadow VRAM (raw 16-bit data)
        if (psx_vram_shadow && vram_tx_w > 0)
        {
            uint16_t p0 = data & 0xFFFF;
            uint16_t p1 = data >> 16;
            int total_pixels = vram_tx_w * vram_tx_h;

            if (mask_set_bit)
            {
                p0 |= 0x8000;
                p1 |= 0x8000;
            }

            if (vram_tx_pixel < total_pixels)
            {
                int px = vram_tx_x + (vram_tx_pixel % vram_tx_w);
                int py = vram_tx_y + (vram_tx_pixel / vram_tx_w);
                if (px < 1024 && py < 512)
                {
                    if (!mask_check_bit || !(psx_vram_shadow[py * 1024 + px] & 0x8000))
                        psx_vram_shadow[py * 1024 + px] = p0;
                }
            }
            vram_tx_pixel++;

            if (vram_tx_pixel < total_pixels)
            {
                int px = vram_tx_x + (vram_tx_pixel % vram_tx_w);
                int py = vram_tx_y + (vram_tx_pixel / vram_tx_w);
                if (px < 1024 && py < 512)
                {
                    if (!mask_check_bit || !(psx_vram_shadow[py * 1024 + px] & 0x8000))
                        psx_vram_shadow[py * 1024 + px] = p1;
                }
            }
            vram_tx_pixel++;
        }

        // For CT16S: accumulate raw 32-bit words into 128-bit qwords
        // Set STP bit for non-zero pixels → GS alpha=0x80 for opaque, 0x00 for transparent
        /* Delegate GS-specific pixel packing & upload to the backend */
        GPU_Backend_VRAMWrite(data);

        gpu_transfer_words--;
        if (gpu_transfer_words == 0)
        {
            GPU_Backend_VRAMFlush(); /* pad and flush remaining VRAM words */
            GPU_Backend_Flush();     /* sync for VRAM-to-CPU readback */

            if (vram_tx_x + vram_tx_w > 1024)
            {
                int wrap_w = (vram_tx_x + vram_tx_w) - 1024;
                GPU_Backend_UploadShadowVRAM(0, vram_tx_y, wrap_w, vram_tx_h);
            }
        }
        return;
    }

    // Polyline continuation
    if (polyline_active)
    {
        if ((data & 0xF000F000) == 0x50005000)
        {
            polyline_active = 0;
            // Flush_GIF(); <-- Removed for batching
            return;
        }

        if (polyline_shaded && polyline_expect_color)
        {
            polyline_next_color = data & 0xFFFFFF;
            polyline_expect_color = 0;
        }
        else
        {
            int16_t x = (int16_t)(data & 0xFFFF);
            int16_t y = (int16_t)(data >> 16);

            uint32_t new_color = polyline_shaded ? polyline_next_color : polyline_prev_color;

            Emit_Line_Segment_AD(polyline_prev_x, polyline_prev_y, polyline_prev_color,
                                 x, y, new_color,
                                 polyline_shaded, polyline_semi_trans);
            // Flush_GIF(); <-- Removed for batching

            polyline_prev_x = x;
            polyline_prev_y = y;
            polyline_prev_color = new_color;

            if (polyline_shaded)
                polyline_expect_color = 1;
        }
        return;
    }

    if (gpu_cmd_remaining > 0)
    {
        gpu_cmd_buffer[gpu_cmd_ptr++] = data;
        gpu_cmd_remaining--;
        if (gpu_cmd_remaining == 0)
        {
            uint32_t cmd = gpu_cmd_buffer[0] >> 24;
            if (cmd == 0xA0)
            {
                uint32_t wh = gpu_cmd_buffer[2];
                uint32_t w = wh & 0xFFFF;
                uint32_t h = wh >> 16;
                if (w == 0)
                    w = 1024;
                if (h == 0)
                    h = 512;
                gpu_transfer_words = (w * h + 1) / 2;
                gpu_transfer_total = gpu_transfer_words;
                DLOG("GP0(A0) Start Transfer: %" PRIu32 "x%" PRIu32 " (%d words)\n", w, h, gpu_transfer_words);

                uint32_t xy = gpu_cmd_buffer[1];
                vram_tx_x = xy & 0xFFFF;
                vram_tx_y = xy >> 16;
                vram_tx_w = w;
                vram_tx_h = h;
                vram_tx_pixel = 0;

                vram_gen_counter++;
                Tex_Cache_DirtyRegion(vram_tx_x, vram_tx_y, w, h);
                gpu_frame_stats.vram_load++;
                GPU_Backend_StartVRAMTransfer(vram_tx_x, vram_tx_y, w, h);
#ifdef TEX_DEBUG_OVERLAY
                if (w * h > 200)
                    printf("[C2V] (%d,%d) %dx%d\n", vram_tx_x, vram_tx_y, w, h);
#endif
            }
            else if (cmd == 0xC0)
            {
                gpu_frame_stats.vram_store++;
                uint32_t xy = gpu_cmd_buffer[1];
                uint32_t wh = gpu_cmd_buffer[2];
                vram_read_x = xy & 0xFFFF;
                vram_read_y = xy >> 16;
                vram_read_w = wh & 0xFFFF;
                vram_read_h = wh >> 16;
                if (vram_read_w == 0)
                    vram_read_w = 1024;
                if (vram_read_h == 0)
                    vram_read_h = 512;
                vram_read_remaining = (vram_read_w * vram_read_h + 1) / 2;
                vram_read_pixel = 0;

                gpu_stat |= 0x08000000;

                /* Readback GS/GPU VRAM into psx_vram_shadow so that
                 * GPU_Read() returns correct data for C0+DMA reads. */
                GPU_Backend_VRAMReadback(vram_read_x, vram_read_y,
                                         vram_read_w, vram_read_h);

                DLOG("GP0(C0) VRAM Read: %dx%d at (%d,%d), %d words\n",
                     vram_read_w, vram_read_h, vram_read_x, vram_read_y, vram_read_remaining);
            }
            else if ((cmd & 0xE0) == 0x80)
            {
                uint32_t src_xy = gpu_cmd_buffer[1];
                uint32_t dst_xy = gpu_cmd_buffer[2];
                uint32_t wh = gpu_cmd_buffer[3];
                int sx = src_xy & 0x3FF;
                int sy = (src_xy >> 16) & 0x1FF;
                int dx = dst_xy & 0x3FF;
                int dy = (dst_xy >> 16) & 0x1FF;
                int w = wh & 0x3FF;
                int h = (wh >> 16) & 0x1FF;
                if (w == 0)
                    w = 0x400;
                if (h == 0)
                    h = 0x200;

                DLOG("GP0(80) VRAM Copy: (%d,%d)->(%d,%d) %dx%d\n", sx, sy, dx, dy, w, h);
#ifdef TEX_DEBUG_OVERLAY
                printf("[V2V] (%d,%d)->(%d,%d) %dx%d\n", sx, sy, dx, dy, w, h);
#endif

                if (psx_vram_shadow)
                {
                    vram_gen_counter++;
                    Tex_Cache_DirtyRegion(dx, dy, w, h);
                    gpu_frame_stats.vram_copy++;

                    /* Copy in shadow VRAM (handles all overlap cases
                     * correctly with PSX pixel-by-pixel semantics).
                     * PSX copies left→right, top→bottom, wrapping at
                     * 1024×512.  Overlapping src/dst produces well-defined
                     * results due to the fixed copy order. */
                    if (sx + w <= 1024 && dx + w <= 1024 &&
                        sy + h <= 512 && dy + h <= 512)
                    {
                        if (sy == dy && !(sx >= dx + w || dx >= sx + w))
                        {
                            /* Same row, horizontal overlap → memmove */
                            for (int row = 0; row < h; row++)
                            {
                                memmove(&psx_vram_shadow[(dy + row) * 1024 + dx],
                                        &psx_vram_shadow[(sy + row) * 1024 + sx],
                                        w * sizeof(uint16_t));
                            }
                        }
                        else if (dy > sy && dy < sy + h)
                        {
                            /* Downward overlap → copy bottom-to-top */
                            for (int row = h - 1; row >= 0; row--)
                            {
                                memcpy(&psx_vram_shadow[(dy + row) * 1024 + dx],
                                       &psx_vram_shadow[(sy + row) * 1024 + sx],
                                       w * sizeof(uint16_t));
                            }
                        }
                        else
                        {
                            /* No overlap or upward overlap → simple memcpy */
                            for (int row = 0; row < h; row++)
                            {
                                memcpy(&psx_vram_shadow[(dy + row) * 1024 + dx],
                                       &psx_vram_shadow[(sy + row) * 1024 + sx],
                                       w * sizeof(uint16_t));
                            }
                        }
                    }
                    else
                    {
                        /* Wrapping path: pixel-by-pixel (rare) */
                        for (int row = 0; row < h; row++)
                        {
                            for (int col = 0; col < w; col++)
                            {
                                int src_px = ((sy + row) & 0x1FF) * 1024 + ((sx + col) & 0x3FF);
                                int dst_px = ((dy + row) & 0x1FF) * 1024 + ((dx + col) & 0x3FF);
                                psx_vram_shadow[dst_px] = psx_vram_shadow[src_px];
                            }
                        }
                    }

                    /* Upload the destination region from shadow VRAM to GS.
                     * This replaces both the GS Local→Local copy and the
                     * BUSDIR readback path.  Benefits:
                     *  - Correct for ALL overlap cases (shadow already copied)
                     *  - Applies STP fixup (Upload_Shadow_VRAM_Region does it)
                     *  - No BUSDIR readback (fragile on real PS2 hardware)
                     *  - No Flush_GIF_Sync stall
                     *  - Eliminates 1MB vram_copy_readback buffer */
                    GPU_Backend_UploadShadowVRAM(dx, dy, w, h);
                }
            }
            else
            {
                /* Try platform-specific fast path first, fall back to generic */
                if (!GPU_Backend_TryFastPoly(gpu_cmd_buffer))
                {
                    Translate_GP0_to_GS(gpu_cmd_buffer);
                }
                // Flush_GIF(); <-- Already removed

                if ((cmd & 0xE0) == 0x40 && (cmd & 0x08))
                {
                    polyline_active = 1;
                    polyline_shaded = (cmd & 0x10) != 0;
                    polyline_semi_trans = (cmd & 0x02) != 0;

                    int v1_idx = polyline_shaded ? 3 : 2;
                    uint32_t xy1 = gpu_cmd_buffer[v1_idx];
                    polyline_prev_x = (int16_t)(xy1 & 0xFFFF);
                    polyline_prev_y = (int16_t)(xy1 >> 16);

                    if (polyline_shaded)
                    {
                        polyline_prev_color = gpu_cmd_buffer[2] & 0xFFFFFF;
                        polyline_expect_color = 1;
                    }
                    else
                    {
                        polyline_prev_color = gpu_cmd_buffer[0] & 0xFFFFFF;
                        polyline_expect_color = 0;
                    }
                }
            }
        }
        return;
    }

    uint32_t cmd = (data >> 24) & 0xFF;

    // Commands that need parameter accumulation
    if (cmd == 0xA0 || cmd == 0xC0 || (cmd & 0xE0) == 0x80)
    {
        gpu_cmd_buffer[0] = data;
        gpu_cmd_ptr = 1;
        if (cmd == 0xA0)
            gpu_cmd_remaining = 2;
        else if (cmd == 0xC0)
            gpu_cmd_remaining = 2;
        else
            gpu_cmd_remaining = 3;
        return;
    }

    switch (cmd)
    {
    case 0xE1: // Draw Mode
    {
        if (data != cache_e1)
        {
            cache_e1 = data;

            uint32_t tp_x = data & 0xF;
            uint32_t tp_y = (data >> 4) & 1;
            uint32_t tpf = (data >> 7) & 3;

            tex_page_x = tp_x * 64;
            tex_page_y = tp_y * 256;
            tex_page_format = tpf;

            uint32_t trans_mode = (data >> 5) & 3;
            semi_trans_mode = trans_mode;

            tex_flip_x = (data >> 12) & 1;
            tex_flip_y = (data >> 13) & 1;

            if (gp1_allow_2mb)
                gpu_stat = (gpu_stat & ~0x87FF) | (data & 0x7FF) | (((data >> 11) & 1) << 15);
            else
                gpu_stat = (gpu_stat & ~0x87FF) | (data & 0x7FF);

            uint32_t dither_enable = (data >> 9) & 1;
            dither_enabled = dither_enable;

            /* G5: No GIF writes here — TEX0/TEXFLUSH/DTHE/ALPHA_1 are
             * always re-emitted by the next primitive via lazy gs_state
             * tracking (which checks gs_state.valid after invalidation).
             * Saves 5 QWs (tag + 4 regs) per E1 change. */
            GPU_Backend_InvalidateState();
        }
    }
    break;
    case 0xE3: // Drawing Area Top-Left
    {
        raw_draw_area_tl = data & 0xFFFFF;
        if (data != cache_e3)
        {
            cache_e3 = data;
            draw_clip_x1 = data & 0x3FF;
            draw_clip_y1 = (data >> 10) & 0x3FF;
#ifdef TEX_DEBUG_OVERLAY
            printf("[DRAW] E3 draw_area_tl=(%d,%d)\n", draw_clip_x1, draw_clip_y1);
#endif
            GPU_Backend_SetScissor(draw_clip_x1, draw_clip_y1,
                                   draw_clip_x2, draw_clip_y2);
        }
    }
    break;
    case 0xE4: // Drawing Area Bottom-Right
    {
        raw_draw_area_br = data & 0xFFFFF;
        if (data != cache_e4)
        {
            cache_e4 = data;
            draw_clip_x2 = data & 0x3FF;
            draw_clip_y2 = (data >> 10) & 0x3FF;
#ifdef TEX_DEBUG_OVERLAY
            printf("[DRAW] E4 draw_area_br=(%d,%d)\n", draw_clip_x2, draw_clip_y2);
#endif
            GPU_Backend_SetScissor(draw_clip_x1, draw_clip_y1,
                                   draw_clip_x2, draw_clip_y2);
        }
    }
    break;
    case 0xE5: // Drawing Offset
    {
        raw_draw_offset = data & 0x3FFFFF;
        if (data != cache_e5)
        {
            cache_e5 = data;
            draw_offset_x = (int16_t)(data & 0x7FF);
            if (draw_offset_x & 0x400)
                draw_offset_x |= 0xF800;
            draw_offset_y = (int16_t)((data >> 11) & 0x7FF);
            if (draw_offset_y & 0x400)
                draw_offset_y |= 0xF800;
        }
    }
    break;
    case 0xE2: // Texture Window Setting
        raw_tex_window = data & 0xFFFFF;
        tex_win_mask_x = data & 0x1F;
        tex_win_mask_y = (data >> 5) & 0x1F;
        tex_win_off_x = (data >> 10) & 0x1F;
        tex_win_off_y = (data >> 15) & 0x1F;
        break;
    case 0xE6: // Mask Bit Setting
        mask_set_bit = data & 1;
        mask_check_bit = (data >> 1) & 1;
        cached_base_test = mask_check_bit ? ((uint64_t)1 << 14) : 0;
        gpu_stat = (gpu_stat & ~0x1800) | (mask_set_bit << 11) | (mask_check_bit << 12);
        // GS: FBA_1 forces bit 15 on all written pixels (mask_set_bit)
        // GS: DATE+DATM in TEST_1 prevents writing to pixels with bit 15 set (mask_check_bit)
        GPU_Backend_SetMaskBit(mask_set_bit, mask_check_bit);
        GPU_Backend_InvalidateState();
        break;
    case 0x1F: // GPU IRQ Request (edge-triggered)
        /* Per psx-spx: I_STAT bits are edge-triggered — they get set ONLY
         * when the interrupt source transitions from false to true.
         * GPUSTAT.24 is the source line for I_STAT bit 1 (GPU IRQ).
         * Only fire SignalInterrupt when GPUSTAT.24 was previously 0. */
        if (!(gpu_stat & 0x01000000))
        {
            gpu_stat |= 0x01000000;
            SignalInterrupt(1); /* Rising edge → set I_STAT bit 1 */
        }
        break;
    case 0x00: // NOP
    case 0x01: // Clear Cache
        break;
    default:
    {
        int size = GPU_GetCommandSize(cmd);
        if (size > 1)
        {
            static int draw_cmd_count = 0;
            draw_cmd_count++;
            if (draw_cmd_count <= 20 || (draw_cmd_count % 10000 == 0))
            {
                DLOG("GP0 draw cmd %02Xh (size=%d) #%d\n", (unsigned)cmd, size, draw_cmd_count);
            }
            gpu_cmd_buffer[0] = data;
            gpu_cmd_ptr = 1;
            gpu_cmd_remaining = size - 1;
        }
        else if (size == 1 && ((cmd & 0xE0) == 0x20 || cmd == 0x02))
        {
            uint32_t buff[1] = {data};
            Translate_GP0_to_GS(buff);
        }
        else if ((cmd & 0xE0) == 0x40 && (cmd & 0x08))
        {
            /* Polyline start (0x48-0x4F flat, 0x58-0x5F shaded):
             * accumulate the first segment like a regular line. */
            int poly_first_size = (cmd & 0x10) ? 4 : 3;
            gpu_cmd_buffer[0] = data;
            gpu_cmd_ptr = 1;
            gpu_cmd_remaining = poly_first_size - 1;
        }
        break;
    }
    }
}

/* ── GP1 Write ───────────────────────────────────────────────────── */

void GPU_WriteGP1(uint32_t data)
{
    uint32_t cmd = (data >> 24) & 0xFF;
    switch (cmd)
    {
    case 0x00: // Reset GPU
        clear_gpu_param_cache();
        GPU_Backend_InvalidateState();
        Prim_InvalidateTexCache();
        gpu_stat = 0x14802000;
        gpu_read = 0;
        draw_offset_x = 0;
        draw_offset_y = 0;
        draw_clip_x1 = 0;
        draw_clip_y1 = 0;
        draw_clip_x2 = 256;
        draw_clip_y2 = 240;
        gpu_cmd_remaining = 0;
        gpu_transfer_words = 0;
        tex_page_x = 0;
        tex_page_y = 0;
        tex_page_format = 0;
        semi_trans_mode = 0;
        tex_flip_x = 0;
        tex_flip_y = 0;
        tex_win_mask_x = 0;
        tex_win_mask_y = 0;
        tex_win_off_x = 0;
        tex_win_off_y = 0;
        dither_enabled = 0;
        mask_set_bit = 0;
        mask_check_bit = 0;
        cached_base_test = 0;
        raw_tex_window = 0;
        raw_draw_area_tl = 0;
        raw_draw_area_br = 0;
        raw_draw_offset = 0;

        // Clear VRAM to black (PSX GPU reset clears VRAM)
        {
            GPU_Backend_Flush();
            GPU_Backend_SetMaskBit(0, 0);
            GPU_Backend_ClearVRAM(draw_clip_x1, draw_clip_y1,
                                  draw_clip_x2, draw_clip_y2);

            // Clear shadow VRAM
            if (psx_vram_shadow)
            {
                memset(psx_vram_shadow, 0, PSX_VRAM_WIDTH * PSX_VRAM_HEIGHT * 2);
            }
        }
        break;
    case 0x01: // Reset Command Buffer
        gpu_cmd_remaining = 0;
        gpu_transfer_words = 0;
        polyline_active = 0;
        break;
    case 0x02:                   // Ack IRQ
        gpu_stat &= ~0x01000000; /* Clear GPUSTAT bit 24 (IRQ flag) */
        /* I_STAT bit 1 is NOT cleared here — software must clear it
         * by writing to I_STAT (0x1F801070) separately.
         * Per psx-spx interrupt acknowledge ordering, clearing the
         * source (GPUSTAT.24) re-arms the edge detector so that a
         * subsequent GP0(1Fh) will trigger a new rising edge. */
        break;
    case 0x03: // Display Enable
        if (data & 1)
            gpu_stat |= 0x00800000;
        else
            gpu_stat &= ~0x00800000;
        DLOG("GP1(03) Display Enable: %s (data=%08X, gpu_stat=%08X)\n",
             (data & 1) ? "DISABLED" : "ENABLED", (unsigned)data, (unsigned)gpu_stat);
        break;
    case 0x04: // DMA Direction
        gpu_stat = (gpu_stat & ~0x60000000) | ((data & 3) << 29);
        break;
    case 0x05: // Display Start
    {
        if (data != cache_gp1_05)
        {
            cache_gp1_05 = data;
            uint32_t x = data & 0x3FF;
            uint32_t y = (data >> 10) & 0x1FF;
            display_start_x = x;
            display_start_y = y;
#ifdef TEX_DEBUG_OVERLAY
            printf("[DISP] GP1(05) display_start=(%u,%u)\n", x, y);
#endif

            GPU_Backend_SetDisplayFB(x, y);
        }
    }
    break;
    case 0x06: // Horizontal Display Range
    {
        if (data != cache_gp1_06)
        {
            cache_gp1_06 = data;
            GPU_Backend_UpdateDisplay();
        }
    }
    break;
    case 0x07: // Vertical Display Range
    {
        if (data != cache_gp1_07)
        {
            cache_gp1_07 = data;
            disp_range_y1 = data & 0x3FF;
            disp_range_y2 = (data >> 10) & 0x3FF;
            GPU_Backend_UpdateDisplay();
        }
    }
    break;
    case 0x08: // Display Mode
    {
        gpu_stat = (gpu_stat & ~0x007F4000) |
                   ((data & 0x3F) << 17) | ((data & 0x40) << 10);

        uint32_t mode_bits = data & 0x7F;
        if (mode_bits != cache_gp1_08)
        {
            gpu_stat = (gpu_stat & ~0x007F4000) |
                       ((data & 0x3F) << 17) | ((data & 0x40) << 10);

            cache_gp1_08 = mode_bits;
            uint32_t pal = (data >> 3) & 1;
            uint32_t interlace = (data >> 5) & 1;
            DLOG("GP1(08) Display Mode CHANGED: %s %s hres=%u vres=%u\n",
                 pal ? "PAL" : "NTSC", interlace ? "Interlaced" : "Progressive",
                 (unsigned)(data & 3), (unsigned)((data >> 2) & 1));

            GPU_Backend_SetResolution(interlace, pal ? 3 : 2);
            GPU_Backend_UpdateDisplay();

            /* Timer0 dotclock divider depends on resolution — refresh cache */
            extern void Timer0_RefreshDividerCache(void);
            Timer0_RefreshDividerCache();
        }
    }
    break;
    case 0x09: // Set VRAM size (v2)
        gp1_allow_2mb = data & 1;
        break;
    case 0x10: // Get GPU Info
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
    {
        uint32_t info_type = data & 0x0F; /* Lower 4 bits select info type */
        switch (info_type)
        {
        case 2: /* Texture window: lower 20 bits, preserve upper 12 */
            gpu_read = (gpu_read & 0xFFF00000) | (raw_tex_window & 0xFFFFF);
            break;
        case 3: /* Draw area top-left: lower 20 bits, preserve upper 12 */
            gpu_read = (gpu_read & 0xFFF00000) | (raw_draw_area_tl & 0xFFFFF);
            break;
        case 4: /* Draw area bottom-right: lower 20 bits, preserve upper 12 */
            gpu_read = (gpu_read & 0xFFF00000) | (raw_draw_area_br & 0xFFFFF);
            break;
        case 5: /* Draw offset: lower 22 bits, preserve upper 10 */
            gpu_read = (gpu_read & 0xFFC00000) | (raw_draw_offset & 0x3FFFFF);
            break;
        case 7: /* GPU version */
            gpu_read = 2;
            break;
        default:
            /* Types 0, 1, 6, 8-F: set GPUREAD to 0 */
            gpu_read = 0;
            break;
        }
    }
    break;
    }
}

void GPU_ProcessDmaBlock(uint32_t *data_ptr, uint32_t word_count)
{
    uint32_t i = 0;

    /* ── Drain any in-progress state from prior calls ── */
    while (i < word_count && (gpu_transfer_words > 0 || gpu_cmd_remaining > 0 || polyline_active))
    {
        GPU_WriteGP0(data_ptr[i]);
        i++;
    }

    /* ── Block-level dispatch: table-driven ── */
    while (i < word_count)
    {
        uint32_t *cmd_ptr = &data_ptr[i];
        uint32_t cmd_word = cmd_ptr[0];
        uint32_t cmd_byte = cmd_word >> 24;
        uint8_t cmd_size = gpu_cmd_size[cmd_byte];

        /* ── Variable-length commands (polylines, LoadImage, StoreImage) ── */
        if (cmd_size == 0)
        {
            if (cmd_byte == 0xA0 && i + 3 <= word_count)
            {
                /* LoadImage: burst upload if entire block fits */
                uint32_t dims = cmd_ptr[2];
                uint32_t w = dims & 0xFFFF;
                uint32_t h = dims >> 16;
                if (w == 0)
                    w = 1024;
                if (h == 0)
                    h = 512;
                uint32_t image_words = (w * h + 1) / 2;
                if (i + 3 + image_words <= word_count)
                {
                    GPU_Backend_UploadRegionFast(cmd_ptr[1], dims, &cmd_ptr[3], image_words);
                    i += 3 + image_words;
                    continue;
                }
            }
            else if (cmd_byte == 0xC0)
            {
                /* StoreImage: feed 3 header words to set up read state */
                uint32_t end = (i + 3 <= word_count) ? i + 3 : word_count;
                while (i < end)
                {
                    GPU_WriteGP0(data_ptr[i]);
                    i++;
                }
                continue;
            }
            /* Polylines / fragmented LoadImage → word-by-word */
            while (i < word_count)
            {
                GPU_WriteGP0(data_ptr[i]);
                i++;
                if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                    break;
            }
            continue;
        }

        /* ── Draw commands: polys, rects, lines, fill-rect (0x02-0x7F) ── */
        if (cmd_byte <= 0x7F)
        {
            int size = GPU_TryFastEmit(cmd_ptr);
            if (size <= 0)
            {
                size = cmd_size;
                if (i + size <= word_count)
                {
                    Translate_GP0_to_GS(cmd_ptr);
                }
                else
                {
                    /* Partial command at end of block → word-at-a-time */
                    while (i < word_count)
                    {
                        GPU_WriteGP0(data_ptr[i]);
                        i++;
                    }
                    continue;
                }
            }
            i += size;
            continue;
        }

        /* ── VRAM-to-VRAM copy (0x80-0x9F): 4 words ── */
        if ((cmd_byte & 0xE0) == 0x80)
        {
            if (i + 4 <= word_count)
            {
                GPU_WriteGP0(cmd_ptr[0]);
                GPU_WriteGP0(cmd_ptr[1]);
                GPU_WriteGP0(cmd_ptr[2]);
                GPU_WriteGP0(cmd_ptr[3]);
                i += 4;
            }
            else
            {
                while (i < word_count)
                {
                    GPU_WriteGP0(data_ptr[i]);
                    i++;
                }
            }
            continue;
        }

        /* ── E1-E6, NOP, IRQ, etc. — single-word via GPU_WriteGP0 ── */
        GPU_WriteGP0(cmd_word);
        i++;
    }
}
