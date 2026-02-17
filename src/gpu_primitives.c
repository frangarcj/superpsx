/**
 * gpu_primitives.c — PSX GP0 primitive → PS2 GS translation
 *
 * Translates PSX polygons, rectangles / sprites, fill-rects and lines
 * into GS GIF packets using A+D mode.  The cursor-based interface
 * allows the DMA chain walker to batch many primitives into a single
 * GIF buffer before flushing.
 */
#include "gpu_state.h"

/* ── Helper: emit a single line segment (A+D mode) ───────────────── */

void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans)
{
    uint64_t prim_reg = 1; // LINE
    if (is_shaded)
        prim_reg |= (1 << 3); // IIP=1 (Gouraud)
    if (is_semi_trans)
        prim_reg |= (1 << 6); // ABE=1

    int nregs = 5; // PRIM + 2*(RGBAQ + XYZ2)
    if (is_semi_trans)
        nregs++;

    Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE); // A+D mode

    if (is_semi_trans)
    {
        Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42); // ALPHA_1
    }

    Push_GIF_Data(prim_reg, 0x00); // PRIM register

    // Vertex 0
    Push_GIF_Data(GS_set_RGBAQ(color0 & 0xFF, (color0 >> 8) & 0xFF,
                               (color0 >> 16) & 0xFF, 0x80, 0x3F800000),
                  0x01);
    int32_t gx0 = ((int32_t)x0 + draw_offset_x + 2048) << 4;
    int32_t gy0 = ((int32_t)y0 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05);

    // Vertex 1
    Push_GIF_Data(GS_set_RGBAQ(color1 & 0xFF, (color1 >> 8) & 0xFF,
                               (color1 >> 16) & 0xFF, 0x80, 0x3F800000),
                  0x01);
    int32_t gx1 = ((int32_t)x1 + draw_offset_x + 2048) << 4;
    int32_t gy1 = ((int32_t)y1 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05);
}

/* ── Main GP0 → GS translator ────────────────────────────────────── */

