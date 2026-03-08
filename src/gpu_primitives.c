/**
 * gpu_primitives.c — PSX GP0 primitive → PS2 GS translation
 *
 * Translates PSX polygons, rectangles / sprites, fill-rects and lines
 * into GS GIF packets.  Vertex data uses REGLIST mode (FLG=1) for
 * 2× density (2 reg values per QW).  State registers use A+D mode.
 */
#include "gpu_state.h"
#include "profiler.h"
#include <gif_tags.h>

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
gs_state_t gs_state = {0, 0, 0, 0, -1, 0};

/* Primitive-level Decode_TexPage_Cached result cache.
 * Eliminates ~80% of redundant texture cache lookups for consecutive
 * same-texture primitives (582K → ~100K calls).
 * 4-entry FIFO: handles A→B→A alternation without thrashing. */
#define PRIM_TEX_CACHE_SIZE 4
static struct
{
    int valid;
    int tex_format; /* 0=4BPP, 1=8BPP, 2=15BPP */
    int tex_page_x;
    int tex_page_y;
    int clut_x;
    int clut_y;
    uint32_t vram_gen;
    int result; /* 0=not cached (15BPP), 2=HW CLUT (CSM1), 3=HW CLUT (CSM2) */
    
    int hw_clut;
    int csm;    /* 0=CSM1, 1=CSM2 (matches GS TEX0.CSM bit directly) */
    int hw_tbp0;
    int hw_cbp;
} prim_tex_cache[PRIM_TEX_CACHE_SIZE] = {{0}};
static int prim_tex_cache_next = 0;  /* FIFO write index */
static int prim_tex_cache_last = -1; /* last-hit index for fast repeated access */
#define PTCACHE prim_tex_cache[prim_tex_cache_last]  /* shorthand for current entry */

/* Invalidate GS state tracking (called on E1, E6, GPU reset, etc.) */
void Prim_InvalidateGSState(void)
{
    gs_state.valid = 0;
}

/* Compute GS CLAMP_1 value for PSX texture window.
 * When texture window is active, use REGION_REPEAT (mode 3) which
 * implements exactly the PSX formula:
 *   U_result = (U & MINU) | MAXU
 *   V_result = (V & MINV) | MAXV
 * PSX:
 *   texX = (texX & ~(mask_x*8)) | ((off_x & mask_x)*8)
 * Mapping: MINU=~(mask_x*8)&0xFF, MAXU=(off_x&mask_x)*8
 * When no texture window: REPEAT mode (0,0). */
static inline uint64_t Compute_TexWin_Clamp(void)
{
    if (tex_win_mask_x == 0 && tex_win_mask_y == 0)
        return GS_SET_CLAMP(0, 0, 0, 0, 0, 0); /* REPEAT */
    uint32_t minu = ~(tex_win_mask_x * 8) & 0xFF;
    uint32_t maxu = (tex_win_off_x & tex_win_mask_x) * 8;
    uint32_t minv = ~(tex_win_mask_y * 8) & 0xFF;
    uint32_t maxv = (tex_win_off_y & tex_win_mask_y) * 8;
    return GS_SET_CLAMP(3, 3, minu, maxu, minv, maxv); /* REGION_REPEAT */
}

/* Invalidate primitive texture cache (called on VRAM writes) */
void Prim_InvalidateTexCache(void)
{
    for (int i = 0; i < PRIM_TEX_CACHE_SIZE; i++)
        prim_tex_cache[i].valid = 0;
    prim_tex_cache_next = 0;
    prim_tex_cache_last = -1;
}

/* Targeted invalidation: only entries referencing a specific CBP slot */
void Prim_InvalidateTexCache_CBP(int cbp)
{
    for (int i = 0; i < PRIM_TEX_CACHE_SIZE; i++)
        if (prim_tex_cache[i].valid && prim_tex_cache[i].hw_cbp == cbp)
            prim_tex_cache[i].valid = 0;
}

/* Try to reuse cached Decode_TexPage_Cached result.
 * Returns 1 if hit (result stored in prim_tex_cache[prim_tex_cache_last]),
 * 0 if miss. */
