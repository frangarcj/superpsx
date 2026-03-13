/*
 * test_vu0_micro.c — VU0 Micro Mode Tests
 *
 * Tests the VU0 micro programs directly (not via JIT).
 * Uses VU0 hardware: loads micro programs into VU0 micro memory,
 * writes input data to VU0 data memory, launches VCALLMS, reads results.
 *
 * These tests validate that the assembled VU0 micro programs produce
 * correct results by comparing with C reference computations.
 *
 * TDD: Tests defined FIRST, then micro programs implemented to pass.
 */
#include "playground.h"
#include "vu0_micro_ps2.h"

#ifdef _EE

#include <stdio.h>
#include <string.h>

/* ====================================================================
 * Helper: write a float QW (128 bits = 4 floats) to VU0 data memory
 * ==================================================================== */
static void vu0_write_qw(int qw_index, float x, float y, float z, float w)
{
    volatile float *dst = (volatile float *)(VU0_DATA_MEM + qw_index * 16);
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

/* Write int32 QW to VU0 data memory (for vertex input) */
static void vu0_write_qw_int(int qw_index, int32_t x, int32_t y, int32_t z, int32_t w)
{
    volatile int32_t *dst = (volatile int32_t *)(VU0_DATA_MEM + qw_index * 16);
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

/* Read int32 QW from VU0 data memory (for result output) */
static void vu0_read_qw_int(int qw_index, int32_t *x, int32_t *y, int32_t *z, int32_t *w)
{
    volatile int32_t *src = (volatile int32_t *)(VU0_DATA_MEM + qw_index * 16);
    /* sync before reading VU0 output */
    __asm__ __volatile__("sync.l" ::: "memory");
    *x = src[0];
    *y = src[1];
    *z = src[2];
    *w = src[3];
}

/* Launch VU0 micro program and wait for completion */
static void vu0_launch_and_wait(int byte_addr)
{
    /* Sync to ensure all data writes are visible */
    __asm__ __volatile__("sync.l" ::: "memory");

    /* Use CTC2 to set CMSAR0 (micro start address register),
     * then VCALLMSR to launch.  This avoids needing a compile-time
     * constant for the VCALLMS immediate encoding. */
    uint32_t addr_dw = (uint32_t)byte_addr >> 3;  /* doubleword address */
    __asm__ __volatile__(
        "ctc2    %0, $27\n\t"      /* CMSAR0 = addr_dw */
        ".word   0x4A000039\n\t"   /* VCALLMSR (func=0x39) */
        : : "r"(addr_dw) : "memory"
    );

    /* Wait for VU0 to finish.
     * Poll VBS0 bit (bit 0) in CFC2 $vi29. */
    uint32_t status;
    do {
        __asm__ __volatile__(
            "cfc2 %0, $29\n\t"
            : "=r"(status)
        );
    } while (status & 1u);

    /* Sync after VU0 completion */
    __asm__ __volatile__("sync.l" ::: "memory");
}

/* ====================================================================
 * C Reference: identity matrix × vertex
 * For sf=1: result = (RT/4096 × V + TR), then truncated to int32.
 * With identity matrix (all 4096/4096 = 1.0) and zero translation:
 *   MAC1 = VX, MAC2 = VY, MAC3 = VZ  (for exact integer inputs)
 * ==================================================================== */

/* ====================================================================
 * Test 1: MVMVA full — identity matrix × vertex
 *
 * Setup: identity RT matrix (4096 on diagonal, 0 elsewhere)
 *        zero translation
 *        vertex = (100, 200, 300)
 * Expected: MAC1=100, MAC2=200, MAC3=300
 * ==================================================================== */
static void test_mvmva_identity(void)
{
    pg_ctx.name = "vu0micro_mvmva_identity";
    pg_ctx.fail_count = 0;
    pg_results.total++;

    /* Write identity matrix to VU0 data mem QW[1-4].
     * Column-major, pre-scaled by 1/4096:
     *   col1 = (1.0, 0.0, 0.0, 0.0)  — M11=4096/4096, M21=0, M31=0
     *   col2 = (0.0, 1.0, 0.0, 0.0)  — M12=0, M22=4096/4096, M32=0
     *   col3 = (0.0, 0.0, 1.0, 0.0)  — M13=0, M23=0, M33=4096/4096
     *   trans = (0.0, 0.0, 0.0, 0.0) */
    vu0_write_qw(VU0_QW_RT_COL1,  1.0f, 0.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL2,  0.0f, 1.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL3,  0.0f, 0.0f, 1.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_TRANS, 0.0f, 0.0f, 0.0f, 0.0f);

    /* Write vertex (int32) to QW[0] */
    vu0_write_qw_int(VU0_QW_VERTEX, 100, 200, 300, 0);

    /* Launch mvmva_full */
    vu0_launch_and_wait(VU0_PROG_MVMVA_FULL);

    /* Read result from QW[15] */
    int32_t mac1, mac2, mac3, mac_w;
    vu0_read_qw_int(VU0_QW_OUT_MAC, &mac1, &mac2, &mac3, &mac_w);

    /* Verify */
    if (mac1 != 100 || mac2 != 200 || mac3 != 300) {
        printf("  [FAIL] %s: MAC=(%ld,%ld,%ld) expected=(100,200,300)\n",
               pg_ctx.name, (long)mac1, (long)mac2, (long)mac3);
        pg_ctx.fail_count++;
    }

    if (pg_ctx.fail_count == 0) {
        printf("[PASS] %s\n", pg_ctx.name);
        pg_results.passed++;
    } else {
        printf("[FAIL] %s (%d assertions failed)\n", pg_ctx.name, pg_ctx.fail_count);
        pg_results.failed++;
    }
}

/* ====================================================================
 * Test 2: MVMVA full — rotation matrix (90° around Z)
 *
 * RT matrix (in 1.3.12 fixed): rotate 90° around Z axis
 *   R = [[0, -4096, 0], [4096, 0, 0], [0, 0, 4096]]
 * Pre-scaled by 1/4096:
 *   col1 = (0, 1, 0, 0)   — M11=0, M21=4096, M31=0
 *   col2 = (-1, 0, 0, 0)  — M12=-4096, M22=0, M32=0
 *   col3 = (0, 0, 1, 0)   — M13=0, M23=0, M33=4096
 * Translation = (10, 20, 30)
 *
 * Vertex = (100, 200, 300)
 * Expected: MAC1 = 0*100 + (-1)*200 + 0*300 + 10 = -190
 *           MAC2 = 1*100 + 0*200 + 0*300 + 20 = 120
 *           MAC3 = 0*100 + 0*200 + 1*300 + 30 = 330
 * ==================================================================== */
static void test_mvmva_rotation(void)
{
    pg_ctx.name = "vu0micro_mvmva_rot90z";
    pg_ctx.fail_count = 0;
    pg_results.total++;

    /* 90° Z rotation, pre-scaled */
    vu0_write_qw(VU0_QW_RT_COL1,   0.0f, 1.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL2,  -1.0f, 0.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL3,   0.0f, 0.0f, 1.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_TRANS, 10.0f, 20.0f, 30.0f, 0.0f);

    vu0_write_qw_int(VU0_QW_VERTEX, 100, 200, 300, 0);

    vu0_launch_and_wait(VU0_PROG_MVMVA_FULL);

    int32_t mac1, mac2, mac3, mac_w;
    vu0_read_qw_int(VU0_QW_OUT_MAC, &mac1, &mac2, &mac3, &mac_w);

    if (mac1 != -190 || mac2 != 120 || mac3 != 330) {
        printf("  [FAIL] %s: MAC=(%ld,%ld,%ld) expected=(-190,120,330)\n",
               pg_ctx.name, (long)mac1, (long)mac2, (long)mac3);
        pg_ctx.fail_count++;
    }

    if (pg_ctx.fail_count == 0) {
        printf("[PASS] %s\n", pg_ctx.name);
        pg_results.passed++;
    } else {
        printf("[FAIL] %s (%d assertions failed)\n", pg_ctx.name, pg_ctx.fail_count);
        pg_results.failed++;
    }
}

/* ====================================================================
 * Test 3: MVMVA core — reuse matrix from previous call
 *
 * After test 2, VF1-4 should still hold the rotation matrix.
 * Write a new vertex and call mvmva_core (no matrix reload).
 *
 * Vertex = (50, 0, 0)
 * With 90°Z rotation + translation(10,20,30):
 *   MAC1 = 0*50 + (-1)*0 + 0*0 + 10 = 10
 *   MAC2 = 1*50 + 0*0 + 0*0 + 20 = 70
 *   MAC3 = 0*50 + 0*0 + 1*0 + 30 = 30
 * ==================================================================== */
static void test_mvmva_core_reuse(void)
{
    pg_ctx.name = "vu0micro_mvmva_core";
    pg_ctx.fail_count = 0;
    pg_results.total++;

    /* Only write new vertex — matrix persists in VF1-4 from test 2 */
    vu0_write_qw_int(VU0_QW_VERTEX, 50, 0, 0, 0);

    /* Use mvmva_core (assumes VF1-4 loaded) */
    vu0_launch_and_wait(VU0_PROG_MVMVA_CORE);

    int32_t mac1, mac2, mac3, mac_w;
    vu0_read_qw_int(VU0_QW_OUT_MAC, &mac1, &mac2, &mac3, &mac_w);

    if (mac1 != 10 || mac2 != 70 || mac3 != 30) {
        printf("  [FAIL] %s: MAC=(%ld,%ld,%ld) expected=(10,70,30)\n",
               pg_ctx.name, (long)mac1, (long)mac2, (long)mac3);
        pg_ctx.fail_count++;
    }

    if (pg_ctx.fail_count == 0) {
        printf("[PASS] %s\n", pg_ctx.name);
        pg_results.passed++;
    } else {
        printf("[FAIL] %s (%d assertions failed)\n", pg_ctx.name, pg_ctx.fail_count);
        pg_results.failed++;
    }
}

/* ====================================================================
 * Test 4: MVMVA scaling — verify float precision
 *
 * Matrix = 2× scale on all axes (pre-scaled: 2.0):
 *   col1 = (2.0, 0, 0, 0), col2 = (0, 2.0, 0, 0), col3 = (0, 0, 2.0, 0)
 *   trans = (0, 0, 0, 0)
 *
 * Vertex = (1000, -500, 250)
 * Expected: MAC1 = 2000, MAC2 = -1000, MAC3 = 500
 * ==================================================================== */
static void test_mvmva_scale(void)
{
    pg_ctx.name = "vu0micro_mvmva_scale";
    pg_ctx.fail_count = 0;
    pg_results.total++;

    vu0_write_qw(VU0_QW_RT_COL1,  2.0f, 0.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL2,  0.0f, 2.0f, 0.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL3,  0.0f, 0.0f, 2.0f, 0.0f);
    vu0_write_qw(VU0_QW_RT_TRANS, 0.0f, 0.0f, 0.0f, 0.0f);

    vu0_write_qw_int(VU0_QW_VERTEX, 1000, -500, 250, 0);

    vu0_launch_and_wait(VU0_PROG_MVMVA_FULL);

    int32_t mac1, mac2, mac3, mac_w;
    vu0_read_qw_int(VU0_QW_OUT_MAC, &mac1, &mac2, &mac3, &mac_w);

    if (mac1 != 2000 || mac2 != -1000 || mac3 != 500) {
        printf("  [FAIL] %s: MAC=(%ld,%ld,%ld) expected=(2000,-1000,500)\n",
               pg_ctx.name, (long)mac1, (long)mac2, (long)mac3);
        pg_ctx.fail_count++;
    }

    if (pg_ctx.fail_count == 0) {
        printf("[PASS] %s\n", pg_ctx.name);
        pg_results.passed++;
    } else {
        printf("[FAIL] %s (%d assertions failed)\n", pg_ctx.name, pg_ctx.fail_count);
        pg_results.failed++;
    }
}

/* ====================================================================
 * Test 5: MVMVA with realistic PSX GTE values
 *
 * Simulating a real RTPS scenario:
 * RT matrix (in 1.3.12): typical rotation
 *   R11=3277, R12=-1638, R13=819  (approx 80°/40°/20° rotation)
 *   R21=1638, R22=3277, R23=-1638
 *   R31=-819, R32=1638, R33=3277
 * Translation: TRX=100, TRY=50, TRZ=200
 *
 * Pre-scaled by 1/4096:
 *   col1 ≈ (0.8, 0.4, -0.2, 0)
 *   col2 ≈ (-0.4, 0.8, 0.4, 0)
 *   col3 ≈ (0.2, -0.4, 0.8, 0)
 *   trans = (100, 50, 200, 0)
 *
 * Vertex = (100, 200, 300)
 * C reference: MAC1 = 0.8*100 + (-0.4)*200 + 0.2*300 + 100 = 80-80+60+100 = 160
 *              MAC2 = 0.4*100 + 0.8*200 + (-0.4)*300 + 50 = 40+160-120+50 = 130
 *              MAC3 = (-0.2)*100 + 0.4*200 + 0.8*300 + 200 = -20+80+240+200 = 500
 *
 * Using exact fixed-point: R/4096 values
 *   col1 = (3277/4096, 1638/4096, -819/4096)
 *   MAC1 = (3277*100 + (-1638)*200 + 819*300)/4096 + 100
 *        = (327700 - 327600 + 245700)/4096 + 100
 *        = 245800/4096 + 100 = 60.009... + 100 ≈ 160
 * Due to float rounding, allow ±1 tolerance.
 * ==================================================================== */
static void test_mvmva_realistic(void)
{
    pg_ctx.name = "vu0micro_mvmva_real";
    pg_ctx.fail_count = 0;
    pg_results.total++;

    const float s = 1.0f / 4096.0f;
    vu0_write_qw(VU0_QW_RT_COL1,  3277.0f*s,  1638.0f*s, -819.0f*s, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL2, -1638.0f*s,  3277.0f*s,  1638.0f*s, 0.0f);
    vu0_write_qw(VU0_QW_RT_COL3,   819.0f*s, -1638.0f*s,  3277.0f*s, 0.0f);
    vu0_write_qw(VU0_QW_RT_TRANS, 100.0f, 50.0f, 200.0f, 0.0f);

    vu0_write_qw_int(VU0_QW_VERTEX, 100, 200, 300, 0);

    vu0_launch_and_wait(VU0_PROG_MVMVA_FULL);

    int32_t mac1, mac2, mac3, mac_w;
    vu0_read_qw_int(VU0_QW_OUT_MAC, &mac1, &mac2, &mac3, &mac_w);

    /* C reference calculation (float, same as VU0):
     * col1.xyz * 100 + col2.xyz * 200 + col3.xyz * 300 + trans */
    float c1[3] = { 3277.0f/4096.0f,  1638.0f/4096.0f, -819.0f/4096.0f};
    float c2[3] = {-1638.0f/4096.0f,  3277.0f/4096.0f,  1638.0f/4096.0f};
    float c3[3] = {  819.0f/4096.0f, -1638.0f/4096.0f,  3277.0f/4096.0f};
    float tr[3] = {100.0f, 50.0f, 200.0f};

    int32_t exp1 = (int32_t)(c1[0]*100 + c2[0]*200 + c3[0]*300 + tr[0]);
    int32_t exp2 = (int32_t)(c1[1]*100 + c2[1]*200 + c3[1]*300 + tr[1]);
    int32_t exp3 = (int32_t)(c1[2]*100 + c2[2]*200 + c3[2]*300 + tr[2]);

    /* Allow ±1 tolerance for float→int truncation differences */
    int ok = 1;
    if (mac1 < exp1 - 1 || mac1 > exp1 + 1) ok = 0;
    if (mac2 < exp2 - 1 || mac2 > exp2 + 1) ok = 0;
    if (mac3 < exp3 - 1 || mac3 > exp3 + 1) ok = 0;

    if (!ok) {
        printf("  [FAIL] %s: MAC=(%ld,%ld,%ld) expected~(%ld,%ld,%ld)\n",
               pg_ctx.name, (long)mac1, (long)mac2, (long)mac3,
               (long)exp1, (long)exp2, (long)exp3);
        pg_ctx.fail_count++;
    }

    if (pg_ctx.fail_count == 0) {
        printf("[PASS] %s\n", pg_ctx.name);
        pg_results.passed++;
    } else {
        printf("[FAIL] %s (%d assertions failed)\n", pg_ctx.name, pg_ctx.fail_count);
        pg_results.failed++;
    }
}

/* ====================================================================
 * Runner
 * ==================================================================== */
void pg_run_vu0_micro_tests(void)
{
    printf("\n=== VU0 Micro Mode Tests ===\n");

    /* Initialize micro programs in VU0 memory */
    vu0_micro_init();

    /* Run tests */
    test_mvmva_identity();     /* Test 1: identity × vertex */
    test_mvmva_rotation();     /* Test 2: 90°Z rotation + translation */
    test_mvmva_core_reuse();   /* Test 3: mvmva_core reuses VF1-4 */
    test_mvmva_scale();        /* Test 4: 2× scale */
    test_mvmva_realistic();    /* Test 5: realistic GTE matrix */
}

#else /* !_EE */

void pg_run_vu0_micro_tests(void)
{
    printf("\n=== VU0 Micro Mode Tests ===\n");
    printf("[SKIP] VU0 micro tests require PS2 EE target\n");
}

#endif /* _EE */
