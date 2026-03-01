/**
 * gpu_primitives.c — PSX GP0 primitive → PS2 GS translation
 *
 * Translates PSX polygons, rectangles / sprites, fill-rects and lines
 * into GS GIF packets using A+D mode.  The cursor-based interface
 * allows the DMA chain walker to batch many primitives into a single
 * GIF buffer before flushing.
 */
#include "gpu_state.h"

/* ── GPU pixel cost accumulator for cycle-accurate rendering delay ── */
uint64_t gpu_estimated_pixels = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Lazy GS State Tracking
 *
 *  Track the last value written to key GS registers so consecutive
 *  primitives with the same state skip redundant writes.  This
 *  eliminates the per-primitive state-setup + state-restore overhead
 *  that dominates GIF traffic for textured primitives.
 *
 *  Invalidation: gs_state_dirty = 1 on any external state change
 *  (E1/E6 handlers, GPU reset, VRAM upload).
 * ═══════════════════════════════════════════════════════════════════ */
static struct
{
    uint64_t tex0;  /* Last TEX0_1 written */
    uint64_t test;  /* Last TEST_1 written */
    uint64_t alpha; /* Last ALPHA_1 written */
    int dthe;       /* Last DTHE written (0 or 1) */
    int valid;      /* 0 = unknown, 1 = tracked values are current */
} gs_state = {0, 0, 0, -1, 0};

/* Primitive-level Decode_TexPage_Cached result cache.
 * Eliminates ~80% of redundant texture cache lookups for consecutive
 * same-texture primitives (582K → ~100K calls). */
static struct
{
    int valid;
    int tex_format;
    int tex_page_x, tex_page_y;
    int clut_x, clut_y;
    uint32_t vram_gen;
    uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int result; /* 0=fail, 1=SW decode, 2=HW CLUT */
    int out_x, out_y;
    int hw_clut;
    int hw_tbp0, hw_cbp;
} prim_tex_cache = {0};

/* Invalidate GS state tracking (called on E1, E6, GPU reset, etc.) */
void Prim_InvalidateGSState(void)
{
    gs_state.valid = 0;
}

/* Invalidate primitive texture cache (called on VRAM writes) */
void Prim_InvalidateTexCache(void)
{
    prim_tex_cache.valid = 0;
}

/* Try to reuse cached Decode_TexPage_Cached result.
 * Returns 1 if hit (result stored in prim_tex_cache), 0 if miss. */
static inline int prim_tex_cache_lookup(int tex_format, int tex_page_x, int tex_page_y,
                                        int clut_x, int clut_y)
{
    if (prim_tex_cache.valid &&
        prim_tex_cache.tex_format == tex_format &&
        prim_tex_cache.tex_page_x == tex_page_x &&
        prim_tex_cache.tex_page_y == tex_page_y &&
        prim_tex_cache.clut_x == clut_x &&
        prim_tex_cache.clut_y == clut_y &&
        prim_tex_cache.vram_gen == vram_gen_counter &&
        prim_tex_cache.tw_mask_x == tex_win_mask_x &&
        prim_tex_cache.tw_mask_y == tex_win_mask_y &&
        prim_tex_cache.tw_off_x == tex_win_off_x &&
        prim_tex_cache.tw_off_y == tex_win_off_y)
        return 1;
    return 0;
}

/* Call Decode_TexPage_Cached and store result in cache. */
static inline int prim_tex_decode(int tex_format, int tex_page_x, int tex_page_y,
                                  int clut_x, int clut_y,
                                  int *out_x, int *out_y)
{
    int result = Decode_TexPage_Cached(tex_format, tex_page_x, tex_page_y,
                                       clut_x, clut_y, out_x, out_y);
    prim_tex_cache.valid = 1;
    prim_tex_cache.tex_format = tex_format;
    prim_tex_cache.tex_page_x = tex_page_x;
    prim_tex_cache.tex_page_y = tex_page_y;
    prim_tex_cache.clut_x = clut_x;
    prim_tex_cache.clut_y = clut_y;
    prim_tex_cache.vram_gen = vram_gen_counter;
    prim_tex_cache.tw_mask_x = tex_win_mask_x;
    prim_tex_cache.tw_mask_y = tex_win_mask_y;
    prim_tex_cache.tw_off_x = tex_win_off_x;
    prim_tex_cache.tw_off_y = tex_win_off_y;
    prim_tex_cache.result = result;
    prim_tex_cache.out_x = *out_x;
    prim_tex_cache.out_y = *out_y;
    prim_tex_cache.hw_clut = (result == 2) ? 1 : 0;
    if (result == 2)
    {
        prim_tex_cache.hw_tbp0 = *out_x;
        prim_tex_cache.hw_cbp = *out_y;
    }
    else
    {
        prim_tex_cache.hw_tbp0 = 0;
        prim_tex_cache.hw_cbp = 0;
    }
    return result;
}

/* Triangle area from integer vertices (absolute, in pixels).
 * Uses the cross-product / shoelace formula.                          */
