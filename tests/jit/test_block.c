/*
 * JIT Playground — Block System & Interaction Tests
 *
 * Covers: store-load forwarding, loops, JAL/JR chains,
 *         cross-block register persistence (pinned + non-pinned),
 *         multi-block chains, super-blocks, nested calls, conditional paths,
 *         all-32-regs comprehensive test, dynamic allocator stress.
 * 21 tests total.
 */
#include "playground.h"

/* ================================================================
 *  Instruction Interactions
 * ================================================================ */

static void test_store_load_forwarding(void)
{
    /* SW followed by LW to same address */
    BEGIN_TEST("store_load_forward");
    EMIT(PSX_LUI(R_T0, 0x8002));
    SET_REG(R_V0, 0xDEADC0DE);
    EMIT(PSX_SW(R_V0, 0, R_T0));
    EMIT(PSX_LW(R_A0, 0, R_T0));
    RUN(2000);
    EXPECT_REG(R_A0, 0xDEADC0DE);
    END_TEST();
}

static void test_loop_counter(void)
{
    /* Simple loop: decrement counter until zero.
     * Tests BNE + ADDIU interaction, super-block fall-through. */
    BEGIN_TEST("loop_counter");
    SET_REG(R_V0, 5);                      /* counter = 5 */
    SET_REG(R_A0, 0);                      /* accumulator */

    /*
     * loop:
     *   0: ADDIU  a0, a0, 1                (accumulate)
     *   1: ADDIU  v0, v0, -1               (counter--)
     *   2: BNE    v0, zero, -3             (branch to insn 0 if v0 != 0)
     *   3: NOP                              (delay slot)
     *   (fall-through when v0==0)
     */
    EMIT(PSX_ADDIU(R_A0, R_A0, 1));
    EMIT(PSX_ADDIU(R_V0, R_V0, (uint16_t)(-1)));
    EMIT(PSX_BNE(R_V0, R_ZERO, (uint16_t)(-3)));  /* branch offset = -3 → back to insn 0 */
    EMIT(PSX_NOP());

    RUN(50000);  /* enough cycles for 5 iterations */
    EXPECT_REG(R_A0, 5);                   /* accumulated 5 times */
    EXPECT_REG(R_V0, 0);                   /* counter reached 0 */
    END_TEST();
}

static void test_jal_jr_ra(void)
{
    /* JAL writes PC+8 into $ra, then we JR $ra from the subroutine.
     *
     * Layout (all relative to PG_CODE_BASE = 0x80010000):
     *   0: JAL  0x80010014  (= insn 5)     → $ra = 0x80010008
     *   1: NOP               (delay slot)
     *   2: ADDIU a0, zero, 1 (return here) ← $ra points here (0x80010008)
     *
     * At insn 5 (subroutine):
     *   5: ADDIU a1, zero, 2
     *   6: JR    $ra
     *   7: NOP
     */
    BEGIN_TEST("jal_jr_ra");
    uint32_t sub_addr = PG_CODE_BASE + 5 * 4;  /* 0x80010014 */
    uint32_t jal_target = (sub_addr >> 2) & 0x03FFFFFF;
    EMIT(PSX_JAL(jal_target));              /* 0: JAL subroutine */
    EMIT(PSX_NOP());                        /* 1: delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));      /* 2: executed after return */
    /* JR $ra + NOP will be auto-appended by RUN() at insns 3,4 */
    /* Manually place subroutine at insns 5,6,7 */
    pg_ctx.code[5] = PSX_ADDIU(R_A1, R_ZERO, 2);
    pg_ctx.code[6] = PSX_JR(R_RA);
    pg_ctx.code[7] = PSX_NOP();
    pg_ctx.count = 3; /* RUN will append JR+NOP at 3,4 */
    RUN(5000);
    EXPECT_REG(R_A0, 1);                   /* executed after return */
    EXPECT_REG(R_A1, 2);                   /* executed in subroutine */
    EXPECT_REG(R_RA, PG_CODE_BASE + 8);    /* ra = PC+8 of JAL */
    END_TEST();
}

/* ================================================================
 *  Cross-Block Register Persistence
 * ================================================================ */

