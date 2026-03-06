/*
 * JIT Playground — Dirty Writeback Tests
 *
 * Exercises the dirty writeback protocol for dynamic register slots.
 * With dirty writeback, stores update the slot EE register and set a
 * compile-time dirty bit; the actual SW to cpu.regs[] is deferred to
 * sync points (block exits, C calls, abort checks).
 *
 * These tests specifically target:
 * 1. Dirty slots flushed correctly at block exits (J, branch, JR)
 * 2. Non-dirty slots surviving block boundaries unchanged
 * 3. Mixed dirty/non-dirty slots across block boundaries
 * 4. Multiple writes to the same slot before exit
 * 5. All 8 dynamic slots dirty at block exit
 * 6. Dirty slots across conditional branch paths
 * 7. Dirty slots in loop iterations
 * 8. Dirty writeback after memory operations (cold slow path)
 *
 * 10 tests total.
 */
#include "playground.h"

/* ================================================================
 *  1. Basic: single dirty slot at block exit (J)
 * ================================================================ */
static void test_dirty_single_slot_j(void)
{
    /* Write to one non-pinned register in block 1, then J to block 2
     * which reads it.  If dirty flush at block exit fails, block 2
     * would see the stale cpu.regs[] value (0) instead of the new one. */
    BEGIN_TEST("dirty_single_slot_j");
    SET_REG(R_T0, 0);

    /* Block 1: write t0 and jump */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 0x1234)); /* t0 = 0x1234 (dirty) */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 3; i < 8; i++)
        EMIT(PSX_NOP());

    /* Block 2: use t0 (forces block 2 to load cpu.regs[t0]) */
    EMIT(PSX_ADDU(R_A0, R_T0, R_ZERO)); /* a0 = t0 */

    RUN(5000);
    EXPECT_REG(R_T0, 0x1234);
    EXPECT_REG(R_A0, 0x1234);
    END_TEST();
}

/* ================================================================
 *  2. Non-dirty slot survives block boundary
 * ================================================================ */
static void test_nondirty_cross_block(void)
{
    /* Set a register BEFORE execution, then DON'T write it in block 1.
     * It should survive the block boundary to block 2.
     * This tests that non-dirty slots in block 1 don't corrupt cpu.regs. */
    BEGIN_TEST("nondirty_cross_block");
    SET_REG(R_T0, 0xAABBCCDD);
    SET_REG(R_T1, 0x11223344);

    /* Block 1: only READ t0 and t1 (no writes → not dirty).
     * Use them in a computation to force them into dynamic slots.
     * a0 = t0 + t1 (uses both, writes a0 which IS dirty) */
    EMIT(PSX_ADDU(R_A0, R_T0, R_T1));
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 3; i < 8; i++)
        EMIT(PSX_NOP());

    /* Block 2: re-read t0 and t1 — they should still have original values */
    EMIT(PSX_ADDU(R_A1, R_T0, R_ZERO)); /* a1 = t0 */
    EMIT(PSX_ADDU(R_A2, R_T1, R_ZERO)); /* a2 = t1 */

    RUN(5000);
    EXPECT_REG(R_T0, 0xAABBCCDD);
    EXPECT_REG(R_T1, 0x11223344);
    EXPECT_REG(R_A0, 0xAABBCCDD + 0x11223344);
    EXPECT_REG(R_A1, 0xAABBCCDD);
    EXPECT_REG(R_A2, 0x11223344);
    END_TEST();
}

/* ================================================================
 *  3. Mixed dirty + non-dirty slots across block boundary
 * ================================================================ */
