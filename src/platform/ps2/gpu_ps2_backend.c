/**
 * gpu_ps2_backend.c — GPU_Backend_* implementation for PS2
 *
 * Bridges the platform-agnostic gpu_commands.c (which calls GPU_Backend_*)
 * to the PS2-specific GS/GIF rendering layer.
 */
#include "gpu_backend.h"
#include "gpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

/* Forward declarations of existing PS2-specific functions
 * (implemented in gpu_ps2_core.c, gpu_ps2_gif.c, gpu_ps2_vram.c etc.) */
void Init_Graphics(void);
void Update_GS_Display(void);
void Setup_GS_Environment(void);
void Flush_GIF(void);
void Flush_GIF_Sync(void);
void GPU_VBlank(void);
void Start_VRAM_Transfer(int x, int y, int w, int h);
void Upload_Shadow_VRAM_Region(int x, int y, int w, int h);
void GS_UploadRegionFast(uint32_t coords, uint32_t dims,
                         uint32_t *data_ptr, uint32_t word_count);
void Prim_InvalidateGSState(void);
uint16_t *GS_ReadbackRegion(int x, int y, int w, int h,
                            void *buf, int buf_qwc);

/* ── Lifecycle ───────────────────────────────────────────────────── */

void GPU_Backend_Init(void)            { Init_Graphics(); }
void GPU_Backend_Flush(void)           { Flush_GIF(); }
void GPU_Backend_FlushSync(void)       { Flush_GIF_Sync(); }
void GPU_Backend_SetupEnvironment(void){ Setup_GS_Environment(); }
void GPU_Backend_UpdateDisplay(void)   { Update_GS_Display(); }
void GPU_Backend_VBlank(void)          { GPU_VBlank(); }

/* ── VRAM write streaming (STP bit + qword packing for GS IMAGE) ── */

static unsigned __int128 _vram_buf[1024] __attribute__((aligned(16)));
static int _vram_buf_ptr = 0;
static uint32_t _vram_pending[4];
static int _vram_pending_cnt = 0;

static void _vram_flush_partial(int eop)
{
    if (_vram_buf_ptr <= 0)
        return;
    Push_GIF_Tag(GIF_TAG_LO(_vram_buf_ptr, eop, 0, 0, 2, 0), 0);
    for (int i = 0; i < _vram_buf_ptr; i++)
    {
        uint64_t *p = (uint64_t *)&_vram_buf[i];
        Push_GIF_Data(p[0], p[1]);
    }
    _vram_buf_ptr = 0;
}

void GPU_Backend_VRAMWrite(uint32_t word)
{
    /* Apply STP bit: GS CT16S bit-15 flags opaque non-zero pixels */
    uint16_t gs_p0 = (uint16_t)(word & 0xFFFF);
    uint16_t gs_p1 = (uint16_t)(word >> 16);
    gs_p0 |= (-(gs_p0 != 0)) & 0x8000;
    gs_p1 |= (-(gs_p1 != 0)) & 0x8000;
    _vram_pending[_vram_pending_cnt++] =
        (uint32_t)gs_p0 | ((uint32_t)gs_p1 << 16);

    if (_vram_pending_cnt >= 4)
    {
        uint64_t lo = (uint64_t)_vram_pending[0] |
                      ((uint64_t)_vram_pending[1] << 32);
        uint64_t hi = (uint64_t)_vram_pending[2] |
                      ((uint64_t)_vram_pending[3] << 32);
        _vram_buf[_vram_buf_ptr++] =
            (unsigned __int128)lo | ((unsigned __int128)hi << 64);
        _vram_pending_cnt = 0;

        if (_vram_buf_ptr >= 1000)
            _vram_flush_partial(0);
    }
}

void GPU_Backend_VRAMFlush(void)
{
    /* Pad last qword */
    if (_vram_pending_cnt > 0)
    {
        while (_vram_pending_cnt < 4)
            _vram_pending[_vram_pending_cnt++] = 0;
        uint64_t lo = (uint64_t)_vram_pending[0] |
                      ((uint64_t)_vram_pending[1] << 32);
        uint64_t hi = (uint64_t)_vram_pending[2] |
                      ((uint64_t)_vram_pending[3] << 32);
        _vram_buf[_vram_buf_ptr++] =
            (unsigned __int128)lo | ((unsigned __int128)hi << 64);
        _vram_pending_cnt = 0;
    }

    if (_vram_buf_ptr > 0)
        _vram_flush_partial(1); /* EOP=1 on final packet */

    /* TEXFLUSH after upload */
    Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH);
    Flush_GIF();
    _vram_pending_cnt = 0;
    _vram_buf_ptr = 0;
}

/* ── VRAM readback ───────────────────────────────────────────────── */