static void test_pinned_regs_cross_block(void)
{
    /* Set all 10 pinned PSX registers, branch to a new block, verify all
     * survive.  Pinned regs: v0(2), v1(3), a0(4), a1(5), a2(6),
     * s0(16), s1(17), gp(28), sp(29), ra(31). */
    BEGIN_TEST("pinned_cross_block");
    SET_REG(R_V0, 0xAAAA0002);
    SET_REG(R_V1, 0xAAAA0003);
    SET_REG(R_A0, 0xAAAA0004);
    SET_REG(R_A1, 0xAAAA0005);
    SET_REG(R_A2, 0xAAAA0006);
    SET_REG(R_S0, 0xAAAA0010);
    SET_REG(R_S1, 0xAAAA0011);
    SET_REG(R_GP, 0xAAAA001C);
    SET_REG(R_SP, 0xAAAA001D);
    /* Note: $ra is set to HALT by BEGIN_TEST, we override it here */
    SET_REG(R_RA, PG_HALT_BASE);

    /* Block 1: J to block 2 (forces new block) */
    uint32_t block2_pc = PG_CODE_BASE + 16 * 4;  /* insn 16 */
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    /* Fill gap with NOPs so block 2 is at a known offset */
    for (int i = 2; i < 16; i++) EMIT(PSX_NOP());

    /* Block 2: just a NOP — prologue loads pinned regs, epilogue flushes */
    EMIT(PSX_NOP()); /* block 2 starts here */

    RUN(5000);
    EXPECT_REG(R_V0, 0xAAAA0002);
    EXPECT_REG(R_V1, 0xAAAA0003);
    EXPECT_REG(R_A0, 0xAAAA0004);
    EXPECT_REG(R_A1, 0xAAAA0005);
    EXPECT_REG(R_A2, 0xAAAA0006);
    EXPECT_REG(R_S0, 0xAAAA0010);
    EXPECT_REG(R_S1, 0xAAAA0011);
    EXPECT_REG(R_GP, 0xAAAA001C);
    EXPECT_REG(R_SP, 0xAAAA001D);
    END_TEST();
}

static void test_nonpinned_regs_cross_block(void)
{
    /* Test non-pinned registers survive across a block boundary.
     * Non-pinned: t0(8), t1(9), t2(10), t3(11), t4(12), t5(13),
     * t6(14), t7(15), s2(18), s3(19), s4(20), s5(21), s6(22),
     * s7(23), t8(24), t9(25), k0(26), k1(27), fp(30), at(1). */
    BEGIN_TEST("nonpinned_cross_block");

    /* Set a selection of non-pinned registers */
    SET_REG(R_T0, 0xBBBB0008);
    SET_REG(R_T1, 0xBBBB0009);
    SET_REG(R_T2, 0xBBBB000A);
    SET_REG(R_T3, 0xBBBB000B);
    SET_REG(R_T4, 0xBBBB000C);
    SET_REG(R_T5, 0xBBBB000D);
    SET_REG(R_S2, 0xBBBB0012);
    SET_REG(R_S3, 0xBBBB0013);
    SET_REG(R_S4, 0xBBBB0014);
    SET_REG(R_S5, 0xBBBB0015);

    /* Block 1: jump forward to force block boundary */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 2; i < 8; i++) EMIT(PSX_NOP());

    /* Block 2: touch a few regs to force the JIT to load them */
    EMIT(PSX_ADDU(R_T0, R_T0, R_ZERO));  /* t0 = t0 (force read) */
    EMIT(PSX_ADDU(R_T1, R_T1, R_ZERO));  /* t1 = t1 */
    EMIT(PSX_ADDU(R_S2, R_S2, R_ZERO));  /* s2 = s2 */

    RUN(5000);
    EXPECT_REG(R_T0, 0xBBBB0008);
    EXPECT_REG(R_T1, 0xBBBB0009);
    EXPECT_REG(R_T2, 0xBBBB000A);
    EXPECT_REG(R_T3, 0xBBBB000B);
    EXPECT_REG(R_T4, 0xBBBB000C);
    EXPECT_REG(R_T5, 0xBBBB000D);
    EXPECT_REG(R_S2, 0xBBBB0012);
    EXPECT_REG(R_S3, 0xBBBB0013);
    EXPECT_REG(R_S4, 0xBBBB0014);
    EXPECT_REG(R_S5, 0xBBBB0015);
    END_TEST();
}

static void test_reg_write_cross_block(void)
{
    /* Write to a register in block 1, read it in block 2.
     * This verifies the write-through / dirty flush path. */
    BEGIN_TEST("reg_write_cross_block");
    SET_REG(R_V0, 100);
    SET_REG(R_V1, 200);

    /* Block 1: compute a0 = v0 + v1, then branch */
    EMIT(PSX_ADDU(R_A0, R_V0, R_V1));     /* a0 = 300 */
    EMIT(PSX_ADDIU(R_T0, R_ZERO, 42));    /* t0 = 42 (non-pinned) */
    uint32_t block2_pc = PG_CODE_BASE + 8 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 4; i < 8; i++) EMIT(PSX_NOP());

    /* Block 2: use a0 and t0 — must reflect block 1's writes */
    EMIT(PSX_ADDU(R_A1, R_A0, R_T0));     /* a1 = 300 + 42 = 342 */

    RUN(5000);
    EXPECT_REG(R_A0, 300);
    EXPECT_REG(R_T0, 42);
    EXPECT_REG(R_A1, 342);
    END_TEST();
}

