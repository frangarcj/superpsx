/*
 * JIT Playground — Memory Tests
 *
 * Covers: LW/SW, LB/SB, LH/SH, LWL/LWR, SWL/SWR.
 * 6 tests total.
 */
#include "playground.h"

static void test_lw_sw_basic(void)
{
    BEGIN_TEST("lw_sw_basic");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xCAFEBABE);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xCAFEBABE);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xCAFEBABE);
    END_TEST();
}

static void test_lb_sb_signext(void)
{
    BEGIN_TEST("lb_sb_signext");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xFF);
    EMIT(PSX_SB(R_V0, 0, R_T0));
    EMIT(PSX_LB(R_A0, 0, R_T0));
    EMIT(PSX_LBU(R_A1, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFFFFFF);
    EXPECT_REG(R_A1, 0x000000FF);
    END_TEST();
}

static void test_lh_sh(void)
{
    BEGIN_TEST("lh_sh");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x8001);
    EMIT(PSX_SH(R_V0, 0, R_T0));
    EMIT(PSX_LH(R_A0, 0, R_T0));
    EMIT(PSX_LHU(R_A1, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xFFFF8001);
    EXPECT_REG(R_A1, 0x00008001);
    END_TEST();
}

static void test_sw_lw_offset(void)
{
    BEGIN_TEST("sw_lw_offset");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x11111111);
    SET_REG(R_V1, 0x22222222);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_SW(R_V1, 4, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    EMIT(PSX_LW(R_A1, 4, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0x11111111);
    EXPECT_REG(R_A1, 0x22222222);
    END_TEST();
}

static void test_lwl_lwr(void)
{
    BEGIN_TEST("lwl_lwr");
    SET_MEM32(PG_DATA_OFFSET, 0xAABBCCDD);
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_A0, 0);
    EMIT(PSX_LWL(R_A0, 3, R_T0));
    EMIT(PSX_LWR(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xAABBCCDD);
    END_TEST();
}

static void test_swl_swr(void)
{
    BEGIN_TEST("swl_swr");
    SET_MEM32(PG_DATA_OFFSET, 0);
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0x12345678);
    EMIT(PSX_SWL(R_V0, 3, R_T0));
    EMIT(PSX_SWR(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0x12345678);
    EXPECT_MEM32(PG_DATA_OFFSET, 0x12345678);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_memory_tests(void)
{
    printf("\n--- Load / Store ---\n");
    test_lw_sw_basic();
    test_lb_sb_signext();
    test_lh_sh();
    test_sw_lw_offset();
    test_lwl_lwr();
    test_swl_swr();
}