static void test_dirty_mixed_cross_block(void)
{
    /* Some regs written (dirty) and some only read (non-dirty).
     * Block 2 should see correct values for ALL of them. */
    BEGIN_TEST("dirty_mixed_cross_block");
    SET_REG(R_T0, 100); /* will be read-only (non-dirty) */
    SET_REG(R_T1, 200); /* will be read-only (non-dirty) */
    SET_REG(R_T2, 0);   /* will be written (dirty) */
    SET_REG(R_T3, 0);   /* will be written (dirty) */

    /* Block 1: read t0,t1 (non-dirty); write t2,t3 (dirty); jump */
    EMIT(PSX_ADDU(R_T2, R_T0, R_T1)); /* t2 = 100 + 200 = 300 (dirty) */
    EMIT(PSX_ADDIU(R_T3, R_T0, 50));  /* t3 = 100 + 50 = 150 (dirty) */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 4; i < 8; i++)
        EMIT(PSX_NOP());

    /* Block 2: use all four regs — all should have correct values */
    EMIT(PSX_ADDU(R_A0, R_T0, R_T2)); /* a0 = 100 + 300 = 400 */
    EMIT(PSX_ADDU(R_A1, R_T1, R_T3)); /* a1 = 200 + 150 = 350 */

    RUN(5000);
    EXPECT_REG(R_T0, 100);
    EXPECT_REG(R_T1, 200);
    EXPECT_REG(R_T2, 300);
    EXPECT_REG(R_T3, 150);
    EXPECT_REG(R_A0, 400);
    EXPECT_REG(R_A1, 350);
    END_TEST();
}

/* ================================================================
 *  4. Multiple writes to the same slot before exit
 * ================================================================ */
static void test_dirty_multi_write_same_slot(void)
{
    /* Write to the same register multiple times within a block.
     * The last value should be what survives the block exit. */
    BEGIN_TEST("dirty_multi_write_slot");
    SET_REG(R_T0, 0);

    /* Block 1: overwrite t0 several times */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 1)); /* t0 = 1 */
    EMIT(PSX_ADDIU(R_T0, R_T0, 1));   /* t0 = 2 */
    EMIT(PSX_ADDIU(R_T0, R_T0, 1));   /* t0 = 3 */
    EMIT(PSX_ADDIU(R_T0, R_T0, 1));   /* t0 = 4 (final) */
    uint32_t block2_pc = PG_CODE_BASE + 12 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 6; i < 12; i++)
        EMIT(PSX_NOP());

    /* Block 2: read t0 — should see 4 */
    EMIT(PSX_ADDU(R_A0, R_T0, R_ZERO));

    RUN(5000);
    EXPECT_REG(R_T0, 4);
    EXPECT_REG(R_A0, 4);
    END_TEST();
}

/* ================================================================
 *  5. All 8 dynamic slots dirty at block exit
 * ================================================================ */
static void test_dirty_all_8_slots(void)
{
    /* Write to 8+ non-pinned registers to fill all dynamic slots.
     * All should survive the block boundary.
     * Non-pinned, non-infrastructure PSX regs: t0-t7(8-15), s2-s7(18-23),
     * t8-t9(24-25), at(1), etc. We use t0-t5, s2, s3 = 8 regs. */
    BEGIN_TEST("dirty_all_8_slots");

    /* Block 1: write to 8 non-pinned regs */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 10));
    EMIT(PSX_ADDIU(R_T1, R_ZERO, 20));
    EMIT(PSX_ADDIU(R_T2, R_ZERO, 30));
    EMIT(PSX_ADDIU(R_T3, R_ZERO, 40));
    EMIT(PSX_ADDIU(R_T4, R_ZERO, 50));
    EMIT(PSX_ADDIU(R_T5, R_ZERO, 60));
    EMIT(PSX_ADDIU(R_S2, R_ZERO, 70));
    EMIT(PSX_ADDIU(R_S3, R_ZERO, 80));
    uint32_t block2_pc = PG_CODE_BASE + 16 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 10; i < 16; i++)
        EMIT(PSX_NOP());

    /* Block 2: read all 8 in a computation */
    EMIT(PSX_ADDU(R_A0, R_T0, R_T1)); /* a0 = 10+20 = 30 */
    EMIT(PSX_ADDU(R_A1, R_T2, R_T3)); /* a1 = 30+40 = 70 */
    EMIT(PSX_ADDU(R_A2, R_T4, R_T5)); /* a2 = 50+60 = 110 */
    EMIT(PSX_ADDU(R_A3, R_S2, R_S3)); /* a3 = 70+80 = 150 */

    RUN(8000);
    EXPECT_REG(R_T0, 10);
    EXPECT_REG(R_T1, 20);
    EXPECT_REG(R_T2, 30);
    EXPECT_REG(R_T3, 40);
    EXPECT_REG(R_T4, 50);
    EXPECT_REG(R_T5, 60);
    EXPECT_REG(R_S2, 70);
    EXPECT_REG(R_S3, 80);
    EXPECT_REG(R_A0, 30);
    EXPECT_REG(R_A1, 70);
    EXPECT_REG(R_A2, 110);
    EXPECT_REG(R_A3, 150);
    END_TEST();
}

