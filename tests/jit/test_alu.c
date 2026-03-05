/*
 * JIT Playground — ALU Tests
 *
 * Covers: ALU basic, shifts, multiply/divide, comparisons, HI/LO.
 * 21 tests total.
 */
#include "playground.h"

/* ================================================================
 *  ALU Basic (8 tests)
 * ================================================================ */

static void test_addu_basic(void)
{
    BEGIN_TEST("addu_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));
    RUN(1000);
    EXPECT_REG(R_A0, 300);
    END_TEST();
}

static void test_addu_overflow_wrap(void)
{
    BEGIN_TEST("addu_overflow_wrap");
    SET_REG(R_V0, 0xFFFFFFFF);
    SET_REG(R_V1, 1);
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));
    RUN(1000);
    EXPECT_REG(R_A0, 0);
    END_TEST();
}

static void test_subu_basic(void)
{
    BEGIN_TEST("subu_basic");
    SET_REG(R_V0, 500);
    SET_REG(R_V1, 200);
    EMIT(PSX_SUBU(R_A0, R_V0, R_V1));
    RUN(1000);
    EXPECT_REG(R_A0, 300);
    END_TEST();
}

static void test_and_or_xor_nor(void)
{
    BEGIN_TEST("and_or_xor_nor");
    SET_REG(R_V0, 0xFF00FF00);
    SET_REG(R_V1, 0x0F0F0F0F);
    EMIT(PSX_AND(R_A0, R_V0, R_V1));
    EMIT(PSX_OR(R_A1, R_V0, R_V1));
    EMIT(PSX_XOR(R_A2, R_V0, R_V1));
    EMIT(PSX_NOR(R_A3, R_V0, R_V1));
    RUN(1000);
    EXPECT_REG(R_A0, 0x0F000F00);
    EXPECT_REG(R_A1, 0xFF0FFF0F);
    EXPECT_REG(R_A2, 0xF00FF00F);
    EXPECT_REG(R_A3, 0x00F000F0);
    END_TEST();
}

static void test_addiu_signext(void)
{
    BEGIN_TEST("addiu_signext");
    SET_REG(R_V0, 100);
    EMIT(PSX_ADDIU(R_A0, R_V0, (uint16_t)(-10)));
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 42));
    RUN(1000);
    EXPECT_REG(R_A0, 90);
    EXPECT_REG(R_A1, 42);
    END_TEST();
}

static void test_lui_ori(void)
{
    BEGIN_TEST("lui_ori");
    EMIT(PSX_LUI(R_V0, 0x1234));
    EMIT(PSX_ORI(R_V0, R_V0, 0x5678));
    RUN(1000);
    EXPECT_REG(R_V0, 0x12345678);
    END_TEST();
}

static void test_andi_xori(void)
{
    BEGIN_TEST("andi_xori");
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_ANDI(R_A0, R_V0, 0x00FF));
    EMIT(PSX_XORI(R_A1, R_V0, 0xFFFF));
    RUN(1000);
    EXPECT_REG(R_A0, 0x000000EF);
    EXPECT_REG(R_A1, 0xDEAD4110);
    END_TEST();
}

static void test_addu_zero_reg(void)
{
    BEGIN_TEST("zero_register");
    SET_REG(R_V0, 42);
    EMIT(PSX_ADDU(R_ZERO, R_V0, R_V0));
    EMIT(PSX_ADDU(R_A0, R_ZERO, R_ZERO));
    RUN(1000);
    EXPECT_REG(R_ZERO, 0);
    EXPECT_REG(R_A0, 0);
    END_TEST();
}

/* ================================================================
 *  Shifts (4 tests)
 * ================================================================ */

static void test_sll_srl_sra(void)
{
    BEGIN_TEST("sll_srl_sra");
    SET_REG(R_V0, 0x00000001);
    EMIT(PSX_SLL(R_A0, R_V0, 8));
    SET_REG(R_V1, 0x00008000);
    EMIT(PSX_SRL(R_A1, R_V1, 4));
    SET_REG(R_T0, 0x80000000);
    EMIT(PSX_SRA(R_A2, R_T0, 4));
    RUN(1000);
    EXPECT_REG(R_A0, 0x00000100);
    EXPECT_REG(R_A1, 0x00000800);
    EXPECT_REG(R_A2, 0xF8000000);
    END_TEST();
}

static void test_sllv_srlv_srav(void)
{
    BEGIN_TEST("sllv_srlv_srav");
    SET_REG(R_V0, 0x00000001);
    SET_REG(R_V1, 16);
    EMIT(PSX_SLLV(R_A0, R_V0, R_V1));
    SET_REG(R_T0, 0x00FF0000);
    SET_REG(R_T1, 8);
    EMIT(PSX_SRLV(R_A1, R_T0, R_T1));
    SET_REG(R_T2, 0x80000000);
    SET_REG(R_T3, 31);
    EMIT(PSX_SRAV(R_A2, R_T2, R_T3));
    RUN(1000);
    EXPECT_REG(R_A0, 0x00010000);
    EXPECT_REG(R_A1, 0x0000FF00);
    EXPECT_REG(R_A2, 0xFFFFFFFF);
    END_TEST();
}

static void test_shift_by_zero(void)
{
    BEGIN_TEST("shift_by_zero");
    SET_REG(R_V0, 0xDEADBEEF);
    EMIT(PSX_SLL(R_A0, R_V0, 0));
    EMIT(PSX_SRL(R_A1, R_V0, 0));
    EMIT(PSX_SRA(R_A2, R_V0, 0));
    RUN(1000);
    EXPECT_REG(R_A0, 0xDEADBEEF);
    EXPECT_REG(R_A1, 0xDEADBEEF);
    EXPECT_REG(R_A2, 0xDEADBEEF);
    END_TEST();
}