void GPU_Backend_VRAMReadback(int x, int y, int w, int h)
{
    if (!psx_vram_shadow || w <= 0 || h <= 0)
        return;

    /* Flush any pending GIF packets so GS VRAM is up-to-date */
    if (fast_gif_ptr != (gif_qword_t *)&gif_packet_buf[current_buffer][0])
        Flush_GIF();

    int w_aligned = (w + 7) & ~7;
    int total_pixels = w_aligned * h;
    int buf_bytes = total_pixels * 2; /* 16bpp */
    int buf_qwc = (buf_bytes + 15) / 16;
    void *rb_buf = memalign(64, buf_qwc * 16);
    if (!rb_buf)
        return;

    uint16_t *pixels = GS_ReadbackRegion(x, y, w_aligned, h, rb_buf, buf_qwc);
    /* Copy readback data into psx_vram_shadow.
     * GS CT16S stores STP in bit 15 with different semantics
     * than PSX: our FillRect writes alpha=0x80 → STP=1 even
     * for black pixels, but on PSX, FillRect(000000) produces
     * raw 0x0000.  Strip bit 15 so the shadow matches PSX
     * behaviour: 0x0000 = transparent, non-zero = visible. */
    if (pixels)
    {
        for (int row = 0; row < h; row++)
        {
            int dy = y + row;
            if (dy >= 512)
                dy -= 512;
            for (int col = 0; col < w; col++)
            {
                int dx = x + col;
                if (dx >= 1024)
                    dx -= 1024;
                uint16_t px = pixels[row * w_aligned + col] & 0x7FFF;
                psx_vram_shadow[dy * 1024 + dx] = px;
            }
        }
    }
    free(rb_buf);
}

/* ── Drawing environment ─────────────────────────────────────────── */

void GPU_Backend_SetScissor(int x1, int y1, int x2, int y2)
{
    Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
    uint64_t scax1 = (x2 > 0) ? (x2 - 1) : 0;
    uint64_t scay1 = (y2 > 0) ? (y2 - 1) : 0;
    Push_GIF_Data(GS_SET_SCISSOR(x1, scax1, y1, scay1), GS_REG_SCISSOR_1);
}

void GPU_Backend_SetDisplayFB(int x, int y)
{
    uint64_t dispfb = 0;
    dispfb |= (uint64_t)PSX_VRAM_FBW << 9;
    dispfb |= (uint64_t)PSX_VRAM_PSM << 15;
    dispfb |= (uint64_t)x << 32;
    dispfb |= (uint64_t)y << 43;
    *((volatile uint64_t *)0x12000070) = dispfb;
    *((volatile uint64_t *)0x12000090) = dispfb;
}

void GPU_Backend_SetResolution(int interlace, int mode)
{
    SetGsCrt(interlace, mode, 0);
}

void GPU_Backend_SetMaskBit(int set, int check)
{
    Push_GIF_Tag(GIF_TAG_LO(2, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_FBA(set), GS_REG_FBA_1);
    Push_GIF_Data(Get_Base_TEST(), GS_REG_TEST_1);
    (void)check;
}

void GPU_Backend_ClearVRAM(int clip_x1, int clip_y1,
                           int clip_x2, int clip_y2)
{
    /* Full-VRAM scissor sprite */
    Push_GIF_Tag(GIF_TAG_LO(5, 1, 0, 0, 0, 1), GIF_REG_AD);
    Push_GIF_Data(GS_SET_SCISSOR(0, PSX_VRAM_WIDTH - 1,
                                 0, PSX_VRAM_HEIGHT - 1),
                  GS_REG_SCISSOR_1);
    Push_GIF_Data(GS_PACK_PRIM_FROM_INT(6), GS_REG_PRIM);
    Push_GIF_Data(GS_SET_RGBAQ(0, 0, 0, 0, 0x3F800000), GS_REG_RGBAQ);
    int32_t x1 = (0 + 2048) << 4, y1 = (0 + 2048) << 4;
    int32_t x2 = (PSX_VRAM_WIDTH + 2048) << 4;
    int32_t y2 = (PSX_VRAM_HEIGHT + 2048) << 4;
    Push_GIF_Data(GS_SET_XYZ(x1, y1, 0), GS_REG_XYZ2);
    Push_GIF_Data(GS_SET_XYZ(x2, y2, 0), GS_REG_XYZ2);
    Flush_GIF();

    /* Restore scissor to current drawing area */
    Push_GIF_Tag(GIF_TAG_LO(1, 1, 0, 0, 0, 1), GIF_REG_AD);
    uint64_t sc_x2 = (clip_x2 > 0) ? (clip_x2 - 1) : 0;
    uint64_t sc_y2 = (clip_y2 > 0) ? (clip_y2 - 1) : 0;
    Push_GIF_Data(GS_SET_SCISSOR(clip_x1, sc_x2, clip_y1, sc_y2),
                  GS_REG_SCISSOR_1);
    Flush_GIF();
}

/* ── Standard VRAM operations ────────────────────────────────────── */

void GPU_Backend_StartVRAMTransfer(int x, int y, int w, int h)
{
    Start_VRAM_Transfer(x, y, w, h);
}

void GPU_Backend_UploadShadowVRAM(int x, int y, int w, int h)
{
    Upload_Shadow_VRAM_Region(x, y, w, h);
}

void GPU_Backend_UploadRegionFast(uint32_t coords, uint32_t dims,
                                  uint32_t *data_ptr, uint32_t word_count)
{
    GS_UploadRegionFast(coords, dims, data_ptr, word_count);
}

void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h)
{
    (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
    /* VRAM-to-VRAM copy handled via shadow VRAM + re-upload for now */
}

/* ── State management ────────────────────────────────────────────── */

void GPU_Backend_InvalidateState(void)
{
    Prim_InvalidateGSState();
}