/* ================================================================
 *  6. Dirty slots across conditional branch (taken path)
 * ================================================================ */
static void test_dirty_branch_taken(void)
{
    /* Write to regs, then take a conditional branch to block 2.
     * Tests DeferredTakenEntry.saved_dyn_dirty + flush at branch epilogue. */
    BEGIN_TEST("dirty_branch_taken");
    SET_REG(R_T0, 0);
    SET_REG(R_V0, 1); /* non-zero for BNE */

    /* Block 1: write t0, then BNE to block 2 */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 0x5678)); /* t0 = 0x5678 (dirty) */
    EMIT(PSX_ADDIU(R_T1, R_ZERO, 0x1ABC)); /* t1 = 0x1ABC (dirty) */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    int16_t offset = (int16_t)(((block2_pc - (PG_CODE_BASE + 2 * 4)) >> 2) - 1);
    EMIT(PSX_BNE(R_V0, R_ZERO, offset)); /* BNE v0, $0, block2 */
    EMIT(PSX_NOP());                     /* delay slot */
    for (int i = 4; i < 8; i++)
        EMIT(PSX_NOP());

    /* Block 2: use t0 and t1 */
    EMIT(PSX_ADDU(R_A0, R_T0, R_T1)); /* a0 = 0x5678 + 0x1ABC */

    RUN(5000);
    EXPECT_REG(R_T0, 0x5678);
    EXPECT_REG(R_T1, 0x1ABC);
    EXPECT_REG(R_A0, 0x5678 + 0x1ABC);
    END_TEST();
}

/* ================================================================
 *  7. Dirty slots across conditional branch (not-taken path)
 * ================================================================ */
static void test_dirty_branch_not_taken(void)
{
    /* Write to regs, then do a BNE that is NOT taken (falls through).
     * The fall-through continues and eventually JR $ra exits.
     * Tests that the main-path dirty mask is preserved. */
    BEGIN_TEST("dirty_branch_not_taken");
    SET_REG(R_T0, 0);
    SET_REG(R_V0, 0); /* zero → BNE not taken */

    /* Block: write t0, BNE not taken, write t1, exit */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 0x111)); /* t0 = 0x111 (dirty) */
    /* BNE v0, $0 with small positive offset — NOT taken since v0=0 */
    EMIT(PSX_BNE(R_V0, R_ZERO, 2)); /* branch over 2 insns */
    EMIT(PSX_NOP());
    /* Fall-through: */
    EMIT(PSX_ADDIU(R_T1, R_ZERO, 0x222)); /* t1 = 0x222 (dirty) */

    RUN(5000);
    EXPECT_REG(R_T0, 0x111);
    EXPECT_REG(R_T1, 0x222);
    END_TEST();
}

/* ================================================================
 *  8. Dirty slots in loop iterations
 * ================================================================ */
static void test_dirty_loop(void)
{
    /* A loop accumulates into a non-pinned register.
     * Each iteration writes (dirtying the slot).
     * The loop exit path must flush the accumulated value. */
    BEGIN_TEST("dirty_loop");
    SET_REG(R_T0, 5); /* counter */
    SET_REG(R_T1, 0); /* accumulator */

    /* loop:
     *   0: ADDIU t1, t1, 10     (accumulate)
     *   1: ADDIU t0, t0, -1     (counter--)
     *   2: BNE   t0, zero, -3   (back to 0)
     *   3: NOP */
    EMIT(PSX_ADDIU(R_T1, R_T1, 10));
    EMIT(PSX_ADDIU(R_T0, R_T0, (uint16_t)(-1)));
    EMIT(PSX_BNE(R_T0, R_ZERO, (uint16_t)(-3)));
    EMIT(PSX_NOP());

    RUN(50000);
    EXPECT_REG(R_T0, 0);
    EXPECT_REG(R_T1, 50); /* 5 * 10 */
    END_TEST();
}

