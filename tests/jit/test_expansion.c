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
extern int block_lite_calls;  /* set during compilation */
extern int block_full_calls;

/* Trampoline sizes (EE words) — counted from dynarec_run.c Init_Dynarec */
#define TRAMP_LITE_WORDS  24   /* 8 sw + 6 call + 8 lw + jr + nop */
#define TRAMP_FULL_WORDS  18   /* 4 sw pinned + 6 call + 4 lw pinned + jr + nop */

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

typedef struct {
    int native;       /* EE words in the compiled block */
    int lite_calls;   /* emit_call_c_lite count */
    int full_calls;   /* emit_call_c count */
    int effective;    /* native + trampoline overhead */
} ExpResult;

static ExpResult compile_and_measure_ex(const uint32_t *insns, int insn_types,
                                         int repeat, setup_fn_t setup)
{
    ExpResult res = {0, 0, 0, 0};

    /* Reset CPU state */
    memset(&cpu, 0, sizeof(cpu));
    cpu.regs[R_SP] = 0x801FFF00u;
    cpu.regs[R_RA] = PG_HALT_BASE;
    cpu.cop0[PSX_COP0_SR_IDX] = (1u << 28); /* CU0 enable */
    if (setup)
        setup();

    /* Reset JIT cache */
    pg_reset_jit_cache();

    /* Place instructions */
    uint32_t *code = (uint32_t *)(psx_ram + PG_CODE_OFFSET);
    memset(code, 0, 4096);
    int total_insns = 0;
    for (int r = 0; r < repeat; r++)
    {
        for (int i = 0; i < insn_types; i++)
        {
            if (total_insns >= PG_MAX_INSN - 2)
                break;
            code[total_insns++] = insns[i];
        }
    }
    code[total_insns] = PSX_JR(R_RA);
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
    if (!block || !be)
    {
        res.native = -1;
        return res;
    }
    res.native = (int)be->native_count;
    res.lite_calls = block_lite_calls;
    res.full_calls = block_full_calls;
    res.effective = res.native
                  + res.lite_calls * TRAMP_LITE_WORDS
                  + res.full_calls * TRAMP_FULL_WORDS;
    return res;
}

/* Legacy wrapper for simple tests that only need native count */
static int compile_and_measure(const uint32_t *insns, int insn_types,
                               int repeat, setup_fn_t setup)
{
    ExpResult r = compile_and_measure_ex(insns, insn_types, repeat, setup);
    return r.native;
}

