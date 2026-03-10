/**
 * gpu_commands.c — GP0 / GP1 command processing
 *
 * Handles the PSX GPU command FIFO: accumulating multi-word commands,
 * VRAM-to-CPU/CPU-to-VRAM transfers, polyline state machine, rendering
 * environment registers (E1-E6), and GP1 display/status commands.
 */
#include "gpu_state.h"
#include <gif_tags.h>

/* ── Command size lookup ─────────────────────────────────────────── */

int GPU_GetCommandSize(uint32_t cmd)
{
    if ((cmd & 0xE0) == 0x20)
    { // Polygon
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int num_verts = is_quad ? 4 : 3;

        int words = 1;
        words += num_verts;
        if (is_textured)
            words += num_verts;
        if (is_shaded)
            words += (num_verts - 1);
        return words;
    }
    if (cmd == 0x02)
        return 3;

    // Rectangles (0x60-0x7F)
    if ((cmd & 0xE0) == 0x60)
    {
        int is_textured = (cmd & 0x04) != 0;
        int size_mode = (cmd >> 3) & 3;
        int words = 1;
        words += 1;
        if (is_textured)
            words += 1;
        if (size_mode == 0)
            words += 1;
        return words;
    }

    // Lines (0x40-0x5F)
    if ((cmd & 0xE0) == 0x40)
    {
        int is_shaded = (cmd & 0x10) != 0;
        if (is_shaded)
            return 4;
        return 3;
    }

    // VRAM-to-VRAM copy (0x80-0x9F)
    if ((cmd & 0xE0) == 0x80)
        return 4;

    return 1;
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

/* ── GP0 Write ───────────────────────────────────────────────────── */

void GPU_WriteGP0(uint32_t data)
{
    // Deferred flush from VBlank ISR
    if (gpu_pending_vblank_flush)
    {
        Flush_GIF();
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
        static uint32_t pending_words[4];
        static int pending_count = 0;

        {
            uint16_t gs_p0 = (uint16_t)(data & 0xFFFF);
            uint16_t gs_p1 = (uint16_t)(data >> 16);
            /* Branchless STP: bit 15 = opaque for non-zero pixels */
            gs_p0 |= (-(gs_p0 != 0)) & 0x8000;
            gs_p1 |= (-(gs_p1 != 0)) & 0x8000;
            pending_words[pending_count++] = (uint32_t)gs_p0 | ((uint32_t)gs_p1 << 16);
        }

        if (pending_count >= 4)
        {
            uint64_t lo = (uint64_t)pending_words[0] | ((uint64_t)pending_words[1] << 32);
            uint64_t hi = (uint64_t)pending_words[2] | ((uint64_t)pending_words[3] << 32);
            unsigned __int128 q = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
            buf_image[buf_image_ptr++] = q;
            pending_count = 0;

            if (buf_image_ptr >= 1000)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
                for (int i = 0; i < buf_image_ptr; i++)
                {
                    uint64_t *p = (uint64_t *)&buf_image[i];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
        }

        gpu_transfer_words--;
        if (gpu_transfer_words == 0)
        {
            if (pending_count > 0)
            {
                while (pending_count < 4)
                    pending_words[pending_count++] = 0;
                uint64_t lo = (uint64_t)pending_words[0] | ((uint64_t)pending_words[1] << 32);
                uint64_t hi = (uint64_t)pending_words[2] | ((uint64_t)pending_words[3] << 32);
                unsigned __int128 q = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
                buf_image[buf_image_ptr++] = q;
                pending_count = 0;
            }
            if (buf_image_ptr > 0)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
                for (int i = 0; i < buf_image_ptr; i++)
                {
                    uint64_t *p = (uint64_t *)&buf_image[i];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
            // Flush_GIF(); <-- Removed: batching primitives

            // Flush GS texture cache after VRAM upload
            Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
            Push_GIF_Data(GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH); // TEXFLUSH
            Flush_GIF();                                        // Keep this one for VRAM-to-CPU sync

            if (vram_tx_x + vram_tx_w > 1024)
            {
                int wrap_w = (vram_tx_x + vram_tx_w) - 1024;
                Upload_Shadow_VRAM_Region(0, vram_tx_y, wrap_w, vram_tx_h);
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
                Start_VRAM_Transfer(vram_tx_x, vram_tx_y, w, h);
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

                /* ── GS→shadow VRAM readback ─────────────────────────────
                 * The PSX BIOS (and some games) render primitives to offscreen
                 * VRAM regions and then read them back via C0+DMA.  Our GS HW
                 * renderer draws to GS VRAM but never updates psx_vram_shadow
                 * for those primitives.  Fix: read the region back from GS VRAM
                 * into the shadow buffer so GPU_Read() returns correct data. */
                if (psx_vram_shadow && vram_read_w > 0 && vram_read_h > 0)
                {
                    /* Flush any pending GIF packets so GS VRAM is up-to-date */
                    if (fast_gif_ptr != (gif_qword_t *)&gif_packet_buf[current_buffer][0])
                        Flush_GIF();

                    int w_aligned = (vram_read_w + 7) & ~7; /* 8-pixel alignment for GS transfer */
                    int total_pixels = w_aligned * vram_read_h;
                    int buf_bytes = total_pixels * 2; /* 16bpp */
                    int buf_qwc = (buf_bytes + 15) / 16;
                    void *rb_buf = memalign(64, buf_qwc * 16);
                    if (rb_buf)
                    {
                        uint16_t *pixels = GS_ReadbackRegion(vram_read_x, vram_read_y,
                                                             w_aligned, vram_read_h,
                                                             rb_buf, buf_qwc);
                        /* Copy readback data into psx_vram_shadow.
                         * GS CT16S stores STP in bit 15 with different semantics
                         * than PSX: our FillRect writes alpha=0x80 → STP=1 even
                         * for black pixels, but on PSX, FillRect(000000) produces
                         * raw 0x0000.  Strip bit 15 so the shadow matches PSX
                         * behaviour: 0x0000 = transparent, non-zero = visible. */
                        if (pixels)
                        {
                            for (int row = 0; row < vram_read_h; row++)
                            {
                                int dy = vram_read_y + row;
                                if (dy >= 512)
                                    dy -= 512;
                                for (int col = 0; col < vram_read_w; col++)
                                {
                                    int dx = vram_read_x + col;
                                    if (dx >= 1024)
                                        dx -= 1024;
                                    uint16_t px = pixels[row * w_aligned + col] & 0x7FFF;
                                    psx_vram_shadow[dy * 1024 + dx] = px;
                                }
                            }
                        }
                        free(rb_buf);
                    }
                }

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
                    Upload_Shadow_VRAM_Region(dx, dy, w, h);
                }
            }
            else
            {
                /* Fast path for common untextured flat polygons — REGLIST */
                uint32_t cmd_byte = gpu_cmd_buffer[0] >> 24;
                if ((cmd_byte == 0x20 || cmd_byte == 0x28) && gs_state.valid && gs_state.dthe == 0)
                {
                    int is_quad = (cmd_byte == 0x28);
                    uint32_t c = gpu_cmd_buffer[0] & 0xFFFFFF;

                    /* Flat Triangle: 4 words, Flat Quad: 5 words */
                    int num_verts = is_quad ? 4 : 3;
                    uint64_t prim = is_quad ? 4 : 3; // 4=TRISTRIP, 3=TRIANGLE

                    /* G4: PRIM in A+D (EOP=0), vertices in REGLIST */
                    Push_GIF_Tag(GIF_TAG_LO(1, 0, 0, 0, 0, 1), GIF_REG_AD);
                    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim), GS_REG_PRIM);

                    uint64_t regs = (uint64_t)GIF_REG_RGBAQ | ((uint64_t)GIF_REG_XYZ2 << 4);
                    Push_GIF_Tag(GIF_TAG_LO(num_verts, 1, 0, 0, 1, 2), regs);

                    uint64_t rgbaq = GS_SET_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000);

                    for (int v = 0; v < num_verts; v++)
                    {
                        int16_t px = (int16_t)((int32_t)((gpu_cmd_buffer[v + 1] & 0xFFFF) << 21) >> 21);
                        int16_t py = (int16_t)((int32_t)((gpu_cmd_buffer[v + 1] >> 16) << 21) >> 21);
                        Push_GIF_Data(rgbaq, GS_SET_XYZ(((int32_t)px + draw_offset_x + 2048) << 4, ((int32_t)py + draw_offset_y + 2048) << 4, 0));
                    }

                    /* Coordinate decoding for pixel area estimation */
                    int16_t x0 = (int16_t)((int32_t)((gpu_cmd_buffer[1] & 0xFFFF) << 21) >> 21);
                    int16_t y0 = (int16_t)((int32_t)((gpu_cmd_buffer[1] >> 16) << 21) >> 21);
                    int16_t x1 = (int16_t)((int32_t)((gpu_cmd_buffer[2] & 0xFFFF) << 21) >> 21);
                    int16_t y1 = (int16_t)((int32_t)((gpu_cmd_buffer[2] >> 16) << 21) >> 21);
                    int16_t x2 = (int16_t)((int32_t)((gpu_cmd_buffer[3] & 0xFFFF) << 21) >> 21);
                    int16_t y2 = (int16_t)((int32_t)((gpu_cmd_buffer[3] >> 16) << 21) >> 21);
                    int32_t a = (int32_t)x0 * ((int32_t)y1 - y2) + (int32_t)x1 * ((int32_t)y2 - y0) + (int32_t)x2 * ((int32_t)y0 - y1);
                    if (a < 0)
                        a = -a;
                    uint32_t area = (uint32_t)(a >> 1);

                    if (is_quad)
                    {
                        int16_t x3 = (int16_t)((int32_t)((gpu_cmd_buffer[4] & 0xFFFF) << 21) >> 21);
                        int16_t y3 = (int16_t)((int32_t)((gpu_cmd_buffer[4] >> 16) << 21) >> 21);
                        int32_t a2 = (int32_t)x1 * ((int32_t)y3 - y2) + (int32_t)x3 * ((int32_t)y2 - y1) + (int32_t)x2 * ((int32_t)y1 - y3);
                        if (a2 < 0)
                            a2 = -a2;
                        area += (uint32_t)(a2 >> 1);
                    }
                    gpu_estimated_pixels += area;
                }
                else
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
            Prim_InvalidateGSState();
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
            {
                Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
                uint64_t scax0 = draw_clip_x1;
                // PSX E4 boundary is EXCLUSIVE: area is [X1,X2) x [Y1,Y2)
                // GS SCISSOR is inclusive, so use X2-1 and Y2-1
                uint64_t scax1 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
                uint64_t scay0 = draw_clip_y1;
                uint64_t scay1 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
                Push_GIF_Data(GS_SET_SCISSOR(scax0, scax1, scay0, scay1), GS_REG_SCISSOR_1);
            }
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
            {
                Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
                uint64_t scax0 = draw_clip_x1;
                // PSX E4 boundary is EXCLUSIVE: area is [X1,X2) x [Y1,Y2)
                // GS SCISSOR is inclusive, so use X2-1 and Y2-1
                uint64_t scax1 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
                uint64_t scay0 = draw_clip_y1;
                uint64_t scay1 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
                Push_GIF_Data(GS_SET_SCISSOR(scax0, scax1, scay0, scay1), GS_REG_SCISSOR_1);
            }
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
        {
            Push_GIF_Tag(GIF_TAG_LO(2, 1, 0, 0, 0, 1), GIF_REG_AD);
            Push_GIF_Data(GS_SET_FBA(mask_set_bit), GS_REG_FBA_1); // FBA_1 via SDK macro
            Push_GIF_Data(Get_Base_TEST(), GS_REG_TEST_1);         // TEST_1 (now composed via GS_SET_TEST)
        }
        Prim_InvalidateGSState();
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
            // Individual draw commands also batched
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
        Prim_InvalidateGSState();
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

        // Clear GS VRAM to black (PSX GPU reset clears VRAM)
        {
            Flush_GIF();
            Push_GIF_Tag(GIF_TAG_LO(5, 1, 0, 0, 0, 1), GIF_REG_AD);
            // Temporarily set full VRAM scissor
            Push_GIF_Data(GS_SET_SCISSOR(0, PSX_VRAM_WIDTH - 1, 0, PSX_VRAM_HEIGHT - 1), GS_REG_SCISSOR_1);
            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(6), GS_REG_PRIM); // PRIM = SPRITE
            Push_GIF_Data(GS_SET_RGBAQ(0, 0, 0, 0, 0x3F800000), GS_REG_RGBAQ);
            int32_t x1 = (0 + 2048) << 4;
            int32_t y1 = (0 + 2048) << 4;
            int32_t x2 = (PSX_VRAM_WIDTH + 2048) << 4;
            int32_t y2 = (PSX_VRAM_HEIGHT + 2048) << 4;
            Push_GIF_Data(GS_SET_XYZ(x1, y1, 0), GS_REG_XYZ2);
            Push_GIF_Data(GS_SET_XYZ(x2, y2, 0), GS_REG_XYZ2);
            Flush_GIF();

            // Restore scissor to current drawing area (PSX E4 is exclusive)
            Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
            uint64_t sc_x2 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
            uint64_t sc_y2 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
            Push_GIF_Data(GS_SET_SCISSOR(draw_clip_x1, sc_x2, draw_clip_y1, sc_y2), GS_REG_SCISSOR_1);
            Flush_GIF();

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
#ifdef TEX_DEBUG_OVERLAY
            printf("[DISP] GP1(05) display_start=(%u,%u)\n", x, y);
#endif

            uint64_t dispfb = 0;
            dispfb |= (uint64_t)0 << 0;
            dispfb |= (uint64_t)PSX_VRAM_FBW << 9;
            dispfb |= (uint64_t)GS_PSM_16S << 15;
            dispfb |= (uint64_t)x << 32;
            dispfb |= (uint64_t)y << 43;

            *((volatile uint64_t *)0x12000070) = dispfb;
            *((volatile uint64_t *)0x12000090) = dispfb;
        }
    }
    break;
    case 0x06: // Horizontal Display Range
    {
        if (data != cache_gp1_06)
        {
            cache_gp1_06 = data;
            Update_GS_Display();
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
            Update_GS_Display();
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

            SetGsCrt(interlace, pal ? 3 : 2, 0);
            Update_GS_Display();

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

    /* ── Block-level dispatch: process complete commands directly ── */
    while (i < word_count)
    {
        uint32_t *cmd_ptr = &data_ptr[i];
        uint32_t cmd_word = cmd_ptr[0];
        uint32_t cmd_byte = cmd_word >> 24;

        /* ── Draw commands (0x20-0x7F) ── */
        if (cmd_byte >= 0x20 && cmd_byte <= 0x7F)
        {
            /* Polylines: must go word-by-word for the state machine */
            if ((cmd_byte & 0xE8) == 0x48)
            {
                while (i < word_count)
                {
                    GPU_WriteGP0(data_ptr[i]);
                    i++;
                    if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                        break;
                }
                continue;
            }

            /* Fast path for flat polygons when gs_state is valid */
            {
                int fast_size = GPU_TryFastEmit(cmd_ptr);
                if (fast_size > 0)
                {
                    i += fast_size;
                    continue;
                }
            }

            /* General draw command: use Translate_GP0_to_GS directly */
            int size = GPU_GetCommandSize(cmd_byte);
            if (i + size <= word_count)
            {
                Translate_GP0_to_GS(cmd_ptr);
                i += size;
            }
            else
            {
                /* Partial command at end of block — fall back to word-at-a-time */
                while (i < word_count)
                {
                    GPU_WriteGP0(data_ptr[i]);
                    i++;
                }
            }
            continue;
        }

        /* ── Fill rect (0x02) ── */
        if (cmd_byte == 0x02)
        {
            if (i + 3 <= word_count)
            {
                Translate_GP0_to_GS(cmd_ptr);
                i += 3;
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

        /* ── LoadImage (0xA0): burst upload ── */
        if (cmd_byte == 0xA0)
        {
            if (i + 3 <= word_count)
            {
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
                    GS_UploadRegionFast(cmd_ptr[1], dims, &cmd_ptr[3], image_words);
                    i += 3 + image_words;
                    continue;
                }
            }
            /* Incomplete — fall back to word-at-a-time for header + data */
            GPU_WriteGP0(data_ptr[i]);
            i++;
            continue;
        }

        /* ── StoreImage (0xC0): 3-word header ── */
        if (cmd_byte == 0xC0)
        {
            /* Feed all 3 words so GPU_WriteGP0 sets up the read state */
            int end = (i + 3 <= word_count) ? i + 3 : word_count;
            while (i < end)
            {
                GPU_WriteGP0(data_ptr[i]);
                i++;
            }
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

        /* ── Environment commands (E1-E6), NOP (00/01), IRQ (1F) ── */
        /* These are all single-word commands; dispatch via GPU_WriteGP0
         * which has the E-command dedup caching. */
        GPU_WriteGP0(cmd_word);
        i++;
    }
}