static void test_multi_block_chain(void)
{
    /* Chain: Block A → Block B → Block C.
     * Each block modifies a register. Verify all survive. */
    BEGIN_TEST("multi_block_chain");
    SET_REG(R_V0, 10);

    /* Block A starts at PG_CODE_BASE */
    uint32_t block_b = PG_CODE_BASE + 8 * 4;
    uint32_t block_c = PG_CODE_BASE + 16 * 4;

    EMIT(PSX_ADDIU(R_A0, R_V0, 1));       /* a0 = 11 */
    EMIT(PSX_J((block_b >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 3; i < 8; i++) EMIT(PSX_NOP());

    /* Block B */
    EMIT(PSX_ADDIU(R_A1, R_A0, 2));       /* a1 = 13 */
    EMIT(PSX_J((block_c >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 11; i < 16; i++) EMIT(PSX_NOP());

    /* Block C */
    EMIT(PSX_ADDIU(R_A2, R_A1, 3));       /* a2 = 16 */

    RUN(10000);
    EXPECT_REG(R_A0, 11);
    EXPECT_REG(R_A1, 13);
    EXPECT_REG(R_A2, 16);
    END_TEST();
}

static void test_super_block_fallthrough(void)
{
    /* BEQ not taken → super-block fall-through.
     * Verify const propagation and register state across the
     * continuation. */
    BEGIN_TEST("super_block_fallthru");
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);  /* v0 != v1 → BEQ not taken */

    /* insn 0: BEQ offset=5 → target=(delay+20)=insn6 */
    EMIT(PSX_BEQ(R_V0, R_V1, 5));
    EMIT(PSX_NOP());                       /* 1: delay slot */

    /* Fall-through path (insns 2-4): */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 100));   /* 2: a0 = 100 */
    EMIT(PSX_ADDIU(R_A1, R_A0, 50));      /* 3: a1 = 150 */
    EMIT(PSX_JR(R_RA));                    /* 4: exit fall-through */
    EMIT(PSX_NOP());                       /* 5: delay */

    /* Taken target (insn 6) — should NOT be reached */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 999));

    RUN(5000);
    EXPECT_REG(R_A0, 100);
    EXPECT_REG(R_A1, 150);
    EXPECT_REG(R_A2, 0);  /* not reached */
    END_TEST();
}

static void test_super_block_taken(void)
{
    /* BEQ taken → cold deferred path.
     * Verify correct register state on the taken path. */
    BEGIN_TEST("super_block_taken");
    SET_REG(R_V0, 5);
    SET_REG(R_V1, 5);  /* v0 == v1 → BEQ taken */

    /* insn 0: BEQ taken → skip to insn 5 */
    EMIT(PSX_BEQ(R_V0, R_V1, 3));         /* +3 insns from delay slot */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));    /* delay: a0 = 11 (always executes) */

    /* insn 2-4: fall-through (skipped) */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 99));    /* skipped */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 99));    /* skipped */
    EMIT(PSX_NOP());                       /* skipped */

    /* insn 5: taken target */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));    /* a1 = 22 */
    EMIT(PSX_ADDIU(R_A2, R_A0, 33));      /* a2 = 11 + 33 = 44 */

    RUN(5000);
    EXPECT_REG(R_A0, 11);   /* delay slot */
    EXPECT_REG(R_A1, 22);   /* taken target */
    EXPECT_REG(R_A2, 44);   /* uses delay slot value */
    END_TEST();
}

