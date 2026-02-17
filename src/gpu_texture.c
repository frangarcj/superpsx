/**
 * gpu_texture.c — CLUT texture decode (4-bit / 8-bit)
 *
 * When PSX uses 4-bit or 8-bit CLUT textures, GS cannot directly decode
 * them from CT16S VRAM.  We read back the packed texture + CLUT, decode
 * in software, and upload the decoded 16-bit pixels to a temporary GS
 * VRAM area at Y=512.
 */
#include "gpu_state.h"

/* ── Texture window coordinate transform ─────────────────────────── */

// Apply PSX texture window formula to a texture coordinate
// texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
uint32_t Apply_Tex_Window_U(uint32_t u)
{
    if (tex_win_mask_x == 0)
        return u;
    uint32_t mask = tex_win_mask_x * 8;
    uint32_t off = (tex_win_off_x & tex_win_mask_x) * 8;
    return (u & ~mask) | off;
}

uint32_t Apply_Tex_Window_V(uint32_t v)
{
    if (tex_win_mask_y == 0)
        return v;
    uint32_t mask = tex_win_mask_y * 8;
    uint32_t off = (tex_win_off_y & tex_win_mask_y) * 8;
    return (v & ~mask) | off;
}

/* ── Per-pixel texture window decode ──────────────────────────────── */

