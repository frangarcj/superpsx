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
static uint32_t cache_e1 = 0xFFFFFFFF;
static uint32_t cache_e3 = 0xFFFFFFFF;
static uint32_t cache_e4 = 0xFFFFFFFF;
static uint32_t cache_e5 = 0xFFFFFFFF;
static uint32_t cache_gp1_05 = 0xFFFFFFFF;
static uint32_t cache_gp1_06 = 0xFFFFFFFF;
static uint32_t cache_gp1_07 = 0xFFFFFFFF;
static uint32_t cache_gp1_08 = 0xFFFFFFFF;

static void clear_gpu_param_cache(void)
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
            Flush_GIF();            // Keep this one for VRAM-to-CPU sync

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
                    vram_gen_counter++;
                    Tex_Cache_DirtyRegion(dx, dy, w, h);
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
                        Flush_GIF_Sync();  /* Must wait: direct GIF channel use follows */

                        unsigned __int128 rb_packet[8] __attribute__((aligned(16)));
                        uint64_t *rp = (uint64_t *)rb_packet;
                        rp[0] = GIF_TAG_LO(4, 1, 0, 0, 0, 1);
                        rp[1] = GIF_REG_AD;
                        rp[2] = GS_SET_BITBLTBUF(0, PSX_VRAM_FBW, GS_PSM_16S, 0, 0, 0); /* Local→Host: source fields only */
                        rp[3] = GS_REG_BITBLTBUF;
                        rp[4] = (uint64_t)ux | ((uint64_t)uy << 16);
                        rp[5] = GS_REG_TRXPOS;
                        rp[6] = GS_SET_TRXREG(uw_aligned, uh);
                        rp[7] = GS_REG_TRXREG;
                        rp[8] = GS_SET_TRXDIR(1);
                        rp[9] = GS_REG_TRXDIR;

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

                        Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
                        Push_GIF_Data(GS_SET_BITBLTBUF(0,0,0, 0, PSX_VRAM_FBW, GS_PSM_16S), GS_REG_BITBLTBUF);
                        Push_GIF_Data(GS_SET_TRXPOS(0,0,dx,dy,0), GS_REG_TRXPOS);
                        Push_GIF_Data(GS_SET_TRXREG(w, h), GS_REG_TRXREG);
                        Push_GIF_Data(GS_SET_TRXDIR(0), GS_REG_TRXDIR);
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
                                            Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
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
                            Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
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
                        Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
                        Push_GIF_Data(GS_SET_BITBLTBUF(0, PSX_VRAM_FBW, GS_PSM_16S, 0, PSX_VRAM_FBW, GS_PSM_16S), GS_REG_BITBLTBUF); /* Local→Local: both src and dst */
                        Push_GIF_Data(GS_SET_TRXPOS(sx, sy, dx, dy, 0), GS_REG_TRXPOS);
                        Push_GIF_Data(GS_SET_TRXREG(w, h), GS_REG_TRXREG);
                        Push_GIF_Data(GS_SET_TRXDIR(2), GS_REG_TRXDIR);
                        Flush_GIF();
                    }
                }
                else
                {
                    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
                    Push_GIF_Data(GS_SET_BITBLTBUF(0, PSX_VRAM_FBW, GS_PSM_16S, 0, PSX_VRAM_FBW, GS_PSM_16S), GS_REG_BITBLTBUF); /* Local→Local: both src and dst */
                    Push_GIF_Data(GS_SET_TRXPOS(sx, sy, dx, dy, 0), GS_REG_TRXPOS);
                    Push_GIF_Data(GS_SET_TRXREG(w, h), GS_REG_TRXREG);
                    Push_GIF_Data(GS_SET_TRXDIR(2), GS_REG_TRXDIR);
                    Flush_GIF();
                }
            }
            else
            {
                Translate_GP0_to_GS(gpu_cmd_buffer);
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

            Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
            /* Use SDK macro for TEX0 packing (small variant covers fields we need) */
            Push_GIF_Data(GS_SET_TEX0_SMALL(0, PSX_VRAM_FBW, GS_PSM_16S, 10, 9, 1, 0), GS_REG_TEX0);
            Push_GIF_Data(GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH);

            uint32_t dither_enable = (data >> 9) & 1;
            dither_enabled = dither_enable;
            Push_GIF_Data(GS_SET_DTHE(dither_enable), GS_REG_DTHE);

            Push_GIF_Data(Get_Alpha_Reg(trans_mode), GS_REG_ALPHA_1);

            Prim_InvalidateGSState();
            // Flush_GIF(); <-- Removed: batching register changes
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
            Push_GIF_Data(Get_Base_TEST(), GS_REG_TEST_1);        // TEST_1 (now composed via GS_SET_TEST)
        }
        Prim_InvalidateGSState();
        break;
    case 0x1F: // GPU IRQ Request (edge-triggered)
        /* Per psx-spx: I_STAT bits are edge-triggered — they get set ONLY
         * when the interrupt source transitions from false to true.
         * GPUSTAT.24 is the source line for I_STAT bit 1 (GPU IRQ).
         * Only fire SignalInterrupt when GPUSTAT.24 was previously 0. */
        if (!(gpu_stat & 0x01000000)) {
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
    case 0x02: // Ack IRQ
        gpu_stat &= ~0x01000000;   /* Clear GPUSTAT bit 24 (IRQ flag) */
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
    case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18:
    case 0x19: case 0x1A: case 0x1B: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F:
    {
        uint32_t info_type = data & 0x0F;  /* Lower 4 bits select info type */
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
    while (i < word_count)
    {
        uint32_t data = data_ptr[i];

        // Vía rápida para ráfagas de imágenes (LoadImage 0xA0)
        // Solo si no estamos ya en medio de otra transferencia y el bloque cabe completo
        if (gpu_transfer_words == 0 && gpu_cmd_remaining == 0 && (data >> 24) == 0xA0)
        {
            if (i + 2 < word_count)
            {
                uint32_t dims = data_ptr[i + 2];
                uint32_t image_words = ((dims & 0xFFFF) * (dims >> 16)) / 2;
                if (i + 3 + image_words <= word_count)
                {
                    uint32_t coords = data_ptr[i + 1];
                    GS_UploadRegionFast(coords, dims, &data_ptr[i + 3], image_words);
                    i += 3 + image_words;
                    continue;
                }
            }
        }

        // Caso normal: procesar palabra por palabra
        GPU_WriteGP0(data);
        i++;
    }
}