static void test_nested_jal(void)
{
    /* Nested subroutine: main → sub1 → sub2 → return chain.
     * Tests $ra save/restore across JAL/JR levels.
     *
     * Layout:
     *  0: LUI t0, 0x8002           (data base)
     *  1: SW  ra, 4(t0)            (save original ra at data+4)
     *  2: JAL sub1                  ($ra = insn 4 addr)
     *  3: NOP                       (delay)
     *  4: ADDIU a2, zero, 33       (after return from sub1)
     *  5: LUI t0, 0x8002
     *  6: LW  ra, 4(t0)            (restore original ra → PG_HALT_BASE)
     *  7: JR  ra                    (exit to halt)
     *  8: NOP                       (delay)
     *  ...pad 9-11...
     *
     * sub1 at insn 12:
     * 12: LUI t0, 0x8002
     * 13: SW  ra, 0(t0)            (save main's ra at data+0)
     * 14: JAL sub2                  ($ra = insn 16 addr)
     * 15: NOP                       (delay)
     * 16: ADDIU a1, zero, 22
     * 17: LUI t0, 0x8002
     * 18: LW  ra, 0(t0)            (restore main's ra)
     * 19: JR  ra                    (return to main insn 4)
     * 20: NOP                       (delay)
     *  ...pad 21-23...
     *
     * sub2 at insn 24:
     * 24: ADDIU a0, zero, 11
     * 25: JR ra                     (return to sub1 insn 16)
     * 26: NOP                       (delay)
     */
    BEGIN_TEST("nested_jal");

    uint32_t sub1_addr = PG_CODE_BASE + 12 * 4;
    uint32_t sub2_addr = PG_CODE_BASE + 24 * 4;

    /* Main block */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 0 */
    EMIT(PSX_SW(R_RA, 4, R_T0));                    /* 1: save original ra */
    EMIT(PSX_JAL((sub1_addr >> 2) & 0x03FFFFFF));   /* 2: JAL sub1 */
    EMIT(PSX_NOP());                                 /* 3: delay */
    EMIT(PSX_ADDIU(R_A2, R_ZERO, 33));              /* 4: a2=33 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 5 */
    EMIT(PSX_LW(R_RA, 4, R_T0));                    /* 6: restore original ra */
    EMIT(PSX_JR(R_RA));                              /* 7: exit to halt */
    EMIT(PSX_NOP());                                 /* 8: delay */
    for (int i = 9; i < 12; i++) EMIT(PSX_NOP());   /* pad */

    /* sub1 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 12 */
    EMIT(PSX_SW(R_RA, 0, R_T0));                    /* 13: save main's ra */
    EMIT(PSX_JAL((sub2_addr >> 2) & 0x03FFFFFF));   /* 14: JAL sub2 */
    EMIT(PSX_NOP());                                 /* 15: delay */
    EMIT(PSX_ADDIU(R_A1, R_ZERO, 22));              /* 16: a1=22 */
    EMIT(PSX_LUI(R_T0, 0x8002));                    /* 17 */
    EMIT(PSX_LW(R_RA, 0, R_T0));                    /* 18: restore main's ra */
    EMIT(PSX_JR(R_RA));                              /* 19: return to main */
    EMIT(PSX_NOP());                                 /* 20: delay */
    for (int i = 21; i < 24; i++) EMIT(PSX_NOP());  /* pad */

    /* sub2 */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 11));              /* 24: a0=11 */
    EMIT(PSX_JR(R_RA));                              /* 25: return to sub1 */
    EMIT(PSX_NOP());                                 /* 26: delay */

    RUN(30000);
    EXPECT_REG(R_A0, 11);   /* set by sub2 */
    EXPECT_REG(R_A1, 22);   /* set by sub1 */
    EXPECT_REG(R_A2, 33);   /* set after return to main */
    END_TEST();
}

static void test_loop_accumulate_memory(void)
{
    /* Loop that modifies both registers and memory each iteration.
     * Verifies cross-block state for loops with memory side-effects. */
    BEGIN_TEST("loop_accum_mem");

    /* Set up: counter=3, accumulator=0, data area cleared */
    SET_REG(R_V0, 3);     /* counter */
    SET_REG(R_A0, 0);     /* accumulator */

    /* loop:
     *   0: ADDIU a0, a0, 10          accumulate
     *   1: LUI   t0, 0x8002          data base
     *   2: SW    a0, 0(t0)           store current acc to mem
     *   3: ADDIU v0, v0, -1          counter--
     *   4: BNE   v0, zero, -5        branch back to insn 0
     *   5: NOP                        delay
     */
    EMIT(PSX_ADDIU(R_A0, R_A0, 10));
    EMIT(PSX_LUI(R_T0, 0x8002));
    EMIT(PSX_SW(R_A0, 0, R_T0));
    EMIT(PSX_ADDIU(R_V0, R_V0, (uint16_t)(-1)));
    EMIT(PSX_BNE(R_V0, R_ZERO, (uint16_t)(-5)));
    EMIT(PSX_NOP());

    RUN(50000);
    EXPECT_REG(R_A0, 30);    /* 3 × 10 = 30 */
    EXPECT_REG(R_V0, 0);     /* counter reached 0 */
    EXPECT_MEM32(PG_DATA_OFFSET, 30);  /* last store was acc=30 */
    END_TEST();
}