// Decode a textured rect region with per-pixel texture window masking.
// This correctly handles texture repeat/mirror that GS SPRITE cannot do.
// tex_format: 0=4BPP, 1=8BPP, 2=15BPP
// Reads from psx_vram_shadow (CPU shadow copy).
// Uploads result to CLUT_DECODED area.
int Decode_TexWindow_Rect(int tex_format,
                          int tex_page_x, int tex_page_y,
                          int clut_x, int clut_y,
                          int u0_cmd, int v0_cmd, int w, int h,
                          int flip_x, int flip_y)
{
    uint16_t *decoded = (uint16_t *)memalign(64, w * h * 2);
    if (!decoded)
        return 0;

    for (int row = 0; row < h; row++)
    {
        for (int col = 0; col < w; col++)
        {
            // Per-pixel UV with texture window applied
            int u_iter = flip_x ? (u0_cmd - col) : (u0_cmd + col);
            int v_iter = flip_y ? (v0_cmd - row) : (v0_cmd + row);
            uint32_t u_win = Apply_Tex_Window_U(u_iter & 0xFF);
            uint32_t v_win = Apply_Tex_Window_V(v_iter & 0xFF);

            uint16_t pixel;
            if (tex_format == 0) // 4BPP CLUT
            {
                int hw_x = tex_page_x + u_win / 4;
                int nibble = u_win % 4;
                uint16_t packed = psx_vram_shadow[(tex_page_y + v_win) * 1024 + hw_x];
                int idx = (packed >> (nibble * 4)) & 0xF;
                pixel = psx_vram_shadow[clut_y * 1024 + clut_x + idx];
            }
            else if (tex_format == 1) // 8BPP CLUT
            {
                int hw_x = tex_page_x + u_win / 2;
                int byte_idx = u_win % 2;
                uint16_t packed = psx_vram_shadow[(tex_page_y + v_win) * 1024 + hw_x];
                int idx = (packed >> (byte_idx * 8)) & 0xFF;
                pixel = psx_vram_shadow[clut_y * 1024 + clut_x + idx];
            }
            else // 15BPP (format 2 or 3)
            {
                pixel = psx_vram_shadow[(tex_page_y + v_win) * 1024 + (tex_page_x + u_win)];
            }

            // STP bit: only 0x0000 is transparent
            if (pixel != 0)
                pixel |= 0x8000;
            decoded[row * w + col] = pixel;
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, w, h, decoded);
    free(decoded);
    return 1;
}

/* ── 4-bit CLUT texture decode ───────────────────────────────────── */

// Decode a 4-bit CLUT texture region and upload to GS VRAM at CLUT_DECODED_Y.
// clut_x, clut_y: CLUT position in PSX VRAM (16 entries for 4-bit)
// tex_x, tex_y: texture page position in PSX VRAM (in halfword coords)
// u0, v0: start UV, tw, th: size to decode (in texel coords)
// Returns 1 on success, 0 on failure.
int Decode_CLUT4_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    // 4-bit mode: each halfword at (tex_x + u/4, tex_y + v) holds 4 nibbles
    // Nibble index = u % 4, from LSB: bits [3:0],[7:4],[11:8],[15:12]
    int hw_x0 = u0 / 4;
    int hw_x1 = (u0 + tw + 3) / 4;
    int hw_w = hw_x1 - hw_x0;
    int rb_x = tex_x + hw_x0;
    int rb_y = tex_y + v0;
    int rb_w = hw_w;
    int rb_h = th;

    int clut_rb_w = 16;

    // Align widths to 8 for qword boundary
    int rb_w_aligned = (rb_w + 7) & ~7;
    int clut_w_aligned = (clut_rb_w + 7) & ~7;

    int tex_bytes = rb_w_aligned * rb_h * 2;
    int clut_bytes = clut_w_aligned * 1 * 2;
    int tex_qwc = (tex_bytes + 15) / 16;
    int clut_qwc = (clut_bytes + 15) / 16;

    void *tex_buf = memalign(64, tex_qwc * 16);
    void *clut_buf = memalign(64, clut_qwc * 16);
    if (!tex_buf || !clut_buf)
    {
        if (tex_buf)
            free(tex_buf);
        if (clut_buf)
            free(clut_buf);
        return 0;
    }

    uint16_t *clut_uc = GS_ReadbackRegion(clut_x, clut_y, clut_w_aligned, 1, clut_buf, clut_qwc);
    uint16_t *tex_uc = GS_ReadbackRegion(rb_x, rb_y, rb_w_aligned, rb_h, tex_buf, tex_qwc);

    uint16_t *decoded = (uint16_t *)memalign(64, tw * th * 2);
    if (!decoded)
    {
        free(tex_buf);
        free(clut_buf);
        return 0;
    }

    for (int row = 0; row < th; row++)
    {
        for (int col = 0; col < tw; col++)
        {
            int texel_u = u0 + col;
            int hw_col = texel_u / 4 - hw_x0;
            int nibble = texel_u % 4;
            uint16_t packed = tex_uc[row * rb_w_aligned + hw_col];
            int idx = (packed >> (nibble * 4)) & 0xF;
            uint16_t cv = clut_uc[idx];
            if (cv != 0)
                cv |= 0x8000;
            decoded[row * tw + col] = cv;
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, tw, th, decoded);

    free(decoded);
    free(tex_buf);
    free(clut_buf);
    return 1;
}

/* ── 8-bit CLUT texture decode ───────────────────────────────────── */

int Decode_CLUT8_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    int hw_x0 = u0 / 2;
    int hw_x1 = (u0 + tw + 1) / 2;
    int hw_w = hw_x1 - hw_x0;
    int rb_x = tex_x + hw_x0;
    int rb_y = tex_y + v0;
    int rb_w = hw_w;
    int rb_h = th;

    int clut_rb_w = 256;
    int rb_w_aligned = (rb_w + 7) & ~7;
    int clut_w_aligned = (clut_rb_w + 7) & ~7;

    int tex_bytes = rb_w_aligned * rb_h * 2;
    int clut_bytes = clut_w_aligned * 1 * 2;
    int tex_qwc = (tex_bytes + 15) / 16;
    int clut_qwc = (clut_bytes + 15) / 16;

    void *tex_buf = memalign(64, tex_qwc * 16);
    void *clut_buf = memalign(64, clut_qwc * 16);
    if (!tex_buf || !clut_buf)
    {
        if (tex_buf)
            free(tex_buf);
        if (clut_buf)
            free(clut_buf);
        return 0;
    }

    uint16_t *clut_uc = GS_ReadbackRegion(clut_x, clut_y, clut_w_aligned, 1, clut_buf, clut_qwc);
    uint16_t *tex_uc = GS_ReadbackRegion(rb_x, rb_y, rb_w_aligned, rb_h, tex_buf, tex_qwc);

    uint16_t *decoded = (uint16_t *)memalign(64, tw * th * 2);
    if (!decoded)
    {
        free(tex_buf);
        free(clut_buf);
        return 0;
    }

    for (int row = 0; row < th; row++)
    {
        for (int col = 0; col < tw; col++)
        {
            int texel_u = u0 + col;
            int hw_col = texel_u / 2 - hw_x0;
            int byte_idx = texel_u % 2;
            uint16_t packed = tex_uc[row * rb_w_aligned + hw_col];
            int idx = (packed >> (byte_idx * 8)) & 0xFF;
            uint16_t cv = clut_uc[idx];
            if (cv != 0)
                cv |= 0x8000;
            decoded[row * tw + col] = cv;
        }
    }

    GS_UploadRegion(CLUT_DECODED_X, CLUT_DECODED_Y, tw, th, decoded);

    free(decoded);
    free(tex_buf);
    free(clut_buf);
    return 1;
}
