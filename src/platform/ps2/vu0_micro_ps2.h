/*
 * vu0_micro.h — VU0 Micro Mode GTE Acceleration
 *
 * Pre-assembled VU0 micro programs for GTE operations.  Programs live in
 * VU0 micro memory (4KB at 0x11004000) and are launched from the JIT via
 * VCALLMS.  Input/output goes through VU0 data memory (4KB at 0x11000000).
 *
 * Compile-time flag: ENABLE_VU0_MICRO  (CMake option)
 * Runtime flag:      gte_vu0_micro     (superpsx.ini, 0/1)
 *
 * Phase 1: MVMVA micro — matrix × vector + translation
 * Phase 2: RTPS micro  — full perspective transform
 * Phase 3: NCS/NCDS    — lighting pipeline
 */
#ifndef VU0_MICRO_H
#define VU0_MICRO_H

#include <stdint.h>

#ifdef _EE
#include "superpsx.h"

/* ====================================================================
 * VU0 Memory Map (EE-accessible addresses)
 * ==================================================================== */
#define VU0_MICRO_MEM   0x11000000u   /* 4KB micro/instruction memory */
#define VU0_DATA_MEM    0x11004000u   /* 4KB data memory (256 QWs)   */

/* ====================================================================
 * VU0 Data Memory Layout (QW = 16 bytes, addressed by QW index)
 *
 * QW[ 0]:  Vertex input (int32 x, y, z, 0) — written by EE per vertex
 * QW[ 1]:  RT matrix col1 (float, pre-scaled /4096)
 * QW[ 2]:  RT matrix col2
 * QW[ 3]:  RT matrix col3
 * QW[ 4]:  RT translation (float)
 * QW[ 5]:  Light matrix col1
 * QW[ 6]:  Light matrix col2
 * QW[ 7]:  Light matrix col3
 * QW[ 8]:  BK translation (Light)
 * QW[ 9]:  Color matrix col1
 * QW[10]:  Color matrix col2
 * QW[11]:  Color matrix col3
 * QW[12]:  BK translation (Color)
 * QW[13]:  RTPS params: H(float), OFX(float), OFY(float), DQA(float)
 * QW[14]:  RTPS params: DQB(float), 0, 0, 0
 * QW[15]:  Output: MAC1, MAC2, MAC3, 0 (int32)
 * QW[16]:  (reserved for future outputs)
 * ==================================================================== */
#define VU0_QW_VERTEX        0   /* vertex input */
#define VU0_QW_RT_COL1       1   /* RT matrix */
#define VU0_QW_RT_COL2       2
#define VU0_QW_RT_COL3       3
#define VU0_QW_RT_TRANS      4
#define VU0_QW_LT_COL1       5   /* Light matrix */
#define VU0_QW_LT_COL2       6
#define VU0_QW_LT_COL3       7
#define VU0_QW_BK_TRANS      8   /* BK translation */
#define VU0_QW_LC_COL1       9   /* Color matrix */
#define VU0_QW_LC_COL2      10
#define VU0_QW_LC_COL3      11
#define VU0_QW_FC_TRANS     12   /* FC/BK translation (color) */
#define VU0_QW_RTPS_PARAM1  13   /* H, OFX, OFY, DQA */
#define VU0_QW_RTPS_PARAM2  14   /* DQB, 0, 0, 0 */
#define VU0_QW_OUT_MAC      15   /* MAC1, MAC2, MAC3, 0 */

/* Byte offsets from VU0_DATA_MEM for EE access */
#define VU0_OFF_VERTEX      (VU0_QW_VERTEX    * 16)
#define VU0_OFF_RT_COL1     (VU0_QW_RT_COL1   * 16)
#define VU0_OFF_OUT_MAC     (VU0_QW_OUT_MAC   * 16)

/* ====================================================================
 * Micro Program Entry Points (byte offsets in VU0 micro memory)
 *
 * VCALLMS takes a byte address.  Each VU instruction = 8 bytes.
 * ==================================================================== */
#define VU0_PROG_MVMVA_FULL  0x000   /* Load matrix + multiply (17 insns) */
#define VU0_PROG_MVMVA_CORE  0x088   /* Multiply only, reuse VF1-4 (13 insns) */
/* Phase 2: */
/* #define VU0_PROG_RTPS_FULL   0x0F0 */
/* #define VU0_PROG_RTPS_CORE   0x??? */
/* Phase 3: */
/* #define VU0_PROG_NCS_FULL    0x??? */
/* #define VU0_PROG_NCDS_FULL   0x??? */

/* Total micro program size in bytes (must be <= 4096) */
#define VU0_MICRO_TOTAL_SIZE 240  /* 30 instructions × 8 bytes */

/* ====================================================================
 * VCALLMS — launch VU0 micro program from EE
 *
 * Encoding: COP2 | CO=1 | (addr/8)<<6 | func=0x38
 *   [31:26] = 0x12 (COP2)
 *   [25]    = 1 (CO bit)
 *   [24:6]  = addr >> 3  (doubleword address, 19 bits)
 *   [5:0]   = 0x38
 * ==================================================================== */
#define EE_VCALLMS(byte_addr) \
    ((0x12u << 26) | (1u << 25) | ((((byte_addr) >> 3) & 0x7FFFFu) << 6) | 0x38u)

/* CFC2 rt, $vi29 — read VU0 status register (bit 0 = VBS0 = busy) */
#define EE_CFC2_STATUS(rt) \
    ((0x12u << 26) | (0x02u << 21) | ((uint32_t)(rt) << 16) | (29u << 11))

/* ====================================================================
 * API
 * ==================================================================== */

/* Initialize VU0 micro: copy programs to micro memory.
 * Call ONCE at dynarec init time. */
void vu0_micro_init(void);

/* Refresh matrix in VU0 data memory (called from JIT via emit_call_c_lite).
 * mx_cv = mx | (cv << 2), same encoding as vu0_prepare_mvmva. */
void vu0_micro_prepare_matrix(R3000CPU *cpu, uint32_t mx_cv);

#endif /* _EE */
#endif /* VU0_MICRO_H */