static void test_conditional_both_paths(void)
{
    /* Run the same code with two different initial conditions.
     * Path 1: v0==v1 → taken
     * Path 2: v0!=v1 → not taken
     * Verifies the JIT works correctly for both paths of the SAME block. */
    /* Path 1: BEQ taken (v0==v1). Offset=4 → target=(delay+16)=insn5 */
    BEGIN_TEST("cond_path_taken");
    SET_REG(R_V0, 7);
    SET_REG(R_V1, 7);
    EMIT(PSX_BEQ(R_V0, R_V1, 4));         /* 0: BEQ taken */
    EMIT(PSX_NOP());                       /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));     /* 2: fall-through value */
    EMIT(PSX_JR(R_RA));                    /* 3: exit fall-through */
    EMIT(PSX_NOP());                       /* 4: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 2));     /* 5: taken value */
    RUN(5000);
    EXPECT_REG(R_A0, 2);
    END_TEST();

    /* Path 2: BEQ not taken (v0!=v1). Same layout, different result. */
    BEGIN_TEST("cond_path_not_taken");
    SET_REG(R_V0, 7);
    SET_REG(R_V1, 8);
    EMIT(PSX_BEQ(R_V0, R_V1, 4));         /* 0: BEQ not taken */
    EMIT(PSX_NOP());                       /* 1: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 1));     /* 2: fall-through value */
    EMIT(PSX_JR(R_RA));                    /* 3: exit fall-through */
    EMIT(PSX_NOP());                       /* 4: delay */
    EMIT(PSX_ADDIU(R_A0, R_ZERO, 2));     /* 5: taken target (not reached) */
    RUN(5000);
    EXPECT_REG(R_A0, 1);
    END_TEST();
}

static void test_all_32_regs(void)
{
    /* Comprehensive: set all 31 non-zero GPRs, execute a block,
     * verify all survive. $zero is always 0. $ra is special (halt). */
    BEGIN_TEST("all_32_regs");

    /* Set regs 1-30 to distinct values. $ra(31) kept as halt. */
    for (int i = 1; i <= 30; i++)
        cpu.regs[i] = 0xA0000000u | (uint32_t)i;

    /* Keep $ra = halt so JR $ra exits cleanly */
    cpu.regs[R_RA] = PG_HALT_BASE;
    /* Keep $sp reasonable for stack operations in prologue */
    cpu.regs[R_SP] = 0x801FFF00u;

    /* A single NOP block — just compiles and exits.
     * The prologue loads pinned regs, epilogue flushes them. */
    EMIT(PSX_NOP());

    RUN(2000);

    EXPECT_REG(R_ZERO, 0);
    EXPECT_REG(R_AT, 0xA0000001);
    EXPECT_REG(R_V0, 0xA0000002);
    EXPECT_REG(R_V1, 0xA0000003);
    EXPECT_REG(R_A0, 0xA0000004);
    EXPECT_REG(R_A1, 0xA0000005);
    EXPECT_REG(R_A2, 0xA0000006);
    EXPECT_REG(R_A3, 0xA0000007);
    EXPECT_REG(R_T0, 0xA0000008);
    EXPECT_REG(R_T1, 0xA0000009);
    EXPECT_REG(R_T2, 0xA000000A);
    EXPECT_REG(R_T3, 0xA000000B);
    EXPECT_REG(R_T4, 0xA000000C);
    EXPECT_REG(R_T5, 0xA000000D);
    EXPECT_REG(R_T6, 0xA000000E);
    EXPECT_REG(R_T7, 0xA000000F);
    EXPECT_REG(R_S0, 0xA0000010);
    EXPECT_REG(R_S1, 0xA0000011);
    EXPECT_REG(R_S2, 0xA0000012);
    EXPECT_REG(R_S3, 0xA0000013);
    EXPECT_REG(R_S4, 0xA0000014);
    EXPECT_REG(R_S5, 0xA0000015);
    EXPECT_REG(R_S6, 0xA0000016);
    EXPECT_REG(R_S7, 0xA0000017);
    EXPECT_REG(R_T8, 0xA0000018);
    EXPECT_REG(R_T9, 0xA0000019);
    /* k0/k1 may be clobbered by exception handling, skip */
    EXPECT_REG(R_GP, 0xA000001C);
    EXPECT_REG(R_SP, 0x801FFF00u); /* we set this specially */
    EXPECT_REG(R_FP, 0xA000001E);
    END_TEST();
}

/* ================================================================
 *  Dynamic Allocator Stress Tests
 *
 *  These validate that the dynamic register allocator works
 *  correctly with many non-pinned regs, across C-call trampolines,
 *  and across block boundaries with different slot assignments.
 *  Currently 3 dynamic slots (T0-T2); after refactor to 8 (T0-T7)
 *  these tests must still pass.
 * ================================================================ */

/* Use 8 non-pinned PSX regs (t0-t7) + a3 as accumulator in one block.
 * There are only 3 dynamic slots, so at least 6 regs must spill to
 * cpu.regs[].  After the refactor to 8 slots, all fit in hardware. */