/* ---- Assertion helper: check expansion within threshold ---- */
static void check_expansion(const char *name, int ee_words, int max_ee,
                            PGTestCtx *ctx)
{
    float ratio = (ee_words > 0) ? (float)ee_words / 18.0f : 0.0f; /* 18 PSX = 16 repeat + JR + NOP */
    if (ee_words <= max_ee)
    {
        printf("    %-16s  %4d EE  (%4.1fx)  [max %d] OK\n",
               name, ee_words, ratio, max_ee);
    }
    else
    {
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
    check_expansion("ADDU", ee, 42, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_ADDIU(R_T1, R_T2, 42)}, 1, REPEAT, NULL);
    check_expansion("ADDIU", ee, 41, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_AND(R_T1, R_T2, R_T3)}, 1, REPEAT, NULL);
    check_expansion("AND", ee, 42, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SLL(R_T1, R_T2, 5)}, 1, REPEAT, NULL);
    check_expansion("SLL", ee, 41, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_LUI(R_T1, 0x8000)}, 1, REPEAT, NULL);
    check_expansion("LUI", ee, 40, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SLT(R_T1, R_T2, R_T3)}, 1, REPEAT, NULL);
    check_expansion("SLT", ee, 42, &pg_ctx);

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
    check_expansion("MULT", ee, 151, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MULTU(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("MULTU", ee, 151, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_DIV(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("DIV", ee, 199, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_DIVU(R_T1, R_T2)}, 1, REPEAT, NULL);
    check_expansion("DIVU", ee, 167, &pg_ctx);

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
    check_expansion("LW", ee, 135, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SW(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("SW", ee, 400, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_LB(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("LB", ee, 135, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SB(R_T1, 0, R_SP)}, 1, REPEAT, NULL);
    check_expansion("SB", ee, 357, &pg_ctx);

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
    ExpResult r;

    ee = compile_and_measure(&(uint32_t){PSX_MTC2(R_T1, GTE_VXY0)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("MTC2", ee, 84, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_MFC2(R_T1, GTE_VXY0)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("MFC2", ee, 85, &pg_ctx);

    r = compile_and_measure_ex(&(uint32_t){GTE_CMD_RTPS(1, 1)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("COP2 RTPS", r.native, 210, &pg_ctx);
    printf("      -> effective: %d EE (%4.1fx) [%d lite calls × %d tramp]\n",
           r.effective, (float)r.effective / 18.0f, r.lite_calls, TRAMP_LITE_WORDS);

    r = compile_and_measure_ex(&(uint32_t){GTE_CMD_NCLIP}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("COP2 NCLIP", r.native, 295, &pg_ctx);
    if (r.lite_calls > 0)
        printf("      -> effective: %d EE (%4.1fx) [%d lite calls × %d tramp]\n",
               r.effective, (float)r.effective / 18.0f, r.lite_calls, TRAMP_LITE_WORDS);
    else
        printf("      -> INLINE (0 C calls) — effective = native\n");

    /* LWC2/SWC2 — coprocessor memory transfers (inline since P3 extension) */
    ee = compile_and_measure(&(uint32_t){PSX_LWC2(GTE_VXY0, 0, R_SP)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("LWC2", ee, 150, &pg_ctx);

    ee = compile_and_measure(&(uint32_t){PSX_SWC2(GTE_VXY0, 0, R_SP)}, 1, REPEAT, expansion_enable_cop2);
    check_expansion("SWC2", ee, 427, &pg_ctx);

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
        check_expansion("ALU chain", ee, 51, &pg_ctx);
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
        check_expansion("GTE xform", ee, 192, &pg_ctx);
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
        check_expansion("SW burst", ee, 406, &pg_ctx);
    }

    END_TEST();
}

/* ================================================================
 *  Test: Full GTE command expansion report
 *
 *  Measures native and effective expansion for all 22 GTE commands.
 *  This is a compile-only report (no assertions), used to prioritize
 *  which GTE ops to inline as native assembly.
 * ================================================================ */
static void test_expansion_gte_report(void)
{
    BEGIN_TEST("expansion_gte_report");
    ExpResult r;

    printf("\n  %-14s  %5s  %5s  %5s  %5s  %5s  %5s\n",
           "Command", "Native", "Ratio", "Lite", "Full", "Effec", "EffRatio");
    printf("  %-14s  %5s  %5s  %5s  %5s  %5s  %5s\n",
           "-----------", "-----", "-----", "----", "----", "-----", "--------");

    /* 18 PSX insns per block (16 repeat + JR + NOP) */
    #define GTE_REPORT(name, insn) do {                                          \
        r = compile_and_measure_ex(&(uint32_t){(insn)}, 1, REPEAT,              \
                                   expansion_enable_cop2);                       \
        printf("  %-14s  %5d  %4.1fx  %5d  %5d  %5d  %5.1fx\n",               \
               (name), r.native, (float)r.native/18.0f,                         \
               r.lite_calls, r.full_calls,                                       \
               r.effective, (float)r.effective/18.0f);                           \
    } while(0)

    /* --- Perspective Transform --- */
    GTE_REPORT("RTPS",   GTE_CMD_RTPS(1, 1));
    GTE_REPORT("RTPT",   GTE_CMD_RTPT(1, 1));

    /* --- Geometry --- */
    GTE_REPORT("NCLIP",  GTE_CMD_NCLIP);
    GTE_REPORT("AVSZ3",  GTE_CMD_AVSZ3);
    GTE_REPORT("AVSZ4",  GTE_CMD_AVSZ4);

    /* --- Simple vector --- */
    GTE_REPORT("OP",     GTE_CMD_OP(1, 0));
    GTE_REPORT("SQR",    GTE_CMD_SQR(1, 0));

    /* --- Interpolation --- */
    GTE_REPORT("GPF",    GTE_CMD_GPF(1, 0));
    GTE_REPORT("GPL",    GTE_CMD_GPL(1, 0));

    /* --- Depth cueing --- */
    GTE_REPORT("DPCS",   GTE_CMD_DPCS(1, 0));
    GTE_REPORT("INTPL",  GTE_CMD_INTPL(1, 0));
    GTE_REPORT("DCPL",   GTE_CMD_DCPL(1, 0));
    GTE_REPORT("DPCT",   GTE_CMD_DPCT(1, 0));

    /* --- Matrix ops --- */
    GTE_REPORT("MVMVA",  GTE_CMD_MVMVA(1, 0, 0, 0, 0));

    /* --- Normal color --- */
    GTE_REPORT("NCS",    GTE_CMD_NCS(1, 1));
    GTE_REPORT("NCT",    GTE_CMD_NCT(1, 1));
    GTE_REPORT("NCCS",   GTE_CMD_NCCS(1, 1));
    GTE_REPORT("NCCT",   GTE_CMD_NCCT(1, 1));
    GTE_REPORT("CC",     GTE_CMD_CC(1, 1));
    GTE_REPORT("CDP",    GTE_CMD_CDP(1, 1));
    GTE_REPORT("NCDS",   GTE_CMD_NCDS(1, 1));
    GTE_REPORT("NCDT",   GTE_CMD_NCDT(1, 1));

    #undef GTE_REPORT

    printf("\n");
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
    test_expansion_gte_report();
}
