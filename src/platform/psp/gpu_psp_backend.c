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
#include "gte_vfpu.h"

/* ── VU0 JIT cache (reused for VFPU on PSP) ───────────────────────── */
VU0JITCache vu0_jit_cache __attribute__((aligned(16)));

/* Called from JIT via emit_call_c_lite: refresh VFPU matrix cache and
 * copy matrix rows + translation into vu0_jit_cache for lv.q access.
 * mx_cv = mx | (cv << 2).
 * Layout matches VU0JITCache: col1=row1(0), col2=row2(16), col3=row3(32), trans(48). */
void vu0_prepare_mvmva(R3000CPU *cpu, uint32_t mx_cv)
{
    int mx = mx_cv & 3, cv = mx_cv >> 2;

    /* Select and refresh matrix rows */
    float *row1, *row2, *row3;
    switch (mx)
    {
    case 0:
        if (vfpu_rt_is_dirty(cpu))
            vfpu_refresh_rt_matrix(cpu);
        row1 = vfpu_rt_row1;
        row2 = vfpu_rt_row2;
        row3 = vfpu_rt_row3;
        break;
    case 1:
        if (vfpu_lt_is_dirty(cpu))
            vfpu_refresh_lt_matrix(cpu);
        row1 = vfpu_lt_row1;
        row2 = vfpu_lt_row2;
        row3 = vfpu_lt_row3;
        break;
    default:
        if (vfpu_lc_is_dirty(cpu))
            vfpu_refresh_lc_matrix(cpu);
        row1 = vfpu_lc_row1;
        row2 = vfpu_lc_row2;
        row3 = vfpu_lc_row3;
        break;
    }

    /* Select and refresh translation vector */
    float *trans;
    switch (cv)
    {
    case 0:
        if (mx != 0 && vfpu_rt_is_dirty(cpu))
            vfpu_refresh_rt_matrix(cpu);
        trans = vfpu_rt_trans;
        break;
    case 1:
        if (vfpu_bk_is_dirty(cpu))
            vfpu_refresh_bk_trans(cpu);
        trans = vfpu_bk_trans;
        break;
    default:
        trans = vfpu_zero_trans;
        break;
    }

    /* Copy to contiguous JIT cache for lv.q access */
    for (int i = 0; i < 4; i++)
    {
        vu0_jit_cache.col1[i] = row1[i];
        vu0_jit_cache.col2[i] = row2[i];
        vu0_jit_cache.col3[i] = row3[i];
        vu0_jit_cache.trans[i] = trans[i];
    }
}