static inline int prim_tex_cache_lookup(int tex_format, int tex_page_x, int tex_page_y,
                                        int clut_x, int clut_y)
{
    /* Fast path: check last-hit entry first */
    if (prim_tex_cache_last >= 0) {
        typeof(prim_tex_cache[0]) *e = &prim_tex_cache[prim_tex_cache_last];
        if (e->valid &&
            e->tex_format == tex_format &&
            e->tex_page_x == tex_page_x &&
            e->tex_page_y == tex_page_y &&
            e->clut_x == clut_x &&
            e->clut_y == clut_y &&
            e->vram_gen == Tex_Cache_GetCombinedGen(
                tex_format, tex_page_x, tex_page_y, clut_x, clut_y))
            return 1;
    }
    /* Search all entries */
    uint32_t gen = Tex_Cache_GetCombinedGen(tex_format, tex_page_x, tex_page_y, clut_x, clut_y);
    for (int i = 0; i < PRIM_TEX_CACHE_SIZE; i++) {
        typeof(prim_tex_cache[0]) *e = &prim_tex_cache[i];
        if (e->valid &&
            e->tex_format == tex_format &&
            e->tex_page_x == tex_page_x &&
            e->tex_page_y == tex_page_y &&
            e->clut_x == clut_x &&
            e->clut_y == clut_y &&
            e->vram_gen == gen) {
            prim_tex_cache_last = i;
            return 1;
        }
    }
    return 0;
}

