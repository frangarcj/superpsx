/**
 * gpu_psp_primitives.c — PSX GP0 primitive → PSP GU translation
 *
 * Texture cache is in gpu_psp_texture.c.
 */
#include "gpu_state.h"
#include "gpu_backend.h"
#include "gpu_psp_state.h"
#include "profiler.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

extern uint64_t gpu_estimated_pixels;

/* Texture API (implemented in gpu_psp_texture.c) */
extern void Tex_SetupIfChanged(uint32_t clut_word);
extern void Tex_ApplyFuncReplace(void);
extern void Tex_InvalidateState(void);

/* ── Coordinate Transformation ──────────────────────────────────── */
/* PSX_to_PSP is now in gpu_psp_state.h */

static inline uint32_t PSX_to_ABGR(uint32_t c)
{
    /* Alpha = 0x00: non-textured PSX pixels have STP=0 by default.
     * In GU_PSM_5551, alpha >= 0x80 → bit15=1 (STP set).
     * mask_set_bit forces STP=1 via stencil, not vertex alpha. */
    return (c & 0x00FFFFFF);
}

/* PSX texture blending: output = tex * vc / 128 (0x80 = 1.0)
 * PSP GU_TFX_MODULATE: output = tex * vc / 255 (0xFF = 1.0)
 * Scale vertex colors by 2x to match PSX brightness. */
static inline uint32_t PSX_to_ABGR_texblend(uint32_t c)
{
    uint32_t r = (c >> 0) & 0xFF;
    uint32_t g = (c >> 8) & 0xFF;
    uint32_t b = (c >> 16) & 0xFF;
    r = r < 128 ? r * 2 : 255;
    g = g < 128 ? g * 2 : 255;
    b = b < 128 ? b * 2 : 255;
    return r | (g << 8) | (b << 16) | 0xFF000000;
}

/* ── State Management ───────────────────────────────────────────── */

/* ── Vertex Batch ────────────────────────────────────────────────── */
#define VBATCH_MAX 256  /* max verts per batch */

/* Index buffer for quad→triangle decomposition: 4 unique verts + 6 indices
 * per quad instead of 6 duplicated verts.  Used when prim == GU_TRIANGLES. */
#define IBATCH_MAX (VBATCH_MAX * 3 / 2)

static struct {
    union {
        PspVertFlat flat[VBATCH_MAX];
        PspVertTex  tex[VBATCH_MAX];
    } v;
    uint16_t idx[IBATCH_MAX];
    int count;   /* unique vertices */
    int icount;  /* index count (0 = non-indexed) */
    int vsize;   /* sizeof per vertex */
    int vfmt;    /* GU format flags (without GU_TRANSFORM_2D) */
    int prim;    /* GU_TRIANGLES, GU_LINES, GU_SPRITES */
    int full_scissor; /* 1 = full VRAM scissor (FillRect), 0 = draw area */
    int stp_two_pass; /* 1 = textured semi-trans T4/T8: alpha-based 2-pass */
} vbatch;

/* Lazy GU state cache — avoid redundant sceGu calls.
 * Reset to -1 (unknown) by Prim_InvalidateGSState().
 * Each helper flushes the batch when it actually emits a GU command,
 * ensuring all batched verts are drawn with the correct prior state. */
static int cached_blend_on = -1;   /* 0=off, 1=on */
static int cached_blend_mode = -1; /* semi_trans_mode 0-3 */
static int cached_dither_on = -1;  /* 0=off, 1=on */
static int cached_tex_on = -1;     /* 0=off, 1=on */
static int cached_alpha_test_on = -1; /* 0=off, 1=alpha test(T4/T8), 2=color test(T16) */

static inline void vbatch_draw(int prim, int vfmt, int count, int icount,
                               void *vdst, void *idst)
{
    if (idst) {
        sceGuDrawArray(prim,
                       vfmt | GU_INDEX_16BIT | GU_TRANSFORM_2D,
                       icount,
                       (void *)((uintptr_t)idst | 0x40000000),
                       (void *)((uintptr_t)vdst | 0x40000000));
    } else {
        sceGuDrawArray(prim, vfmt | GU_TRANSFORM_2D,
                       count, NULL,
                       (void *)((uintptr_t)vdst | 0x40000000));
    }
}

