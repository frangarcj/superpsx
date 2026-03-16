/**
 * gte_vfpu.h — VFPU-accelerated GTE declarations for PSP
 */
#ifndef GTE_VFPU_H
#define GTE_VFPU_H

#include "superpsx.h"

/* VFPU MVMVA: Matrix x Vector + Translation (sf=1, mx!=3, cv!=2 only) */
void gte_mvmva_vfpu(R3000CPU *cpu, int lm, int mx, int v, int cv);

/* VFPU RT multiply: RT_matrix × vertex + TR → mac1/mac2/mac3
 * Used by RTPS/RTPT in gte.c for the matrix multiply portion only. */
void vfpu_rt_multiply(R3000CPU *cpu, int v, int32_t *out_mac1, int32_t *out_mac2, int32_t *out_mac3);

/* Matrix cache management */
void vfpu_refresh_rt_matrix(R3000CPU *cpu);
void vfpu_refresh_lt_matrix(R3000CPU *cpu);
void vfpu_refresh_lc_matrix(R3000CPU *cpu);
void vfpu_refresh_bk_trans(R3000CPU *cpu);
int vfpu_rt_is_dirty(R3000CPU *cpu);
int vfpu_lt_is_dirty(R3000CPU *cpu);
int vfpu_lc_is_dirty(R3000CPU *cpu);
int vfpu_bk_is_dirty(R3000CPU *cpu);

/* Shared flag word — gte.c copies gte_flag here before VFPU call,
 * and copies it back after (so VFPU saturate_ir sets the right flags) */
extern uint32_t gte_flag_vfpu;

/* Cached float arrays (aligned, extern for playground access) */
extern float vfpu_rt_row1[4], vfpu_rt_row2[4], vfpu_rt_row3[4], vfpu_rt_trans[4];
extern float vfpu_lt_row1[4], vfpu_lt_row2[4], vfpu_lt_row3[4];
extern float vfpu_lc_row1[4], vfpu_lc_row2[4], vfpu_lc_row3[4];
extern float vfpu_bk_trans[4];
extern float vfpu_zero_trans[4];

#endif /* GTE_VFPU_H */