/* Call Decode_TexPage_Cached and store result in cache (FIFO slot). */
static inline int prim_tex_decode(int tex_format, int tex_page_x, int tex_page_y,
                                  int clut_x, int clut_y,
                                  int *out_x, int *out_y)
{
    PROF_PUSH(PROF_GPU_TEXCACHE);
    int result = Decode_TexPage_Cached(tex_format, tex_page_x, tex_page_y,
                                       clut_x, clut_y, out_x, out_y);
    PROF_POP(PROF_GPU_TEXCACHE);
    int idx = prim_tex_cache_next;
    prim_tex_cache_next = (prim_tex_cache_next + 1) % PRIM_TEX_CACHE_SIZE;
    prim_tex_cache_last = idx;
    typeof(prim_tex_cache[0]) *e = &prim_tex_cache[idx];
    e->result = result;
    e->valid = 1;
    e->tex_format = tex_format;
    e->tex_page_x = tex_page_x;
    e->tex_page_y = tex_page_y;
    e->clut_x = clut_x;
    e->clut_y = clut_y;
    e->vram_gen = Tex_Cache_GetCombinedGen(
        tex_format, tex_page_x, tex_page_y, clut_x, clut_y);
    e->hw_clut = (result == 2 || result == 3) ? 1 : 0;
    e->csm = (result == 3) ? 1 : 0; /* 0=CSM1, 1=CSM2 */
    
    if (e->hw_clut)
    {
        e->hw_tbp0 = *out_x;
        e->hw_cbp = *out_y;
    }
    else
    {
        e->hw_tbp0 = 0;
        e->hw_cbp = 0;
    }
    return result;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Debug Texture Overlay — colored bounding boxes + printf log
 *
 *  Draws a colored LINE_STRIP outline around each textured primitive:
 *    RED   = 4BPP (tex_format=0)
 *    GREEN = 8BPP (tex_format=1)
 *    CYAN  = 15BPP / SW decoded
 *
 *  Enable: cmake -DENABLE_TEX_DEBUG=ON or #define TEX_DEBUG_OVERLAY 1
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef TEX_DEBUG_OVERLAY

static uint32_t tex_debug_prim_count = 0;

/* Emit colored bounding-box outline and log primitive info to stdout.
 * sx0/sy0/sx1/sy1 are RAW PSX screen coords (before draw_offset).
 * tex_fmt: 0=4BPP, 1=8BPP, 2=15BPP.  page_id: 0..31 (-1 if N/A). */
static void emit_debug_box(int sx0, int sy0, int sx1, int sy1,
                           int tex_fmt, int page_id,
                           int tbp0, int cbp,
                           const char *prim_type)
{
    /* Color by format */
    uint8_t r = (tex_fmt == 0) ? 0xFF : 0x30;
    uint8_t g = (tex_fmt == 1) ? 0xFF : 0x30;
    uint8_t b = (tex_fmt >= 2) ? 0xFF : 0x30;

    int32_t gx0 = ((int32_t)sx0 + draw_offset_x + 2048) << 4;
    int32_t gy0 = ((int32_t)sy0 + draw_offset_y + 2048) << 4;
    int32_t gx1 = ((int32_t)sx1 + draw_offset_x + 2048) << 4;
    int32_t gy1 = ((int32_t)sy1 + draw_offset_y + 2048) << 4;

    /* 13 regs: TEST + DTHE + PRIM + 5×(RGBAQ + XYZ2) */
    Push_GIF_Tag(GIF_TAG_LO(13, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(Get_Base_TEST(), GS_REG_TEST_1);
    Push_GIF_Data(0, GS_REG_DTHE);

    uint64_t prim = 3; /* LINE_STRIP */
    prim |= (1 << 8);  /* FST */
    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim), GS_REG_PRIM);

    uint64_t rgbaq = GS_SET_RGBAQ(r, g, b, 0x80, 0x3F800000);

    /* TL → TR → BR → BL → TL */
    Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
    Push_GIF_Data(GS_SET_XYZ(gx0, gy0, 0), GS_REG_XYZ2);
    Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
    Push_GIF_Data(GS_SET_XYZ(gx1, gy0, 0), GS_REG_XYZ2);
    Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
    Push_GIF_Data(GS_SET_XYZ(gx1, gy1, 0), GS_REG_XYZ2);
    Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
    Push_GIF_Data(GS_SET_XYZ(gx0, gy1, 0), GS_REG_XYZ2);
    Push_GIF_Data(rgbaq, GS_REG_RGBAQ);
    Push_GIF_Data(GS_SET_XYZ(gx0, gy0, 0), GS_REG_XYZ2);

    /* Invalidate lazy state */
    gs_state.valid = 0;

    /* Printf log */
    printf("[TEXDBG] #%u %s fmt=%d pg=%d tbp=%d cbp=%d sc=(%d,%d)+(%dx%d)\n",
           tex_debug_prim_count, prim_type, tex_fmt, page_id,
           tbp0, cbp,
           sx0, sy0, sx1 - sx0, sy1 - sy0);
    tex_debug_prim_count++;
}

#endif /* TEX_DEBUG_OVERLAY */

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

/* ── Shared Vertex struct (used by polygon emitter and translator) ── */
typedef struct {
    int16_t x, y;
    uint32_t color;
    uint32_t uv;
} PolyVertex;

/* ═══════════════════════════════════════════════════════════════════
 *  Shared polygon GIF emitter — CLUT decode + lazy state + REGLIST
 *
 *  Handles both triangles (num_verts=3, gs_prim=3) and quads
 *  (num_verts=4, gs_prim=4=TRIANGLE_STRIP).  Eliminates ~300 lines
 *  of duplicated code between the quad and tri paths, reducing
 *  I-cache pressure on the EE's 16KB L1.
 * ═══════════════════════════════════════════════════════════════════ */
static void emit_poly_state_and_verts(
    const PolyVertex *verts,
    int num_verts,          /* 3 or 4 */
    int gs_prim_type,       /* 3=TRIANGLE, 4=TRIANGLE_STRIP */
    uint32_t cmd,           /* full GP0 command byte (top 8 bits of cmd word) */
    int poly_tex_page_x,
    int poly_tex_page_y)
{
    int is_shaded    = (cmd & 0x10) != 0;
    int is_textured  = (cmd & 0x04) != 0;
    int is_semi_trans = (cmd & 0x02) != 0;
    int is_raw_tex   = is_textured && (cmd & 0x01);
    int use_dither   = dither_enabled && (is_shaded || (is_textured && !is_raw_tex));

    uint64_t prim_reg = gs_prim_type;
    if (is_shaded)    prim_reg |= (1 << 3);  /* IIP */
    if (is_textured) { prim_reg |= (1 << 4); prim_reg |= (1 << 8); } /* TME + FST */
    if (is_semi_trans) prim_reg |= (1 << 6); /* ABE */

    /* ── CLUT decode for textured polygons ── */
    int clut_decoded = 0, hw_clut = 0;
    int uv_off_u = 0, uv_off_v = 0;
    int cache_hit = 0;
    int csm = 0;
    int clut_x = 0, clut_y = 0;

    if (is_textured)
    {
        clut_x = ((verts[0].uv >> 16) & 0x3F) * 16;
        clut_y = (verts[0].uv >> 22) & 0x1FF;

        int result;
        cache_hit = prim_tex_cache_lookup(tex_page_format,
                                          poly_tex_page_x, poly_tex_page_y,
                                          clut_x, clut_y);
        if (cache_hit)
        {
            result = PTCACHE.result;
            uv_off_u = PTCACHE.hw_tbp0;
            uv_off_v = PTCACHE.hw_cbp;
            csm = PTCACHE.csm;
        }
        else
        {
            int out_x, out_y;
            result = prim_tex_decode(tex_page_format,
                                     poly_tex_page_x, poly_tex_page_y,
                                     clut_x, clut_y,
                                     &out_x, &out_y);
            csm = PTCACHE.csm;
            uv_off_u = out_x;
            uv_off_v = out_y;
        }
        if (result == 2 || result == 3)
        {
            clut_decoded = 1;
            hw_clut = 1;
            uv_off_u = 0;
            uv_off_v = 0;
        }
        else if (result == 1)
        {
            clut_decoded = 1;
        }
    }

    /* ── Lazy GS state: pre-compute desired register values ── */
    int want_dthe = use_dither;
    uint64_t want_alpha = is_semi_trans ? Get_Alpha_Reg(semi_trans_mode) : 0;
    uint64_t want_test = is_textured
                             ? ((uint64_t)1 | ((uint64_t)6 << 1) | Get_Base_TEST())
                             : 0;
    uint64_t want_tex0 = 0;
    uint64_t want_clamp = 0;
    uint64_t want_texclut = 0;
    int need_texflush = 0;

    if (is_textured)
    {
        if (clut_decoded)
        {
            if (hw_clut)
            {
                int psm = (tex_page_format == 0) ? GS_PSM_4 : GS_PSM_8;
                if (csm) {
                    /* CSM2: TCC=1+TEXA for transparency, CLD=2 for TEXCLUT */
                    want_tex0 = GS_SET_TEX0(PTCACHE.hw_tbp0, 4, psm, 8, 8,
                                             1/*TCC*/, (is_raw_tex ? 1 : 0),
                                             0/*CBA*/, GS_PSM_16S, 1/*CSM2*/, 0, 2/*CLD*/);
                    want_texclut = GS_SET_TEXCLUT(PSX_VRAM_FBW,
                                                   clut_x / 16, clut_y);
                } else {
                    /* CSM1: CLD=1 always reload */
                    want_tex0 = GS_SET_TEX0(PTCACHE.hw_tbp0, 4, psm, 8, 8,
                                             1/*TCC*/, (is_raw_tex ? 1 : 0),
                                             PTCACHE.hw_cbp, GS_PSM_16,
                                             0/*CSM1*/, 0, 1/*CLD*/);
                }
                want_clamp = Compute_TexWin_Clamp();
            }
            else
            {
                want_tex0 = GS_SET_TEX0(4096, PSX_VRAM_FBW, GS_PSM_16S, 10, 10,
                                         1, (is_raw_tex ? 1 : 0), 0, 0, 0, 0, 0);
            }
            need_texflush = !cache_hit;
        }
        else
        {
            /* Non-CLUT 15BPP: default VRAM view */
            want_tex0 = GS_SET_TEX0(0, PSX_VRAM_FBW, GS_PSM_16S, 10, 9,
                                     1, (is_raw_tex ? 1 : 0), 0, 0, 0, 0, 0);
        }
    }

    /* ── Determine which GS registers actually need updating ── */
    int emit_dthe    = (!gs_state.valid || gs_state.dthe != want_dthe);
    int emit_alpha   = (is_semi_trans && (!gs_state.valid || gs_state.alpha != want_alpha));
    int emit_tex0    = (is_textured && (!gs_state.valid || gs_state.tex0 != want_tex0 || need_texflush));
    int emit_test    = (is_textured && (!gs_state.valid || gs_state.test != want_test));
    int emit_clamp   = (is_textured && hw_clut && (!gs_state.valid || gs_state.clamp != want_clamp));
    int emit_texclut = (is_textured && hw_clut && csm &&
                        (!gs_state.valid || gs_state.texclut != want_texclut));
    int state_qws = emit_dthe + emit_alpha + emit_tex0 + (emit_tex0 && need_texflush) + emit_test + emit_clamp + emit_texclut;

    /* ── A+D tag: State + PRIM (EOP=0) ── */
    Push_GIF_Tag(GIF_TAG_LO(state_qws + 1, 0, 0, 0, 0, 1), GIF_REG_AD);

    if (emit_dthe)    Push_GIF_Data((uint64_t)want_dthe, GS_REG_DTHE);
    if (emit_alpha)   Push_GIF_Data(want_alpha, GS_REG_ALPHA_1);
    if (emit_clamp)   Push_GIF_Data(want_clamp, GS_REG_CLAMP_1);
    if (emit_texclut) Push_GIF_Data(want_texclut, GS_REG_TEXCLUT);
    if (emit_tex0)
    {
        Push_GIF_Data(want_tex0, GS_REG_TEX0);
        if (need_texflush)
            Push_GIF_Data(0, GS_REG_TEXFLUSH);
    }
    if (emit_test)    Push_GIF_Data(want_test, GS_REG_TEST_1);
    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

    /* ── Update lazy tracking ── */
    if (!gs_state.valid)
    {
        if (!is_textured) { gs_state.tex0 = ~0ULL; gs_state.test = ~0ULL; gs_state.clamp = ~0ULL; }
        if (!is_semi_trans) gs_state.alpha = ~0ULL;
    }
    gs_state.dthe = want_dthe;
    if (is_semi_trans) gs_state.alpha = want_alpha;
    if (is_textured)
    {
        gs_state.tex0 = want_tex0;
        gs_state.test = want_test;
        if (hw_clut) {
            gs_state.clamp = want_clamp;
            if (csm) gs_state.texclut = want_texclut;
        }
    }
    gs_state.valid = 1;

    /* ── REGLIST vertex packet: FLG=1, no PRE ── */
    {
        int nreg = is_textured ? 3 : 2;
        uint64_t regs = is_textured
            ? ((uint64_t)GIF_REG_UV | ((uint64_t)GIF_REG_RGBAQ << 4) | ((uint64_t)GIF_REG_XYZ2 << 8))
            : ((uint64_t)GIF_REG_RGBAQ | ((uint64_t)GIF_REG_XYZ2 << 4));
        Push_GIF_Tag(GIF_TAG_LO(num_verts, 1, 0, 0, 1, nreg), regs);

        uint64_t vdata[12]; /* max 4 × 3 */
        int vc = 0;
        for (int i = 0; i < num_verts; i++)
        {
            if (is_textured)
            {
                uint32_t u, v_coord;
                if (clut_decoded)
                {
                    u = (verts[i].uv & 0xFF) + uv_off_u;
                    v_coord = ((verts[i].uv >> 8) & 0xFF) + uv_off_v;
                }
                else
                {
                    u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                    v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                }
                vdata[vc++] = GS_SET_XYZ(u << 4, v_coord << 4, 0);
            }
            uint32_t c = verts[i].color;
            vdata[vc++] = GS_SET_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000);
            int32_t gx = ((int32_t)verts[i].x + draw_offset_x + 2048) << 4;
            int32_t gy = ((int32_t)verts[i].y + draw_offset_y + 2048) << 4;
            vdata[vc++] = GS_SET_XYZ(gx, gy, 0);
        }
        for (int i = 0; i < vc; i += 2)
            Push_GIF_Data(vdata[i], (i + 1 < vc) ? vdata[i + 1] : 0);
    }

#ifdef TEX_DEBUG_OVERLAY
    if (is_textured)
    {
        int bx0 = verts[0].x, by0 = verts[0].y;
        int bx1 = bx0, by1 = by0;
        for (int vi = 1; vi < num_verts; vi++) {
            if (verts[vi].x < bx0) bx0 = verts[vi].x;
            if (verts[vi].y < by0) by0 = verts[vi].y;
            if (verts[vi].x > bx1) bx1 = verts[vi].x;
            if (verts[vi].y > by1) by1 = verts[vi].y;
        }
        emit_debug_box(bx0, by0, bx1, by1,
                       tex_page_format,
                       (poly_tex_page_x >> 6) + ((poly_tex_page_y >> 8) << 4),
                       PTCACHE.hw_tbp0, PTCACHE.hw_cbp,
                       num_verts == 4 ? "QUAD" : "TRI");
    }
#endif
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

        int num_psx_verts = is_quad ? 4 : 3;
        uint32_t color = cmd_word & 0xFFFFFF;
        int idx = 1;

        PolyVertex verts[4];

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


        /* Unified polygon emitter: quad=TRISTRIP(4), tri=TRIANGLE(3) */
        emit_poly_state_and_verts(verts, num_psx_verts,
                                  is_quad ? 4 : 3,
                                  cmd, poly_tex_page_x, poly_tex_page_y);

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
            int rect_csm = 0; /* 0=CSM1, 1=CSM2 */
            int rect_clut_x = 0, rect_clut_y = 0;
            int rect_tex_cache_hit = 0;

            // Use page-level cache when CLUT format or tex window active
            int need_perpixel = tex_win_active ||
                                (tex_page_format == 0 || tex_page_format == 1);

            if (need_perpixel)
            {
                int clut_x = ((uv_clut >> 16) & 0x3F) * 16;
                int clut_y = (uv_clut >> 22) & 0x1FF;
                rect_clut_x = clut_x;
                rect_clut_y = clut_y;

                int result;
                if (prim_tex_cache_lookup(tex_page_format,
                                          tex_page_x, tex_page_y,
                                          clut_x, clut_y))
                {
                    result = PTCACHE.result;
                    cache_slot_x = PTCACHE.hw_tbp0;
                    cache_slot_y = PTCACHE.hw_cbp;
                    rect_csm = PTCACHE.csm;
                    rect_tex_cache_hit = 1;
                }
                else
                {
                    result = prim_tex_decode(tex_page_format,
                                             tex_page_x, tex_page_y,
                                             clut_x, clut_y,
                                             &cache_slot_x, &cache_slot_y);
                    rect_csm = PTCACHE.csm; /* consistent: read from cache */
                }
                if (result == 2 || result == 3)
                {
                    clut_decoded = 1;
                    rect_hw_clut = 1;
                    rect_hw_tbp0 = PTCACHE.hw_tbp0;
                    rect_hw_cbp = PTCACHE.hw_cbp;
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

                /* Reset CLAMP to REPEAT — flip path uses raw VRAM STQ coords,
                 * REGION_REPEAT from a previous HW CLUT prim would corrupt them */
                int need_reset_clamp = (!gs_state.valid || gs_state.clamp != 0);

                /* G4: State+PRIM in A+D (EOP=0), vertices in REGLIST */
                int state_qws_flip = 3 + (is_semi_trans ? 1 : 0) + (need_reset_clamp ? 1 : 0) + (is_raw_texture ? 2 : 0); /* DTHE + TEST + PRIM always */

                Push_GIF_Tag(GIF_TAG_LO(state_qws_flip, 0, 0, 0, 0, 1), GIF_REG_AD);
                Push_GIF_Data(GS_SET_DTHE(0), GS_REG_DTHE); // DTHE = 0

                if (is_semi_trans)
                    Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), GS_REG_ALPHA_1);
                if (need_reset_clamp)
                    Push_GIF_Data(0, GS_REG_CLAMP_1); /* REPEAT mode */

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

                /* REGLIST vertex packet: FLG=1, no PRE, NLOOP=4 (ST,RGBAQ,XYZ2) */
                {
                    uint64_t regs = (uint64_t)GIF_REG_ST | ((uint64_t)GIF_REG_RGBAQ << 4) | ((uint64_t)GIF_REG_XYZ2 << 8);
                    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 1, 3), regs);
                    uint64_t vdata[12];
                    /* TL */
                    vdata[0] = GS_SET_ST(s_l_bits, t_t_bits);
                    vdata[1] = rgbaq_q;
                    vdata[2] = GS_SET_XYZ(sgx0, sgy0, 0);
                    /* TR */
                    vdata[3] = GS_SET_ST(s_r_bits, t_t_bits);
                    vdata[4] = rgbaq_q;
                    vdata[5] = GS_SET_XYZ(sgx1, sgy0, 0);
                    /* BL */
                    vdata[6] = GS_SET_ST(s_l_bits, t_b_bits);
                    vdata[7] = rgbaq_q;
                    vdata[8] = GS_SET_XYZ(sgx0, sgy1, 0);
                    /* BR */
                    vdata[9] = GS_SET_ST(s_r_bits, t_b_bits);
                    vdata[10] = rgbaq_q;
                    vdata[11] = GS_SET_XYZ(sgx1, sgy1, 0);
                    for (int i = 0; i < 12; i += 2)
                        Push_GIF_Data(vdata[i], vdata[i + 1]);
                }

                /* Update lazy state — no restore needed */
                gs_state.dthe = 0;
                gs_state.clamp = 0; /* REPEAT */
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
                uint64_t want_clamp_r = 0;
                uint64_t want_texclut_r = 0;
                int need_texflush_r = 0;
                if (clut_decoded || is_raw_texture)
                {
                    if (rect_hw_clut)
                    {
                        int psm = (tex_page_format == 0) ? GS_PSM_4 : GS_PSM_8;
                        if (rect_csm) {
                            /* CSM2: TCC=1+TEXA for transparency, CLD=2 for TEXCLUT */
                            want_tex0_r = GS_SET_TEX0(rect_hw_tbp0, 4, psm, 8, 8,
                                                       1/*TCC*/, (is_raw_texture ? 1 : 0),
                                                       0/*CBA*/, GS_PSM_16S, 1/*CSM2*/, 0, 2/*CLD*/);
                            want_texclut_r = GS_SET_TEXCLUT(PSX_VRAM_FBW,
                                                             rect_clut_x / 16, rect_clut_y);
                        } else {
                            /* CSM1: CLD=1 always reload */
                            want_tex0_r = GS_SET_TEX0(rect_hw_tbp0, 4, psm, 8, 8,
                                                       1/*TCC*/, (is_raw_texture ? 1 : 0),
                                                       rect_hw_cbp, GS_PSM_16,
                                                       0/*CSM1*/, 0, 1/*CLD*/);
                        }
                        /* GS CLAMP_1 handles PSX texture window via REGION_REPEAT */
                        want_clamp_r = Compute_TexWin_Clamp();
                    }
                    else if (clut_decoded)
                    {
                        want_tex0_r = GS_SET_TEX0(4096, PSX_VRAM_FBW, GS_PSM_16S, 10, 10,
                                                   1, (is_raw_texture ? 1 : 0), 0, 0, 0, 0, 0);
                    }
                    else
                    {
                        want_tex0_r = GS_SET_TEX0(0, PSX_VRAM_FBW, GS_PSM_16S, 10, 9,
                                                   1, (is_raw_texture ? 1 : 0), 0, 0, 0, 0, 0);
                    }
                    need_texflush_r = !rect_tex_cache_hit;
                }

                int emit_dthe_r = (!gs_state.valid || gs_state.dthe != want_dthe_r);
                int emit_alpha_r = (is_semi_trans && (!gs_state.valid || gs_state.alpha != want_alpha_r));
                int emit_tex0_r = ((clut_decoded || is_raw_texture) &&
                                   (!gs_state.valid || gs_state.tex0 != want_tex0_r || need_texflush_r));
                int emit_test_r = (!gs_state.valid || gs_state.test != want_test_r);
                int emit_clamp_r = (rect_hw_clut && (!gs_state.valid || gs_state.clamp != want_clamp_r));
                int emit_texclut_r = (rect_hw_clut && rect_csm &&
                                      (!gs_state.valid || gs_state.texclut != want_texclut_r));
                int state_qws_r = emit_dthe_r + emit_alpha_r + emit_tex0_r + (emit_tex0_r && need_texflush_r) + emit_test_r + emit_clamp_r + emit_texclut_r;

                /* G4: State+PRIM in A+D, vertices in REGLIST */
                Push_GIF_Tag(GIF_TAG_LO(state_qws_r + 1, 0, 0, 0, 0, 1), GIF_REG_AD);

                if (emit_dthe_r)
                    Push_GIF_Data(0, GS_REG_DTHE);
                if (emit_alpha_r)
                    Push_GIF_Data(want_alpha_r, GS_REG_ALPHA_1);
                if (emit_clamp_r)
                    Push_GIF_Data(want_clamp_r, GS_REG_CLAMP_1);
                if (emit_texclut_r)
                    Push_GIF_Data(want_texclut_r, GS_REG_TEXCLUT);
                if (emit_tex0_r)
                {
                    Push_GIF_Data(want_tex0_r, GS_REG_TEX0);
                    if (need_texflush_r)
                        Push_GIF_Data(0, GS_REG_TEXFLUSH);
                }
                if (emit_test_r)
                    Push_GIF_Data(want_test_r, GS_REG_TEST_1);
                Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

                /* Update lazy tracking */
                if (!gs_state.valid)
                {
                    if (!is_semi_trans)
                        gs_state.alpha = ~0ULL;
                    if (!(clut_decoded || is_raw_texture))
                    {
                        gs_state.tex0 = ~0ULL;
                        gs_state.clamp = ~0ULL;
                    }
                }
                gs_state.dthe = want_dthe_r;
                if (is_semi_trans)
                    gs_state.alpha = want_alpha_r;
                if (clut_decoded || is_raw_texture)
                    gs_state.tex0 = want_tex0_r;
                if (rect_hw_clut)
                {
                    gs_state.clamp = want_clamp_r;
                    if (rect_csm)
                        gs_state.texclut = want_texclut_r;
                }
                gs_state.test = want_test_r;
                gs_state.valid = 1;

                /* REGLIST vertex packet: FLG=1, no PRE, NLOOP=2 */
                {
                    uint64_t regs = (uint64_t)GIF_REG_UV | ((uint64_t)GIF_REG_RGBAQ << 4) | ((uint64_t)GIF_REG_XYZ2 << 8);
                    Push_GIF_Tag(GIF_TAG_LO(2, 1, 0, 0, 1, 3), regs);
                    uint64_t vdata[6];
                    vdata[0] = GS_SET_XYZ(u0_gs << 4, v0_gs << 4, 0);
                    vdata[1] = rgbaq;
                    vdata[2] = GS_SET_XYZ(sgx0, sgy0, 0);
                    vdata[3] = GS_SET_XYZ(u1_gs << 4, v1_gs << 4, 0);
                    vdata[4] = rgbaq;
                    vdata[5] = GS_SET_XYZ(sgx1, sgy1, 0);
                    for (int i = 0; i < 6; i += 2)
                        Push_GIF_Data(vdata[i], vdata[i + 1]);
                }
                /* No state restore — lazy tracking handles next primitive */
