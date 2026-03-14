/**
 * gpu_psp_primitives.c — PSX GP0 primitive → PSP GU translation
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "profiler.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

extern uint64_t gpu_estimated_pixels;

/* ── Coordinate Transformation ──────────────────────────────────── */
/* PSX_to_PSP is now in gpu_psp_state.h */

static inline uint32_t PSX_to_ABGR(uint32_t c)
{
    return (c & 0x00FFFFFF) | 0xFF000000;
}

/* PSX texture blending: output = tex * vc / 128 (0x80 = 1.0)
 * PSP GU_TFX_MODULATE: output = tex * vc / 255 (0xFF = 1.0)
 * Scale vertex colors by 2x to match PSX brightness. */
static inline uint32_t PSX_to_ABGR_texblend(uint32_t c)
{
    uint32_t r = (c >>  0) & 0xFF;
    uint32_t g = (c >>  8) & 0xFF;
    uint32_t b = (c >> 16) & 0xFF;
    r = r < 128 ? r * 2 : 255;
    g = g < 128 ? g * 2 : 255;
    b = b < 128 ? b * 2 : 255;
    return r | (g << 8) | (b << 16) | 0xFF000000;
}

/* ── Texture Setup ──────────────────────────────────────────────── */

/* Texture decode buffer in main RAM (256×256 × 16bpp = 128KB).
 * T4/T8 are decoded to 16bpp via software CLUT lookup — avoids:
 *  - PSP GE TEX_BUF_WIDTH 11-bit overflow (stride 4096/2048 → 0)
 *  - CLUT pointer async race (GE reads pointer after CPU overwrites)
 * Cached by (tpx, tpy, format, clut_word): skip decode when unchanged. */
static uint16_t __attribute__((aligned(16))) tex_decoded[256 * 256]; /* 128KB */
static int cached_tpx = -1, cached_tpy = -1, cached_fmt = -1;
static uint32_t cached_clut_word = 0xFFFFFFFF;

/* Set up GE texture from PSX VRAM for the current tex_page/CLUT.
 * T4/T8: decoded to 16bpp GU_PSM_5551, cached by (tpx,tpy,fmt,clut).
 * 15bpp: always re-read from shadow (no cache) to stay up-to-date. */
static void setup_psx_texture(uint32_t clut_word)
{
    int tpx = tex_page_x;
    int tpy = tex_page_y;

    /* For T4/T8 indexed formats, use decode cache */
    if (tex_page_format < 2) {
        int dirty = (tpx != cached_tpx || tpy != cached_tpy ||
                     tex_page_format != cached_fmt || clut_word != cached_clut_word);
        if (!dirty) {
            sceGuTexFlush();
            sceGuTexMode(GU_PSM_5551, 0, 0, 0);
            sceGuTexImage(0, 256, 256, 256, tex_decoded);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexScale(1.0f, 1.0f);
            sceGuTexOffset(0.0f, 0.0f);
            return;
        }
    }

    int clut_x = ((clut_word >> 16) & 0x3F) * 16;
    int clut_y = (clut_word >> 22) & 0x1FF;

    if (tex_page_format == 0) {
        /* 4-bit indexed → 16bpp: lookup each nibble in CLUT */
        uint16_t clut[16];
        uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
        for (int i = 0; i < 16; i++)
            clut[i] = csrc[i] ? (csrc[i] | 0x8000) : 0;

        for (int row = 0; row < 256; row++) {
            int vy = (tpy + row) & 511;
            uint16_t *src = &psx_vram_shadow[vy * 1024 + tpx];
            uint16_t *dst = &tex_decoded[row * 256];
            for (int col = 0; col < 64; col++) {
                uint16_t w = src[col];
                dst[col * 4 + 0] = clut[(w >>  0) & 0xF];
                dst[col * 4 + 1] = clut[(w >>  4) & 0xF];
                dst[col * 4 + 2] = clut[(w >>  8) & 0xF];
                dst[col * 4 + 3] = clut[(w >> 12) & 0xF];
            }
        }
    } else if (tex_page_format == 1) {
        /* 8-bit indexed → 16bpp: lookup each byte in CLUT */
        uint16_t clut[256];
        uint16_t *csrc = &psx_vram_shadow[clut_y * 1024 + clut_x];
        for (int i = 0; i < 256; i++)
            clut[i] = csrc[i] ? (csrc[i] | 0x8000) : 0;

        for (int row = 0; row < 256; row++) {
            int vy = (tpy + row) & 511;
            uint16_t *src = &psx_vram_shadow[vy * 1024 + tpx];
            uint16_t *dst = &tex_decoded[row * 256];
            for (int col = 0; col < 128; col++) {
                uint16_t w = src[col];
                dst[col * 2 + 0] = clut[w & 0xFF];
                dst[col * 2 + 1] = clut[(w >> 8) & 0xFF];
            }
        }
    } else {
        /* 15-bit direct color → copy with alpha fixup.
         * Always re-decoded (no cache) so GE sees latest VRAM content.
         * PSX: 0x0000 = transparent, non-zero = opaque (set bit 15). */
        for (int row = 0; row < 256; row++) {
            int vy = (tpy + row) & 511;
            uint16_t *src = &psx_vram_shadow[vy * 1024 + tpx];
            uint16_t *dst = &tex_decoded[row * 256];
            for (int i = 0; i < 256; i++)
                dst[i] = src[i] ? (src[i] | 0x8000) : 0;
        }
    }

    sceKernelDcacheWritebackRange(tex_decoded, sizeof(tex_decoded));

    /* Only cache T4/T8 — 15bpp always re-decodes */
    if (tex_page_format < 2) {
        cached_tpx = tpx;
        cached_tpy = tpy;
        cached_fmt = tex_page_format;
        cached_clut_word = clut_word;
    }

    sceGuTexFlush();
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, 256, 256, 256, tex_decoded);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
}

