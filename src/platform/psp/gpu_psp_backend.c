/**
 * gpu_psp_backend.c — PSP GPU backend (stubs only)
 *
 * VU0 stubs and GIF stub variables referenced by shared code.
 * Actual GPU_Backend_* implementations are in:
 *   gpu_psp_core.c    — state, lifecycle, display
 *   gpu_psp_vram.c    — VRAM transfers
 *   gpu_psp_texture.c — texture / CLUT cache
 *   gpu_psp_dma.c     — GPU DMA
 */
#include "gpu_state.h"
#include "gpu_psp_state.h"

/* ── VU0 stubs (not available on PSP) ────────────────────────────── */
VU0JITCache vu0_jit_cache;
void vu0_prepare_mvmva(R3000CPU *cpu, uint32_t mx_cv) {
    (void)cpu; (void)mx_cv;
}