static void test_sra_sign_extend(void)
{
    BEGIN_TEST("sra_sign_extend");
    SET_REG(R_V0, 0x7FFFFFFF);
    EMIT(PSX_SRA(R_A0, R_V0, 16));
    SET_REG(R_V1, 0x80000000);
    EMIT(PSX_SRA(R_A1, R_V1, 16));
    RUN(1000);
    EXPECT_REG(R_A0, 0x00007FFF);
    EXPECT_REG(R_A1, 0xFFFF8000);
    END_TEST();
}

/* ================================================================
 *  Multiply / Divide (5 tests)
 * ================================================================ */

static void test_mult_basic(void)
{
    BEGIN_TEST("mult_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);
    EMIT(PSX_MULT(R_V0, R_V1));
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(2000);
    EXPECT_REG(R_A0, 20000);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

static void test_multu_basic(void)
{
    BEGIN_TEST("multu_basic");
    SET_REG(R_V0, 0x10000);
    SET_REG(R_V1, 0x10000);
    EMIT(PSX_MULTU(R_V0, R_V1));
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(2000);
    EXPECT_REG(R_A0, 0x00000000);
    EXPECT_REG(R_A1, 0x00000001);
    END_TEST();
}

static void test_div_basic(void)
{
    BEGIN_TEST("div_basic");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 7);
    EMIT(PSX_DIV(R_V0, R_V1));
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(5000);
    EXPECT_REG(R_A0, 14);
    EXPECT_REG(R_A1, 2);
    END_TEST();
}

static void test_divu_basic(void)
{
    BEGIN_TEST("divu_basic");
    SET_REG(R_V0, 0xFFFFFFFF);
    SET_REG(R_V1, 10);
    EMIT(PSX_DIVU(R_V0, R_V1));
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(5000);
    EXPECT_REG(R_A0, 429496729u);
    EXPECT_REG(R_A1, 5);
    END_TEST();
}

static void test_mult_signed_negative(void)
{
    BEGIN_TEST("mult_signed_neg");
    SET_REG(R_V0, (uint32_t)(-3));
    SET_REG(R_V1, (uint32_t)(-4));
    EMIT(PSX_MULT(R_V0, R_V1));
    EMIT(PSX_MFLO(R_A0));
    EMIT(PSX_MFHI(R_A1));
    RUN(2000);
    EXPECT_REG(R_A0, 12);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

/* ================================================================
 *  Comparisons (3 tests)
 * ================================================================ */

static void test_slt_signed(void)
{
    BEGIN_TEST("slt_signed");
    SET_REG(R_V0, (uint32_t)(-5));
    SET_REG(R_V1, 5);
    EMIT(PSX_SLT(R_A0, R_V0, R_V1));
    EMIT(PSX_SLT(R_A1, R_V1, R_V0));
    EMIT(PSX_SLT(R_A2, R_V0, R_V0));
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    EXPECT_REG(R_A2, 0);
    END_TEST();
}

static void test_sltu_unsigned(void)
{
    BEGIN_TEST("sltu_unsigned");
    SET_REG(R_V0, 0xFFFFFFFB);
    SET_REG(R_V1, 5);
    EMIT(PSX_SLTU(R_A0, R_V1, R_V0));
    EMIT(PSX_SLTU(R_A1, R_V0, R_V1));
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    END_TEST();
}

static void test_slti_sltiu(void)
{
    BEGIN_TEST("slti_sltiu");
    SET_REG(R_V0, 10);
    EMIT(PSX_SLTI(R_A0, R_V0, 20));
    EMIT(PSX_SLTI(R_A1, R_V0, 5));
    SET_REG(R_V1, 0xFFFFFFFB);
    EMIT(PSX_SLTIU(R_A2, R_V1, 0));
    RUN(1000);
    EXPECT_REG(R_A0, 1);
    EXPECT_REG(R_A1, 0);
    EXPECT_REG(R_A2, 0);
    END_TEST();
}

/* ================================================================
 *  HI/LO register management (1 test)
 * ================================================================ */

static void test_mthi_mtlo(void)
{
    BEGIN_TEST("mthi_mtlo");
    SET_REG(R_V0, 0xAAAAAAAA);
    SET_REG(R_V1, 0x55555555);
    EMIT(PSX_MTHI(R_V0));
    EMIT(PSX_MTLO(R_V1));
    EMIT(PSX_MFHI(R_A0));
    EMIT(PSX_MFLO(R_A1));
    RUN(1000);
    EXPECT_REG(R_A0, 0xAAAAAAAA);
    EXPECT_REG(R_A1, 0x55555555);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_alu_tests(void)
{
    printf("--- ALU Basic ---\n");
    test_addu_basic();
    test_addu_overflow_wrap();
    test_subu_basic();
    test_and_or_xor_nor();
    test_addiu_signext();
    test_lui_ori();
    test_andi_xori();
    test_addu_zero_reg();

    printf("\n--- Shifts ---\n");
    test_sll_srl_sra();
    test_sllv_srlv_srav();
    test_shift_by_zero();
    test_sra_sign_extend();

    printf("\n--- Multiply / Divide ---\n");
    test_mult_basic();
    test_multu_basic();
    test_div_basic();
    test_divu_basic();
    test_mult_signed_negative();

    printf("\n--- Comparisons ---\n");
    test_slt_signed();
    test_sltu_unsigned();
    test_slti_sltiu();

    printf("\n--- HI/LO ---\n");
    test_mthi_mtlo();
}