/* ── State Management ───────────────────────────────────────────── */

static void apply_blend(int is_semi_trans)
{
    if (is_semi_trans)
    {
        sceGuEnable(GU_BLEND);
        switch (semi_trans_mode)
        {
        case 0:
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            break;
        case 1:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
            break;
        case 2:
            sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
            break;
        case 3:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x40404040, 0xFFFFFFFF);
            break;
        }
    }
    else
    {
        sceGuDisable(GU_BLEND);
    }
}

void Prim_InvalidateGSState(void)
{
    gs_state.valid = 0;
}

void Prim_InvalidateTexCache(void) { cached_tpx = -1; }
void Prim_InvalidateTexCache_Page(int tpx, int tpy)
{
    if (tpx == cached_tpx && tpy == cached_tpy)
        cached_tpx = -1;
}

/* ── Primary Dispatch ───────────────────────────────────────────── */

int GPU_TryFastEmit(uint32_t *psx_cmd)
{
    (void)psx_cmd;
    return 0;
}

int Translate_GP0_to_GS(uint32_t *psx_cmd)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    /* FillRect (GP0 0x02) — special case before the switch */
    if (cmd == 0x02)
    {
        uint32_t color = PSX_to_ABGR(psx_cmd[0]);
        uint32_t xy = psx_cmd[1];
        uint32_t wh = psx_cmd[2];
        int16_t fx = (int16_t)(xy & 0x3F0); /* X aligned to 16px */
        int16_t fy = (int16_t)((xy >> 16) & 0x1FF);
        int16_t fw = (int16_t)(((wh & 0x3FF) + 0xF) & ~0xF); /* round up to 16 */
        int16_t fh = (int16_t)((wh >> 16) & 0x1FF);
        if (fw == 0 || fh == 0)
            return 3;

        /* Update both shadow RAM and EDRAM directly */
        {
            uint16_t r5 = (uint16_t)((cmd_word >> 3) & 0x1F);
            uint16_t g5 = (uint16_t)((cmd_word >> 11) & 0x1F);
            uint16_t b5 = (uint16_t)((cmd_word >> 19) & 0x1F);
            uint16_t col16 = r5 | (g5 << 5) | (b5 << 10);
            uint16_t *edram_vram = (uint16_t *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET);
            for (int y = fy; y < fy + fh && y < 512; y++)
                for (int x = fx; x < fx + fw && x < 1024; x++) {
                    if (psx_vram_shadow)
                        psx_vram_shadow[y * 1024 + x] = col16;
                    edram_vram[y * 1024 + x] = col16;
                }
            /* Flush dcache so GE sees CPU-written EDRAM data */
            sceKernelDcacheWritebackRange(
                &edram_vram[fy * 1024 + fx],
                (uint32_t)(fw * 2 + (fh - 1) * 1024 * 2));
        }

        /* Draw via sceGu — FillRect uses absolute VRAM coords (no draw offset) */
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_BLEND);
        PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
        v[0].color = color;
        v[0].x = fx; v[0].y = fy; v[0].z = 0;
        v[1].color = color;
        v[1].x = fx + fw; v[1].y = fy + fh; v[1].z = 0;

        /* Temporarily expand scissor — FillRect ignores draw area */
        sceGuScissor(0, 0, 1024, 512);
        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, v);
        /* Restore draw area scissor */
        sceGuScissor(draw_clip_x1, draw_clip_y1,
                     draw_clip_x2 + 1, draw_clip_y2 + 1);
        gpu_frame_stats.fill++;
        return 3;
    }

    switch (cmd & 0xE0)
    {
    case 0x20:
    { /* Polygons */
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        int nv = is_quad ? 4 : 3;

        apply_blend(is_semi);

        if (!is_textured)
        {
            sceGuDisable(GU_TEXTURE_2D);
            PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(nv * sizeof(PspVertFlat));
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1; /* skip cmd+color word */
            for (int i = 0; i < nv; i++)
            {
                uint32_t color;
                if (i == 0)
                {
                    color = base_color;
                }
                else if (is_shaded)
                {
                    color = PSX_to_ABGR(psx_cmd[p++]);
                }
                else
                {
                    color = base_color;
                }
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                v[i].color = color;
                v[i].x = x;
                v[i].y = y;
                v[i].z = 0;
            }
            sceGuDrawArray(is_quad ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
                           GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           nv, NULL, v);
            gpu_frame_stats.poly_flat++;
            return p;
        }
        else
        {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
            /* Extract texpage from vertex 1's UV word (upper 16 bits) */
            {
                int tpage_idx = is_shaded ? 5 : 4; /* vertex1 UV+texpage position */
                uint32_t tpage = psx_cmd[tpage_idx] >> 16;
                tex_page_x = (tpage & 0xF) * 64;
                tex_page_y = ((tpage >> 4) & 0x1) * 256;
                tex_page_format = (tpage >> 7) & 3;
                semi_trans_mode = (tpage >> 5) & 3;
            }
            /* Extract CLUT word from the first textured vertex (word after first xy) */
            int clut_src_p = is_shaded ? 2 : 2; /* first vertex: [cmd+color][xy][uv+clut]... */
            uint32_t clut_word = psx_cmd[clut_src_p];
            setup_psx_texture(clut_word);

            /* Raw-texture: use texture color directly, ignore vertex color */
            int is_raw_tex = (cmd & 0x01) && is_textured;
            if (is_raw_tex)
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);

            PspVertTex *v = (PspVertTex *)sceGuGetMemory(nv * sizeof(PspVertTex));
            uint32_t base_color = is_raw_tex ? PSX_to_ABGR(psx_cmd[0])
                                             : PSX_to_ABGR_texblend(psx_cmd[0]);
            int p = 1; /* skip cmd+color word */
            for (int i = 0; i < nv; i++)
            {
                uint32_t color;
                if (i == 0)
                {
                    color = base_color;
                }
                else if (is_shaded)
                {
                    color = is_raw_tex ? PSX_to_ABGR(psx_cmd[p++])
                                       : PSX_to_ABGR_texblend(psx_cmd[p++]);
                }
                else
                {
                    color = base_color;
                }
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                uint32_t uv_clut = psx_cmd[p++];
                v[i].u = (float)(uv_clut & 0xFF);
                v[i].v = (float)((uv_clut >> 8) & 0xFF);
                v[i].color = color;
                v[i].x = x;
                v[i].y = y;
                v[i].z = 0;
            }
            sceGuDrawArray(is_quad ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           nv, NULL, v);
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_ALPHA_TEST);
            gpu_frame_stats.poly_tex++;
            return p;
        }
    }

    case 0x40:
    { /* Lines */
        int is_semi = (cmd & 0x02) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        sceGuDisable(GU_TEXTURE_2D);
        apply_blend(is_semi);

        PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
        uint32_t c0 = PSX_to_ABGR(psx_cmd[0]);
        int p = 1;
        int16_t x0, y0, x1, y1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;
        uint32_t c1 = is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : c0;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x1, &y1);
        p++;
        v[0].color = c0;
        v[0].x = x0;
        v[0].y = y0;
        v[0].z = 0;
        v[1].color = c1;
        v[1].x = x1;
        v[1].y = y1;
        v[1].z = 0;
        sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       2, NULL, v);
        gpu_frame_stats.line++;
        return p;
    }

    case 0x60:
    { /* Rectangles / Sprites */
        int is_textured = (cmd & 0x04) != 0;
        int is_semi = (cmd & 0x02) != 0;
        int is_raw_rect = is_textured && (cmd & 0x01);
        uint32_t color = (is_textured && !is_raw_rect)
                         ? PSX_to_ABGR_texblend(psx_cmd[0])
                         : PSX_to_ABGR(psx_cmd[0]);

        int16_t x0, y0;
        int p = 1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;

        /* Extract texcoord + CLUT for textured sprites */
        uint8_t u0 = 0, v0 = 0;
        uint32_t clut_word = 0;
        if (is_textured) {
            clut_word = psx_cmd[p];
            u0 = (uint8_t)(clut_word & 0xFF);
            v0 = (uint8_t)((clut_word >> 8) & 0xFF);
            p++;
        }

        int16_t w, h;
        int size_code = (cmd >> 3) & 3;
        if (size_code == 0)
        {
            w = (int16_t)(psx_cmd[p] & 0xFFFF);
            h = (int16_t)(psx_cmd[p] >> 16);
            p++;
        }
        else
        {
            static const int16_t sz[] = {0, 1, 8, 16};
            w = sz[size_code];
            h = sz[size_code];
        }

        apply_blend(is_semi);

        if (!is_textured)
        {
            sceGuDisable(GU_TEXTURE_2D);
            PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
            v[0].color = color;
            v[0].x = x0;        v[0].y = y0;        v[0].z = 0;
            v[1].color = color;
            v[1].x = x0 + w;    v[1].y = y0 + h;    v[1].z = 0;
            sceGuDrawArray(GU_SPRITES,
                           GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           2, NULL, v);
            gpu_frame_stats.rect_flat++;
        }
        else
        {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
            setup_psx_texture(clut_word);

            /* Raw-texture: use texture color directly, ignore vertex color */
            if (is_raw_rect)
                sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);

            PspVertTex *v = (PspVertTex *)sceGuGetMemory(2 * sizeof(PspVertTex));
            v[0].u = (float)u0;         v[0].v = (float)v0;
            v[0].color = color;
            v[0].x = x0;                v[0].y = y0;                v[0].z = 0;
            v[1].u = (float)(u0 + w);   v[1].v = (float)(v0 + h);
            v[1].color = color;
            v[1].x = x0 + w;            v[1].y = y0 + h;            v[1].z = 0;
            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           2, NULL, v);
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_ALPHA_TEST);
            gpu_frame_stats.rect_tex++;
        }
        return p;
    }

    default:
        return 0;
    }
}

void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans)
{
    sceGuDisable(GU_TEXTURE_2D);
    apply_blend(is_semi_trans);
    PspVertFlat *v = (PspVertFlat *)sceGuGetMemory(2 * sizeof(PspVertFlat));
    int16_t sx0, sy0, sx1, sy1;
    PSX_to_PSP(x0, y0, &sx0, &sy0);
    PSX_to_PSP(x1, y1, &sx1, &sy1);
    v[0].color = PSX_to_ABGR(color0);
    v[0].x = sx0;
    v[0].y = sy0;
    v[0].z = 0;
    v[1].color = is_shaded ? PSX_to_ABGR(color1) : PSX_to_ABGR(color0);
    v[1].x = sx1;
    v[1].y = sy1;
    v[1].z = 0;
    sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, v);
}