static inline uint32_t tri_area_abs(int16_t x0, int16_t y0,
                                    int16_t x1, int16_t y1,
                                    int16_t x2, int16_t y2)
{
    int32_t a = (int32_t)x0 * ((int32_t)y1 - y2) + (int32_t)x1 * ((int32_t)y2 - y0) + (int32_t)x2 * ((int32_t)y0 - y1);
    if (a < 0)
        a = -a;
    return (uint32_t)(a >> 1); /* divide by 2 */
}

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

    Push_GIF_Tag(GIF_TAG_LO(nregs, 1, 0, 0, 0, 1), GIF_REG_AD); // A+D mode

    if (is_semi_trans)
    {
        Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), GS_REG_ALPHA_1); // ALPHA_1
    }

    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM); // PRIM register

    // Vertex 0 (lower Y / lower X = PSX start)
    Push_GIF_Data(GS_SET_RGBAQ(color0 & 0xFF, (color0 >> 8) & 0xFF,
                               (color0 >> 16) & 0xFF, 0x80, 0x3F800000),
                  GS_REG_RGBAQ);
    int32_t gx0 = ((int32_t)x0 + draw_offset_x + 2048) << 4;
    int32_t gy0 = ((int32_t)y0 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_SET_XYZ(gx0, gy0, 0), GS_REG_XYZ2);

    // Vertex 1 (higher Y / higher X = PSX end, not drawn)
    Push_GIF_Data(GS_SET_RGBAQ(color1 & 0xFF, (color1 >> 8) & 0xFF,
                               (color1 >> 16) & 0xFF, 0x80, 0x3F800000),
                  GS_REG_RGBAQ);
    int32_t gx1 = ((int32_t)x1 + draw_offset_x + 2048) << 4;
    int32_t gy1 = ((int32_t)y1 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_SET_XYZ(gx1, gy1, 0), GS_REG_XYZ2);
}

/* ── Main GP0 → GS translator ────────────────────────────────────── */

