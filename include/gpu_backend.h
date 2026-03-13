#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

#include <stdint.h>

/* ── Renderer lifecycle ──────────────────────────────────────────── */
void GPU_Backend_Init(void);
void GPU_Backend_Flush(void);
void GPU_Backend_FlushSync(void);
void GPU_Backend_SetupEnvironment(void);
void GPU_Backend_UpdateDisplay(void);
void GPU_Backend_VBlank(void);

/* ── VRAM operations ─────────────────────────────────────────────── */
void GPU_Backend_StartVRAMTransfer(int x, int y, int w, int h);
void GPU_Backend_UploadShadowVRAM(int x, int y, int w, int h);
void GPU_Backend_UploadRegionFast(uint32_t coords, uint32_t dims,
                                  uint32_t *data_ptr, uint32_t word_count);
void GPU_Backend_VRAMCopy(int sx, int sy, int dx, int dy, int w, int h);

void GPU_Backend_VRAMWrite(uint32_t word);
void GPU_Backend_VRAMFlush(void);

void GPU_Backend_VRAMReadback(int x, int y, int w, int h);

/* ── Drawing environment ─────────────────────────────────────────── */
void GPU_Backend_SetScissor(int x1, int y1, int x2, int y2);
void GPU_Backend_SetDisplayFB(int x, int y);
void GPU_Backend_SetResolution(int interlace, int mode);
void GPU_Backend_SetMaskBit(int set, int check);
void GPU_Backend_ClearVRAM(int clip_x1, int clip_y1,
                           int clip_x2, int clip_y2);

/* ── State management ────────────────────────────────────────────── */
void GPU_Backend_InvalidateState(void);

#endif /* GPU_BACKEND_H */
