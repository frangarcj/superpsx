/*
 * JIT Playground — Expansion Ratio Tests
 *
 * Measures the code expansion ratio (native EE words per PSX instruction)
 * for each instruction category.  Each test compiles a block and asserts
 * that native_count does not exceed a threshold (current baseline).
 *
 * When an optimization reduces expansion, LOWER the threshold to lock in
 * the improvement.  Tests FAIL if expansion regresses above the threshold.
 *
 * The expansion ratio is the key metric for JIT code density optimization.
 * Lower is better — PSX and EE share the MIPS ISA, so the ideal ratio
 * for simple ALU ops is ~1.0x (1 native word per PSX instruction).
 *
 * NOTE: native_count includes per-block overhead (prologue, epilogue,
 * slot load/store, cycle accounting).  To isolate per-instruction cost,
 * compare blocks of different sizes.
 */
#include "playground.h"
#include <string.h>

/* ---- Externs from dynarec ---- */
extern uint32_t *dynarec_ensure_block(uint32_t pc, BlockEntry **out_be);

/* ---- Helper: enable COP2 in SR (needed for GTE instructions) ---- */
static void expansion_enable_cop2(void)
{
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 30) | (1u << 28);
}

/* ================================================================
 *  Core measurement function
 *
 *  Places `count` repetitions of `insn` into code area, appends
 *  JR $ra + NOP, compiles via dynarec, and returns native_count.
 * ================================================================ */
typedef void (*setup_fn_t)(void);

static int compile_and_measure(const uint32_t *insns, int insn_types,
                               int repeat, setup_fn_t setup)
{
    /* Reset CPU state */
    memset(&cpu, 0, sizeof(cpu));
    cpu.regs[R_SP] = 0x801FFF00u;
    cpu.regs[R_RA] = PG_HALT_BASE;
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 28); /* CU0 enable */
    if (setup) setup();

    /* Reset JIT cache */
    pg_reset_jit_cache();

    /* Place instructions */
    uint32_t *code = (uint32_t *)(psx_ram + PG_CODE_OFFSET);
    memset(code, 0, 4096);
    int total_insns = 0;
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < insn_types; i++) {
            if (total_insns >= PG_MAX_INSN - 2) break;
            code[total_insns++] = insns[i];
        }
    }
    code[total_insns]     = PSX_JR(R_RA);
    code[total_insns + 1] = PSX_NOP();

    /* Install halt loop */
    memset(psx_ram + PG_HALT_OFFSET, 0, 256);
    {
        uint32_t *halt = (uint32_t *)(psx_ram + PG_HALT_OFFSET);
        halt[0] = MK_I(0x04, 0, 0, (uint16_t)(-1));
        halt[1] = 0x00000000u;
    }

    /* Compile */
    BlockEntry *be = NULL;
    uint32_t *block = dynarec_ensure_block(PG_CODE_BASE, &be);
    if (!block || !be) return -1;
    return (int)be->native_count;
}

/* ---- Assertion helper: check expansion within threshold ---- */
static void check_expansion(const char *name, int ee_words, int max_ee,
                             PGTestCtx *ctx)
{
    float ratio = (ee_words > 0) ? (float)ee_words / 18.0f : 0.0f; /* 18 PSX = 16 repeat + JR + NOP */
    if (ee_words <= max_ee) {
        printf("    %-16s  %4d EE  (%4.1fx)  [max %d] OK\n",
               name, ee_words, ratio, max_ee);
    } else {
        printf("  [FAIL] %-16s  %4d EE  (%4.1fx)  EXCEEDS max %d\n",
               name, ee_words, ratio, max_ee);
        ctx->fail_count++;
    }
}

#define REPEAT 16

/* ================================================================
 *  Test: ALU expansion (ADDU, ADDIU, SLL, LUI, etc.)
 *  Current baseline: 36-38 EE words for 18 PSX insns (~2.1x)
 * ================================================================ */
static void test_expansion_alu(void)
{
    BEGIN_TEST("expansion_alu");
    int ee;

    ee = compile_and_measure(&(uint32_t){PSX_ADDU(R_T1, R_T2, R_T3)}, 1, REPEAT, NULL);
    check_expansion("ADDU", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_ADDIU(R_T1, R_T2, 42)}, 1, REPEAT, NULL);
    check_expansion("ADDIU", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_AND(R_T1, R_T2, R_T3)}, 1, REPEAT, NULL);
    check_expansion("AND", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SLL(R_T1, R_T2, 5)}, 1, REPEAT, NULL);
    check_expansion("SLL", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_LUI(R_T1, 0x8000)}, 1, REPEAT, NULL);
    check_expansion("LUI", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SLT(R_T1, R_T2, R_T3)}, 1, REPEAT, NULL);
    check_expansion("SLT", ee, 40, &pg_ctx);

    END_TEST();
}