static void vbatch_flush(void)
{
    if (vbatch.count == 0) return;
    gpu_frame_stats.vbatch_flushes++;
    gpu_frame_stats.vbatch_verts += vbatch.count;
    int vbytes = vbatch.count * vbatch.vsize;
    void *vdst = vpool_alloc(vbytes);
    memcpy(vdst, &vbatch.v, vbytes);

    void *idst = NULL;
    int ibytes = 0;
    if (vbatch.icount > 0) {
        ibytes = vbatch.icount * sizeof(uint16_t);
        idst = vpool_alloc(ibytes);
        memcpy(idst, vbatch.idx, ibytes);
    }

    if (vbatch.full_scissor)
        sceGuScissor(0, 0, 1024, 512);

    if (vbatch.stp_two_pass) {
        /* Stencil-based STP 3-pass for textured semi-transparent T4/T8.
         *
         * CLUT 8888 alpha: 0x00=transparent, 0x80=STP0, 0xFF=STP1.
         * After modulate: 0x00, 0x7F, 0xFE.
         *
         * Pass 1: blend STP=1 + mark stencil (background still original)
         * Pass 2: draw STP=0 opaque (stencil skips STP=1 areas)
         * Pass 3: clear stencil marks
         */

        /* Pass 1: blend STP=1 texels + mark stencil */
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilFunc(GU_ALWAYS, 0xFF, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GEQUAL, 0xC0, 0xFF);
        sceGuEnable(GU_BLEND);
        vbatch_draw(vbatch.prim, vbatch.vfmt, vbatch.count, vbatch.icount,
                    vdst, idst);

        /* Pass 2: draw STP=0 texels opaque (skip stencil-marked STP=1) */
        sceGuStencilFunc(GU_NOTEQUAL, 0xFF, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
        sceGuAlphaFunc(GU_GEQUAL, 0x40, 0xFF);
        sceGuDisable(GU_BLEND);
        vbatch_draw(vbatch.prim, vbatch.vfmt, vbatch.count, vbatch.icount,
                    vdst, idst);

        /* Pass 3: stencil cleanup (no color write) */
        sceGuStencilFunc(GU_NEVER, 0x00, 0xFF);
        sceGuStencilOp(GU_REPLACE, GU_KEEP, GU_KEEP);
        vbatch_draw(vbatch.prim, vbatch.vfmt, vbatch.count, vbatch.icount,
                    vdst, idst);

        /* Restore stencil state */
        if (mask_set_bit || mask_check_bit)
            GPU_Backend_SetMaskBit(mask_set_bit, mask_check_bit);
        else
            sceGuDisable(GU_STENCIL_TEST);
        sceGuDisable(GU_ALPHA_TEST);
        cached_alpha_test_on = 0;
        cached_blend_on = 0;
    } else {
        vbatch_draw(vbatch.prim, vbatch.vfmt, vbatch.count, vbatch.icount,
                    vdst, idst);
    }

    if (vbatch.full_scissor)
        sceGuScissor(draw_clip_x1, draw_clip_y1,
                     draw_clip_x2 - draw_clip_x1 + 1,
                     draw_clip_y2 - draw_clip_y1 + 1);

    vbatch.count = 0;
    vbatch.icount = 0;
    vbatch.full_scissor = 0;
    vbatch.stp_two_pass = 0;
}

/* Ensure batch is compatible; flush if not.
 * nverts = unique vertices to add, nidx = indices to add (0 for non-indexed). */
static inline void vbatch_prepare_idx(int prim, int vfmt, int vsize,
                                      int nverts, int nidx)
{
    if (vbatch.count > 0 &&
        (vbatch.prim != prim || vbatch.vfmt != vfmt ||
         vbatch.full_scissor != 0 || vbatch.stp_two_pass != 0 ||
         vbatch.count + nverts > VBATCH_MAX ||
         vbatch.icount + nidx > IBATCH_MAX))
        vbatch_flush();
    vbatch.prim = prim;
    vbatch.vfmt = vfmt;
    vbatch.vsize = vsize;
}

static inline void vbatch_prepare(int prim, int vfmt, int vsize, int nverts)
{
    vbatch_prepare_idx(prim, vfmt, vsize, nverts, 0);
}

/* Prepare batch for textured semi-transparent T4/T8: alpha-based STP two-pass.
 * Always flush first — never batch multiple STP prims together, because
 * the 2-pass (Pass1=all opaque, Pass2=all blend) would break polygon
 * draw order for overlapping primitives (painter's algorithm). */
static inline void vbatch_prepare_idx_stp(int prim, int vfmt, int vsize,
                                          int nverts, int nidx)
{
    if (vbatch.count > 0)
        vbatch_flush();
    vbatch.prim = prim;
    vbatch.vfmt = vfmt;
    vbatch.vsize = vsize;
    vbatch.stp_two_pass = 1;
}

static inline void vbatch_prepare_stp(int prim, int vfmt, int vsize, int nverts)
{
    vbatch_prepare_idx_stp(prim, vfmt, vsize, nverts, 0);
}

/* Prepare for FillRect sprites (full VRAM scissor). */
static inline void vbatch_prepare_fill(int nverts)
{
    int prim = GU_SPRITES;
    int vfmt = GU_COLOR_8888 | GU_VERTEX_16BIT;
    int vsize = (int)sizeof(PspVertFlat);
    if (vbatch.count > 0 &&
        (vbatch.prim != prim || vbatch.vfmt != vfmt ||
         vbatch.full_scissor != 1 || vbatch.stp_two_pass != 0 ||
         vbatch.count + nverts > VBATCH_MAX))
        vbatch_flush();
    vbatch.prim = prim;
    vbatch.vfmt = vfmt;
    vbatch.vsize = vsize;
    vbatch.full_scissor = 1;
}

void Prim_FlushBatch(void) { vbatch_flush(); }

static void apply_dither(int is_shaded, int is_textured, int is_raw_tex)
{
    int want = dither_enabled && (is_shaded || (is_textured && !is_raw_tex));
    if (want != cached_dither_on) {
        vbatch_flush();
        if (want) sceGuEnable(GU_DITHER);
        else      sceGuDisable(GU_DITHER);
        cached_dither_on = want;
    }
}

static void apply_blend(int is_semi_trans)
{
    if (is_semi_trans)
    {
        if (cached_blend_on != 1) {
            vbatch_flush();
            sceGuEnable(GU_BLEND);
            cached_blend_on = 1;
        }
        if (semi_trans_mode != cached_blend_mode) {
            vbatch_flush();
            switch (semi_trans_mode)
            {
            case 0: /* 0.5B + 0.5F — POPS uses 0x7F (≈50%) */
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x7F7F7F7F, 0x7F7F7F7F);
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
            cached_blend_mode = semi_trans_mode;
        }
    }
    else
    {
        if (cached_blend_on != 0) {
            vbatch_flush();
            sceGuDisable(GU_BLEND);
            cached_blend_on = 0;
        }
    }
}

static inline void gu_enable_texture(void)
{
    if (cached_tex_on != 1) {
        vbatch_flush();
        sceGuEnable(GU_TEXTURE_2D);
        cached_tex_on = 1;
    }
}

static inline void gu_disable_texture(void)
{
    if (cached_tex_on != 0) {
        vbatch_flush();
        sceGuDisable(GU_TEXTURE_2D);
        cached_tex_on = 0;
    }
}

static inline void gu_enable_color_test(void)
{
    /* CLUT'd textures (T4/T8): 8888 CLUT encodes STP in alpha:
     *   0x0000 → alpha=0x00, STP=0 non-zero → alpha=0x80, STP=1 → alpha=0xFF.
     * After TFX_MODULATE: 0x80*0xFF>>8=0x7F, 0xFF*0xFF>>8=0xFE.
     * Alpha test GEQUAL 0x40 passes both STP=0 and STP=1, blocks transparent.
     *
     * Direct 15bpp (T16): pixels keep original STP bits from VRAM.
     * Many valid texels may have STP=0 (alpha=0x00), so use color test
     * (RGB != 0) to reject only truly transparent pixels. */
    int want = (tex_page_format < 2) ? 1 : 2; /* 1=alpha, 2=color */
    if (cached_alpha_test_on != want) {
        vbatch_flush();
        /* Disable whichever test is currently active (or both if unknown) */
        if (cached_alpha_test_on == 1) sceGuDisable(GU_ALPHA_TEST);
        else if (cached_alpha_test_on == 2) sceGuDisable(GU_COLOR_TEST);
        else if (cached_alpha_test_on == -1) {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_COLOR_TEST);
        }
        if (want == 1) {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GEQUAL, 0x40, 0xFF);
        } else {
            sceGuEnable(GU_COLOR_TEST);
            sceGuColorFunc(GU_NOTEQUAL, 0x00000000, 0x00FFFFFF);
        }
        cached_alpha_test_on = want;
    }
}