/* ================================================================
 *  9. Dirty slots + memory store (SW to RAM)
 * ================================================================ */
static void test_dirty_with_sw(void)
{
    /* Write to a dynamic slot, then use SW to store it to memory.
     * SW reads the slot value (not cpu.regs[]) for the data,
     * so the in-register value must be up-to-date.
     * Then jump to a new block that loads it back. */
    BEGIN_TEST("dirty_with_sw");
    SET_REG(R_T0, 0);

    /* Block 1: write t0, store to RAM, jump */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 0x4AFE));
    EMIT(PSX_LUI(R_T2, 0x8002)); /* t2 = data area base */
    EMIT(PSX_SW(R_T0, 0, R_T2)); /* MEM[data+0] = t0 */
    uint32_t block2_pc = PG_CODE_BASE + 12 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 5; i < 12; i++)
        EMIT(PSX_NOP());

    /* Block 2: load from same address */
    EMIT(PSX_LUI(R_T2, 0x8002));
    EMIT(PSX_LW(R_A0, 0, R_T2)); /* a0 = MEM[data+0] */

    RUN(8000);
    EXPECT_REG(R_T0, 0x4AFE);
    EXPECT_REG(R_A0, 0x4AFE);
    EXPECT_MEM32(PG_DATA_OFFSET, 0x4AFE);
    END_TEST();
}

/* ================================================================
 *  10. Three-block chain with progressive dirty accumulation
 *      Tests that each block exit correctly flushes its dirty state.
 * ================================================================ */
static void test_dirty_three_block_chain(void)
{
    BEGIN_TEST("dirty_three_block_chain");

    /* Block A (insns 0-7): write t0, t1, jump to B */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 1)); /* t0 = 1 */
    EMIT(PSX_ADDIU(R_T1, R_ZERO, 2)); /* t1 = 2 */
    uint32_t block_b = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block_b >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 4; i < 8; i++)
        EMIT(PSX_NOP());

    /* Block B (insns 8-15): accumulate into t0,t1, write t2, jump to C */
    pg_ctx.count = 8;
    EMIT(PSX_ADDIU(R_T0, R_T0, 10));  /* t0 = 1+10 = 11 */
    EMIT(PSX_ADDIU(R_T1, R_T1, 20));  /* t1 = 2+20 = 22 */
    EMIT(PSX_ADDIU(R_T2, R_ZERO, 3)); /* t2 = 3 (new) */
    uint32_t block_c = PG_CODE_BASE + 16 * 4;
    EMIT(PSX_J((block_c >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = pg_ctx.count; i < 16; i++)
        EMIT(PSX_NOP());

    /* Block C (insns 16+): final computation */
    pg_ctx.count = 16;
    EMIT(PSX_ADDU(R_A0, R_T0, R_T1)); /* a0 = 11+22 = 33 */
    EMIT(PSX_ADDU(R_A1, R_A0, R_T2)); /* a1 = 33+3 = 36 */

    RUN(10000);
    EXPECT_REG(R_T0, 11);
    EXPECT_REG(R_T1, 22);
    EXPECT_REG(R_T2, 3);
    EXPECT_REG(R_A0, 33);
    EXPECT_REG(R_A1, 36);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_dirty_tests(void)
{
    printf("\n--- Dirty Writeback ---\n");
    test_dirty_single_slot_j();
    test_nondirty_cross_block();
    test_dirty_mixed_cross_block();
    test_dirty_multi_write_same_slot();
    test_dirty_all_8_slots();
    test_dirty_branch_taken();
    test_dirty_branch_not_taken();
    test_dirty_loop();
    test_dirty_with_sw();
    test_dirty_three_block_chain();
}
