/**
 * gpu_stub.c — No-op GPU implementations for HEADLESS builds.
 *
 * Compiled in place of all gpu_*.c files when cmake -DHEADLESS=ON is used.
 * Provides definitions for every GPU global variable and a no-op body for
 * every GPU public function so the rest of the emulator links and runs
 * without a display.
 */

#include "gpu_state.h"

/* ── Global variable definitions (externed in gpu_state.h) ──────────── */

uint32_t gpu_stat = 0x1C000000; /* GPU ready, display on */
uint32_t gpu_read = 0;
volatile int gpu_pending_vblank_flush = 0;

uint64_t gpu_estimated_pixels = 0;

int fb_address = 0;
int fb_width   = 640;
int fb_height  = 480;
int fb_psm     = 0;

unsigned __int128 gif_packet_buf[2][GIF_BUFFER_SIZE];
gif_qword_t *fast_gif_ptr       = NULL;
gif_qword_t *gif_buffer_end_safe = NULL;
int current_buffer = 0;

int draw_offset_x = 0;
int draw_offset_y = 0;
int draw_clip_x1  = 0;
int draw_clip_y1  = 0;
int draw_clip_x2  = 1023;
int draw_clip_y2  = 511;

int disp_range_y1 = 0;
int disp_range_y2 = 240;

int tex_page_x      = 0;
int tex_page_y      = 0;
int tex_page_format = 0;
int semi_trans_mode = 0;
int dither_enabled  = 0;

uint16_t *psx_vram_shadow = NULL;

int vram_tx_x = 0, vram_tx_y = 0, vram_tx_w = 0, vram_tx_h = 0, vram_tx_pixel = 0;

int vram_read_x = 0, vram_read_y = 0, vram_read_w = 0, vram_read_h = 0;
int vram_read_remaining = 0;
int vram_read_pixel     = 0;

int      polyline_active     = 0;
int      polyline_shaded     = 0;
int      polyline_semi_trans = 0;
uint32_t polyline_prev_color = 0;
uint32_t polyline_next_color = 0;
int16_t  polyline_prev_x    = 0;
int16_t  polyline_prev_y    = 0;
int      polyline_expect_color = 0;

int tex_flip_x = 0;
int tex_flip_y = 0;

int mask_set_bit   = 0;
int mask_check_bit = 0;
uint64_t cached_base_test = 0;

int gp1_allow_2mb = 0;

uint32_t tex_win_mask_x = 0;
uint32_t tex_win_mask_y = 0;
uint32_t tex_win_off_x  = 0;
uint32_t tex_win_off_y  = 0;

int      gpu_cmd_remaining = 0;
uint32_t gpu_cmd_buffer[16];
int      gpu_cmd_ptr        = 0;
int      gpu_transfer_words = 0;
int      gpu_transfer_total = 0;

unsigned __int128 buf_image[1024];
int buf_image_ptr = 0;

uint32_t vram_gen_counter = 0;

uint64_t gpu_busy_until = 0;

/* ── superpsx.h public GPU functions ────────────────────────────────── */

void Init_Graphics(void) {}

void GPU_WriteGP0(uint32_t data)  { (void)data; }
void GPU_WriteGP1(uint32_t data)  { (void)data; }

uint32_t GPU_Read(void)       { return 0; }
uint32_t GPU_ReadStatus(void) { return 0x1C000000; }

void GPU_VBlank(void) {}
void GPU_Flush(void)  {}

void GPU_DMA2(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    (void)madr; (void)bcr; (void)chcr;
}

/* ── gpu_core.c interface ───────────────────────────────────────────── */

void Update_GS_Display(void) {}

/* ── gpu_gif.c interface ────────────────────────────────────────────── */

void Flush_GIF(void) {}
void Flush_GIF_Sync(void) {}

/* Get_Base_TEST() is now static inline in gpu_state.h */
void     Setup_GS_Environment(void) {}

/* ── gpu_vram.c interface ───────────────────────────────────────────── */

void Start_VRAM_Transfer(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}

void Upload_Shadow_VRAM_Region(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}

uint16_t *GS_ReadbackRegion(int x, int y, int w_aligned, int h,
                             void *buf, int buf_qwc)
{
    (void)x; (void)y; (void)w_aligned; (void)h; (void)buf; (void)buf_qwc;
    return NULL;
}

void GS_UploadRegion(int x, int y, int w, int h, const uint16_t *pixels)
{
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
}

void GS_UploadRegionFast(uint32_t coords, uint32_t dims,
                         uint32_t *data_ptr, uint32_t word_count)
{
    (void)coords; (void)dims; (void)data_ptr; (void)word_count;
}

void DumpVRAM(const char *filename) { (void)filename; }

/* ── gpu_texture.c interface ────────────────────────────────────────── */

uint32_t Apply_Tex_Window_U(uint32_t u) { return u; }
uint32_t Apply_Tex_Window_V(uint32_t v) { return v; }

int Decode_CLUT4_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    (void)clut_x; (void)clut_y; (void)tex_x; (void)tex_y;
    (void)u0; (void)v0; (void)tw; (void)th;
    return 0;
}

int Decode_CLUT8_Texture(int clut_x, int clut_y, int tex_x, int tex_y,
                         int u0, int v0, int tw, int th)
{
    (void)clut_x; (void)clut_y; (void)tex_x; (void)tex_y;
    (void)u0; (void)v0; (void)tw; (void)th;
    return 0;
}

int Decode_TexWindow_Rect(int tex_format,
                          int tpx, int tpy,
                          int clut_x, int clut_y,
                          int u0_cmd, int v0_cmd, int w, int h,
                          int flip_x, int flip_y)
{
    (void)tex_format; (void)tpx; (void)tpy;
    (void)clut_x; (void)clut_y;
    (void)u0_cmd; (void)v0_cmd; (void)w; (void)h;
    (void)flip_x; (void)flip_y;
    return 0;
}

int Decode_TexPage_Cached(int tex_format,
                          int tpx, int tpy,
                          int clut_x, int clut_y,
                          int *out_slot_x, int *out_slot_y)
{
    (void)tex_format; (void)tpx; (void)tpy;
    (void)clut_x; (void)clut_y;
    if (out_slot_x) *out_slot_x = 0;
    if (out_slot_y) *out_slot_y = 0;
    return 0;
}

void Tex_Cache_DumpStats(void)  {}
void Tex_Cache_ResetStats(void) {}

void Tex_Cache_DirtyRegion(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}

/* ── gpu_primitives.c interface ─────────────────────────────────────── */

void Translate_GP0_to_GS(uint32_t *psx_cmd) { (void)psx_cmd; }
void Prim_InvalidateGSState(void) {}
void Prim_InvalidateTexCache(void) {}

void Emit_Line_Segment_AD(int16_t x0, int16_t y0, uint32_t color0,
                          int16_t x1, int16_t y1, uint32_t color1,
                          int is_shaded, int is_semi_trans)
{
    (void)x0; (void)y0; (void)color0;
    (void)x1; (void)y1; (void)color1;
    (void)is_shaded; (void)is_semi_trans;
}

/* ── gpu_commands.c interface ───────────────────────────────────────── */

int GPU_GetCommandSize(uint32_t cmd) { (void)cmd; return 1; }

void GPU_ProcessDmaBlock(uint32_t *data_ptr, uint32_t word_count)
{
    (void)data_ptr; (void)word_count;
}