static inline void gu_disable_color_test(void)
{
    if (cached_alpha_test_on != 0) {
        vbatch_flush();
        if (cached_alpha_test_on == 1) sceGuDisable(GU_ALPHA_TEST);
        else if (cached_alpha_test_on == 2) sceGuDisable(GU_COLOR_TEST);
        else if (cached_alpha_test_on == -1) {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuDisable(GU_COLOR_TEST);
        }
        cached_alpha_test_on = 0;
    }
}

void Prim_InvalidateGSState(void)
{
    vbatch_flush();
    gs_state.valid = 0;
    cached_blend_on = -1;
    cached_blend_mode = -1;
    cached_dither_on = -1;
    cached_tex_on = -1;
    cached_alpha_test_on = -1;
    Tex_InvalidateState();
}

/* ── Primary Dispatch ───────────────────────────────────────────── */

int GPU_TryFastEmit(uint32_t *psx_cmd)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    if (!gs_state.valid || cmd != gs_state.last_cmd_byte)
        return 0;

    /* ── Polygon fast path (0x20-0x3F) ── */
    if ((cmd & 0xE0) == 0x20)
    {
        int is_quad     = (cmd & 0x08) != 0;
        int is_shaded   = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int is_raw_tex  = is_textured && (cmd & 0x01);
        int nv = is_quad ? 4 : 3;
        int nidx = is_quad ? 6 : 3;

        if (is_textured)
        {
            /* Verify texpage matches — tpage is in V1's UV word (same offset
             * for tris and quads: shaded=5, flat=4) */
            int tpage_idx = is_shaded ? 5 : 4;
            uint32_t tpage = psx_cmd[tpage_idx] >> 16;
            int tx = (tpage & 0xF) * 64;
            int ty = ((tpage >> 4) & 0x1) * 256;
            int fmt = (tpage >> 7) & 3;

            if (tx != tex_page_x || ty != tex_page_y || fmt != tex_page_format)
                return 0;

            /* STP two-pass: never fast-path T4/T8 semi-trans */
            int is_semi = (cmd & 0x02) != 0;
            if (is_semi && fmt < 2)
                return 0;

            uint32_t clut_word = psx_cmd[2] & 0xFFFF0000;
            if (clut_word != gs_state.last_clut_word)
                return 0;

            semi_trans_mode = (tpage >> 5) & 3;

            /* ── Emit textured vertices directly to vbatch ── */
            vbatch_prepare_idx(GU_TRIANGLES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertTex), nv, nidx);

            uint16_t base = (uint16_t)vbatch.count;
            PspVertTex *dst = &vbatch.v.tex[vbatch.count];
            uint32_t base_color = is_raw_tex ? PSX_to_ABGR(psx_cmd[0])
                                             : PSX_to_ABGR_texblend(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? (is_raw_tex ? PSX_to_ABGR(psx_cmd[p++])
                                              : PSX_to_ABGR_texblend(psx_cmd[p++]))
                    : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                uint32_t uv_clut = psx_cmd[p++];
                dst[i].u = (float)Apply_Tex_Window_U(uv_clut & 0xFF);
#ifdef ENABLE_PSP_STRIDE_HACK
                dst[i].v = (float)Apply_Tex_Window_V((uv_clut >> 8) & 0xFF) * tex_v_scale;
#else
                dst[i].v = (float)Apply_Tex_Window_V((uv_clut >> 8) & 0xFF);
#endif
                dst[i].color = color; dst[i].x = x; dst[i].y = y; dst[i].z = 0;
            }
            vbatch.count += nv;

            uint16_t *ix = &vbatch.idx[vbatch.icount];
            ix[0] = base; ix[1] = base+1; ix[2] = base+2;
            if (is_quad) {
                ix[3] = base+2; ix[4] = base+1; ix[5] = base+3;
            }
            vbatch.icount += nidx;
            gpu_frame_stats.poly_tex++;
            return p;
        }
        else
        {
            /* ── Emit flat vertices directly to vbatch ── */
            vbatch_prepare_idx(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                               sizeof(PspVertFlat), nv, nidx);

            uint16_t base = (uint16_t)vbatch.count;
            PspVertFlat *dst = &vbatch.v.flat[vbatch.count];
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                dst[i].color = color; dst[i].x = x; dst[i].y = y; dst[i].z = 0;
            }
            vbatch.count += nv;

            uint16_t *ix = &vbatch.idx[vbatch.icount];
            ix[0] = base; ix[1] = base+1; ix[2] = base+2;
            if (is_quad) {
                ix[3] = base+2; ix[4] = base+1; ix[5] = base+3;
            }
            vbatch.icount += nidx;
            gpu_frame_stats.poly_flat++;
            return p;
        }
    }

    /* ── Rectangle fast path (0x60-0x7F) ── */
    if ((cmd & 0xE0) == 0x60)
    {
        int is_textured = (cmd & 0x04) != 0;
        int is_semi     = (cmd & 0x02) != 0;
        int is_raw_rect = is_textured && (cmd & 0x01);

        /* STP: never fast-path T4/T8 semi-trans rects */
        if (is_textured && is_semi && tex_page_format < 2)
            return 0;

        if (is_textured) {
            uint32_t clut_word = psx_cmd[2] & 0xFFFF0000;
            if (clut_word != gs_state.last_clut_word)
                return 0;
        }

        uint32_t color = (is_textured && !is_raw_rect)
                             ? PSX_to_ABGR_texblend(psx_cmd[0])
                             : PSX_to_ABGR(psx_cmd[0]);

        int16_t x0, y0;
        int p = 1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;

        uint8_t u0 = 0, v0_coord = 0;
        if (is_textured) {
            uint32_t uv_clut = psx_cmd[p];
            u0 = (uint8_t)(uv_clut & 0xFF);
            v0_coord = (uint8_t)((uv_clut >> 8) & 0xFF);
            p++;
        }

        int16_t w, h;
        int size_code = (cmd >> 3) & 3;
        if (size_code == 0) {
            w = (int16_t)(psx_cmd[p] & 0xFFFF);
            h = (int16_t)(psx_cmd[p] >> 16);
            p++;
        } else {
            static const int16_t sz[] = {0, 1, 8, 16};
            w = sz[size_code]; h = sz[size_code];
        }

        if (is_textured) {
            vbatch_prepare(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertTex), 2);
            PspVertTex *v = &vbatch.v.tex[vbatch.count];
            v[0].u = (float)Apply_Tex_Window_U(u0);
#ifdef ENABLE_PSP_STRIDE_HACK
            v[0].v = (float)Apply_Tex_Window_V(v0_coord) * tex_v_scale;
#else
            v[0].v = (float)Apply_Tex_Window_V(v0_coord);
#endif
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].u = (float)Apply_Tex_Window_U(u0 + w);
#ifdef ENABLE_PSP_STRIDE_HACK
            v[1].v = (float)Apply_Tex_Window_V(v0_coord + h) * tex_v_scale;