void Translate_GP0_to_GS(uint32_t *psx_cmd, unsigned __int128 **gif_cursor)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    // Polygon (0x20-0x3F)
    if ((cmd & 0xE0) == 0x20)
    {
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;

        int prim_type = 3; // Triangle
        uint64_t prim_reg = prim_type;
        if (is_shaded)
            prim_reg |= (1 << 3);
        if (is_textured)
        {
            prim_reg |= (1 << 4);
            prim_reg |= (1 << 8);
        }
        if (cmd & 0x02)
            prim_reg |= (1 << 6);

        int num_psx_verts = is_quad ? 4 : 3;
        uint32_t color = cmd_word & 0xFFFFFF;
        int idx = 1;

        struct Vertex
        {
            int16_t x, y;
            uint32_t color;
            uint32_t uv;
        } verts[4];

        int poly_tex_page_x = tex_page_x;
        int poly_tex_page_y = tex_page_y;

        for (int i = 0; i < num_psx_verts; i++)
        {
            if (i == 0)
                verts[i].color = color;
            else if (is_shaded)
                verts[i].color = psx_cmd[idx++] & 0xFFFFFF;
            else
                verts[i].color = color;

            uint32_t xy = psx_cmd[idx++];
            verts[i].x = (int16_t)(xy & 0xFFFF);
            verts[i].y = (int16_t)(xy >> 16);

            if (is_textured)
            {
                verts[i].uv = psx_cmd[idx++];
                if (i == 1)
                {
                    uint32_t tpage = verts[i].uv >> 16;
                    poly_tex_page_x = (tpage & 0xF) * 64;
                    poly_tex_page_y = ((tpage >> 4) & 0x1) * 256;

                    gpu_stat = (gpu_stat & ~0x81FF) | (tpage & 0x1FF);
                    if (gp1_allow_2mb)
                        gpu_stat = (gpu_stat & ~0x8000) | (((tpage >> 11) & 1) << 15);
                    else
                        gpu_stat &= ~0x8000;

                    tex_page_x = poly_tex_page_x;
                    tex_page_y = poly_tex_page_y;
                    tex_page_format = (tpage >> 7) & 3;
                    semi_trans_mode = (tpage >> 5) & 3;
                }
            }
        }

        if (is_quad)
        {
            int use_sprite = 0;
            if (is_textured && !is_shaded)
            {
                int quad_w = verts[1].x - verts[0].x;
                int quad_h = verts[2].y - verts[0].y;
                if (quad_w > 0 && quad_h > 0 &&
                    verts[0].y == verts[1].y && verts[2].y == verts[3].y &&
                    verts[0].x == verts[2].x && verts[1].x == verts[3].x)
                {
                    use_sprite = 1;
                }
            }

            if (use_sprite)
            {
                gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

                uint64_t sprite_prim = 6;
                sprite_prim |= (1 << 4);
                sprite_prim |= (1 << 8);
                if (cmd & 0x02)
                    sprite_prim |= (1 << 6);

                uint32_t u0 = Apply_Tex_Window_U(verts[0].uv & 0xFF) + poly_tex_page_x;
                uint32_t v0 = Apply_Tex_Window_V((verts[0].uv >> 8) & 0xFF) + poly_tex_page_y;
                uint32_t u1 = Apply_Tex_Window_U(verts[3].uv & 0xFF) + 1 + poly_tex_page_x;
                uint32_t v1 = Apply_Tex_Window_V((verts[3].uv >> 8) & 0xFF) + 1 + poly_tex_page_y;

                int32_t gx0 = ((int32_t)verts[0].x + draw_offset_x + 2048) << 4;
                int32_t gy0 = ((int32_t)verts[0].y + draw_offset_y + 2048) << 4;
                int32_t gx1 = ((int32_t)(verts[3].x + 1) + draw_offset_x + 2048) << 4;
                int32_t gy1 = ((int32_t)(verts[3].y + 1) + draw_offset_y + 2048) << 4;

                uint32_t c = verts[0].color;
                uint64_t rgbaq = GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000);

                Push_GIF_Tag(7, 1, 0, 0, 0, 1, 0xE);
                Push_GIF_Data(sprite_prim, 0x00);
                Push_GIF_Data(GS_set_XYZ(u0 << 4, v0 << 4, 0), 0x03);
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05);
                Push_GIF_Data(GS_set_XYZ(u1 << 4, v1 << 4, 0), 0x03);
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05);

                *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
            }
            else
            {
                int tris[2][3] = {{0, 1, 2}, {1, 3, 2}};
                gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

                for (int t = 0; t < 2; t++)
                {
                    int ndata = is_textured ? 10 : 7;
                    Push_GIF_Tag(ndata, (t == 1) ? 1 : 0, 0, 0, 0, 1, 0xE);
                    Push_GIF_Data(prim_reg, 0x00);

                    for (int v = 0; v < 3; v++)
                    {
                        int i = tris[t][v];
                        if (is_textured)
                        {
                            uint32_t u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                            uint32_t v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                            Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03);
                        }
                        uint32_t c = verts[i].color;
                        Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                        int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                        int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                        Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
                    }
                }
                *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
            }
        }
        else
        {
            gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

            int ndata = is_textured ? 10 : 7;
            Push_GIF_Tag(ndata, 1, 0, 0, 0, 1, 0xE);
            Push_GIF_Data(prim_reg, 0x00);

            for (int i = 0; i < 3; i++)
            {
                if (is_textured)
                {
                    uint32_t u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                    uint32_t v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                    Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03);
                }
                uint32_t c = verts[i].color;
                Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
            }
            *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];

            if (gpu_debug_log)
            {
                fprintf(gpu_debug_log, "[GPU] Triangle A+D: PRIM=%llu\n", (unsigned long long)prim_reg);
                fflush(gpu_debug_log);
            }
        }

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Draw Poly: Cmd=%02" PRIX32 " Shaded=%d Quad=%d Verts=%d Color=%06" PRIX32 " Offset=(%d,%d)\n",
                    cmd, is_shaded, is_quad, num_psx_verts, color, draw_offset_x, draw_offset_y);
            for (int i = 0; i < num_psx_verts; i++)
            {
                fprintf(gpu_debug_log, "\tV%d: (%d, %d) Col=%06" PRIX32 " UV=%04" PRIX32 "\n", i, verts[i].x, verts[i].y, verts[i].color, verts[i].uv);
            }
            fflush(gpu_debug_log);
        }
    }
    else if ((cmd & 0xE0) == 0x60)
    { // Rectangle (Sprite) - use GS SPRITE primitive for reliable rendering
        int is_textured = (cmd & 0x04) != 0;
        int is_var_size = (cmd & 0x18) == 0x00;
        int size_mode = (cmd >> 3) & 3;

        uint32_t color = cmd_word & 0xFFFFFF;
        int idx = 1;

        int16_t x, y;
        uint32_t xy = psx_cmd[idx++];
        x = (int16_t)(xy & 0xFFFF);
        y = (int16_t)(xy >> 16);

        uint32_t uv_clut = 0;
        if (is_textured)
            uv_clut = psx_cmd[idx++];

        int w, h;
        if (is_var_size)
        {
            uint32_t wh = psx_cmd[idx++];
            w = wh & 0x3FF;
            h = (wh >> 16) & 0x1FF;
        }
        else
        {
            if (size_mode == 1)
            {
                w = 1;
                h = 1;
            }
            else if (size_mode == 2)
            {
                w = 8;
                h = 8;
            }
            else
            {
                w = 16;
                h = 16;
            }
        }

        uint64_t prim_reg = 6; // SPRITE
        if (is_textured)
        {
            prim_reg |= (1 << 4);
            prim_reg |= (1 << 8);
        }
        if (cmd & 0x02)
            prim_reg |= (1 << 6);

        if (is_textured)
        {
            gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

            if (gpu_debug_log)
            {
                fprintf(gpu_debug_log, "[GPU] TexRect: Cmd=%02" PRIX32 " (%d,%d) %dx%d UV_CLUT=%08" PRIX32 " TexPage(%d,%d) fmt=%d flipXY=(%d,%d)\n",
                        cmd, x, y, w, h, uv_clut, tex_page_x, tex_page_y, tex_page_format, tex_flip_x, tex_flip_y);
                fflush(gpu_debug_log);
            }

            uint32_t u0_raw = Apply_Tex_Window_U(uv_clut & 0xFF);
            uint32_t v0_raw = Apply_Tex_Window_V((uv_clut >> 8) & 0xFF);
            uint32_t v1_raw = v0_raw + h;

            // --- CLUT texture decode for 4-bit/8-bit modes ---
            int clut_decoded = 0;
            if (tex_page_format == 0 || tex_page_format == 1)
            {
                int clut_x = ((uv_clut >> 16) & 0x3F) * 16;
                int clut_y = (uv_clut >> 22) & 0x1FF;

                if (tex_page_format == 0)
                    clut_decoded = Decode_CLUT4_Texture(clut_x, clut_y,
                                                        tex_page_x, tex_page_y,
                                                        u0_raw, v0_raw, w, h);
                else
                    clut_decoded = Decode_CLUT8_Texture(clut_x, clut_y,
                                                        tex_page_x, tex_page_y,
                                                        u0_raw, v0_raw, w, h);
            }

            uint32_t v0_gs, v1_gs;
            if (clut_decoded)
            {
                v0_gs = CLUT_DECODED_Y;
                v1_gs = CLUT_DECODED_Y + h;
            }
            else
            {
                v0_gs = v0_raw + tex_page_y;
                v1_gs = v1_raw + tex_page_y;
            }
            if (tex_flip_y)
            {
                uint32_t tmp = v0_gs;
                v0_gs = v1_gs;
                v1_gs = tmp;
            }

            int32_t gy0 = ((int32_t)y + draw_offset_y + 2048) << 4;
            int32_t gy1 = ((int32_t)(y + h) + draw_offset_y + 2048) << 4;

            uint64_t rgbaq = GS_set_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

            int is_raw_texture = (cmd & 0x01) != 0;
            int is_semi_trans = (cmd & 0x02) != 0;

            // --- X-flip SPRITE splitting ---
            int num_sprites;
            int32_t sp_gx0[2], sp_gx1[2];
            uint32_t sp_u0[2], sp_u1[2];
            int use_stq_wrap = 0;
            float stq_s0, stq_s1, stq_t0, stq_t1;

            if (tex_flip_x && w > 1)
            {
                if (clut_decoded)
                {
                    num_sprites = 1;
                    sp_gx0[0] = ((int32_t)x + draw_offset_x + 2048) << 4;
                    sp_gx1[0] = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
                    sp_u0[0] = w;
                    sp_u1[0] = 0;
                }
                else
                {
                    int u_start = (int)u0_raw;
                    int u_eff = (u_start + 1) & 0xFF;
                    int k_at_zero = u_eff;
                    int n_a = (k_at_zero + 1 < (int)w) ? (k_at_zero + 1) : (int)w;
                    int n_b = (int)w - n_a;

                    num_sprites = (n_b > 0) ? 2 : 1;

                    sp_gx0[0] = ((int32_t)x + draw_offset_x + 2048) << 4;
                    sp_gx1[0] = ((int32_t)(x + n_a) + draw_offset_x + 2048) << 4;
                    sp_u0[0] = u_eff + tex_page_x;
                    sp_u1[0] = (n_a == 1) ? (sp_u0[0] + 1) : (sp_u0[0] - n_a);

                    if (n_b > 0)
                    {
                        sp_gx0[1] = sp_gx1[0];
                        sp_gx1[1] = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
                        sp_u0[1] = 255 + tex_page_x;
                        sp_u1[1] = sp_u0[1] - n_b;
                    }
                }
            }
            else
            {
                num_sprites = 1;
                sp_gx0[0] = ((int32_t)x + draw_offset_x + 2048) << 4;
                sp_gx1[0] = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
                sp_u0[0] = u0_raw + tex_page_x;
                sp_u1[0] = u0_raw + w + tex_page_x;

                if (clut_decoded)
                {
                    sp_u0[0] = 0;
                    sp_u1[0] = w;
                }

                if (!clut_decoded && sp_u1[0] > 1024)
                {
                    use_stq_wrap = 1;
                    num_sprites = 1;
                    stq_s0 = (float)sp_u0[0] / 1024.0f;
                    stq_s1 = (float)sp_u1[0] / 1024.0f;
                    stq_t0 = (float)v0_gs / 512.0f;
                    stq_t1 = (float)v1_gs / 512.0f;
                }
            }

            int nregs = 1 + num_sprites * 6;
            nregs += 2;
            if (clut_decoded || is_raw_texture)
                nregs += 4;
            if (clut_decoded)
                nregs += 2;
            if (is_semi_trans)
                nregs += 1;

            if (use_stq_wrap)
                prim_reg &= ~(1 << 8);

            Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE);

            Push_GIF_Data(0, 0x45); // DTHE = 0

            if (is_semi_trans)
                Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);

            if (clut_decoded || is_raw_texture)
            {
                uint64_t tex0_before = 0;
                tex0_before |= (uint64_t)PSX_VRAM_FBW << 14;
                tex0_before |= (uint64_t)GS_PSM_16S << 20;
                tex0_before |= (uint64_t)10 << 26;
                tex0_before |= (uint64_t)(clut_decoded ? 10 : 9) << 30;
                tex0_before |= (uint64_t)1 << 34;
                tex0_before |= (uint64_t)(is_raw_texture ? 1 : 0) << 35;
                Push_GIF_Data(tex0_before, 0x06);
                Push_GIF_Data(0, 0x3F);
            }

            if (clut_decoded)
            {
                uint64_t test_at = (uint64_t)1;
                test_at |= (uint64_t)6 << 1;
                Push_GIF_Data(test_at, 0x47);
            }

            Push_GIF_Data(prim_reg, 0x00);

            for (int si = 0; si < num_sprites; si++)
            {
                if (use_stq_wrap)
                {
                    uint32_t s0_bits, t0_bits, s1_bits, t1_bits;
                    memcpy(&s0_bits, &stq_s0, 4);
                    memcpy(&t0_bits, &stq_t0, 4);
                    memcpy(&s1_bits, &stq_s1, 4);
                    memcpy(&t1_bits, &stq_t1, 4);

                    Push_GIF_Data((uint64_t)s0_bits | ((uint64_t)t0_bits << 32), 0x02);
                    Push_GIF_Data(rgbaq, 0x01);
                    Push_GIF_Data(GS_set_XYZ(sp_gx0[si], gy0, 0), 0x05);

                    Push_GIF_Data((uint64_t)s1_bits | ((uint64_t)t1_bits << 32), 0x02);
                    Push_GIF_Data(rgbaq, 0x01);
                    Push_GIF_Data(GS_set_XYZ(sp_gx1[si], gy1, 0), 0x05);
                }
                else
                {
                    Push_GIF_Data(GS_set_XYZ(sp_u0[si] << 4, v0_gs << 4, 0), 0x03);
                    Push_GIF_Data(rgbaq, 0x01);
                    Push_GIF_Data(GS_set_XYZ(sp_gx0[si], gy0, 0), 0x05);

                    Push_GIF_Data(GS_set_XYZ(sp_u1[si] << 4, v1_gs << 4, 0), 0x03);
                    Push_GIF_Data(rgbaq, 0x01);
                    Push_GIF_Data(GS_set_XYZ(sp_gx1[si], gy1, 0), 0x05);
                }
            }

            if (clut_decoded)
                Push_GIF_Data(0, 0x47);

            if (clut_decoded || is_raw_texture)
            {
                uint64_t tex0_mod = 0;
                tex0_mod |= (uint64_t)PSX_VRAM_FBW << 14;
                tex0_mod |= (uint64_t)GS_PSM_16S << 20;
                tex0_mod |= (uint64_t)10 << 26;
                tex0_mod |= (uint64_t)9 << 30;
                tex0_mod |= (uint64_t)1 << 34;
                tex0_mod |= (uint64_t)0 << 35;
                Push_GIF_Data(tex0_mod, 0x06);
                Push_GIF_Data(0, 0x3F);
            }

            Push_GIF_Data((uint64_t)dither_enabled, 0x45);

            *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
        }
        else
        {
            // Flat sprite using A+D mode
            gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

            int32_t gx0 = ((int32_t)x + draw_offset_x + 2048) << 4;
            int32_t gy0 = ((int32_t)y + draw_offset_y + 2048) << 4;
            int32_t gx1 = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
            int32_t gy1 = ((int32_t)(y + h) + draw_offset_y + 2048) << 4;

            uint64_t rgbaq = GS_set_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

            int is_semi_trans = (cmd & 0x02) != 0;
            int nregs = 5;
            nregs += 2;
            if (is_semi_trans)
                nregs += 1;

            Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE);

            Push_GIF_Data(0, 0x45);

            if (is_semi_trans)
                Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);

            Push_GIF_Data(prim_reg, 0x00);

            Push_GIF_Data(rgbaq, 0x01);
            Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05);

            Push_GIF_Data(rgbaq, 0x01);
            Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05);

            Push_GIF_Data((uint64_t)dither_enabled, 0x45);

            *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
        }
    }
    else if (cmd == 0x02)
    { // FillRect
        uint32_t color = cmd_word & 0xFFFFFF;
        uint32_t xy = psx_cmd[1];
        uint32_t wh = psx_cmd[2];
        int x = (xy & 0xFFFF) & 0x3F0;
        int y = (xy >> 16) & 0x1FF;
        int w = ((wh & 0xFFFF) & 0x3FF) + 0xF;
        w &= ~0xF;
        if (w == 0)
            w = 0x400;
        int h = (wh >> 16) & 0x1FF;
        if (h == 0)
            h = 0x200;

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Fill Rect: (%d,%d %dx%d) Color=%06" PRIX32 "\n", x, y, w, h, color);
            fflush(gpu_debug_log);
        }

        Push_GIF_Tag(5, 1, 0, 0, 0, 1, 0xE);
        uint64_t full_scissor = 0 | ((uint64_t)(PSX_VRAM_WIDTH - 1) << 16) | ((uint64_t)0 << 32) | ((uint64_t)(PSX_VRAM_HEIGHT - 1) << 48);
        Push_GIF_Data(full_scissor, 0x40);
        Push_GIF_Data(6, 0x00);
        uint32_t r = color & 0xFF;
        uint32_t g = (color >> 8) & 0xFF;
        uint32_t b = (color >> 16) & 0xFF;
        Push_GIF_Data(GS_set_RGBAQ(r, g, b, 0x80, 0x3F800000), 0x01);
        int32_t x1 = (x + 2048) << 4;
        int32_t y1 = (y + 2048) << 4;
        int32_t x2 = (x + w + 2048) << 4;
        int32_t y2 = (y + h + 2048) << 4;
        Push_GIF_Data(GS_set_XYZ(x1, y1, 0), 0x05);
        Push_GIF_Data(GS_set_XYZ(x2, y2, 0), 0x05);
        Flush_GIF();

        // Restore original scissor
        Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
        uint64_t orig_scissor = (uint64_t)draw_clip_x1 | ((uint64_t)draw_clip_x2 << 16) | ((uint64_t)draw_clip_y1 << 32) | ((uint64_t)draw_clip_y2 << 48);
        Push_GIF_Data(orig_scissor, 0x40);
        Flush_GIF();

        // Update shadow VRAM for filled area
        if (psx_vram_shadow)
        {
            uint16_t psx_color = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10);
            for (int row = y; row < y + h && row < 512; row++)
            {
                for (int col = x; col < x + w && col < 1024; col++)
                {
                    psx_vram_shadow[row * 1024 + col] = psx_color;
                }
            }
        }

        *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
    }
    else if ((cmd & 0xE0) == 0x40)
    { // Line
        int is_shaded = (cmd & 0x10) != 0;
        int is_semi_trans = (cmd & 0x02) != 0;

        uint32_t color0 = cmd_word & 0xFFFFFF;
        int idx = 1;

        uint32_t xy0 = psx_cmd[idx++];
        int16_t x0 = (int16_t)(xy0 & 0xFFFF);
        int16_t y0 = (int16_t)(xy0 >> 16);

        uint32_t color1 = color0;
        if (is_shaded)
            color1 = psx_cmd[idx++] & 0xFFFFFF;
        uint32_t xy1 = psx_cmd[idx++];
        int16_t x1 = (int16_t)(xy1 & 0xFFFF);
        int16_t y1 = (int16_t)(xy1 >> 16);

        gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

        Emit_Line_Segment_AD(x0, y0, color0, x1, y1, color1, is_shaded, is_semi_trans);

        *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Draw Line: (%d,%d)-(%d,%d) Color=%06" PRIX32 "->%06" PRIX32 "\n",
                    x0, y0, x1, y1, color0, color1);
            fflush(gpu_debug_log);
        }
    }
}
