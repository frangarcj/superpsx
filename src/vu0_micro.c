/*
 * vu0_micro.c — VU0 Micro Program Library for GTE Acceleration
 *
 * Pre-assembled VU0 micro programs (verified via dvp-as).
 * Uploaded to VU0 micro memory at init.
 * Data exchange via VU0 data memory (0x11000000).
 */
#include "vu0_micro.h"

#ifdef _EE

#include <string.h>

/* ====================================================================
 * Micro Programs — assembled with dvp-as, verified binary
 *
 * Format: pairs of {lower_word, upper_word} per VU instruction.
 * In VU micro memory (little-endian):
 *   address N+0: lower 32 bits
 *   address N+4: upper 32 bits
 * ==================================================================== */

/* VU constants for readability */
#define VU_NOP_UPPER  0x000002FFu   /* NOP (upper pipe) */
#define VU_NOP_LOWER  0x8000033Cu   /* NOP (lower pipe) */
#define VU_NOP_E      0x400002FFu   /* NOP with E bit (end program) */

/*
 * mvmva_full: Load matrix from VU data mem + multiply vertex + store result
 * Entry: VCALLMS 0x000  (byte offset 0)
 *
 * Input (VU data mem):
 *   QW[0]  = vertex (int32 x, y, z, 0)
 *   QW[1]  = matrix col1 (float, pre-scaled /4096)
 *   QW[2]  = matrix col2
 *   QW[3]  = matrix col3
 *   QW[4]  = translation (float)
 *
 * Output (VU data mem):
 *   QW[15] = MAC1, MAC2, MAC3, 0 (int32)
 *
 * VF register state after execution:
 *   VF1-VF4 = matrix cols + translation (persist for mvmva_core reuse)
 *   VF6     = result as float (before FTOI)
 *   VF7     = result as int32
 *
 * 17 instructions × 8 bytes = 136 bytes
 */
static const uint32_t vu0_prog_mvmva_full[] = {
    /* insn  0: NOP                / LQ.xyzw VF1, 1(VI0)  — load col1 */
    0x01E10001, VU_NOP_UPPER,
    /* insn  1: NOP                / LQ.xyzw VF2, 2(VI0)  — load col2 */
    0x01E20002, VU_NOP_UPPER,
    /* insn  2: NOP                / LQ.xyzw VF3, 3(VI0)  — load col3 */
    0x01E30003, VU_NOP_UPPER,
    /* insn  3: NOP                / LQ.xyzw VF4, 4(VI0)  — load trans */
    0x01E40004, VU_NOP_UPPER,
    /* insn  4: NOP                / LQ.xyzw VF5, 0(VI0)  — load vertex */
    0x01E50000, VU_NOP_UPPER,
    /* insn  5: ITOF0.xyzw VF5,VF5 / NOP                  — int → float */
    VU_NOP_LOWER, 0x01E5293Cu,
    /* insn  6: NOP                / NOP                   — ITOF latency */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn  7: NOP                / NOP                   — pipeline */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn  8: MULAx.xyz ACC,VF1,VF5 / NOP              — ACC = col1 × vx */
    VU_NOP_LOWER, 0x01C509BCu,
    /* insn  9: MADDAy.xyz ACC,VF2,VF5 / NOP             — ACC += col2 × vy */
    VU_NOP_LOWER, 0x01C510BDu,
    /* insn 10: MADDz.xyz VF6,VF3,VF5 / NOP              — VF6 = ACC + col3 × vz */
    VU_NOP_LOWER, 0x01C5198Au,
    /* insn 11: ADD.xyz VF6,VF6,VF4    / NOP              — VF6 += translation */
    VU_NOP_LOWER, 0x01C431A8u,
    /* insn 12: FTOI0.xyzw VF7,VF6     / NOP              — float → int32 */
    VU_NOP_LOWER, 0x01E7317Cu,
    /* insn 13: NOP                    / NOP               — FTOI latency */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn 14: NOP                    / SQ.xyzw VF7, 15(VI0) — store result */
    0x03E0380Fu, VU_NOP_UPPER,
    /* insn 15: NOP[E]                 / NOP               — end program */
    VU_NOP_LOWER, VU_NOP_E,
    /* insn 16: NOP                    / NOP               — E delay slot */
    VU_NOP_LOWER, VU_NOP_UPPER,
};

