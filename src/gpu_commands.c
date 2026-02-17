/**
 * gpu_commands.c — GP0 / GP1 command processing
 *
 * Handles the PSX GPU command FIFO: accumulating multi-word commands,
 * VRAM-to-CPU/CPU-to-VRAM transfers, polyline state machine, rendering
 * environment registers (E1-E6), and GP1 display/status commands.
 */
#include "gpu_state.h"

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

/* ── GP0 Write ───────────────────────────────────────────────────── */

void GPU_WriteGP0(uint32_t data)
{
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
            // Only 0x0000 is transparent; 0x8000 (black + STP=1) is opaque
            if (gs_p0 != 0)
                gs_p0 |= 0x8000;
            if (gs_p1 != 0)
                gs_p1 |= 0x8000;
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
                Push_GIF_Tag(buf_image_ptr, 0, 0, 0, 2, 0, 0);
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
                Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 2, 0, 0);
                for (int i = 0; i < buf_image_ptr; i++)
                {
                    uint64_t *p = (uint64_t *)&buf_image[i];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
            Flush_GIF();

            // Flush GS texture cache after VRAM upload
            Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
            Push_GIF_Data(0, 0x3F); // TEXFLUSH
            Flush_GIF();

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
            Flush_GIF();
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
            Flush_GIF();

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

                Start_VRAM_Transfer(vram_tx_x, vram_tx_y, w, h);
            }
            else if (cmd == 0xC0)
            {
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

                if (psx_vram_shadow)
                {
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

                uint64_t bitblt = ((uint64_t)PSX_VRAM_FBW << 16) | ((uint64_t)GS_PSM_16S << 24) |
                                  ((uint64_t)PSX_VRAM_FBW << 48) | ((uint64_t)GS_PSM_16S << 56);

                int y_overlap_down = (dy > sy) && (dy < sy + h);

                Flush_GIF();

                if (y_overlap_down)
                {
                    int ux = (sx < dx) ? sx : dx;
                    int uy = (sy < dy) ? sy : dy;
                    int ux2 = ((sx + w) > (dx + w)) ? (sx + w) : (dx + w);
                    int uy2 = ((sy + h) > (dy + h)) ? (sy + h) : (dy + h);
                    int uw = ux2 - ux;
                    int uh = uy2 - uy;

                    if (uw > 1024)
                        uw = 1024;
                    if (uh > 512)
                        uh = 512;

                    int uw_aligned = (uw + 7) & ~7;
                    if (ux + uw_aligned > 1024)
                        uw_aligned = 1024 - ux;

                    int buf_bytes = uw_aligned * uh * 2;
                    int buf_qwc = (buf_bytes + 15) / 16;

                    uint16_t *tbuf = (uint16_t *)memalign(64, buf_qwc * 16);
                    if (tbuf)
                    {
                        Flush_GIF();

                        unsigned __int128 rb_packet[8] __attribute__((aligned(16)));
                        uint64_t *rp = (uint64_t *)rb_packet;
                        rp[0] = 4 | ((uint64_t)1 << 15) | ((uint64_t)0 << 58) | ((uint64_t)1 << 60);
                        rp[1] = 0xE;
                        rp[2] = ((uint64_t)PSX_VRAM_FBW << 16) | ((uint64_t)GS_PSM_16S << 24);
                        rp[3] = 0x50;
                        rp[4] = (uint64_t)ux | ((uint64_t)uy << 16);
                        rp[5] = 0x51;
                        rp[6] = (uint64_t)uw_aligned | ((uint64_t)uh << 32);
                        rp[7] = 0x52;
                        rp[8] = 1;
                        rp[9] = 0x53;

                        dma_channel_send_normal(DMA_CHANNEL_GIF, rb_packet, 5, 0, 0);
                        dma_wait_fast();

                        uint32_t phys = (uint32_t)tbuf & 0x1FFFFFFF;
                        uint32_t rem = buf_qwc;
                        uint32_t addr = phys;
                        while (rem > 0)
                        {
                            uint32_t xfer = (rem > 0xFFFF) ? 0xFFFF : rem;
                            *D1_MADR = addr;
                            *D1_QWC = xfer;
                            *D1_CHCR = 0x100;
                            while (*D1_CHCR & 0x100)
                                ;
                            addr += xfer * 16;
                            rem -= xfer;
                        }

                        uint16_t *uc = (uint16_t *)((uint32_t)tbuf | 0xA0000000);

                        for (int row = 0; row < h; row++)
                        {
                            for (int col = 0; col < w; col++)
                            {
                                int sry = (sy + row) - uy;
                                int srx = (sx + col) - ux;
                                int dry = (dy + row) - uy;
                                int drx = (dx + col) - ux;
                                uc[dry * uw_aligned + drx] = uc[sry * uw_aligned + srx];
                            }
                        }

                        Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
                        Push_GIF_Data(((uint64_t)GS_PSM_16S << 56) | ((uint64_t)PSX_VRAM_FBW << 48), 0x50);
                        Push_GIF_Data(((uint64_t)dy << 48) | ((uint64_t)dx << 32), 0x51);
                        Push_GIF_Data(((uint64_t)h << 32) | (uint64_t)w, 0x52);
                        Push_GIF_Data(0, 0x53);
                        Flush_GIF();

                        buf_image_ptr = 0;
                        uint32_t pend[4];
                        int pc = 0;
                        uint16_t prev_px = 0;
                        int pixel_idx = 0;
                        for (int row = 0; row < h; row++)
                        {
                            int dry = (dy + row) - uy;
                            for (int col = 0; col < w; col++)
                            {
                                int drx = (dx + col) - ux;
                                uint16_t px = uc[dry * uw_aligned + drx];
                                if ((pixel_idx & 1) == 0)
                                    prev_px = px;
                                else
                                {
                                    pend[pc++] = (uint32_t)prev_px | ((uint32_t)px << 16);
                                    if (pc >= 4)
                                    {
                                        uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
                                        uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
                                        buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
                                        pc = 0;
                                        if (buf_image_ptr >= 1000)
                                        {
                                            Push_GIF_Tag(buf_image_ptr, 0, 0, 0, 2, 0, 0);
                                            for (int i = 0; i < buf_image_ptr; i++)
                                            {
                                                uint64_t *pp = (uint64_t *)&buf_image[i];
                                                Push_GIF_Data(pp[0], pp[1]);
                                            }
                                            buf_image_ptr = 0;
                                        }
                                    }
                                }
                                pixel_idx++;
                            }
                        }
                        if (pixel_idx & 1)
                            pend[pc++] = (uint32_t)prev_px;
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
                            Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 2, 0, 0);
                            for (int i = 0; i < buf_image_ptr; i++)
                            {
                                uint64_t *pp = (uint64_t *)&buf_image[i];
                                Push_GIF_Data(pp[0], pp[1]);
                            }
                            buf_image_ptr = 0;
                        }
                        Flush_GIF();

                        free(tbuf);
                    }
                    else
                    {
                        Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
                        Push_GIF_Data(bitblt, 0x50);
                        uint64_t trxpos = (uint64_t)sx | ((uint64_t)sy << 16) | ((uint64_t)dx << 32) | ((uint64_t)dy << 48);
                        Push_GIF_Data(trxpos, 0x51);
                        Push_GIF_Data((uint64_t)w | ((uint64_t)h << 32), 0x52);
                        Push_GIF_Data(2, 0x53);
                        Flush_GIF();
                    }
                }
                else
                {
                    Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
                    Push_GIF_Data(bitblt, 0x50);
                    uint64_t trxpos = (uint64_t)sx | ((uint64_t)sy << 16) | ((uint64_t)dx << 32) | ((uint64_t)dy << 48);
                    Push_GIF_Data(trxpos, 0x51);
                    Push_GIF_Data((uint64_t)w | ((uint64_t)h << 32), 0x52);
                    Push_GIF_Data(2, 0x53);
                    Flush_GIF();
                }
            }
            else
            {
                unsigned __int128 *cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
                Translate_GP0_to_GS(gpu_cmd_buffer, &cursor);
                gif_packet_ptr = cursor - gif_packet_buf[current_buffer];
                Flush_GIF();

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
        static u32 last_e1 = 0xFFFFFFFF;
        if (data != last_e1)
        {
            last_e1 = data;

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

            Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
            uint64_t tex0 = 0;
            tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
            tex0 |= (uint64_t)GS_PSM_16S << 20;
            tex0 |= (uint64_t)10 << 26;
            tex0 |= (uint64_t)9 << 30;
            tex0 |= (uint64_t)1 << 34;
            tex0 |= (uint64_t)0 << 35;

            Push_GIF_Data(tex0, 0x06);
            Push_GIF_Data(0, 0x3F);

            uint32_t dither_enable = (data >> 9) & 1;
            dither_enabled = dither_enable;
            Push_GIF_Data(dither_enable, 0x45);

            {
                uint64_t alpha_reg = Get_Alpha_Reg(trans_mode);
                Push_GIF_Data(alpha_reg, 0x42);
            }

            if (gpu_debug_log)
            {
                fprintf(gpu_debug_log, "[GPU] E1: TexPage(%d,%d) fmt=%" PRIu32 " trans=%" PRIu32 " dither=%" PRIu32 " flipX=%d flipY=%d\n",
                        tex_page_x, tex_page_y, tpf, trans_mode, dither_enable, tex_flip_x, tex_flip_y);
                fflush(gpu_debug_log);
            }
            Flush_GIF();
        }
    }
    break;
    case 0xE3: // Drawing Area Top-Left
    {
        static uint32_t last_e3 = 0xFFFFFFFF;
        if (data != last_e3)
        {
            last_e3 = data;
            draw_clip_x1 = data & 0x3FF;
            draw_clip_y1 = (data >> 10) & 0x3FF;
            {
                Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
                uint64_t scax0 = draw_clip_x1;
                // PSX E4 boundary is EXCLUSIVE: area is [X1,X2) x [Y1,Y2)
                // GS SCISSOR is inclusive, so use X2-1 and Y2-1
                uint64_t scax1 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
                uint64_t scay0 = draw_clip_y1;
                uint64_t scay1 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
                Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40);
            }
        }
    }
    break;
    case 0xE4: // Drawing Area Bottom-Right
    {
        static uint32_t last_e4 = 0xFFFFFFFF;
        if (data != last_e4)
        {
            last_e4 = data;
            draw_clip_x2 = data & 0x3FF;
            draw_clip_y2 = (data >> 10) & 0x3FF;
            {
                Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
                uint64_t scax0 = draw_clip_x1;
                // PSX E4 boundary is EXCLUSIVE: area is [X1,X2) x [Y1,Y2)
                // GS SCISSOR is inclusive, so use X2-1 and Y2-1
                uint64_t scax1 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
                uint64_t scay0 = draw_clip_y1;
                uint64_t scay1 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
                Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40);
            }
        }
    }
    break;
    case 0xE5: // Drawing Offset
    {
        static uint32_t last_e5 = 0xFFFFFFFF;
        if (data != last_e5)
        {
            last_e5 = data;
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
        tex_win_mask_x = data & 0x1F;
        tex_win_mask_y = (data >> 5) & 0x1F;
        tex_win_off_x = (data >> 10) & 0x1F;
        tex_win_off_y = (data >> 15) & 0x1F;
        break;
    case 0xE6: // Mask Bit Setting
        mask_set_bit = data & 1;
        mask_check_bit = (data >> 1) & 1;
        gpu_stat = (gpu_stat & ~0x1800) | (mask_set_bit << 11) | (mask_check_bit << 12);
        // GS: FBA_1 forces bit 15 on all written pixels (mask_set_bit)
        // GS: DATE+DATM in TEST_1 prevents writing to pixels with bit 15 set (mask_check_bit)
        {
            Push_GIF_Tag(2, 1, 0, 0, 0, 1, 0xE);
            Push_GIF_Data((uint64_t)mask_set_bit, 0x4A); // FBA_1
            Push_GIF_Data(Get_Base_TEST(), 0x47);        // TEST_1
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
                DLOG("GP0 draw cmd %02Xh (size=%d) #%d\n", cmd, size, draw_cmd_count);
            }
            gpu_cmd_buffer[0] = data;
            gpu_cmd_ptr = 1;
            gpu_cmd_remaining = size - 1;
        }
        else if (size == 1 && ((cmd & 0xE0) == 0x20 || cmd == 0x02))
        {
            uint32_t buff[1] = {data};
            unsigned __int128 *cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
            Translate_GP0_to_GS(buff, &cursor);
            gif_packet_ptr = cursor - gif_packet_buf[current_buffer];
            if (gif_packet_ptr > GIF_BUFFER_SIZE - 32)
                Flush_GIF();
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
        gpu_stat = 0x14802000;
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

        // Clear GS VRAM to black (PSX GPU reset clears VRAM)
        {
            Flush_GIF();
            Push_GIF_Tag(5, 1, 0, 0, 0, 1, 0xE);
            // Temporarily set full VRAM scissor
            uint64_t full_scissor = 0 | ((uint64_t)(PSX_VRAM_WIDTH - 1) << 16) |
                                    ((uint64_t)0 << 32) | ((uint64_t)(PSX_VRAM_HEIGHT - 1) << 48);
            Push_GIF_Data(full_scissor, 0x40);
            Push_GIF_Data(6, 0x00); // PRIM = SPRITE
            Push_GIF_Data(GS_set_RGBAQ(0, 0, 0, 0, 0x3F800000), 0x01);
            int32_t x1 = (0 + 2048) << 4;
            int32_t y1 = (0 + 2048) << 4;
            int32_t x2 = (PSX_VRAM_WIDTH + 2048) << 4;
            int32_t y2 = (PSX_VRAM_HEIGHT + 2048) << 4;
            Push_GIF_Data(GS_set_XYZ(x1, y1, 0), 0x05);
            Push_GIF_Data(GS_set_XYZ(x2, y2, 0), 0x05);
            Flush_GIF();

            // Restore scissor to current drawing area (PSX E4 is exclusive)
            Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
            uint64_t sc_x2 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
            uint64_t sc_y2 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
            uint64_t orig_scissor = (uint64_t)draw_clip_x1 | (sc_x2 << 16) |
                                    ((uint64_t)draw_clip_y1 << 32) | (sc_y2 << 48);
            Push_GIF_Data(orig_scissor, 0x40);
            Flush_GIF();

            // Clear shadow VRAM
            if (psx_vram_shadow)
                memset(psx_vram_shadow, 0, PSX_VRAM_WIDTH * PSX_VRAM_HEIGHT * 2);
        }
        break;
    case 0x01: // Reset Command Buffer
        gpu_cmd_remaining = 0;
        gpu_transfer_words = 0;
        break;
    case 0x02: // Ack IRQ
        gpu_stat &= ~0x01000000;
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
        static u32 last_gp1_05 = 0xFFFFFFFF;
        if (data != last_gp1_05)
        {
            last_gp1_05 = data;
            uint32_t x = data & 0x3FF;
            uint32_t y = (data >> 10) & 0x1FF;

            uint64_t dispfb = 0;
            dispfb |= (uint64_t)0 << 0;
            dispfb |= (uint64_t)PSX_VRAM_FBW << 9;
            dispfb |= (uint64_t)GS_PSM_16S << 15;
            dispfb |= (uint64_t)x << 32;
            dispfb |= (uint64_t)y << 43;

            *((volatile uint64_t *)0xB2000070) = dispfb;
            *((volatile uint64_t *)0xB2000090) = dispfb;
        }
    }
    break;
    case 0x06: // Horizontal Display Range
    {
        static uint32_t last_h_range = 0xFFFFFFFF;
        if (data != last_h_range)
        {
            last_h_range = data;
            Update_GS_Display();
        }
    }
    break;
    case 0x07: // Vertical Display Range
    {
        static uint32_t last_v_range = 0xFFFFFFFF;
        if (data != last_v_range)
        {
            last_v_range = data;
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

        static uint32_t last_display_mode = 0xFFFFFFFF;
        uint32_t mode_bits = data & 0x7F;
        if (mode_bits != last_display_mode)
        {
            gpu_stat = (gpu_stat & ~0x007F4000) |
                       ((data & 0x3F) << 17) | ((data & 0x40) << 10);

            last_display_mode = mode_bits;
            uint32_t hres = data & 3;
            uint32_t vres = (data >> 2) & 1;
            uint32_t pal = (data >> 3) & 1;
            uint32_t interlace = (data >> 5) & 1;
            int widths[] = {256, 320, 512, 640};
            DLOG("GP1(08) Display Mode CHANGED: %dx%d %s %s\n",
                 widths[hres], vres ? 480 : 240,
                 pal ? "PAL" : "NTSC", interlace ? "Interlaced" : "Progressive");

            SetGsCrt(interlace, pal ? 3 : 2, 0);
            Update_GS_Display();
        }
    }
    break;
    case 0x09: // Set VRAM size (v2)
        gp1_allow_2mb = data & 1;
        break;
    case 0x10: // Get GPU Info
    {
        uint32_t info_type = data & 0x0F;
        switch (info_type)
        {
        case 2:
            gpu_read = 0;
            break;
        case 3:
            gpu_read = (draw_clip_y1 << 10) | draw_clip_x1;
            break;
        case 4:
            gpu_read = (draw_clip_y2 << 10) | draw_clip_x2;
            break;
        case 5:
            gpu_read = ((draw_offset_y & 0x7FF) << 11) | (draw_offset_x & 0x7FF);
            break;
        case 7:
            gpu_read = 2;
            break;
        default:
            gpu_read = 0;
            break;
        }
    }
    break;
    }
}