#ifdef TEX_DEBUG_OVERLAY
                emit_debug_box(x, y, x + w, y + h,
                               tex_page_format,
                               (tex_page_x >> 6) + ((tex_page_y >> 8) << 4),
                               rect_hw_tbp0, rect_hw_cbp, "RECT");
#endif
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

            /* G4: State+PRIM in A+D, vertices in REGLIST */
            int state_qws_f = emit_dthe_f + emit_alpha_f;
            Push_GIF_Tag(GIF_TAG_LO(state_qws_f + 1, 0, 0, 0, 0, 1), GIF_REG_AD);

            if (emit_dthe_f)
                Push_GIF_Data(0, GS_REG_DTHE);
            if (emit_alpha_f)
                Push_GIF_Data(want_alpha_f, GS_REG_ALPHA_1);
            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(prim_reg), GS_REG_PRIM);

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
                gs_state.clamp = ~0ULL;
            }
            gs_state.valid = 1;

            /* REGLIST vertex packet: FLG=1, no PRE, NLOOP=2 */
            {
                uint64_t regs = (uint64_t)GIF_REG_RGBAQ | ((uint64_t)GIF_REG_XYZ2 << 4);
                Push_GIF_Tag(GIF_TAG_LO(2, 1, 0, 0, 1, 2), regs);
                Push_GIF_Data(rgbaq, GS_SET_XYZ(gx0, gy0, 0));
                Push_GIF_Data(rgbaq, GS_SET_XYZ(gx1, gy1, 0));
            }
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

        uint32_t r = color & 0xFF;
        uint32_t g = (color >> 8) & 0xFF;
        uint32_t b = (color >> 16) & 0xFF;
        int32_t x1 = (x + 2048) << 4;
        int32_t y1 = (y + 2048) << 4;
        int32_t x2 = (x + w + 2048) << 4;
        int32_t y2 = (y + h + 2048) << 4;

        /* G6: Skip SCISSOR expand/restore when fill fits within draw_clip */
        int clip_ok = (x >= draw_clip_x1) && ((x + w) <= draw_clip_x2) &&
                      (y >= draw_clip_y1) && ((y + h) <= draw_clip_y2);

        if (clip_ok) {
            /* Fill fits within current SCISSOR — 4 QW packet */
            Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), GIF_REG_AD);
            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(6), GS_REG_PRIM);
            Push_GIF_Data(GS_SET_RGBAQ(r, g, b, 0x80, 0x3F800000), GS_REG_RGBAQ);
            Push_GIF_Data(GS_SET_XYZ(x1, y1, 0), GS_REG_XYZ2);
            Push_GIF_Data(GS_SET_XYZ(x2, y2, 0), GS_REG_XYZ2);
        } else {
            /* Fill outside draw_clip — expand SCISSOR to full VRAM, then restore */
            Push_GIF_Tag(GIF_TAG_LO(5, 1, 0, 0, 0, 1), GIF_REG_AD);
            Push_GIF_Data(GS_SET_SCISSOR(0, PSX_VRAM_WIDTH - 1, 0, PSX_VRAM_HEIGHT - 1), GS_REG_SCISSOR_1);
            Push_GIF_Data(GS_PACK_PRIM_FROM_INT(6), GS_REG_PRIM);
            Push_GIF_Data(GS_SET_RGBAQ(r, g, b, 0x80, 0x3F800000), GS_REG_RGBAQ);
            Push_GIF_Data(GS_SET_XYZ(x1, y1, 0), GS_REG_XYZ2);
            Push_GIF_Data(GS_SET_XYZ(x2, y2, 0), GS_REG_XYZ2);

            /* Restore original scissor (PSX E4 is exclusive, GS SCISSOR is inclusive) */
            uint64_t sc_x2 = (draw_clip_x2 > 0) ? (draw_clip_x2 - 1) : 0;
            uint64_t sc_y2 = (draw_clip_y2 > 0) ? (draw_clip_y2 - 1) : 0;
            Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
            Push_GIF_Data(GS_SET_SCISSOR(draw_clip_x1, sc_x2, draw_clip_y1, sc_y2), GS_REG_SCISSOR_1);
        }

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
        /* G7: Lines only emit ALPHA_1 (semi-trans) + PRIM + vertices.
         * They don't touch TEX0/TEST/DTHE/CLAMP/TEXCLUT, so keep
         * gs_state valid and just update alpha when semi-trans. */
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

        /* G7: Update gs_state.alpha so next primitive's lazy check works */
        if (is_semi_trans)
            gs_state.alpha = Get_Alpha_Reg(semi_trans_mode);

        return idx;
    }
    return 1; /* Unknown command — consume 1 word */
}