#else
            v[1].v = (float)Apply_Tex_Window_V(v0_coord + h);
#endif
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_tex++;
        } else {
            vbatch_prepare(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertFlat), 2);
            PspVertFlat *v = &vbatch.v.flat[vbatch.count];
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_flat++;
        }
        return p;
    }

    return 0;
}

int Translate_GP0_to_GS(uint32_t *psx_cmd)
{
    uint32_t cmd_word = psx_cmd[0];
    uint32_t cmd = (cmd_word >> 24) & 0xFF;

    /* Any cold-path entry invalidates the fast emit cache */
    gs_state.valid = 0;

    /* FillRect (GP0 0x02) — GU sprite in current list, no flush needed. */
    if (cmd == 0x02)
    {
        uint32_t color = PSX_to_ABGR(psx_cmd[0]);
        uint32_t xy = psx_cmd[1];
        uint32_t wh = psx_cmd[2];
        int16_t fx = (int16_t)(xy & 0x3F0);
        int16_t fy = (int16_t)((xy >> 16) & 0x1FF);
        int16_t fw = (int16_t)(((wh & 0x3FF) + 0xF) & ~0xF);
        int16_t fh = (int16_t)((wh >> 16) & 0x1FF);
        if (fw == 0 || fh == 0)
            return 3;

        /* Update shadow VRAM for CPU readback (GP0 C0h) */
        if (psx_vram_shadow)
        {
            uint16_t r5 = (uint16_t)((cmd_word >> 3) & 0x1F);
            uint16_t g5 = (uint16_t)((cmd_word >> 11) & 0x1F);
            uint16_t b5 = (uint16_t)((cmd_word >> 19) & 0x1F);
            uint16_t col16 = r5 | (g5 << 5) | (b5 << 10);
            if (mask_set_bit) col16 |= 0x8000;
            int ey = (fy + fh > 512) ? 512 : fy + fh;
            int ex = (fx + fw > 1024) ? 1024 : fx + fw;
            int w = ex - fx;
            for (int y = fy; y < ey; y++) {
                uint16_t *sr = &psx_vram_shadow[y * 1024 + fx];
                for (int x = 0; x < w; x++) sr[x] = col16;
            }
        }

        /* Batched FillRect — full VRAM scissor, no texture */
        gu_disable_texture();
        gu_disable_color_test();
        apply_blend(0);
        vbatch_prepare_fill(2);
        PspVertFlat *dst = &vbatch.v.flat[vbatch.count];
        dst[0].color = color; dst[0].x = fx;      dst[0].y = fy;      dst[0].z = 0;
        dst[1].color = color; dst[1].x = fx + fw; dst[1].y = fy + fh; dst[1].z = 0;
        vbatch.count += 2;
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
        int is_raw_tex = is_textured && (cmd & 0x01);
        int nv = is_quad ? 4 : 3;

        apply_blend(is_semi);
        apply_dither(is_shaded, is_textured, is_raw_tex);

        if (!is_textured)
        {
            gu_disable_texture();
            gu_disable_color_test();
            /* Quads: 4 unique verts + 6 indices.  Tris: 3 verts + 3 indices. */
            int unique_nv = nv;
            int nidx = is_quad ? 6 : 3;
            vbatch_prepare_idx(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                               sizeof(PspVertFlat), unique_nv, nidx);

            uint16_t base = (uint16_t)vbatch.count;
            PspVertFlat *dst = &vbatch.v.flat[vbatch.count];
            uint32_t base_color = PSX_to_ABGR(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                dst[i].color = color; dst[i].x = x; dst[i].y = y; dst[i].z = 0;
            }
            vbatch.count += unique_nv;

            uint16_t *ix = &vbatch.idx[vbatch.icount];
            ix[0] = base; ix[1] = base+1; ix[2] = base+2;
            if (is_quad) {
                ix[3] = base+2; ix[4] = base+1; ix[5] = base+3;
            }
            vbatch.icount += nidx;
            gpu_frame_stats.poly_flat++;
            gs_state.valid = 1;
            gs_state.last_cmd_byte = cmd;
            return p;
        }
        else
        {
            gu_enable_texture();
            /* Extract texpage from vertex 1's UV word (before color test,
             * which needs tex_page_format to choose alpha vs color test) */
            {
                int tpage_idx = is_shaded ? 5 : 4;
                uint32_t tpage = psx_cmd[tpage_idx] >> 16;
                tex_page_x = (tpage & 0xF) * 64;
                tex_page_y = ((tpage >> 4) & 0x1) * 256;
                tex_page_format = (tpage >> 7) & 3;
                semi_trans_mode = (tpage >> 5) & 3;
            }
            gu_enable_color_test();
            int clut_src_p = 2;
            uint32_t clut_word = psx_cmd[clut_src_p] & 0xFFFF0000;
            Tex_SetupIfChanged(clut_word);

            if (is_raw_tex)
                Tex_ApplyFuncReplace();

            int unique_nv = nv;
            int nidx = is_quad ? 6 : 3;
            if (is_semi && tex_page_format < 2)
                vbatch_prepare_idx_stp(GU_TRIANGLES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertTex), unique_nv, nidx);
            else
                vbatch_prepare_idx(GU_TRIANGLES,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertTex), unique_nv, nidx);

            uint16_t base = (uint16_t)vbatch.count;
            PspVertTex *dst = &vbatch.v.tex[vbatch.count];
            uint32_t base_color = is_raw_tex ? PSX_to_ABGR(psx_cmd[0])
                                             : PSX_to_ABGR_texblend(psx_cmd[0]);
            int p = 1;
            for (int i = 0; i < nv; i++)
            {
                uint32_t color = (i == 0) ? base_color
                    : is_shaded ? (is_raw_tex ? PSX_to_ABGR(psx_cmd[p++])
                                              : PSX_to_ABGR_texblend(psx_cmd[p++]))
                    : base_color;
                int16_t x, y;
                PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x, &y);
                p++;
                uint32_t uv_clut = psx_cmd[p++];
                dst[i].u = (float)Apply_Tex_Window_U(uv_clut & 0xFF);
#ifdef ENABLE_PSP_STRIDE_HACK
                dst[i].v = (float)Apply_Tex_Window_V((uv_clut >> 8) & 0xFF) * tex_v_scale;
#else
                dst[i].v = (float)Apply_Tex_Window_V((uv_clut >> 8) & 0xFF);
#endif
                dst[i].color = color; dst[i].x = x; dst[i].y = y; dst[i].z = 0;
            }
            vbatch.count += unique_nv;

            uint16_t *ix = &vbatch.idx[vbatch.icount];
            ix[0] = base; ix[1] = base+1; ix[2] = base+2;
            if (is_quad) {
                ix[3] = base+2; ix[4] = base+1; ix[5] = base+3;
            }
            vbatch.icount += nidx;
            gpu_frame_stats.poly_tex++;
            gs_state.valid = 1;
            gs_state.last_cmd_byte = cmd;
            gs_state.last_clut_word = clut_word;

            return p;
        }
    }

    case 0x40:
    { /* Lines */
        int is_semi = (cmd & 0x02) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        gu_disable_texture();
        gu_disable_color_test();
        apply_blend(is_semi);
        apply_dither(is_shaded, 0, 0);

        vbatch_prepare(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertFlat), 2);

        uint32_t c0 = PSX_to_ABGR(psx_cmd[0]);
        int p = 1;
        int16_t x0, y0, x1, y1;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x0, &y0);
        p++;
        uint32_t c1 = is_shaded ? PSX_to_ABGR(psx_cmd[p++]) : c0;
        PSX_to_PSP((int16_t)(psx_cmd[p] & 0xFFFF), (int16_t)(psx_cmd[p] >> 16), &x1, &y1);
        p++;

        PspVertFlat *v = &vbatch.v.flat[vbatch.count];
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        vbatch.count += 2;
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

        uint8_t u0 = 0, v0 = 0;
        uint32_t clut_word = 0;
        if (is_textured) {
            uint32_t uv_clut = psx_cmd[p];
            clut_word = uv_clut & 0xFFFF0000;
            u0 = (uint8_t)(uv_clut & 0xFF);
            v0 = (uint8_t)((uv_clut >> 8) & 0xFF);
            p++;
        }

        int16_t w, h;
        int size_code = (cmd >> 3) & 3;
        if (size_code == 0) {
            w = (int16_t)(psx_cmd[p] & 0xFFFF);
            h = (int16_t)(psx_cmd[p] >> 16);
            p++;
        } else {
            static const int16_t sz[] = {0, 1, 8, 16};
            w = sz[size_code]; h = sz[size_code];
        }

        apply_blend(is_semi);
        apply_dither(0, is_textured, is_raw_rect);

        if (!is_textured)
        {
            gu_disable_texture();
            gu_disable_color_test();
            vbatch_prepare(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                           sizeof(PspVertFlat), 2);
            PspVertFlat *v = &vbatch.v.flat[vbatch.count];
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_flat++;
        }
        else
        {
            gu_enable_texture();
            gu_enable_color_test();
            Tex_SetupIfChanged(clut_word);

            if (is_raw_rect)
                Tex_ApplyFuncReplace();

            if (is_semi && tex_page_format < 2)
                vbatch_prepare_stp(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertTex), 2);
            else
                vbatch_prepare(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_16BIT,
                       sizeof(PspVertTex), 2);
            PspVertTex *v = &vbatch.v.tex[vbatch.count];
            v[0].u = (float)Apply_Tex_Window_U(u0);
