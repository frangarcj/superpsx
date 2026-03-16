/**
 * gpu_psp_vram.c — PSP VRAM transfer operations
 *
 * Handles HOST→LOCAL / LOCAL→HOST / LOCAL→LOCAL VRAM transfers
 * via CPU memcpy to/from EDRAM + shadow RAM synchronization.
 *
 * Equivalent to gpu_ps2_vram.c on the PS2 platform.
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "gpu_backend.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

#define VRAM_BASE_PTR ((void *)((uintptr_t)sceGeEdramGetAddr() + PSP_VRAM_OFFSET))

/* ── VRAM Transfer Operations ──────────────────────────────────── */

void GPU_Backend_StartVRAMTransfer(int x, int y, int w, int h) {
    vram_tx_x = x;
    vram_tx_y = y;
    vram_tx_w = w;
    vram_tx_h = h;
    vram_tx_pixel = 0;
}

void GPU_Backend_UploadShadowVRAM(int x, int y, int w, int h) {
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;
    /* In SEND mode, flush pending GE draws before CPU EDRAM write.
     * Otherwise deferred GE draws overwrite this data when executed. */
    GPU_Backend_Flush();
    for (int row = 0; row < h; row++) {
        uint16_t *src = &psx_vram_shadow[(y + row) * 1024 + x];
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + (y + row) * 1024 + x;
        memcpy(dst, src, w * 2);
    }
    /* Flush dcache so GE sees shadow→EDRAM copy */
    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    sceKernelDcacheWritebackRange(
        &edram_vram[y * 1024 + x],
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    /* Invalidate texture cache — shadow data changed in this region */
    Prim_InvalidateTexCache_Region(x, y, w, h);
}

void GPU_Backend_UploadRegionFast(uint32_t coords, uint32_t dims,
                                  uint32_t *data_ptr, uint32_t word_count) {
    int x = coords & 0xFFFF;
    int y = coords >> 16;
    int w = dims & 0xFFFF;
    int h = dims >> 16;
    if (w <= 0 || h <= 0) return;
    (void)word_count;
    GPU_Backend_Flush();

    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < h; row++) {
        int ry = (y + row) & 511;
        memcpy(&edram_vram[ry * 1024 + x], (uint16_t *)data_ptr + row * w, w * 2);
        if (psx_vram_shadow)
            memcpy(&psx_vram_shadow[ry * 1024 + x], (uint16_t *)data_ptr + row * w, w * 2);
    }

    sceKernelDcacheWritebackRange(
        &edram_vram[(y & 511) * 1024 + x],
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    Prim_InvalidateTexCache_Region(x, y, w, h);
}

void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    GPU_Backend_Flush();
    /* Copy within EDRAM PSX VRAM (GE render target) */
    for (int row = 0; row < h; row++) {
        uint16_t *src = (uint16_t *)VRAM_BASE_PTR + ((sy + row) & 511) * 1024 + (sx & 1023);
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + ((dy + row) & 511) * 1024 + (dx & 1023);
        memmove(dst, src, w * 2);
    }
    /* Keep shadow in sync */
    if (psx_vram_shadow) {
        for (int row = 0; row < h; row++) {
            uint16_t *src = &psx_vram_shadow[((sy + row) & 511) * 1024 + (sx & 1023)];
            uint16_t *dst = &psx_vram_shadow[((dy + row) & 511) * 1024 + (dx & 1023)];
            memmove(dst, src, w * 2);
        }
    }
    /* Flush dcache for destination region */
    uint16_t *edram_dst = (uint16_t *)VRAM_BASE_PTR + ((dy & 511) * 1024 + (dx & 1023));
    sceKernelDcacheWritebackRange(edram_dst,
        (uint32_t)(w * 2 + (h - 1) * 1024 * 2));

    Prim_InvalidateTexCache_Region(dx, dy, w, h);
}

void GPU_Backend_VRAMWrite(uint32_t word) {
    /* No-op: gpu_commands.c already writes to psx_vram_shadow and increments
     * vram_tx_pixel.  GPU_Backend_VRAMFlush() will copy shadow → EDRAM. */
    (void)word;
}

void GPU_Backend_VRAMFlush(void) {
    /* After A0 VRAM transfer completes, sync shadow RAM → EDRAM
     * so the data is available as a texture source for GE draws. */
    if (!psx_vram_shadow || vram_tx_w <= 0 || vram_tx_h <= 0) return;

    GPU_Backend_Flush();

    int tx = vram_tx_x, ty = vram_tx_y;
    int tw = vram_tx_w, th = vram_tx_h;

    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < th; row++) {
        int y = (ty + row) & 511;
        memcpy(&edram_vram[y * 1024 + tx],
               &psx_vram_shadow[y * 1024 + tx],
               tw * 2);
    }

    sceKernelDcacheWritebackRange(
        &edram_vram[ty * 1024 + tx],
        (uint32_t)(tw * 2 + (th - 1) * 1024 * 2));

    Prim_InvalidateTexCache_Region(tx, ty, tw, th);
}

void GPU_Backend_VRAMReadback(int x, int y, int w, int h) {
    /* Read from EDRAM PSX VRAM back to shadow for CPU access (C0 StoreImage). */
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;

    /* Need to finish pending GE draws first */
    Prim_FlushBatch();
    sceKernelDcacheWritebackRange(vpool_buf, vpool_offset);
    sceGuFinish();
    sceGuSendList(GU_TAIL, display_list, NULL);
    sceGuSync(0, 0);

    uint16_t *edram_vram = (uint16_t *)VRAM_BASE_PTR;
    for (int row = 0; row < h; row++) {
        int ry = (y + row) & 511;
        int rx = x & 1023;
        memcpy(&psx_vram_shadow[ry * 1024 + rx],
               &edram_vram[ry * 1024 + rx],
               w * 2);
    }

    /* Re-open display list for further draws (SEND mode) */
    vpool_offset = 0;
    sceGuStart(GU_SEND, display_list);
    sceGuDrawBufferList(GU_PSM_5551, (void *)PSP_VRAM_OFFSET, 1024);
    sceGuScissor(draw_clip_x1, draw_clip_y1,
                 draw_clip_x2 + 1, draw_clip_y2 + 1);
    sceGuEnable(GU_SCISSOR_TEST);
}

/* ── Debug VRAM Dump (stubs on PSP) ────────────────────────────── */

void DumpVRAM(const char *filename) { (void)filename; }
void DumpShadowVRAM(const char *filename) { (void)filename; }
