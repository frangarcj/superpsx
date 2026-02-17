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
    // PSX Bresenham always walks from the vertex with lower Y (then lower X
    // if equal), and does NOT draw the last pixel.  GS LINE also excludes its
    // second vertex.  Reorder so that GS V0 = PSX start, GS V1 = PSX end.
    if (y0 > y1 || (y0 == y1 && x0 > x1))
    {
        int16_t tx = x0;
        x0 = x1;
        x1 = tx;
        int16_t ty = y0;
        y0 = y1;
        y1 = ty;
        uint32_t tc = color0;
        color0 = color1;
        color1 = tc;
    }

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

    // Vertex 0 (lower Y / lower X = PSX start)
    Push_GIF_Data(GS_set_RGBAQ(color0 & 0xFF, (color0 >> 8) & 0xFF,
                               (color0 >> 16) & 0xFF, 0x80, 0x3F800000),
                  0x01);
    int32_t gx0 = ((int32_t)x0 + draw_offset_x + 2048) << 4;
    int32_t gy0 = ((int32_t)y0 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05);

    // Vertex 1 (higher Y / higher X = PSX end, not drawn)
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
            // Always use two-triangle + Decode_TexWindow_Rect path for textured quads.
            // The sprite shortcut bypasses CLUT decode (broken for 4BPP/8BPP) and has
            // rasterization edge-pixel differences vs the reference even for 15BPP.
            int use_sprite = 0;

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

                int is_semi_trans_sprite = (cmd & 0x02) != 0;
                int nregs_sprite = 7;
                if (is_semi_trans_sprite)
                    nregs_sprite += 1;
                Push_GIF_Tag(nregs_sprite, 1, 0, 0, 0, 1, 0xE);
                if (is_semi_trans_sprite)
                    Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);
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

                int is_semi_trans = (cmd & 0x02) != 0;
                // PSX dithering applies to shaded and textured-blending (not raw) polygons
                int is_raw_tex = is_textured && (cmd & 0x01);
                int use_dither = dither_enabled && (is_shaded || (is_textured && !is_raw_tex));

                // CLUT decode for textured polygons (4BPP/8BPP)
                int poly_clut_decoded = 0;
                int poly_uv_off_u = 0, poly_uv_off_v = 0;
                if (is_textured)
                {
                    // Find UV bounding box across all 4 vertices
                    int u_min = 255, u_max = 0, v_min = 255, v_max = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        int u_tw = verts[i].uv & 0xFF;
                        int v_tw = (verts[i].uv >> 8) & 0xFF;
                        if (u_tw < u_min)
                            u_min = u_tw;
                        if (u_tw > u_max)
                            u_max = u_tw;
                        if (v_tw < v_min)
                            v_min = v_tw;
                        if (v_tw > v_max)
                            v_max = v_tw;
                    }
                    int dec_w = u_max - u_min + 1;
                    int dec_h = v_max - v_min + 1;
                    int clut_x = ((verts[0].uv >> 16) & 0x3F) * 16;
                    int clut_y = (verts[0].uv >> 22) & 0x1FF;

                    int ok = Decode_TexWindow_Rect(tex_page_format,
                                                   poly_tex_page_x, poly_tex_page_y,
                                                   clut_x, clut_y,
                                                   u_min, v_min, dec_w, dec_h,
                                                   0, 0);
                    if (ok)
                    {
                        poly_clut_decoded = 1;
                        poly_uv_off_u = u_min;
                        poly_uv_off_v = v_min;
                    }
                }

                for (int t = 0; t < 2; t++)
                {
                    int ndata = is_textured ? 10 : 7;
                    if (t == 0)
                    {
                        ndata += 1; // DTHE register
                        if (is_semi_trans)
                            ndata += 1; // ALPHA_1
                        if (poly_clut_decoded)
                            ndata += 2; // TEX0 + TEXFLUSH before
                        if (is_textured)
                            ndata += 1; // TEST (alpha test enable)
                    }
                    if (t == 1 && poly_clut_decoded)
                        ndata += 2; // TEX0 + TEXFLUSH restore
                    if (t == 1 && is_textured)
                        ndata += 1; // TEST restore
                    Push_GIF_Tag(ndata, (t == 1) ? 1 : 0, 0, 0, 0, 1, 0xE);

                    if (t == 0)
                    {
                        Push_GIF_Data((uint64_t)use_dither, 0x45); // DTHE
                        if (is_semi_trans)
                            Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);
                        if (poly_clut_decoded)
                        {
                            // Switch TEX0 to decoded CLUT area (TW=10, TH=10 for room)
                            uint64_t tex0_c = 0;
                            tex0_c |= (uint64_t)PSX_VRAM_FBW << 14;
                            tex0_c |= (uint64_t)GS_PSM_16S << 20;
                            tex0_c |= (uint64_t)10 << 26;
                            tex0_c |= (uint64_t)10 << 30;
                            tex0_c |= (uint64_t)1 << 34;
                            tex0_c |= (uint64_t)(is_raw_tex ? 1 : 0) << 35;
                            Push_GIF_Data(tex0_c, 0x06);
                            Push_GIF_Data(0, 0x3F); // TEXFLUSH
                        }
                        // Alpha test: skip transparent pixels (STP=0 → alpha=0)
                        if (is_textured)
                        {
                            uint64_t test_at = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                            Push_GIF_Data(test_at, 0x47);
                        }
                    }

                    Push_GIF_Data(prim_reg, 0x00);

                    for (int v = 0; v < 3; v++)
                    {
                        int i = tris[t][v];
                        if (is_textured)
                        {
                            uint32_t u, v_coord;
                            if (poly_clut_decoded)
                            {
                                u = (verts[i].uv & 0xFF) - poly_uv_off_u;
                                v_coord = ((verts[i].uv >> 8) & 0xFF) - poly_uv_off_v + CLUT_DECODED_Y;
                            }
                            else
                            {
                                u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                                v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                            }
                            Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03);
                        }
                        uint32_t c = verts[i].color;
                        Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                        int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                        int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                        Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
                    }
                    // Restore TEX0 after second triangle
                    if (t == 1 && poly_clut_decoded)
                    {
                        uint64_t tex0_r = 0;
                        tex0_r |= (uint64_t)PSX_VRAM_FBW << 14;
                        tex0_r |= (uint64_t)GS_PSM_16S << 20;
                        tex0_r |= (uint64_t)10 << 26;
                        tex0_r |= (uint64_t)9 << 30;
                        tex0_r |= (uint64_t)1 << 34;
                        Push_GIF_Data(tex0_r, 0x06);
                        Push_GIF_Data(0, 0x3F);
                    }
                    // Restore alpha test after second triangle
                    if (t == 1 && is_textured)
                    {
                        Push_GIF_Data(Get_Base_TEST(), 0x47);
                    }
                }
                *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
            }
        }
        else
        {
            gif_packet_ptr = *gif_cursor - gif_packet_buf[current_buffer];

            int is_semi_trans_tri = (cmd & 0x02) != 0;
            int is_raw_tex_tri = is_textured && (cmd & 0x01);
            int use_dither_tri = dither_enabled && (is_shaded || (is_textured && !is_raw_tex_tri));

            // CLUT decode for textured triangle
            int tri_clut_decoded = 0;
            int tri_uv_off_u = 0, tri_uv_off_v = 0;
            if (is_textured)
            {
                int u_min = 255, u_max = 0, v_min = 255, v_max = 0;
                for (int i = 0; i < 3; i++)
                {
                    int u_tw = verts[i].uv & 0xFF;
                    int v_tw = (verts[i].uv >> 8) & 0xFF;
                    if (u_tw < u_min)
                        u_min = u_tw;
                    if (u_tw > u_max)
                        u_max = u_tw;
                    if (v_tw < v_min)
                        v_min = v_tw;
                    if (v_tw > v_max)
                        v_max = v_tw;
                }
                int dec_w = u_max - u_min + 1;
                int dec_h = v_max - v_min + 1;
                int clut_x = ((verts[0].uv >> 16) & 0x3F) * 16;
                int clut_y = (verts[0].uv >> 22) & 0x1FF;
                int ok = Decode_TexWindow_Rect(tex_page_format,
                                               poly_tex_page_x, poly_tex_page_y,
                                               clut_x, clut_y,
                                               u_min, v_min, dec_w, dec_h,
                                               0, 0);
                if (ok)
                {
                    tri_clut_decoded = 1;
                    tri_uv_off_u = u_min;
                    tri_uv_off_v = v_min;
                }
            }

            int ndata = is_textured ? 10 : 7;
            ndata += 1; // DTHE register
            if (is_semi_trans_tri)
                ndata += 1;
            if (tri_clut_decoded)
                ndata += 4; // TEX0+TEXFLUSH before + TEX0+TEXFLUSH after
            if (is_textured)
                ndata += 2; // TEST enable + TEST restore
            Push_GIF_Tag(ndata, 1, 0, 0, 0, 1, 0xE);

            Push_GIF_Data((uint64_t)use_dither_tri, 0x45); // DTHE
            if (is_semi_trans_tri)
                Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);

            if (tri_clut_decoded)
            {
                uint64_t tex0_c = 0;
                tex0_c |= (uint64_t)PSX_VRAM_FBW << 14;
                tex0_c |= (uint64_t)GS_PSM_16S << 20;
                tex0_c |= (uint64_t)10 << 26;
                tex0_c |= (uint64_t)10 << 30;
                tex0_c |= (uint64_t)1 << 34;
                tex0_c |= (uint64_t)(is_raw_tex_tri ? 1 : 0) << 35;
                Push_GIF_Data(tex0_c, 0x06);
                Push_GIF_Data(0, 0x3F);
            }

            // Alpha test: skip transparent pixels (STP=0 → alpha=0)
            if (is_textured)
            {
                uint64_t test_at = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                Push_GIF_Data(test_at, 0x47);
            }

            Push_GIF_Data(prim_reg, 0x00);

            for (int i = 0; i < 3; i++)
            {
                if (is_textured)
                {
                    uint32_t u, v_coord;
                    if (tri_clut_decoded)
                    {
                        u = (verts[i].uv & 0xFF) - tri_uv_off_u;
                        v_coord = ((verts[i].uv >> 8) & 0xFF) - tri_uv_off_v + CLUT_DECODED_Y;
                    }
                    else
                    {
                        u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                        v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                    }
                    Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03);
                }
                uint32_t c = verts[i].color;
                Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
            }
            if (tri_clut_decoded)
            {
                uint64_t tex0_r = 0;
                tex0_r |= (uint64_t)PSX_VRAM_FBW << 14;
                tex0_r |= (uint64_t)GS_PSM_16S << 20;
                tex0_r |= (uint64_t)10 << 26;
                tex0_r |= (uint64_t)9 << 30;
                tex0_r |= (uint64_t)1 << 34;
                Push_GIF_Data(tex0_r, 0x06);
                Push_GIF_Data(0, 0x3F);
            }
            // Restore alpha test
            if (is_textured)
            {
                Push_GIF_Data(Get_Base_TEST(), 0x47);
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

            uint32_t u0_cmd = uv_clut & 0xFF;
            uint32_t v0_cmd = (uv_clut >> 8) & 0xFF;
            uint32_t u0_raw = Apply_Tex_Window_U(u0_cmd);
            uint32_t v0_raw = Apply_Tex_Window_V(v0_cmd);
            uint32_t v1_raw = v0_raw + h;

            int tex_win_active = (tex_win_mask_x != 0 || tex_win_mask_y != 0);

            // --- Texture decode: CLUT, texture window, or both ---
            int clut_decoded = 0;
            int flip_handled = 0; // set when decode handles flip internally

            // Use per-pixel decode when tex window is active OR CLUT format
            // (psx_vram_shadow reads are more reliable than GS readback for CLUT)
            int need_perpixel = tex_win_active ||
                                (tex_page_format == 0 || tex_page_format == 1);

            if (need_perpixel)
            {
                // Per-pixel decode handles ALL formats + flip + texture window
                int clut_x = ((uv_clut >> 16) & 0x3F) * 16;
                int clut_y = (uv_clut >> 22) & 0x1FF;
                clut_decoded = Decode_TexWindow_Rect(tex_page_format,
                                                     tex_page_x, tex_page_y,
                                                     clut_x, clut_y,
                                                     u0_cmd, v0_cmd, w, h,
                                                     tex_flip_x, tex_flip_y);
                flip_handled = clut_decoded;
            }
            else if (tex_page_format == 0 || tex_page_format == 1)
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
            if (tex_flip_y && !flip_handled)
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

            // Use SPRITE (type 6) for non-flip: precise axis-aligned rasterization
            // Use TRIANGLE_STRIP + STQ for flip: handles reversed/negative UV coords
            int use_flip_path = (tex_flip_x || tex_flip_y) && !clut_decoded;

            int32_t sgx0 = ((int32_t)x + draw_offset_x + 2048) << 4;
            int32_t sgy0 = ((int32_t)y + draw_offset_y + 2048) << 4;
            int32_t sgx1 = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
            int32_t sgy1 = ((int32_t)(y + h) + draw_offset_y + 2048) << 4;

            if (use_flip_path)
            {
                // --- TRIANGLE_STRIP + STQ for flipped textures ---
                uint64_t tri_prim = 4; // TRIANGLE_STRIP
                tri_prim |= (1 << 4);  // TME
                // STQ mode (no FST bit)
                if (cmd & 0x02)
                    tri_prim |= (1 << 6); // ABE

                // Flip UV: PSX reads u0, u0-1, u0-2, ... (decrementing)
                int32_t u_left_i = (int32_t)u0_raw + (int32_t)tex_page_x;
                int32_t u_right_i = ((int32_t)u0_raw - (int32_t)w) + (int32_t)tex_page_x;
                int32_t v_top_i = (int32_t)v0_raw + (int32_t)tex_page_y;
                int32_t v_bottom_i = ((int32_t)v0_raw - (int32_t)h) + (int32_t)tex_page_y;
                if (!tex_flip_y)
                {
                    v_top_i = (int32_t)v0_raw + (int32_t)tex_page_y;
                    v_bottom_i = (int32_t)(v0_raw + h) + (int32_t)tex_page_y;
                }
                if (!tex_flip_x)
                {
                    u_left_i = (int32_t)u0_raw + (int32_t)tex_page_x;
                    u_right_i = (int32_t)(u0_raw + w) + (int32_t)tex_page_x;
                }

                if (gpu_debug_log)
                {
                    fprintf(gpu_debug_log, "[GPU] TexRect FLIP_TRI: gx(%d..%d) gy(%d..%d) UV(%d..%d, %d..%d) flip=(%d,%d)\n",
                            sgx0 >> 4, sgx1 >> 4, sgy0 >> 4, sgy1 >> 4,
                            u_left_i, u_right_i, v_top_i, v_bottom_i, tex_flip_x, tex_flip_y);
                    fflush(gpu_debug_log);
                }

                int nregs_tri = 15 + 2; // base(DTHE+PRIM+4*3vertices+DTHE_restore) + alpha test enable/restore
                if (is_semi_trans)
                    nregs_tri += 1;
                if (is_raw_texture)
                    nregs_tri += 4;

                Push_GIF_Tag(nregs_tri, 1, 0, 0, 0, 1, 0xE);
                Push_GIF_Data(0, 0x45); // DTHE = 0

                if (is_semi_trans)
                    Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42);

                if (is_raw_texture)
                {
                    uint64_t tex0_before = 0;
                    tex0_before |= (uint64_t)PSX_VRAM_FBW << 14;
                    tex0_before |= (uint64_t)GS_PSM_16S << 20;
                    tex0_before |= (uint64_t)10 << 26;
                    tex0_before |= (uint64_t)9 << 30;
                    tex0_before |= (uint64_t)1 << 34;
                    tex0_before |= (uint64_t)1 << 35; // TFX=DECAL
                    Push_GIF_Data(tex0_before, 0x06);
                    Push_GIF_Data(0, 0x3F);
                }

                // Alpha test: skip transparent pixels (STP=0 → alpha=0)
                {
                    uint64_t test_at = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                    Push_GIF_Data(test_at, 0x47);
                }

                Push_GIF_Data(tri_prim, 0x00);

                // STQ float UV normalized by texture size (1024x512)
                float s_left = (float)u_left_i / 1024.0f;
                float s_right = (float)u_right_i / 1024.0f;
                float t_top = (float)v_top_i / 512.0f;
                float t_bottom = (float)v_bottom_i / 512.0f;
                float q = 1.0f;

                uint32_t s_l_bits, s_r_bits, t_t_bits, t_b_bits, q_bits;
                memcpy(&s_l_bits, &s_left, 4);
                memcpy(&s_r_bits, &s_right, 4);
                memcpy(&t_t_bits, &t_top, 4);
                memcpy(&t_b_bits, &t_bottom, 4);
                memcpy(&q_bits, &q, 4);

                uint64_t rgbaq_q = GS_set_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, q_bits);

                // TL, TR, BL, BR
                Push_GIF_Data((uint64_t)s_l_bits | ((uint64_t)t_t_bits << 32), 0x02);
                Push_GIF_Data(rgbaq_q, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx0, sgy0, 0), 0x05);

                Push_GIF_Data((uint64_t)s_r_bits | ((uint64_t)t_t_bits << 32), 0x02);
                Push_GIF_Data(rgbaq_q, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx1, sgy0, 0), 0x05);

                Push_GIF_Data((uint64_t)s_l_bits | ((uint64_t)t_b_bits << 32), 0x02);
                Push_GIF_Data(rgbaq_q, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx0, sgy1, 0), 0x05);

                Push_GIF_Data((uint64_t)s_r_bits | ((uint64_t)t_b_bits << 32), 0x02);
                Push_GIF_Data(rgbaq_q, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx1, sgy1, 0), 0x05);

                // Restore alpha test + TEX0
                Push_GIF_Data(Get_Base_TEST(), 0x47);
                if (is_raw_texture)
                {
                    uint64_t tex0_mod = 0;
                    tex0_mod |= (uint64_t)PSX_VRAM_FBW << 14;
                    tex0_mod |= (uint64_t)GS_PSM_16S << 20;
                    tex0_mod |= (uint64_t)10 << 26;
                    tex0_mod |= (uint64_t)9 << 30;
                    tex0_mod |= (uint64_t)1 << 34;
                    Push_GIF_Data(tex0_mod, 0x06);
                    Push_GIF_Data(0, 0x3F);
                }
                Push_GIF_Data((uint64_t)dither_enabled, 0x45);

                *gif_cursor = &gif_packet_buf[current_buffer][gif_packet_ptr];
            }
            else
            {
                // --- SPRITE path for non-flip rects (precise rasterization) ---
                uint64_t prim_reg = 6; // SPRITE
                prim_reg |= (1 << 4);  // TME
                prim_reg |= (1 << 8);  // FST
                if (cmd & 0x02)
                    prim_reg |= (1 << 6); // ABE

                uint32_t u0_gs, u1_gs;
                if (clut_decoded)
                {
                    u0_gs = 0;
                    u1_gs = w;
                }
                else
                {
                    u0_gs = u0_raw + tex_page_x;
                    u1_gs = u0_raw + w + tex_page_x;
                }

                if (gpu_debug_log)
                {
                    fprintf(gpu_debug_log, "[GPU] TexRect SPRITE: gx(%d..%d) gy(%d..%d) UV(%u..%u, %u..%u) clut_dec=%d\n",
                            sgx0 >> 4, sgx1 >> 4, sgy0 >> 4, sgy1 >> 4,
                            u0_gs, u1_gs, v0_gs, v1_gs, clut_decoded);
                    fflush(gpu_debug_log);
                }

                // nregs: DTHE + PRIM + 2*(UV+RGBAQ+XYZ) + alpha_test_en + alpha_test_restore + DTHE_restore
                int nregs = 1 + 1 + 6 + 2 + 1; // = 11
                if (is_semi_trans)
                    nregs += 1;
                if (clut_decoded || is_raw_texture)
                    nregs += 4;

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

                // Alpha test: skip transparent pixels (STP=0 → alpha=0)
                {
                    uint64_t test_at = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                    Push_GIF_Data(test_at, 0x47);
                }

                Push_GIF_Data(prim_reg, 0x00);

                // SPRITE: TL vertex + BR vertex
                Push_GIF_Data(GS_set_XYZ(u0_gs << 4, v0_gs << 4, 0), 0x03);
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx0, sgy0, 0), 0x05);

                Push_GIF_Data(GS_set_XYZ(u1_gs << 4, v1_gs << 4, 0), 0x03);
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(sgx1, sgy1, 0), 0x05);

                // Restore alpha test
                Push_GIF_Data(Get_Base_TEST(), 0x47);

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

        // Restore original scissor (PSX E4 is exclusive, GS SCISSOR is inclusive)
        Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
        uint64_t sc_x2 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
        uint64_t sc_y2 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
        uint64_t orig_scissor = (uint64_t)draw_clip_x1 | (sc_x2 << 16) | ((uint64_t)draw_clip_y1 << 32) | (sc_y2 << 48);
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
