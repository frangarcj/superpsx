/*
 * JIT Playground — Branch Tests
 *
 * Covers: BEQ, BNE, BLTZ, BGEZ, BLEZ, BGTZ, delay slots.
 * 7 tests total.
 */
#include "playground.h"

static void test_beq_taken(void)
{
    BEGIN_TEST("beq_taken");
    SET_REG(R_V0, 42);
    SET_REG(R_V1, 42);
    /*
     * 0: BEQ v0, v1, +2     (skip next insn, jump to insn at offset 3)
     * 1: NOP                 (delay slot)
     * 2: ADDIU a0, zero, 99  (skipped)
     * 3: ADDIU a0, zero, 77  (branch target)
     */
    EMIT(PSX_BEQ(R_V0, R_V1, 2));         /* branch to PC+4+2*4 = insn 3 */
    EMIT(PSX_NOP());                       /* delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 77));    /* branch target */
    RUN(2000);
    EXPECT_REG(R_A0, 77);
    END_TEST();
}

static void test_beq_not_taken(void)
{
    BEGIN_TEST("beq_not_taken");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);
    /*
     * 0: BEQ v0, v1, +2     (not taken)
     * 1: NOP                 (delay slot — executes either way)
     * 2: ADDIU a0, zero, 99  (fall-through)
     */
    EMIT(PSX_BEQ(R_V0, R_V1, 2));
    EMIT(PSX_NOP());
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));
    RUN(2000);
    EXPECT_REG(R_A0, 99);
    END_TEST();
}

static void test_bne_taken(void)
{
    BEGIN_TEST("bne_taken");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);
    EMIT(PSX_BNE(R_V0, R_V1, 2));         /* 1 != 2 → taken */
    EMIT(PSX_NOP());
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 55));    /* target */
    RUN(2000);
    EXPECT_REG(R_A0, 55);
    END_TEST();
}

static void test_branch_delay_slot(void)
{
    /* Instruction in the delay slot MUST execute regardless of branch outcome */
    BEGIN_TEST("branch_delay_slot");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 1);
    EMIT(PSX_BEQ(R_V0, R_V1, 2));         /* taken */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 42));    /* delay slot — must execute */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 77));    /* branch target */
    RUN(2000);
    EXPECT_REG(R_A0, 42);                 /* delay slot executed */
    EXPECT_REG(R_A1, 77);                 /* branch target executed */
    END_TEST();
}

static void test_branch_delay_store(void)
{
    /* Store in delay slot must complete before branch executes */
    BEGIN_TEST("branch_delay_store");
    EMIT(PSX_LUI(R_T0, 0x8002));           /* t0 = data base */
    SET_REG(R_V0, 0);
    SET_REG(R_V1, 0);
    SET_REG(R_A0, 0xBEEF);
    EMIT(PSX_BEQ(R_V0, R_V1, 2));          /* taken */
    EMIT(PSX_SW(R_A0, 0, R_T0));           /* delay slot: store 0xBEEF */
    EMIT(PSX_NOP());                        /* skipped */
    EMIT(PSX_LW(R_A1, 0, R_T0));           /* target: load it back */
    RUN(2000);
    EXPECT_REG(R_A1, 0xBEEF);
    EXPECT_MEM32(PG_DATA_OFFSET, 0xBEEF);
    END_TEST();
}

static void test_bltz_bgez(void)
{
    BEGIN_TEST("bltz_bgez");
    SET_REG(R_V0, (uint32_t)(-5));
    SET_REG(R_V1, 5);

    /* BLTZ with negative → taken, skip over ADDIU a0,zero,99 */
    EMIT(PSX_BLTZ(R_V0, 2));               /* 0: taken (v0 < 0) */
    EMIT(PSX_NOP());                        /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));     /* 2: skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));     /* 3: target */

    /* BGEZ with positive → taken */
    EMIT(PSX_BGEZ(R_V1, 2));               /* 4: taken (v1 >= 0) */
    EMIT(PSX_NOP());                        /* 5: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));     /* 6: skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));     /* 7: target */

    RUN(3000);
    EXPECT_REG(R_A0, 11);
    EXPECT_REG(R_A1, 22);
    END_TEST();
}

static void test_blez_bgtz(void)
{
    BEGIN_TEST("blez_bgtz");
    SET_REG(R_V0, 0);
    SET_REG(R_V1, 5);

    /* BLEZ with zero → taken */
    EMIT(PSX_BLEZ(R_V0, 2));               /* 0: taken (v0 <= 0) */
    EMIT(PSX_NOP());                        /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 99));     /* 2: skipped */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 33));     /* 3: target */

    /* BGTZ with positive → taken */
    EMIT(PSX_BGTZ(R_V1, 2));               /* 4: taken (v1 > 0) */
    EMIT(PSX_NOP());                        /* 5: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));     /* 6: skipped */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 44));     /* 7: target */

    RUN(3000);
    EXPECT_REG(R_A0, 33);
    EXPECT_REG(R_A1, 44);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_branch_tests(void)
{
    printf("\n--- Branches ---\n");
    test_beq_taken();
    test_beq_not_taken();
    test_bne_taken();
    test_branch_delay_slot();
    test_branch_delay_store();

    printf("\n--- REGIMM Branches ---\n");
    test_bltz_bgez();
    test_blez_bgtz();
}