/* ================================================================
 *  Test: Multiply/Divide expansion
 *  MULT: 147 EE (8.2x), DIV: 275 EE (15.3x), DIVU: 243 EE (13.5x)
 * ================================================================ */
static void test_expansion_muldiv(void)
{
    BEGIN_TEST("expansion_muldiv");
    int ee;

    ee = compile_and_measure(&(uint32_t){PSX_MULT(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("MULT", ee, 150, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MULTU(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("MULTU", ee, 150, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_DIV(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("DIV", ee, 280, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_DIVU(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("DIVU", ee, 250, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MFHI(R_T1)}, 1, REPEAT, NULL);
    check_expansion("MFHI", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MFLO(R_T1)}, 1, REPEAT, NULL);
    check_expansion("MFLO", ee, 40, &pg_ctx);

    END_TEST();
}

/* ================================================================
 *  Test: Load/Store expansion
 *  LW: 420 (23.3x), SW: 481 (26.7x), LB: 131 (7.3x)
 * ================================================================ */
static void test_expansion_loadstore(void)
{
    BEGIN_TEST("expansion_loadstore");
    int ee;

    ee = compile_and_measure(&(uint32_t){PSX_LW(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("LW", ee, 425, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SW(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("SW", ee, 485, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_LB(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("LB", ee, 135, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SB(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("SB", ee, 355, &pg_ctx);

    END_TEST();
}

/* ================================================================
 *  Test: COP2/GTE expansion
 *  MTC2: 433 (24.1x), MFC2: 305 (16.9x), RTPS: 416 (23.1x)
 * ================================================================ */
static void test_expansion_gte(void)
{
    BEGIN_TEST("expansion_gte");
    int ee;

    ee = compile_and_measure(&(uint32_t){PSX_MTC2(R_T1, GTE_VXY0)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("MTC2", ee, 90, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MFC2(R_T1, GTE_VXY0)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("MFC2", ee, 90, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){GTE_CMD_RTPS(1, 1)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("COP2 RTPS", ee, 230, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){GTE_CMD_NCLIP}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("COP2 NCLIP", ee, 200, &pg_ctx);

    END_TEST();
}

/* ================================================================
 *  Test: Mixed game patterns
 *  GTE xform: 424 (24.9x), SW burst: 484 (26.9x)
 * ================================================================ */
static void test_expansion_mixed(void)
{
    BEGIN_TEST("expansion_mixed");
    int ee;

    /* ALU chain */
    {
        uint32_t alu_chain[] = {
            PSX_ADDU(R_T1, R_T2, R_T3),
            PSX_ANDI(R_T4, R_T1, 0xFF),
            PSX_SLL(R_T5, R_T4, 2),
            PSX_OR(R_T6, R_T5, R_T1),
        };
        ee = compile_and_measure(alu_chain, 4, 4, NULL);
        check_expansion("ALU chain", ee, 50, &pg_ctx);
    }

    /* GTE transform (Crash-like) */
    {
        uint32_t gte_xform[] = {
            PSX_MTC2(R_T1, GTE_VXY0),
            PSX_MTC2(R_T2, GTE_VZ0),
            GTE_CMD_RTPS(1, 1),
            PSX_MFC2(R_T3, GTE_SXY2),
            PSX_SW(R_T3, 0, R_SP),
        };
        ee = compile_and_measure(gte_xform, 5, 3, expansion_enable_cop2);
        check_expansion("GTE xform", ee, 210, &pg_ctx);
    }

    /* SW burst */
    {
        uint32_t sw_burst[] = {
            PSX_SW(R_T1, 0, R_SP),
            PSX_SW(R_T2, 4, R_SP),
            PSX_SW(R_T3, 8, R_SP),
            PSX_SW(R_T4, 12, R_SP),
        };
        ee = compile_and_measure(sw_burst, 4, 4, NULL);
        check_expansion("SW burst", ee, 490, &pg_ctx);
    }

    END_TEST();
}


/* ================================================================
 *  Category runner
 * ================================================================ */
void pg_run_expansion_tests(void)
{
    printf("--- Expansion Ratio Tests ---\n");
    test_expansion_alu();
    test_expansion_muldiv();
    test_expansion_loadstore();
    test_expansion_gte();
    test_expansion_mixed();
}