/*
 * mvmva_core: Multiply only — reuse VF1-4 from previous call
 * Entry: VCALLMS 0x088  (byte offset 136)
 *
 * Assumes VF1-4 already loaded with matrix (from mvmva_full or prior core).
 * Same input/output layout as mvmva_full.
 *
 * 13 instructions × 8 bytes = 104 bytes
 */
static const uint32_t vu0_prog_mvmva_core[] = {
    /* insn  0: NOP                / LQ.xyzw VF5, 0(VI0)  — load vertex */
    0x01E50000, VU_NOP_UPPER,
    /* insn  1: ITOF0.xyzw VF5,VF5 / NOP                  — int → float */
    VU_NOP_LOWER, 0x01E5293Cu,
    /* insn  2: NOP / NOP — ITOF latency */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn  3: NOP / NOP — pipeline */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn  4: MULAx.xyz ACC,VF1,VF5 / NOP */
    VU_NOP_LOWER, 0x01C509BCu,
    /* insn  5: MADDAy.xyz ACC,VF2,VF5 / NOP */
    VU_NOP_LOWER, 0x01C510BDu,
    /* insn  6: MADDz.xyz VF6,VF3,VF5 / NOP */
    VU_NOP_LOWER, 0x01C5198Au,
    /* insn  7: ADD.xyz VF6,VF6,VF4 / NOP */
    VU_NOP_LOWER, 0x01C431A8u,
    /* insn  8: FTOI0.xyzw VF7,VF6 / NOP — float → int32 */
    VU_NOP_LOWER, 0x01E7317Cu,
    /* insn  9: NOP / NOP — FTOI latency */
    VU_NOP_LOWER, VU_NOP_UPPER,
    /* insn 10: NOP / SQ.xyzw VF7, 15(VI0) — store result */
    0x03E0380Fu, VU_NOP_UPPER,
    /* insn 11: NOP[E] / NOP — end program */
    VU_NOP_LOWER, VU_NOP_E,
    /* insn 12: NOP / NOP — E delay slot */
    VU_NOP_LOWER, VU_NOP_UPPER,
};

/* ====================================================================
 * Initialization
 * ==================================================================== */

void vu0_micro_init(void)
{
    volatile uint32_t *micro_mem = (volatile uint32_t *)VU0_MICRO_MEM;

    /* Copy mvmva_full at offset 0x000 */
    for (unsigned i = 0; i < sizeof(vu0_prog_mvmva_full) / 4; i++)
        micro_mem[i] = vu0_prog_mvmva_full[i];

    /* Copy mvmva_core at offset 0x088 (word offset = 0x088/4 = 34) */
    unsigned core_off = VU0_PROG_MVMVA_CORE / 4;
    for (unsigned i = 0; i < sizeof(vu0_prog_mvmva_core) / 4; i++)
        micro_mem[core_off + i] = vu0_prog_mvmva_core[i];

    /* Sync: ensure all writes are visible to VU0 */
    __asm__ __volatile__("sync.l" ::: "memory");
}

/* ====================================================================
 * Matrix Refresh — write matrix to VU0 data memory
 *
 * Takes the same mx_cv encoding as vu0_prepare_mvmva().
 * Writes 4 QWs (col1, col2, col3, translation) to VU0 data memory
 * at the appropriate QW offset for the requested matrix.
 * ==================================================================== */