int Translate_GP0_to_GS(uint32_t *psx_cmd)
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
            verts[i].x = (int16_t)((int32_t)((xy & 0xFFFF) << 21) >> 21);
            verts[i].y = (int16_t)((int32_t)((xy >> 16) << 21) >> 21);

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

        /* ── Estimate pixel fill for GPU cycle accounting ── */
        {
            uint32_t area = tri_area_abs(verts[0].x, verts[0].y,
                                         verts[1].x, verts[1].y,
                                         verts[2].x, verts[2].y);
            if (is_quad)
                area += tri_area_abs(verts[1].x, verts[1].y,
                                     verts[3].x, verts[3].y,
                                     verts[2].x, verts[2].y);
            gpu_estimated_pixels += area;
        }

        if (is_quad)
        {
            // Always use two-triangle + Decode_TexPage_Cached path for textured quads.
            // The sprite shortcut bypasses CLUT decode (broken for 4BPP/8BPP) and has
            // rasterization edge-pixel differences vs the reference even for 15BPP.
            int use_sprite = 0;

            if (use_sprite)
            {

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
                uint64_t rgbaq = GS_SET_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000);

                int is_semi_trans_sprite = (cmd & 0x02) != 0;
                int nregs_sprite = 7;
                if (is_semi_trans_sprite)
                    nregs_sprite += 1;
                Push_GIF_Tag(GIF_TAG_LO(nregs_sprite, 1, 0, 0, 0, 1), GIF_REG_AD);
                if (is_semi_trans_sprite)
                    Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), GS_REG_ALPHA_1);
                Push_GIF_Data(GS_PACK_PRIM_FROM_INT(sprite_prim), GS_REG_PRIM);
                Push_GIF_Data(GS_SET_XYZ(u0 << 4, v0 << 4, 0), GS_REG_UV);
                Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(gx0, gy0, 0), GS_REG_XYZ2);
                Push_GIF_Data(GS_SET_XYZ(u1 << 4, v1 << 4, 0), GS_REG_UV);
                Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(gx1, gy1, 0), GS_REG_XYZ2);
            }
            else
            {
                int tris[2][3] = {{0, 1, 2}, {1, 3, 2}};

                int is_semi_trans = (cmd & 0x02) != 0;
                // PSX dithering applies to shaded and textured-blending (not raw) polygons
                int is_raw_tex = is_textured && (cmd & 0x01);
                int use_dither = dither_enabled && (is_shaded || (is_textured && !is_raw_tex));

                // CLUT decode for textured polygons (4BPP/8BPP)
                int poly_clut_decoded = 0;
                int poly_hw_clut = 0; /* 1 = HW CLUT (PSMT8/4) */
                int poly_uv_off_u = 0, poly_uv_off_v = 0;
                int poly_hw_tbp0 = 0, poly_hw_cbp = 0;
                int tex_cache_hit = 0;
                if (is_textured)
                {
                    int clut_x = ((verts[0].uv >> 16) & 0x3F) * 16;
                    int clut_y = (verts[0].uv >> 22) & 0x1FF;

                    int result;
                    tex_cache_hit = prim_tex_cache_lookup(tex_page_format,
                                                          poly_tex_page_x, poly_tex_page_y,
                                                          clut_x, clut_y);
                    if (tex_cache_hit)
                    {
                        result = prim_tex_cache.result;
                        poly_uv_off_u = prim_tex_cache.out_x;
                        poly_uv_off_v = prim_tex_cache.out_y;
                    }
                    else
                    {
                        result = prim_tex_decode(tex_page_format,
                                                 poly_tex_page_x, poly_tex_page_y,
                                                 clut_x, clut_y,
                                                 &poly_uv_off_u, &poly_uv_off_v);
                    }
                    if (result == 2)
                    {
                        poly_clut_decoded = 1;
                        poly_hw_clut = 1;
                        poly_hw_tbp0 = prim_tex_cache.hw_tbp0;
                        poly_hw_cbp = prim_tex_cache.hw_cbp;
                        poly_uv_off_u = 0;
                        poly_uv_off_v = 0;
                    }
                    else if (result == 1)
                    {
                        poly_clut_decoded = 1;
                        /* poly_uv_off_u/v hold SW decode UV offsets */
                    }
                }

                /* ── Lazy GS state: pre-compute desired register values ── */
                int want_dthe = use_dither;
                uint64_t want_alpha = is_semi_trans ? Get_Alpha_Reg(semi_trans_mode) : 0;
                uint64_t want_test = is_textured
                                         ? ((uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST())
                                         : 0;
                uint64_t want_tex0 = 0;
                int need_texflush = 0;
                if (is_textured)
                {
                    if (poly_clut_decoded)
                    {
                        if (poly_hw_clut)
                        {
                            int psm = (tex_page_format == 0) ? GS_PSM_4 : GS_PSM_8;
                            want_tex0 |= (uint64_t)poly_hw_tbp0;
                            want_tex0 |= (uint64_t)4 << 14;
                            want_tex0 |= (uint64_t)psm << 20;
                            want_tex0 |= (uint64_t)8 << 26;
                            want_tex0 |= (uint64_t)8 << 30;
                            want_tex0 |= (uint64_t)1 << 34;
                            want_tex0 |= (uint64_t)(is_raw_tex ? 1 : 0) << 35;
                            want_tex0 |= (uint64_t)poly_hw_cbp << 37;
                            want_tex0 |= (uint64_t)GS_PSM_16 << 51;
                            want_tex0 |= (uint64_t)1 << 61;
                        }
                        else
                        {
                            want_tex0 |= (uint64_t)4096;
                            want_tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
                            want_tex0 |= (uint64_t)GS_PSM_16S << 20;
                            want_tex0 |= (uint64_t)10 << 26;
                            want_tex0 |= (uint64_t)10 << 30;
                            want_tex0 |= (uint64_t)1 << 34;
                            want_tex0 |= (uint64_t)(is_raw_tex ? 1 : 0) << 35;
                        }
                        need_texflush = !tex_cache_hit;
                    }
                    else
                    {
                        /* Non-CLUT 15BPP: default VRAM view */
                        want_tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
                        want_tex0 |= (uint64_t)GS_PSM_16S << 20;
                        want_tex0 |= (uint64_t)10 << 26;
                        want_tex0 |= (uint64_t)9 << 30;
                        want_tex0 |= (uint64_t)1 << 34;
                        want_tex0 |= (uint64_t)(is_raw_tex ? 1 : 0) << 35;
                    }
                }

                /* Determine which GS registers actually need updating */
                int emit_dthe = (!gs_state.valid || gs_state.dthe != want_dthe);
                int emit_alpha = (is_semi_trans && (!gs_state.valid || gs_state.alpha != want_alpha));
                int emit_tex0 = (is_textured && (!gs_state.valid || gs_state.tex0 != want_tex0 || need_texflush));
                int emit_test = (is_textured && (!gs_state.valid || gs_state.test != want_test));
                int state_qws = emit_dthe + emit_alpha + emit_tex0 * 2 + emit_test;

                for (int t = 0; t < 2; t++)
                {
                    int ndata = is_textured ? 10 : 7; /* PRIM + 3×(UV+RGBAQ+XYZ) or 3×(RGBAQ+XYZ) */
                    if (t == 0)
                        ndata += state_qws;
                    Push_GIF_Tag(GIF_TAG_LO(ndata, (t == 1) ? 1 : 0, 0, 0, 0, 1), GIF_REG_AD);

                    if (t == 0)
                    {
                        if (emit_dthe)
                            Push_GIF_Data((uint64_t)want_dthe, GS_REG_DTHE);
                        if (emit_alpha)
                            Push_GIF_Data(want_alpha, GS_REG_ALPHA_1);
                        if (emit_tex0)
                        {
                            Push_GIF_Data(want_tex0, GS_REG_TEX0);
                            Push_GIF_Data(0, GS_REG_TEXFLUSH);
                        }
                        if (emit_test)
                            Push_GIF_Data(want_test, GS_REG_TEST_1);
                        /* Update lazy tracking */
                        if (!gs_state.valid)
                        {
                            /* Transitioning from unknown: sentinel for unemitted regs
                             * so stale values can't accidentally match future prims */
                            if (!is_textured)
                            {
                                gs_state.tex0 = ~0ULL;
                                gs_state.test = ~0ULL;
                            }
                            if (!is_semi_trans)
                                gs_state.alpha = ~0ULL;
                        }
                        gs_state.dthe = want_dthe;
                        if (is_semi_trans)
                            gs_state.alpha = want_alpha;
                        if (is_textured)
                        {
                            gs_state.tex0 = want_tex0;
                            gs_state.test = want_test;
                        }
                        gs_state.valid = 1;
                    }

                    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

                    for (int v = 0; v < 3; v++)
                    {
                        int i = tris[t][v];
                        if (is_textured)
                        {
                            uint32_t u, v_coord;
                            if (poly_clut_decoded)
                            {
                                u = (verts[i].uv & 0xFF) + poly_uv_off_u;
                                v_coord = ((verts[i].uv >> 8) & 0xFF) + poly_uv_off_v;
                            }
                            else
                            {
                                u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                                v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                            }
                            Push_GIF_Data(GS_SET_XYZ(u << 4, v_coord << 4, 0), GS_REG_UV);
                        }
                        uint32_t c = verts[i].color;
                        Push_GIF_Data(GS_SET_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), GS_REG_RGBAQ);

                        int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                        int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                        Push_GIF_Data(GS_SET_XYZ(gx, gy, 0), GS_REG_XYZ2);
                    }
                    /* No state restore — lazy tracking handles next primitive */
                }
            }
        }
        else
        {

            int is_semi_trans_tri = (cmd & 0x02) != 0;
            int is_raw_tex_tri = is_textured && (cmd & 0x01);
            int use_dither_tri = dither_enabled && (is_shaded || (is_textured && !is_raw_tex_tri));

            // CLUT decode for textured triangle
            int tri_clut_decoded = 0;
            int tri_hw_clut = 0;
            int tri_uv_off_u = 0, tri_uv_off_v = 0;
            int tri_hw_tbp0 = 0, tri_hw_cbp = 0;
            int tri_cache_hit = 0;
            if (is_textured)
            {
                int clut_x = ((verts[0].uv >> 16) & 0x3F) * 16;
                int clut_y = (verts[0].uv >> 22) & 0x1FF;

                int result;
                tri_cache_hit = prim_tex_cache_lookup(tex_page_format,
                                                      poly_tex_page_x, poly_tex_page_y,
                                                      clut_x, clut_y);
                if (tri_cache_hit)
                {
                    result = prim_tex_cache.result;
                    tri_uv_off_u = prim_tex_cache.out_x;
                    tri_uv_off_v = prim_tex_cache.out_y;
                }
                else
                {
                    result = prim_tex_decode(tex_page_format,
                                             poly_tex_page_x, poly_tex_page_y,
                                             clut_x, clut_y,
                                             &tri_uv_off_u, &tri_uv_off_v);
                }
                if (result == 2)
                {
                    tri_clut_decoded = 1;
                    tri_hw_clut = 1;
                    tri_hw_tbp0 = prim_tex_cache.hw_tbp0;
                    tri_hw_cbp = prim_tex_cache.hw_cbp;
                    tri_uv_off_u = 0;
                    tri_uv_off_v = 0;
                }
                else if (result == 1)
                {
                    tri_clut_decoded = 1;
                }
            }

            /* ── Lazy GS state: pre-compute desired register values ── */
            int tw_dthe = use_dither_tri;
            uint64_t tw_alpha = is_semi_trans_tri ? Get_Alpha_Reg(semi_trans_mode) : 0;
            uint64_t tw_test = is_textured
                                   ? ((uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST())
                                   : 0;
            uint64_t tw_tex0 = 0;
            int tw_texflush = 0;
            if (is_textured)
            {
                if (tri_clut_decoded)
                {
                    if (tri_hw_clut)
                    {
                        int psm = (tex_page_format == 0) ? GS_PSM_4 : GS_PSM_8;
                        tw_tex0 |= (uint64_t)tri_hw_tbp0;
                        tw_tex0 |= (uint64_t)4 << 14;
                        tw_tex0 |= (uint64_t)psm << 20;
                        tw_tex0 |= (uint64_t)8 << 26;
                        tw_tex0 |= (uint64_t)8 << 30;
                        tw_tex0 |= (uint64_t)1 << 34;
                        tw_tex0 |= (uint64_t)(is_raw_tex_tri ? 1 : 0) << 35;
                        tw_tex0 |= (uint64_t)tri_hw_cbp << 37;
                        tw_tex0 |= (uint64_t)GS_PSM_16 << 51;
                        tw_tex0 |= (uint64_t)1 << 61;
                    }
                    else
                    {
                        tw_tex0 |= (uint64_t)4096;
                        tw_tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
                        tw_tex0 |= (uint64_t)GS_PSM_16S << 20;
                        tw_tex0 |= (uint64_t)10 << 26;
                        tw_tex0 |= (uint64_t)10 << 30;
                        tw_tex0 |= (uint64_t)1 << 34;
                        tw_tex0 |= (uint64_t)(is_raw_tex_tri ? 1 : 0) << 35;
                    }
                    tw_texflush = !tri_cache_hit;
                }
                else
                {
                    /* Non-CLUT 15BPP: default VRAM view */
                    tw_tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
                    tw_tex0 |= (uint64_t)GS_PSM_16S << 20;
                    tw_tex0 |= (uint64_t)10 << 26;
                    tw_tex0 |= (uint64_t)9 << 30;
                    tw_tex0 |= (uint64_t)1 << 34;
                    tw_tex0 |= (uint64_t)(is_raw_tex_tri ? 1 : 0) << 35;
                }
            }

            /* Determine which GS registers actually need updating */
            int e_dthe = (!gs_state.valid || gs_state.dthe != tw_dthe);
            int e_alpha = (is_semi_trans_tri && (!gs_state.valid || gs_state.alpha != tw_alpha));
            int e_tex0 = (is_textured && (!gs_state.valid || gs_state.tex0 != tw_tex0 || tw_texflush));
            int e_test = (is_textured && (!gs_state.valid || gs_state.test != tw_test));

            int ndata = is_textured ? 10 : 7;
            ndata += e_dthe + e_alpha + e_tex0 * 2 + e_test;
            Push_GIF_Tag(GIF_TAG_LO(ndata, 1, 0, 0, 0, 1), GIF_REG_AD);

            if (e_dthe)
                Push_GIF_Data((uint64_t)tw_dthe, GS_REG_DTHE);
            if (e_alpha)
                Push_GIF_Data(tw_alpha, GS_REG_ALPHA_1);
            if (e_tex0)
            {
                Push_GIF_Data(tw_tex0, GS_REG_TEX0);
                Push_GIF_Data(0, GS_REG_TEXFLUSH);
            }
            if (e_test)
                Push_GIF_Data(tw_test, GS_REG_TEST_1);

            /* Update lazy tracking */
            if (!gs_state.valid)
            {
                /* Transitioning from unknown: sentinel for unemitted regs */
                if (!is_textured)
                {
                    gs_state.tex0 = ~0ULL;
                    gs_state.test = ~0ULL;
                }
                if (!is_semi_trans_tri)
                    gs_state.alpha = ~0ULL;
            }
            gs_state.dthe = tw_dthe;
            if (is_semi_trans_tri)
                gs_state.alpha = tw_alpha;
            if (is_textured)
            {
                gs_state.tex0 = tw_tex0;
                gs_state.test = tw_test;
            }
            gs_state.valid = 1;

            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

            for (int i = 0; i < 3; i++)
            {
                if (is_textured)
                {
                    uint32_t u, v_coord;
                    if (tri_clut_decoded)
                    {
                        u = (verts[i].uv & 0xFF) + tri_uv_off_u;
                        v_coord = ((verts[i].uv >> 8) & 0xFF) + tri_uv_off_v;
                    }
                    else
                    {
                        u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                        v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                    }
                    Push_GIF_Data(GS_SET_XYZ(u << 4, v_coord << 4, 0), GS_REG_UV);
                }
                uint32_t c = verts[i].color;
                Push_GIF_Data(GS_SET_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), GS_REG_RGBAQ);

                int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
                int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
                        Push_GIF_Data(GS_SET_XYZ(gx, gy, 0), GS_REG_XYZ2);
            }
            /* No state restore — lazy tracking handles next primitive */
        }
        return idx;
    }
    else if ((cmd & 0xE0) == 0x60)
    {                       // Rectangle (Sprite) - use GS SPRITE primitive for reliable rendering
        int is_textured = (cmd & 0x04) != 0;
        int is_var_size = (cmd & 0x18) == 0x00;
        int size_mode = (cmd >> 3) & 3;

        uint32_t color = cmd_word & 0xFFFFFF;
        int idx = 1;

        int16_t x, y;
        uint32_t xy = psx_cmd[idx++];
        x = (int16_t)((int32_t)((xy & 0xFFFF) << 21) >> 21);
        y = (int16_t)((int32_t)((xy >> 16) << 21) >> 21);

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

        /* ── Pixel fill estimate for rectangles ── */
        gpu_estimated_pixels += (uint32_t)w * (uint32_t)h;

        if (is_textured)
        {

            uint32_t u0_cmd = uv_clut & 0xFF;
            uint32_t v0_cmd = (uv_clut >> 8) & 0xFF;
            uint32_t u0_raw = Apply_Tex_Window_U(u0_cmd);
            uint32_t v0_raw = Apply_Tex_Window_V(v0_cmd);
            uint32_t v1_raw = v0_raw + h;

            int tex_win_active = (tex_win_mask_x != 0 || tex_win_mask_y != 0);

            // --- Texture decode: page-level cache for CLUT / tex window ---
            int clut_decoded = 0;
            int rect_hw_clut = 0;
            int flip_handled = 0;
            int cache_slot_x = 0, cache_slot_y = 0;
            int rect_hw_tbp0 = 0, rect_hw_cbp = 0;

            // Use page-level cache when CLUT format or tex window active
            int need_perpixel = tex_win_active ||
                                (tex_page_format == 0 || tex_page_format == 1);

            if (need_perpixel)
            {
                int clut_x = ((uv_clut >> 16) & 0x3F) * 16;
                int clut_y = (uv_clut >> 22) & 0x1FF;

                int result;
                if (prim_tex_cache_lookup(tex_page_format,
                                          tex_page_x, tex_page_y,
                                          clut_x, clut_y))
                {
                    result = prim_tex_cache.result;
                    cache_slot_x = prim_tex_cache.out_x;
                    cache_slot_y = prim_tex_cache.out_y;
                }
                else
                {
                    result = prim_tex_decode(tex_page_format,
                                             tex_page_x, tex_page_y,
                                             clut_x, clut_y,
                                             &cache_slot_x, &cache_slot_y);
                }
                if (result == 2)
                {
                    clut_decoded = 1;
                    rect_hw_clut = 1;
                    rect_hw_tbp0 = prim_tex_cache.hw_tbp0;
                    rect_hw_cbp = prim_tex_cache.hw_cbp;
                    cache_slot_x = 0;
                    cache_slot_y = 0;
                }
                else if (result == 1)
                {
                    clut_decoded = 1;
                }
                flip_handled = 0; /* page-level cache: flip handled by caller */
            }

            uint32_t v0_gs, v1_gs;
            if (clut_decoded)
            {
                v0_gs = cache_slot_y + v0_cmd;
                v1_gs = cache_slot_y + v0_cmd + h;
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

            uint64_t rgbaq = GS_SET_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

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

                int nregs_tri = 15; // DTHE + TEST + PRIM + 4×3 vertices (no restore needed)
                if (is_semi_trans)
                    nregs_tri += 1;
                if (is_raw_texture)
                    nregs_tri += 2; // TEX0 + TEXFLUSH only (no restore)

                Push_GIF_Tag(GIF_TAG_LO(nregs_tri, 1, 0, 0, 0, 1), GIF_REG_AD);
                Push_GIF_Data(GS_SET_DTHE(0), GS_REG_DTHE); // DTHE = 0

                if (is_semi_trans)
                    Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), GS_REG_ALPHA_1);

                if (is_raw_texture)
                {
                    Push_GIF_Data(GS_SET_TEX0_SMALL(0, PSX_VRAM_FBW, GS_PSM_16S, 10, 9, 1, 1), GS_REG_TEX0);
                    Push_GIF_Data(GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH);
                }

                // Alpha test: skip transparent pixels (STP=0 → alpha=0)
                {
                    uint64_t test_at = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                    Push_GIF_Data(test_at, GS_REG_TEST_1);
                }

                Push_GIF_Data(GS_PACK_PRIM_FROM_INT(tri_prim), GS_REG_PRIM);

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

                uint64_t rgbaq_q = GS_SET_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, q_bits);

                // TL, TR, BL, BR
                Push_GIF_Data(GS_SET_ST(s_l_bits, t_t_bits), GS_REG_ST);
                Push_GIF_Data(rgbaq_q, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx0, sgy0, 0), GS_REG_XYZ2);

                Push_GIF_Data(GS_SET_ST(s_r_bits, t_t_bits), GS_REG_ST);
                Push_GIF_Data(rgbaq_q, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx1, sgy0, 0), GS_REG_XYZ2);

                Push_GIF_Data(GS_SET_ST(s_l_bits, t_b_bits), GS_REG_ST);
                Push_GIF_Data(rgbaq_q, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx0, sgy1, 0), GS_REG_XYZ2);

                Push_GIF_Data(GS_SET_ST(s_r_bits, t_b_bits), GS_REG_ST);
                Push_GIF_Data(rgbaq_q, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx1, sgy1, 0), GS_REG_XYZ2);

                /* Update lazy state — no restore needed */
                gs_state.dthe = 0;
                if (is_semi_trans)
                    gs_state.alpha = Get_Alpha_Reg(semi_trans_mode);
                else if (!gs_state.valid)
                    gs_state.alpha = ~0ULL;
                {
                    uint64_t flip_test = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                    gs_state.test = flip_test;
                }
                if (is_raw_texture)
                {
                    uint64_t flip_tex0 = 0;
                    flip_tex0 |= (uint64_t)PSX_VRAM_FBW << 14;
                    flip_tex0 |= (uint64_t)GS_PSM_16S << 20;
                    flip_tex0 |= (uint64_t)10 << 26;
                    flip_tex0 |= (uint64_t)9 << 30;
                    flip_tex0 |= (uint64_t)1 << 34;
                    flip_tex0 |= (uint64_t)1 << 35;
                    gs_state.tex0 = flip_tex0;
                }
                else if (!gs_state.valid)
                    gs_state.tex0 = ~0ULL;
                gs_state.valid = 1;
            }
            else
            {
                // --- SPRITE path for non-flip rects (precise rasterization) ---
                // With lazy GS state tracking (matching polygon path)
                uint64_t prim_reg = 6; // SPRITE
                prim_reg |= (1 << 4);  // TME
                prim_reg |= (1 << 8);  // FST
                if (cmd & 0x02)
                    prim_reg |= (1 << 6); // ABE

                uint32_t u0_gs, u1_gs;
                if (clut_decoded)
                {
                    u0_gs = cache_slot_x + u0_cmd;
                    u1_gs = cache_slot_x + u0_cmd + w;
                }
                else
                {
                    u0_gs = u0_raw + tex_page_x;
                    u1_gs = u0_raw + w + tex_page_x;
                }

                /* Handle tex flip for page-level cache (not baked in) */
                if (tex_flip_x && clut_decoded)
                {
                    uint32_t tmp = u0_gs;
                    u0_gs = u1_gs;
                    u1_gs = tmp;
                }

                /* ── Lazy GS state for SPRITE ── */
                int want_dthe_r = 0; /* SPRITE: always disable dithering */
                uint64_t want_alpha_r = is_semi_trans ? Get_Alpha_Reg(semi_trans_mode) : 0;
                uint64_t want_test_r = (uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST();
                uint64_t want_tex0_r = 0;
                int need_texflush_r = 0;
                if (clut_decoded || is_raw_texture)
                {
                    if (rect_hw_clut)
                    {
                        int psm = (tex_page_format == 0) ? GS_PSM_4 : GS_PSM_8;
                        want_tex0_r |= (uint64_t)rect_hw_tbp0;
                        want_tex0_r |= (uint64_t)4 << 14;
                        want_tex0_r |= (uint64_t)psm << 20;
                        want_tex0_r |= (uint64_t)8 << 26;
                        want_tex0_r |= (uint64_t)8 << 30;
                        want_tex0_r |= (uint64_t)1 << 34;
                        want_tex0_r |= (uint64_t)(is_raw_texture ? 1 : 0) << 35;
                        want_tex0_r |= (uint64_t)rect_hw_cbp << 37;
                        want_tex0_r |= (uint64_t)GS_PSM_16 << 51;
                        want_tex0_r |= (uint64_t)1 << 61;
                    }
                    else if (clut_decoded)
                    {
                        want_tex0_r |= (uint64_t)4096;
                        want_tex0_r |= (uint64_t)PSX_VRAM_FBW << 14;
                        want_tex0_r |= (uint64_t)GS_PSM_16S << 20;
                        want_tex0_r |= (uint64_t)10 << 26;
                        want_tex0_r |= (uint64_t)10 << 30;
                        want_tex0_r |= (uint64_t)1 << 34;
                        want_tex0_r |= (uint64_t)(is_raw_texture ? 1 : 0) << 35;
                    }
                    else
                    {
                        want_tex0_r |= (uint64_t)PSX_VRAM_FBW << 14;
                        want_tex0_r |= (uint64_t)GS_PSM_16S << 20;
                        want_tex0_r |= (uint64_t)10 << 26;
                        want_tex0_r |= (uint64_t)9 << 30;
                        want_tex0_r |= (uint64_t)1 << 34;
                        want_tex0_r |= (uint64_t)1 << 35;
                    }
                    need_texflush_r = !gs_state.valid || gs_state.tex0 != want_tex0_r;
                }

                int emit_dthe_r = (!gs_state.valid || gs_state.dthe != want_dthe_r);
                int emit_alpha_r = (is_semi_trans && (!gs_state.valid || gs_state.alpha != want_alpha_r));
                int emit_tex0_r = ((clut_decoded || is_raw_texture) &&
                                   (!gs_state.valid || gs_state.tex0 != want_tex0_r || need_texflush_r));
                int emit_test_r = (!gs_state.valid || gs_state.test != want_test_r);
                int state_qws_r = emit_dthe_r + emit_alpha_r + emit_tex0_r * 2 + emit_test_r;

                int nregs = 1 + 6 + state_qws_r; /* PRIM + 2×(UV+RGBAQ+XYZ) + state */

                Push_GIF_Tag(GIF_TAG_LO(nregs, 1, 0, 0, 0, 1), GIF_REG_AD);

                if (emit_dthe_r)
                    Push_GIF_Data(0, GS_REG_DTHE);
                if (emit_alpha_r)
                    Push_GIF_Data(want_alpha_r, GS_REG_ALPHA_1);
                if (emit_tex0_r)
                {
                    Push_GIF_Data(want_tex0_r, GS_REG_TEX0);
                    Push_GIF_Data(0, GS_REG_TEXFLUSH);
                }
                if (emit_test_r)
                    Push_GIF_Data(want_test_r, GS_REG_TEST_1);

                /* Update lazy tracking */
                if (!gs_state.valid)
                {
                    if (!is_semi_trans)
                        gs_state.alpha = ~0ULL;
                    if (!(clut_decoded || is_raw_texture))
                    {
                        gs_state.tex0 = ~0ULL;
                    }
                }
                gs_state.dthe = want_dthe_r;
                if (is_semi_trans)
                    gs_state.alpha = want_alpha_r;
                if (clut_decoded || is_raw_texture)
                    gs_state.tex0 = want_tex0_r;
                gs_state.test = want_test_r;
                gs_state.valid = 1;

                Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

                // SPRITE: TL vertex + BR vertex
                /* UV/RGBAQ/XYZ2 already emitted below using GS_REG_* names */

                Push_GIF_Data(GS_SET_XYZ(u0_gs << 4, v0_gs << 4, 0), GS_REG_UV);
                Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx0, sgy0, 0), GS_REG_XYZ2);
                Push_GIF_Data(GS_SET_XYZ(u1_gs << 4, v1_gs << 4, 0), GS_REG_UV);
                Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
                Push_GIF_Data(GS_SET_XYZ(sgx1, sgy1, 0), GS_REG_XYZ2);
                /* No state restore — lazy tracking handles next primitive */
            }
        }
        else
        {
            // Flat sprite using A+D mode — with lazy state tracking

            int32_t gx0 = ((int32_t)x + draw_offset_x + 2048) << 4;
            int32_t gy0 = ((int32_t)y + draw_offset_y + 2048) << 4;
            int32_t gx1 = ((int32_t)(x + w) + draw_offset_x + 2048) << 4;
            int32_t gy1 = ((int32_t)(y + h) + draw_offset_y + 2048) << 4;

            uint64_t rgbaq = GS_SET_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

            int is_semi_trans = (cmd & 0x02) != 0;
            int want_dthe_f = 0;
            uint64_t want_alpha_f = is_semi_trans ? Get_Alpha_Reg(semi_trans_mode) : 0;
            int emit_dthe_f = (!gs_state.valid || gs_state.dthe != want_dthe_f);
            int emit_alpha_f = (is_semi_trans && (!gs_state.valid || gs_state.alpha != want_alpha_f));

            int nregs = 1 + 4 + emit_dthe_f + emit_alpha_f; /* PRIM + 2×(RGBAQ+XYZ) + state */

            Push_GIF_Tag(GIF_TAG_LO(nregs, 1, 0, 0, 0, 1), GIF_REG_AD);

            if (emit_dthe_f)
                Push_GIF_Data(0, GS_REG_DTHE);
            if (emit_alpha_f)
                Push_GIF_Data(want_alpha_f, GS_REG_ALPHA_1);

            /* Update lazy tracking */
            gs_state.dthe = want_dthe_f;
            if (is_semi_trans)
                gs_state.alpha = want_alpha_f;
            else if (!gs_state.valid)
                gs_state.alpha = ~0ULL;
            if (!gs_state.valid)
            {
                gs_state.tex0 = ~0ULL;
                gs_state.test = ~0ULL;
            }
            gs_state.valid = 1;

            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

            Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
            Push_GIF_Data(GS_SET_XYZ(gx0, gy0, 0), GS_REG_XYZ2);

            Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
            Push_GIF_Data(GS_SET_XYZ(gx1, gy1, 0), GS_REG_XYZ2);
            /* No state restore — lazy tracking handles next primitive */
        }
        return idx;
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
        int h = (wh >> 16) & 0x1FF;

        /* Width=0 or Height=0 → no fill (real PSX HW does nothing) */
        if (w == 0 || h == 0)
            goto fillrect_done;

        /* ── Pixel fill estimate for fill-rect ── */
        gpu_estimated_pixels += (uint32_t)w * (uint32_t)h;

        Push_GIF_Tag(GIF_TAG_LO(5, 1, 0, 0, 0, 1), GIF_REG_AD);
        Push_GIF_Data(GS_SET_SCISSOR(0, PSX_VRAM_WIDTH - 1, 0, PSX_VRAM_HEIGHT - 1), GS_REG_SCISSOR_1);
        Push_GIF_Data(GS_PACK_PRIM_FROM_INT(6), GS_REG_PRIM);
        uint32_t r = color & 0xFF;
        uint32_t g = (color >> 8) & 0xFF;
        uint32_t b = (color >> 16) & 0xFF;
        Push_GIF_Data(GS_SET_RGBAQ(r, g, b, 0x80, 0x3F800000), GS_REG_RGBAQ);
        int32_t x1 = (x + 2048) << 4;
        int32_t y1 = (y + 2048) << 4;
        int32_t x2 = (x + w + 2048) << 4;
        int32_t y2 = (y + h + 2048) << 4;
        Push_GIF_Data(GS_SET_XYZ(x1, y1, 0), GS_REG_XYZ2);
        Push_GIF_Data(GS_SET_XYZ(x2, y2, 0), GS_REG_XYZ2);

        // Restore original scissor (PSX E4 is exclusive, GS SCISSOR is inclusive)
        uint64_t sc_x2 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
        uint64_t sc_y2 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
        Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
        Push_GIF_Data(GS_SET_SCISSOR(draw_clip_x1, sc_x2, draw_clip_y1, sc_y2), GS_REG_SCISSOR_1);

        // Update shadow VRAM for filled area
        if (psx_vram_shadow)
        {
            vram_gen_counter++;
            Tex_Cache_DirtyRegion(x, y, w, h);
            uint16_t psx_color = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10);
            uint32_t fill32 = (uint32_t)psx_color | ((uint32_t)psx_color << 16);
            uint64_t fill64 = (uint64_t)fill32 | ((uint64_t)fill32 << 32);
            int end_y = (y + h < 512) ? y + h : 512;
            int end_x = (x + w < 1024) ? x + w : 1024;
            int fill_w = end_x - x;
            for (int row = y; row < end_y; row++)
            {
                uint16_t *row_ptr = &psx_vram_shadow[row * 1024 + x];
                int col = 0;
                /* Fill 4 pixels (64 bits) at a time */
                int bulk = fill_w & ~3;
                for (; col < bulk; col += 4)
                    *(uint64_t *)&row_ptr[col] = fill64;
                /* Fill remaining pixels */
                for (; col < fill_w; col++)
                    row_ptr[col] = psx_color;
            }
        }
    fillrect_done:
        return 3;
    }
    else if ((cmd & 0xE0) == 0x40)
    {                       // Line
        gs_state.valid = 0; /* Line path does unconditional state writes */
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

        Emit_Line_Segment_AD(x0, y0, color0, x1, y1, color1, is_shaded, is_semi_trans);
        return idx;
    }
    return 1; /* Unknown command — consume 1 word */
}