static void test_many_dynamic_regs(void)
{
    BEGIN_TEST("many_dynamic_regs");
    SET_REG(R_T0, 10);
    SET_REG(R_T1, 20);
    SET_REG(R_T2, 30);
    SET_REG(R_T3, 40);
    SET_REG(R_T4, 50);
    SET_REG(R_T5, 60);
    SET_REG(R_T6, 70);
    SET_REG(R_T7, 80);

    /* Chain: a3 = sum of t0..t7 = 360 */
    EMIT(PSX_ADDU(R_A3, R_T0, R_T1));   /* a3 = 10+20 = 30  */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T2));   /* a3 = 30+30 = 60  */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T3));   /* a3 = 60+40 = 100 */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T4));   /* a3 = 100+50 = 150 */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T5));   /* a3 = 150+60 = 210 */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T6));   /* a3 = 210+70 = 280 */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T7));   /* a3 = 280+80 = 360 */

    /* Also write back to some non-pinned regs to test write-through */
    EMIT(PSX_ADDU(R_T0, R_T1, R_T2));   /* t0 = 20+30 = 50  (overwrite) */
    EMIT(PSX_ADDU(R_T3, R_T4, R_T5));   /* t3 = 50+60 = 110 (overwrite) */

    RUN(2000);

    EXPECT_REG(R_A3, 360);   /* accumulator result */
    EXPECT_REG(R_T0, 50);    /* overwritten */
    EXPECT_REG(R_T3, 110);   /* overwritten */
    /* Read-only inputs must survive in cpu.regs[] */
    EXPECT_REG(R_T1, 20);
    EXPECT_REG(R_T4, 50);
    EXPECT_REG(R_T7, 80);
    END_TEST();
}

/* Trigger SMC (Self-Modifying Code) detection mid-block by writing to
 * the code page (0x80010000).  This exercises the lite C-call trampoline
 * which saves/restores T0-T7.  Dynamic slot values must survive. */
static void test_dynamic_survives_smc(void)
{
    BEGIN_TEST("dynamic_survives_smc");
    /* Use non-pinned regs that go through dynamic slots */
    SET_REG(R_T0, 0xAABBCCDD);
    SET_REG(R_T1, 0x11223344);
    SET_REG(R_T2, 0x55667788);
    SET_REG(R_S2, 0xDEADDEAD);   /* data to write */

    /* Build code page address in t3 */
    EMIT(PSX_LUI(R_T3, 0x8001));               /* t3 = 0x80010000 (code page) */

    /* Compute a3 = t0 + t1 BEFORE the SMC write */
    EMIT(PSX_ADDU(R_A3, R_T0, R_T1));          /* a3 = 0xBBDE0021 */

    /* Write to code page → triggers SMC detection → lite trampoline
     * saves T0-T7, calls jit_smc_handler, restores T0-T7.
     * Writing at offset 0x100 avoids overwriting our own JR $ra exit. */
    EMIT(PSX_SW(R_S2, 0x100, R_T3));           /* SW to 0x80010100 */

    /* After trampoline returns, dynamic slots must still be correct.
     * Compute a3 += t2 using values that must survive the trampoline. */
    EMIT(PSX_ADDU(R_A3, R_A3, R_T2));          /* a3 = 0xBBDE0021 + 0x55667788 */

    RUN(5000);

    /* If lite trampoline corrupted T0-T2, a3 would be wrong */
    EXPECT_REG(R_A3, 0x114477A9u);
    /* Dynamic slot values must survive the trampoline */
    EXPECT_REG(R_T0, 0xAABBCCDD);
    EXPECT_REG(R_T1, 0x11223344);
    EXPECT_REG(R_T2, 0x55667788);
    END_TEST();
}

/* Two blocks with different dominant non-pinned regs.
 * Block 1: heavy use of t0-t3 (get dynamic slots T0-T2).
 * Block 2: heavy use of s2-s5 (different dynamic slot assignment).
 * Values computed in block 1 must be readable in block 2 via cpu.regs[]
 * (write-through ensures cpu.regs[] is always current). */
