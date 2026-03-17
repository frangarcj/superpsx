/**
 * gpu_psp_vram.c — PSP VRAM transfer operations
 *
 * Handles HOST→LOCAL / LOCAL→HOST / LOCAL→LOCAL VRAM transfers
 * via GE BitBlt (sceGuCopyImage) or CPU fallback.
 *
 * All operations handle PSX VRAM 1024x512 coordinate wrapping.
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"
#include "gpu_backend.h"
#include <pspgu.h>
#include <pspge.h>
#include <psputils.h>
#include <string.h>

#define VRAM_BASE_PTR ((void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000) + PSP_VRAM_OFFSET))

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
    GPU_Backend_Flush();
    for (int row = 0; row < h; row++) {
        int ry = (y + row) & 511;
        uint16_t *src = &psx_vram_shadow[ry * 1024 + (x & 1023)];
        uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + ry * 1024 + (x & 1023);
        memcpy(dst, src, w * 2);
    }
    /* Flush dcache so GE sees shadow→EDRAM copy. 
     * Since it might wrap, it's safer to just flush the whole region if wrapping is complex,
     * but here we just flush the bounding box which covers the wrap. */
    sceKernelDcacheWritebackRange(VRAM_BASE_PTR, 1024 * 512 * 2);
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

    /* Use GE to copy from RAM to EDRAM. Split if it wraps 512 rows. */
    for (int row = 0; row < h; ) {
        int ry = (y + row) & 511;
        int rows_to_edge = 512 - ry;
        int current_h = (h - row < rows_to_edge) ? h - row : rows_to_edge;

        if ((x & 7) == 0 && (w & 7) == 0) {
            sceGuCopyImage(GU_PSM_5551, 0, 0, w, current_h, w, (uint16_t *)data_ptr + row * w, x & 1023, ry, 1024, VRAM_BASE_PTR);
        } else {
            GPU_Backend_Flush();
            uint16_t *dst = (uint16_t *)VRAM_BASE_PTR + ry * 1024 + (x & 1023);
            memcpy(dst, (uint16_t *)data_ptr + row * w, w * 2 * current_h);
        }
        row += current_h;
    }

    /* Keep shadow in sync (CPU write) */
    if (psx_vram_shadow) {
        for (int row = 0; row < h; row++) {
            int ry = (y + row) & 511;
            memcpy(&psx_vram_shadow[ry * 1024 + (x & 1023)], (uint16_t *)data_ptr + row * w, w * 2);
        }
    }

    Prim_InvalidateTexCache_Region(x, y, w, h);
}

void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;

    /* PSX VRAM wraps 1024x512. Split if we cross the 512-row boundary. */
    for (int row = 0; row < h; ) {
        int sy_cur = (sy + row) & 511;
        int dy_cur = (dy + row) & 511;
        int rows_to_edge = 512 - sy_cur;
        if (512 - dy_cur < rows_to_edge) rows_to_edge = 512 - dy_cur;
        int current_h = (h - row < rows_to_edge) ? h - row : rows_to_edge;

        /* Hardware BitBlt if aligned. Otherwise fallback to CPU. */
        if (((sx | dx) & 7) == 0 && (w & 7) == 0) {
            sceGuCopyImage(GU_PSM_5551, sx & 1023, sy_cur, w, current_h, 1024, VRAM_BASE_PTR, dx & 1023, dy_cur, 1024, VRAM_BASE_PTR);
        } else {
            GPU_Backend_Flush();
            uint16_t *vram = (uint16_t *)VRAM_BASE_PTR;
            for (int r = 0; r < current_h; r++) {
                memmove(&vram[(dy_cur + r) * 1024 + (dx & 1023)],
                        &vram[(sy_cur + r) * 1024 + (sx & 1023)], w * 2);
            }
        }
        row += current_h;
    }

    /* Keep shadow in sync via CPU */
    if (psx_vram_shadow) {
        for (int row = 0; row < h; row++) {
            uint16_t *src = &psx_vram_shadow[((sy + row) & 511) * 1024 + (sx & 1023)];
            uint16_t *dst = &psx_vram_shadow[((dy + row) & 511) * 1024 + (dx & 1023)];
            memmove(dst, src, w * 2);
        }
    }

    Prim_InvalidateTexCache_Region(dx, dy, w, h);
}

void GPU_Backend_VRAMWrite(uint32_t word) {
    /* No-op: handled by gpu_commands.c + VRAMFlush */
    (void)word;
}

void GPU_Backend_VRAMFlush(void) {
    if (!psx_vram_shadow || vram_tx_w <= 0 || vram_tx_h <= 0) return;

    int tx = vram_tx_x, ty = vram_tx_y;
    int tw = vram_tx_w, th = vram_tx_h;

    /* Expand to 8-pixel boundaries for GE performance (16-byte alignment) */
    int x1 = tx & ~7;
    int x2 = (tx + tw + 7) & ~7;
    int aligned_w = x2 - x1;
    if (x1 + aligned_w > 1024) aligned_w = 1024 - x1;

    for (int row = 0; row < th; ) {
        int ry = (ty + row) & 511;
        int rows_to_edge = 512 - ry;
        int current_h = (th - row < rows_to_edge) ? th - row : rows_to_edge;

        /* Always use GE BitBlt for Flush. It handles unaligned, but aligned is faster. */
        sceKernelDcacheWritebackRange(&psx_vram_shadow[ry * 1024 + x1], aligned_w * 2 + (current_h - 1) * 1024 * 2);
        sceGuCopyImage(GU_PSM_5551, x1, ry, aligned_w, current_h, 1024, psx_vram_shadow, x1, ry, 1024, VRAM_BASE_PTR);
        
        row += current_h;
    }

    Prim_InvalidateTexCache_Region(tx, ty, tw, th);
}

void GPU_Backend_VRAMReadback(int x, int y, int w, int h) {
    if (!psx_vram_shadow || w <= 0 || h <= 0) return;

    GPU_Backend_Flush();

    /* Expand to 8-pixel boundaries for GE performance */
    int x1 = x & ~7;
    int x2 = (x + w + 7) & ~7;
    int aligned_w = x2 - x1;
    if (x1 + aligned_w > 1024) aligned_w = 1024 - x1;

    for (int row = 0; row < h; ) {
        int ry = (y + row) & 511;
        int rows_to_edge = 512 - ry;
        int current_h = (h - row < rows_to_edge) ? h - row : rows_to_edge;

        /* GE Readback: EDRAM -> RAM */
        sceGuCopyImage(GU_PSM_5551, x1, ry, aligned_w, current_h, 1024, VRAM_BASE_PTR, x1, ry, 1024, psx_vram_shadow);
        row += current_h;
    }

    /* Wait for GE to finish and invalidate dcache so CPU sees new data */
    GPU_Backend_FlushSync();
    sceKernelDcacheInvalidateRange(&psx_vram_shadow[(y & 511) * 1024 + x1], aligned_w * 2 + (h - 1) * 1024 * 2);
}

void DumpVRAM(const char *filename) { (void)filename; }
void DumpShadowVRAM(const char *filename) { (void)filename; }