/* Forward declarations from gte.c (VU0 float matrix caches) */
extern float vu0_rt_col1[4], vu0_rt_col2[4], vu0_rt_col3[4], vu0_rt_trans[4];
extern float vu0_lt_col1[4], vu0_lt_col2[4], vu0_lt_col3[4];
extern float vu0_lc_col1[4], vu0_lc_col2[4], vu0_lc_col3[4];
extern float vu0_bk_trans[4], vu0_zero_trans[4];

/* Dirty check functions from gte.c */
extern int vu0_rt_is_dirty(R3000CPU *cpu);
extern int vu0_lt_is_dirty(R3000CPU *cpu);
extern int vu0_lc_is_dirty(R3000CPU *cpu);
extern int vu0_bk_is_dirty(R3000CPU *cpu);
extern void vu0_refresh_rt_matrix(R3000CPU *cpu);
extern void vu0_refresh_lt_matrix(R3000CPU *cpu);
extern void vu0_refresh_lc_matrix(R3000CPU *cpu);
extern void vu0_refresh_bk_trans(R3000CPU *cpu);

/* Write a 4-QW matrix block (col1,col2,col3,trans) to VU0 data memory.
 * Uses uncached store (VU data mem is in I/O region). */
static void vu0_write_matrix_to_datamem(int qw_base,
                                         const float *col1, const float *col2,
                                         const float *col3, const float *trans)
{
    /* VU0 data mem is at 0x11000000, accessible via normal stores.
     * On PS2 hardware this address range is uncached, so stores are
     * immediately visible to VU0. */
    volatile float *dst = (volatile float *)(VU0_DATA_MEM + qw_base * 16);

    /* QW[base+0] = col1 */
    dst[0] = col1[0]; dst[1] = col1[1]; dst[2] = col1[2]; dst[3] = col1[3];
    /* QW[base+1] = col2 */
    dst[4] = col2[0]; dst[5] = col2[1]; dst[6] = col2[2]; dst[7] = col2[3];
    /* QW[base+2] = col3 */
    dst[8] = col3[0]; dst[9] = col3[1]; dst[10] = col3[2]; dst[11] = col3[3];
    /* QW[base+3] = translation */
    dst[12] = trans[0]; dst[13] = trans[1]; dst[14] = trans[2]; dst[15] = trans[3];
}

void vu0_micro_prepare_matrix(R3000CPU *cpu, uint32_t mx_cv)
{
    int mx = mx_cv & 3;
    int cv = mx_cv >> 2;

    /* Always write to QW[1-4] — micro program _FULL reads from fixed QW[1-4].
     * The separate QW bases (RT=1, LT=5, LC=9) are reserved for future
     * multi-matrix programs that preload several matrices simultaneously. */
    float *col1, *col2, *col3;

    switch (mx) {
    case 0:
        if (vu0_rt_is_dirty(cpu)) vu0_refresh_rt_matrix(cpu);
        col1 = vu0_rt_col1; col2 = vu0_rt_col2; col3 = vu0_rt_col3;
        break;
    case 1:
        if (vu0_lt_is_dirty(cpu)) vu0_refresh_lt_matrix(cpu);
        col1 = vu0_lt_col1; col2 = vu0_lt_col2; col3 = vu0_lt_col3;
        break;
    default:
        if (vu0_lc_is_dirty(cpu)) vu0_refresh_lc_matrix(cpu);
        col1 = vu0_lc_col1; col2 = vu0_lc_col2; col3 = vu0_lc_col3;
        break;
    }

    /* Determine translation vector */
    float *trans;
    switch (cv) {
    case 0:
        if (mx != 0 && vu0_rt_is_dirty(cpu)) vu0_refresh_rt_matrix(cpu);
        trans = vu0_rt_trans;
        break;
    case 1:
        if (vu0_bk_is_dirty(cpu)) vu0_refresh_bk_trans(cpu);
        trans = vu0_bk_trans;
        break;
    default:
        trans = vu0_zero_trans;
        break;
    }

    vu0_write_matrix_to_datamem(VU0_QW_RT_COL1, col1, col2, col3, trans);
}

#endif /* _EE */