static void test_dynamic_cross_block_alloc(void)
{
    BEGIN_TEST("dynamic_cross_block_alloc");

    SET_REG(R_T0, 100);
    SET_REG(R_T1, 200);
    SET_REG(R_T2, 300);
    SET_REG(R_T3, 400);
    SET_REG(R_S2, 0);
    SET_REG(R_S3, 0);

    /* Block 1: heavy use of t0-t3 */
    EMIT(PSX_ADDU(R_T0, R_T0, R_T1));    /* t0 = 100+200 = 300 */
    EMIT(PSX_ADDU(R_T2, R_T2, R_T3));    /* t2 = 300+400 = 700 */
    EMIT(PSX_ADDU(R_T0, R_T0, R_T2));    /* t0 = 300+700 = 1000 */

    /* Force block boundary with J */
    uint32_t block2_pc = PG_CODE_BASE + 12 * 4;
    EMIT(PSX_J((block2_pc >> 2) & 0x03FFFFFF));
    EMIT(PSX_NOP());
    for (int i = 5; i < 12; i++) EMIT(PSX_NOP());  /* pad to insn 12 */

    /* Block 2: heavy use of s2-s3, reads t0 from block 1 */
    EMIT(PSX_ADDIU(R_S2, R_S2, 10));     /* s2 = 10 */
    EMIT(PSX_ADDIU(R_S3, R_S3, 20));     /* s3 = 20 */
    EMIT(PSX_ADDU(R_S2, R_S2, R_T0));    /* s2 = 10 + 1000 = 1010 */
    EMIT(PSX_ADDU(R_S3, R_S3, R_S2));    /* s3 = 20 + 1010 = 1030 */

    RUN(10000);

    /* Block 1 result must survive into block 2 */
    EXPECT_REG(R_T0, 1000);
    EXPECT_REG(R_T2, 700);
    /* Block 2 results using cross-block values */
    EXPECT_REG(R_S2, 1010);
    EXPECT_REG(R_S3, 1030);
    END_TEST();
}

/* ================================================================
 *  Prologue / Register Pin Tests
 *
 *  These validate that the prologue/epilogue correctly handles
 *  partial register use.  With a full prologue all tests pass;
 *  after partial-prologue optimization they must still pass.
 *
 *  Pinned caller-saved: v0→T3, v1→T4, a0→T5, a1→T6, a2→T7
 *  Pinned callee-saved: s0→S6, s1→S7, gp→FP, sp→S4, ra→S5
 * ================================================================ */

/* Block only uses v0 (caller-saved pin). All callee-saved pins
 * (s0, s1, gp, sp) must be preserved untouched.
 * ra is set by BEGIN_TEST to PG_HALT_BASE, so we check it separately. */
static void test_prologue_only_caller_pins(void)
{
    BEGIN_TEST("prologue_only_caller_pins");
    /* Pre-set callee-saved pinned regs to sentinel values */
    SET_REG(R_S0, 0x10101010);
    SET_REG(R_S1, 0x20202020);
    SET_REG(R_GP, 0x30303030);
    /* sp is already set to 0x801FFF00 by BEGIN_TEST */
    /* ra is set to PG_HALT_BASE by BEGIN_TEST */

    /* Also set caller-saved pins we don't use */
    SET_REG(R_V1, 0xBBBBBBBB);
    SET_REG(R_A1, 0xCCCCCCCC);
    SET_REG(R_A2, 0xDDDDDDDD);

    /* Block: only modifies v0, reads a0 */
    SET_REG(R_A0, 5);
    EMIT(PSX_ADDIU(R_V0, R_A0, 10));  /* v0 = a0 + 10 = 15 */
    RUN(2000);

    /* v0 should have the result */
    EXPECT_REG(R_V0, 15);
    /* Callee-saved pins must be untouched */
    EXPECT_REG(R_S0, 0x10101010);
    EXPECT_REG(R_S1, 0x20202020);
    EXPECT_REG(R_GP, 0x30303030);
    EXPECT_REG(R_SP, 0x801FFF00u);
    /* Unused caller-saved pins should also be preserved */
    EXPECT_REG(R_V1, 0xBBBBBBBB);
    EXPECT_REG(R_A1, 0xCCCCCCCC);
    EXPECT_REG(R_A2, 0xDDDDDDDD);
    END_TEST();
}

/* Block only uses callee-saved pins (s0, s1, gp).
 * Caller-saved pins (v0, v1, a0, a1, a2) must be preserved. */
static void test_prologue_only_callee_pins(void)
{
    BEGIN_TEST("prologue_only_callee_pins");
    /* Pre-set caller-saved pins to sentinel values */
    SET_REG(R_V0, 0xAA000001);
    SET_REG(R_V1, 0xAA000002);
    SET_REG(R_A0, 0xAA000003);
    SET_REG(R_A1, 0xAA000004);
    SET_REG(R_A2, 0xAA000005);

    /* Block: modify s0, read s1 */
    SET_REG(R_S1, 100);
    EMIT(PSX_ADDIU(R_S0, R_S1, 50));  /* s0 = s1 + 50 = 150 */
    RUN(2000);

    /* s0 should have the answer */
    EXPECT_REG(R_S0, 150);
    EXPECT_REG(R_S1, 100);
    /* All caller-saved pins must be untouched */
    EXPECT_REG(R_V0, 0xAA000001);
    EXPECT_REG(R_V1, 0xAA000002);
    EXPECT_REG(R_A0, 0xAA000003);
    EXPECT_REG(R_A1, 0xAA000004);
    EXPECT_REG(R_A2, 0xAA000005);
    END_TEST();
}