#ifdef ENABLE_PSP_STRIDE_HACK
            v[0].v = (float)Apply_Tex_Window_V(v0) * tex_v_scale;
#else
            v[0].v = (float)Apply_Tex_Window_V(v0);
#endif
            v[0].color = color; v[0].x = x0;     v[0].y = y0;     v[0].z = 0;
            v[1].u = (float)Apply_Tex_Window_U(u0 + w);
#ifdef ENABLE_PSP_STRIDE_HACK
            v[1].v = (float)Apply_Tex_Window_V(v0 + h) * tex_v_scale;
#else
            v[1].v = (float)Apply_Tex_Window_V(v0 + h);
#endif
            v[1].color = color; v[1].x = x0 + w; v[1].y = y0 + h; v[1].z = 0;
            vbatch.count += 2;
            gpu_frame_stats.rect_tex++;
        }
        gs_state.valid = 1;
        gs_state.last_cmd_byte = cmd;
        if (is_textured) gs_state.last_clut_word = clut_word;
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
    gu_disable_texture();
    apply_blend(is_semi_trans);
    apply_dither(is_shaded, 0, 0);

    vbatch_prepare(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT,
                   sizeof(PspVertFlat), 2);
    int16_t sx0, sy0, sx1, sy1;
    PSX_to_PSP(x0, y0, &sx0, &sy0);
    PSX_to_PSP(x1, y1, &sx1, &sy1);
    PspVertFlat *v = &vbatch.v.flat[vbatch.count];
    v[0].color = PSX_to_ABGR(color0);
    v[0].x = sx0; v[0].y = sy0; v[0].z = 0;
    v[1].color = is_shaded ? PSX_to_ABGR(color1) : PSX_to_ABGR(color0);
    v[1].x = sx1; v[1].y = sy1; v[1].z = 0;
    vbatch.count += 2;
}