/* Block uses NO pinned registers at all — only non-pinned regs (t0-t7).
 * All 10 pinned regs must survive untouched. */
static void test_prologue_no_pinned_regs(void)
{
    BEGIN_TEST("prologue_no_pinned_regs");
    /* Pre-set all pinned regs */
    SET_REG(R_V0, 0x11000001);
    SET_REG(R_V1, 0x11000002);
    SET_REG(R_A0, 0x11000003);
    SET_REG(R_A1, 0x11000004);
    SET_REG(R_A2, 0x11000005);
    SET_REG(R_S0, 0x11000006);
    SET_REG(R_S1, 0x11000007);
    SET_REG(R_GP, 0x11000008);
    /* sp set by BEGIN_TEST, ra set by BEGIN_TEST */

    /* Block: only uses non-pinned regs t0, t1 */
    SET_REG(R_T0, 7);
    EMIT(PSX_ADDIU(R_T1, R_T0, 3));   /* t1 = t0 + 3 = 10 */
    RUN(2000);

    EXPECT_REG(R_T1, 10);
    /* ALL pinned regs must survive */
    EXPECT_REG(R_V0, 0x11000001);
    EXPECT_REG(R_V1, 0x11000002);
    EXPECT_REG(R_A0, 0x11000003);
    EXPECT_REG(R_A1, 0x11000004);
    EXPECT_REG(R_A2, 0x11000005);
    EXPECT_REG(R_S0, 0x11000006);
    EXPECT_REG(R_S1, 0x11000007);
    EXPECT_REG(R_GP, 0x11000008);
    EXPECT_REG(R_SP, 0x801FFF00u);
    END_TEST();
}

/* Block uses ALL 10 pinned regs — full prologue is required.
 * This is the "worst case" that must still work after optimization. */
static void test_prologue_all_pinned_rw(void)
{
    BEGIN_TEST("prologue_all_pinned_rw");
    /* Set up all 10 pinned regs with known values */
    SET_REG(R_V0, 1);
    SET_REG(R_V1, 2);
    SET_REG(R_A0, 3);
    SET_REG(R_A1, 4);
    SET_REG(R_A2, 5);
    SET_REG(R_S0, 10);
    SET_REG(R_S1, 20);
    SET_REG(R_GP, 30);
    /* sp is special (used by EE stack) — don't modify via PSX code */
    /* ra is set by BEGIN_TEST */

    /* Block: accumulate caller-saved into v0, callee-saved into s0 */
    EMIT(PSX_ADDU(R_V0, R_V0, R_V1));   /* v0 = 1+2 = 3 */
    EMIT(PSX_ADDU(R_V0, R_V0, R_A0));   /* v0 = 3+3 = 6 */
    EMIT(PSX_ADDU(R_V0, R_V0, R_A1));   /* v0 = 6+4 = 10 */
    EMIT(PSX_ADDU(R_V0, R_V0, R_A2));   /* v0 = 10+5 = 15 */
    EMIT(PSX_ADDU(R_S0, R_S0, R_S1));   /* s0 = 10+20 = 30 */
    EMIT(PSX_ADDU(R_S0, R_S0, R_GP));   /* s0 = 30+30 = 60 */
    RUN(2000);

    EXPECT_REG(R_V0, 15);
    EXPECT_REG(R_S0, 60);
    /* Regs that were only read should keep their values */
    EXPECT_REG(R_V1, 2);
    EXPECT_REG(R_A0, 3);
    EXPECT_REG(R_A1, 4);
    EXPECT_REG(R_A2, 5);
    EXPECT_REG(R_S1, 20);
    EXPECT_REG(R_GP, 30);
    END_TEST();
}

/* ================================================================
 *  Category Runner
 * ================================================================ */

void pg_run_block_tests(void)
{
    printf("\n--- Instruction Interactions ---\n");
    test_store_load_forwarding();
    test_loop_counter();
    test_jal_jr_ra();

    printf("\n--- Block System ---\n");
    test_pinned_regs_cross_block();
    test_nonpinned_regs_cross_block();
    test_reg_write_cross_block();
    test_multi_block_chain();
    test_super_block_fallthrough();
    test_super_block_taken();
    test_nested_jal();
    test_loop_accumulate_memory();
    test_conditional_both_paths();
    test_all_32_regs();

    printf("\n--- Dynamic Allocator Stress ---\n");
    test_many_dynamic_regs();
    test_dynamic_survives_smc();
    test_dynamic_cross_block_alloc();

    printf("\n--- Prologue / Register Pin ---\n");
    test_prologue_only_caller_pins();
    test_prologue_only_callee_pins();
    test_prologue_no_pinned_regs();
    test_prologue_all_pinned_rw();
}
