/*
 * JIT Playground — Test DSL Header
 *
 * Provides macros for:
 *   - Encoding R3000A (PSX) instructions
 *   - Setting up / tearing down test cases
 *   - Asserting CPU state after JIT execution
 */
#ifndef PLAYGROUND_H
#define PLAYGROUND_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- pull in MK_R / MK_I / MK_J from the dynarec header ---- */
#include "dynarec.h"

/* ================================================================
 *  PSX register aliases (R3000A GPR indices 0..31)
 * ================================================================ */
#define R_ZERO 0
#define R_AT   1
#define R_V0   2
#define R_V1   3
#define R_A0   4
#define R_A1   5
#define R_A2   6
#define R_A3   7
#define R_T0   8
#define R_T1   9
#define R_T2  10
#define R_T3  11
#define R_T4  12
#define R_T5  13
#define R_T6  14
#define R_T7  15
#define R_S0  16
#define R_S1  17
#define R_S2  18
#define R_S3  19
#define R_S4  20
#define R_S5  21
#define R_S6  22
#define R_S7  23
#define R_T8  24
#define R_T9  25
#define R_K0  26
#define R_K1  27
#define R_GP  28
#define R_SP  29
#define R_FP  30
#define R_RA  31

/* ================================================================
 *  PSX opcode encoding macros  (R3000A instruction set)
 *
 *  These produce a uint32_t that can be written into PSX RAM
 *  for the JIT to compile and execute.
 * ================================================================ */

/* R-type: SPECIAL (op=0) */
#define PSX_NOP()               0x00000000u
#define PSX_SLL(rd,rt,sa)       MK_R(0, 0,    (rt),(rd),(sa), 0x00)
#define PSX_SRL(rd,rt,sa)       MK_R(0, 0,    (rt),(rd),(sa), 0x02)
#define PSX_SRA(rd,rt,sa)       MK_R(0, 0,    (rt),(rd),(sa), 0x03)
#define PSX_SLLV(rd,rt,rs)      MK_R(0, (rs), (rt),(rd), 0,   0x04)
#define PSX_SRLV(rd,rt,rs)      MK_R(0, (rs), (rt),(rd), 0,   0x06)
#define PSX_SRAV(rd,rt,rs)      MK_R(0, (rs), (rt),(rd), 0,   0x07)
#define PSX_JR(rs)              MK_R(0, (rs), 0, 0, 0, 0x08)
#define PSX_JALR(rd,rs)         MK_R(0, (rs), 0,(rd), 0, 0x09)
#define PSX_SYSCALL()           MK_R(0, 0, 0, 0, 0, 0x0C)
#define PSX_BREAK()             MK_R(0, 0, 0, 0, 0, 0x0D)
#define PSX_MFHI(rd)            MK_R(0, 0, 0, (rd), 0, 0x10)
#define PSX_MTHI(rs)            MK_R(0, (rs), 0, 0, 0, 0x11)
#define PSX_MFLO(rd)            MK_R(0, 0, 0, (rd), 0, 0x12)
#define PSX_MTLO(rs)            MK_R(0, (rs), 0, 0, 0, 0x13)
#define PSX_MULT(rs,rt)         MK_R(0, (rs), (rt), 0, 0, 0x18)
#define PSX_MULTU(rs,rt)        MK_R(0, (rs), (rt), 0, 0, 0x19)
#define PSX_DIV(rs,rt)          MK_R(0, (rs), (rt), 0, 0, 0x1A)
#define PSX_DIVU(rs,rt)         MK_R(0, (rs), (rt), 0, 0, 0x1B)
#define PSX_ADD(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x20)
#define PSX_ADDU(rd,rs,rt)      MK_R(0, (rs), (rt),(rd), 0, 0x21)
#define PSX_SUB(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x22)
#define PSX_SUBU(rd,rs,rt)      MK_R(0, (rs), (rt),(rd), 0, 0x23)
#define PSX_AND(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x24)
#define PSX_OR(rd,rs,rt)        MK_R(0, (rs), (rt),(rd), 0, 0x25)
#define PSX_XOR(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x26)
#define PSX_NOR(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x27)
#define PSX_SLT(rd,rs,rt)       MK_R(0, (rs), (rt),(rd), 0, 0x2A)
#define PSX_SLTU(rd,rs,rt)      MK_R(0, (rs), (rt),(rd), 0, 0x2B)

/* I-type */
#define PSX_BEQ(rs,rt,off)      MK_I(0x04, (rs), (rt), (off))
#define PSX_BNE(rs,rt,off)      MK_I(0x05, (rs), (rt), (off))
#define PSX_BLEZ(rs,off)        MK_I(0x06, (rs), 0,    (off))
#define PSX_BGTZ(rs,off)        MK_I(0x07, (rs), 0,    (off))
#define PSX_ADDI(rt,rs,imm)     MK_I(0x08, (rs), (rt), (imm))
#define PSX_ADDIU(rt,rs,imm)    MK_I(0x09, (rs), (rt), (imm))
#define PSX_SLTI(rt,rs,imm)     MK_I(0x0A, (rs), (rt), (imm))
#define PSX_SLTIU(rt,rs,imm)    MK_I(0x0B, (rs), (rt), (imm))
#define PSX_ANDI(rt,rs,imm)     MK_I(0x0C, (rs), (rt), (imm))
#define PSX_ORI(rt,rs,imm)      MK_I(0x0D, (rs), (rt), (imm))
#define PSX_XORI(rt,rs,imm)     MK_I(0x0E, (rs), (rt), (imm))
#define PSX_LUI(rt,imm)         MK_I(0x0F, 0,    (rt), (imm))
#define PSX_LB(rt,off,rs)       MK_I(0x20, (rs), (rt), (off))
#define PSX_LH(rt,off,rs)       MK_I(0x21, (rs), (rt), (off))
#define PSX_LWL(rt,off,rs)      MK_I(0x22, (rs), (rt), (off))
#define PSX_LW(rt,off,rs)       MK_I(0x23, (rs), (rt), (off))
#define PSX_LBU(rt,off,rs)      MK_I(0x24, (rs), (rt), (off))
#define PSX_LHU(rt,off,rs)      MK_I(0x25, (rs), (rt), (off))
#define PSX_LWR(rt,off,rs)      MK_I(0x26, (rs), (rt), (off))
#define PSX_SB(rt,off,rs)       MK_I(0x28, (rs), (rt), (off))
#define PSX_SH(rt,off,rs)       MK_I(0x29, (rs), (rt), (off))
#define PSX_SWL(rt,off,rs)      MK_I(0x2A, (rs), (rt), (off))
#define PSX_SW(rt,off,rs)       MK_I(0x2B, (rs), (rt), (off))
#define PSX_SWR(rt,off,rs)      MK_I(0x2E, (rs), (rt), (off))

/* J-type */
#define PSX_J(tgt26)            MK_J(0x02, (tgt26))
#define PSX_JAL(tgt26)          MK_J(0x03, (tgt26))

/* REGIMM (op=1) */
#define PSX_BLTZ(rs,off)        MK_I(0x01, (rs), 0x00, (off))
#define PSX_BGEZ(rs,off)        MK_I(0x01, (rs), 0x01, (off))
#define PSX_BLTZAL(rs,off)      MK_I(0x01, (rs), 0x10, (off))
#define PSX_BGEZAL(rs,off)      MK_I(0x01, (rs), 0x11, (off))

/* LWC2 / SWC2 — Coprocessor 2 load/store (GTE) */
#define PSX_LWC2(rt,off,rs)    MK_I(0x32, (rs), (rt), (off))
#define PSX_SWC2(rt,off,rs)    MK_I(0x3A, (rs), (rt), (off))

/* COP0 */
#define PSX_MTC0(rt,rd)  ((0x10u << 26) | (0x04u << 21) | ((rt) << 16) | ((rd) << 11))
#define PSX_MFC0(rt,rd)  ((0x10u << 26) | (0x00u << 21) | ((rt) << 16) | ((rd) << 11))

/* COP0 register numbers */
#define PSX_COP0_SR_IDX  12

/* ---- COP2 (GTE) instructions ---- */

/* MTC2 rt, rd — Move To Coprocessor 2 (GTE data register write) */
#define PSX_MTC2(rt,rd) ((0x12u << 26) | (0x04u << 21) | ((rt) << 16) | ((rd) << 11))

/* MFC2 rt, rd — Move From Coprocessor 2 (GTE data register read) */
#define PSX_MFC2(rt,rd) ((0x12u << 26) | (0x00u << 21) | ((rt) << 16) | ((rd) << 11))

/* CTC2 rt, rd — Move To Coprocessor 2 Control (GTE control register write) */
#define PSX_CTC2(rt,rd) ((0x12u << 26) | (0x06u << 21) | ((rt) << 16) | ((rd) << 11))

/* CFC2 rt, rd — Move From Coprocessor 2 Control (GTE control register read) */
#define PSX_CFC2(rt,rd) ((0x12u << 26) | (0x02u << 21) | ((rt) << 16) | ((rd) << 11))

/* COP2 command word: opcode=0x12, bit25=1, func in lower 25 bits */
#define PSX_COP2(func) ((0x12u << 26) | (1u << 25) | ((func) & 0x01FFFFFFu))

/* Common GTE commands (encoding: sf=bit19, lm=bit10, cmd=lower 6 bits) */
#define GTE_CMD_RTPS(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x01)
#define GTE_CMD_NCLIP          PSX_COP2(0x06)
#define GTE_CMD_OP(sf,lm)     PSX_COP2(((sf)<<19)|((lm)<<10)|0x0C)
#define GTE_CMD_DPCS(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x10)
#define GTE_CMD_INTPL(sf,lm)  PSX_COP2(((sf)<<19)|((lm)<<10)|0x11)
#define GTE_CMD_MVMVA(sf,lm,mx,v,cv) \
    PSX_COP2(((sf)<<19)|((mx)<<17)|((v)<<15)|((cv)<<13)|((lm)<<10)|0x12)
#define GTE_CMD_NCDS(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x13)
#define GTE_CMD_CDP(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x14)
#define GTE_CMD_NCDT(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x16)
#define GTE_CMD_NCCS(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x1B)
#define GTE_CMD_CC(sf,lm)     PSX_COP2(((sf)<<19)|((lm)<<10)|0x1C)
#define GTE_CMD_NCS(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x1E)
#define GTE_CMD_NCT(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x20)
#define GTE_CMD_SQR(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x28)
#define GTE_CMD_DCPL(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x29)
#define GTE_CMD_DPCT(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x2A)
#define GTE_CMD_AVSZ3          PSX_COP2(0x2D)
#define GTE_CMD_AVSZ4          PSX_COP2(0x2E)
#define GTE_CMD_RTPT(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x30)
#define GTE_CMD_GPF(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x3D)
#define GTE_CMD_GPL(sf,lm)    PSX_COP2(((sf)<<19)|((lm)<<10)|0x3E)
#define GTE_CMD_NCCT(sf,lm)   PSX_COP2(((sf)<<19)|((lm)<<10)|0x3F)

/* GTE data register indices */
#define GTE_VXY0 0   /* V0 vector X,Y (packed: lo=X, hi=Y) */
#define GTE_VZ0  1   /* V0 vector Z */
#define GTE_VXY1 2   /* V1 vector X,Y */
#define GTE_VZ1  3   /* V1 vector Z */
#define GTE_VXY2 4   /* V2 vector X,Y */
#define GTE_VZ2  5   /* V2 vector Z */
#define GTE_RGBC 6   /* Color + code byte (R=lo, G, B, code=hi) */
#define GTE_OTZ  7   /* Average Z value */
#define GTE_IR0  8   /* Interpolation factor (1.3.12 fixed) */
#define GTE_IR1  9   /* Vector IR 1 */
#define GTE_IR2  10  /* Vector IR 2 */
#define GTE_IR3  11  /* Vector IR 3 */
#define GTE_SXY0 12  /* Screen X,Y pair 0 */
#define GTE_SXY1 13  /* Screen X,Y pair 1 */
#define GTE_SXY2 14  /* Screen X,Y pair 2 (most recent RTPS result) */
#define GTE_SXYP 15  /* Screen X,Y pair (FIFO read; special) */
#define GTE_SZ0  16  /* Screen Z 0 */
#define GTE_SZ1  17
#define GTE_SZ2  18
#define GTE_SZ3  19
#define GTE_RGB0 20  /* Color FIFO 0 (R, G, B, code) */
#define GTE_RGB1 21  /* Color FIFO 1 */
#define GTE_RGB2 22  /* Color FIFO 2 (most recent) */
#define GTE_MAC0 24  /* MAC0 (NCLIP result, AVSZ result, etc.) */
#define GTE_MAC1 25
#define GTE_MAC2 26
#define GTE_MAC3 27

/* GTE control register indices */
#define GTE_FLAG_CTRL 31 /* FLAG register (ctrl[31]: overflow/saturation bits) */
#define GTE_RT11RT12 0  /* Rotation matrix [0] */
#define GTE_RT13RT21 1
#define GTE_RT22RT23 2
#define GTE_RT31RT32 3
#define GTE_RT33     4
#define GTE_TRX      5  /* Translation X */
#define GTE_TRY      6
#define GTE_TRZ      7
#define GTE_L11L12   8  /* Light matrix */
#define GTE_L13L21   9
#define GTE_L22L23  10
#define GTE_L31L32  11
#define GTE_L33     12
#define GTE_RBK     13  /* Background color R */
#define GTE_GBK     14  /* Background color G */
#define GTE_BBK     15  /* Background color B */
#define GTE_LR1LR2  16 /* Light color matrix */
#define GTE_LR3LG1  17
#define GTE_LG2LG3  18
#define GTE_LB1LB2  19
#define GTE_LB3     20
#define GTE_RFC     21  /* Far color R */
#define GTE_GFC     22  /* Far color G */
#define GTE_BFC     23  /* Far color B */
#define GTE_OFX     24  /* Screen offset X (fixed 16.16) */
#define GTE_OFY     25  /* Screen offset Y (fixed 16.16) */
#define GTE_H       26  /* Projection plane distance */
#define GTE_DQA     27
#define GTE_DQB     28
#define GTE_ZSF3    29  /* Z scale factor 3 (for AVSZ3) */
#define GTE_ZSF4    30  /* Z scale factor 4 (for AVSZ4) */

/* ================================================================
 *  Test Framework
 * ================================================================ */

/* Test result tracking */
typedef struct {
    int total;
    int passed;
    int failed;
} PlaygroundResults;

extern PlaygroundResults pg_results;

/* Externals from the runtime */
extern R3000CPU cpu;
extern uint8_t *psx_ram;

/* Forward: compile + execute a block at given PC with given cycle budget.
 * Defined in playground_main.c. */
void pg_run_jit(uint32_t pc, int32_t cycles);

/* Set to 1 to hex-dump the first compiled block (debug) */
extern int pg_dump_next_block;

/* Full cache flush + page table reset (between tests) */
void pg_reset_jit_cache(void);

/* ---- Test context ---- */

/* Base PSX address where test code is placed.
 * 0x80010000 = well inside kseg0 RAM, page-aligned.
 * That maps to psx_ram offset 0x10000. */
#define PG_CODE_BASE   0x80010000u
#define PG_CODE_OFFSET 0x00010000u

/* Data area for load/store tests:
 * 0x80020000 = psx_ram + 0x20000. */
#define PG_DATA_BASE   0x80020000u
#define PG_DATA_OFFSET 0x00020000u

/* Halt loop: tight BEQ $0,$0 self-loop that burns cycles without
 * clobbering any GPR.  JR $ra lands here after the test code finishes. */
#define PG_HALT_BASE   0x80030000u
#define PG_HALT_OFFSET 0x00030000u

/* Maximum instructions per test */
#define PG_MAX_INSN    128

/* Current test context (set by BEGIN_TEST) */
typedef struct {
    const char *name;
    uint32_t   *code;       /* host pointer into psx_ram */
    int         count;      /* instructions written */
    int         fail_count; /* assertions failed in this test */
} PGTestCtx;

extern PGTestCtx pg_ctx;

/* ---- Macros ---- */

#define BEGIN_TEST(test_name) do {                                       \
    /* reset CPU state */                                                \
    memset(&cpu, 0, sizeof(cpu));                                        \
    cpu.regs[R_SP] = 0x801FFF00u; /* valid stack within RAM */           \
    cpu.regs[R_RA] = PG_HALT_BASE; /* JR $ra → halt loop */             \
    /* Clear code + data areas in RAM */                                 \
    memset(psx_ram + PG_CODE_OFFSET, 0, 4096);                          \
    memset(psx_ram + PG_DATA_OFFSET, 0, 4096);                          \
    /* Install halt loop at PG_HALT_BASE: BEQ $0,$0,-1; NOP */          \
    /* Clear 256 bytes so JIT fall-through compiles clean NOPs */      \
    memset(psx_ram + PG_HALT_OFFSET, 0, 256);                         \
    {                                                                    \
        uint32_t *_halt = (uint32_t *)(psx_ram + PG_HALT_OFFSET);       \
        _halt[0] = MK_I(0x04, 0, 0, (uint16_t)(-1)); /* BEQ $0,$0,-1 */\
        _halt[1] = 0x00000000u;                        /* NOP */         \
    }                                                                    \
    /* Reset JIT cache (invalidate all compiled blocks) */               \
    pg_reset_jit_cache();                                                \
    /* Set up context */                                                 \
    pg_ctx.name = (test_name);                                           \
    pg_ctx.code = (uint32_t *)(psx_ram + PG_CODE_OFFSET);               \
    pg_ctx.count = 0;                                                    \
    pg_ctx.fail_count = 0;                                               \
} while(0)

#define SET_REG(r, val)    cpu.regs[(r)] = (uint32_t)(val)
#define SET_HI(val)        cpu.hi = (uint32_t)(val)
#define SET_LO(val)        cpu.lo = (uint32_t)(val)
#define SET_COP0(r, val)   cpu.cop0[(r)] = (uint32_t)(val)
#define GET_COP0(r)        cpu.cop0[(r)]
#define SET_MEM32(off, val) (*(uint32_t *)(psx_ram + (off))) = (uint32_t)(val)
#define SET_MEM16(off, val) (*(uint16_t *)(psx_ram + (off))) = (uint16_t)(val)
#define SET_MEM8(off, val)  (*(uint8_t  *)(psx_ram + (off))) = (uint8_t)(val)
#define GET_MEM32(off)      (*(uint32_t *)(psx_ram + (off)))
#define GET_MEM16(off)      (*(uint16_t *)(psx_ram + (off)))
#define GET_MEM8(off)       (*(uint8_t  *)(psx_ram + (off)))

/* Write a PSX opcode into the code area */
#define EMIT(opcode) do {                                                \
    if (pg_ctx.count < PG_MAX_INSN)                                      \
        pg_ctx.code[pg_ctx.count++] = (uint32_t)(opcode);               \
} while(0)

/* Run the JIT on the emitted code. Adds JR $ra + NOP as epilogue. */
#define RUN(cycles) do {                                                 \
    /* Append JR $ra + NOP if not already terminated */                  \
    EMIT(PSX_JR(R_RA));                                                  \
    EMIT(PSX_NOP());                                                     \
    /* Set RA to a known sentinel: return to 0x80010000+offset past end */ \
    /* Actually: set PC and run. The block will JR $ra which is 0       */ \
    /* (cpu.regs[31]=0). The JIT will try to jump to 0 which triggers    */ \
    /* lookup_block(0) → compile_block(0) → get_psx_code_ptr fails →    */ \
    /* abort. We rely on cycle budget exhaustion to stop.                */ \
    /* Simpler: set RA to PG_CODE_BASE + (count-2)*4 which is the JR    */ \
    /* itself — creates a tight idle loop that burns cycles.             */ \
    /* Simplest: just give enough cycles for 1 pass. */                  \
    pg_run_jit(PG_CODE_BASE, (cycles));                                  \
} while(0)

/* Assertions */
#define EXPECT_REG(r, expected) do {                                     \
    uint32_t _got = cpu.regs[(r)];                                       \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: $%d expected=0x%08X got=0x%08X\n",         \
               pg_ctx.name, (r), (unsigned)_exp, (unsigned)_got);        \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_HI(expected) do {                                         \
    uint32_t _got = cpu.hi;                                              \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: HI expected=0x%08X got=0x%08X\n",          \
               pg_ctx.name, (unsigned)_exp, (unsigned)_got);             \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_LO(expected) do {                                         \
    uint32_t _got = cpu.lo;                                              \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: LO expected=0x%08X got=0x%08X\n",          \
               pg_ctx.name, (unsigned)_exp, (unsigned)_got);             \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_MEM32(off, expected) do {                                 \
    uint32_t _got = GET_MEM32(off);                                      \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: MEM[0x%05X] expected=0x%08X got=0x%08X\n", \
               pg_ctx.name, (unsigned)(off), (unsigned)_exp,             \
               (unsigned)_got);                                          \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_MEM8(off, expected) do {                                  \
    uint8_t _got = GET_MEM8(off);                                        \
    uint8_t _exp = (uint8_t)(expected);                                  \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: MEM8[0x%05X] expected=0x%02X got=0x%02X\n",\
               pg_ctx.name, (unsigned)(off), (unsigned)_exp,             \
               (unsigned)_got);                                          \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_MEM16(off, expected) do {                                 \
    uint16_t _got = GET_MEM16(off);                                      \
    uint16_t _exp = (uint16_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: MEM16[0x%05X] expected=0x%04X got=0x%04X\n",\
               pg_ctx.name, (unsigned)(off), (unsigned)_exp,             \
               (unsigned)_got);                                          \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define END_TEST() do {                                                  \
    pg_results.total++;                                                  \
    if (pg_ctx.fail_count == 0) {                                        \
        printf("[PASS] %s\n", pg_ctx.name);                              \
        pg_results.passed++;                                             \
    } else {                                                             \
        printf("[FAIL] %s (%d assertions failed)\n",                     \
               pg_ctx.name, pg_ctx.fail_count);                          \
        pg_results.failed++;                                             \
    }                                                                    \
} while(0)

/* GTE register assertions */
#define EXPECT_CP2_DATA(idx, expected) do {                              \
    uint32_t _got = cpu.cp2_data[(idx)];                                 \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: cp2_data[%d] expected=0x%08X got=0x%08X\n",\
               pg_ctx.name, (idx), (unsigned)_exp, (unsigned)_got);      \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

#define EXPECT_CP2_CTRL(idx, expected) do {                              \
    uint32_t _got = cpu.cp2_ctrl[(idx)];                                 \
    uint32_t _exp = (uint32_t)(expected);                                \
    if (_got != _exp) {                                                  \
        printf("  [FAIL] %s: cp2_ctrl[%d] expected=0x%08X got=0x%08X\n",\
               pg_ctx.name, (idx), (unsigned)_exp, (unsigned)_got);      \
        pg_ctx.fail_count++;                                             \
    }                                                                    \
} while(0)

/* Pack signed 16-bit X,Y into SXY register format */
#define PACK_SXY(x, y)  ((uint32_t)((((y) & 0xFFFF) << 16) | ((x) & 0xFFFF)))
/* Pack signed 16-bit X,Y into VXY register format (same layout) */
#define PACK_VXY(x, y)  PACK_SXY((x), (y))

/* ================================================================
 *  Test category runners (one per file)
 * ================================================================ */
void pg_run_alu_tests(void);      /* test_alu.c    */
void pg_run_memory_tests(void);   /* test_memory.c */
void pg_run_branch_tests(void);   /* test_branch.c */
void pg_run_block_tests(void);    /* test_block.c  */
void pg_run_dirty_tests(void);    /* test_dirty.c  */
void pg_run_gte_tests(void);      /* test_gte.c    */
void pg_run_expansion_tests(void); /* test_expansion.c */
void pg_run_vu0_micro_tests(void); /* test_vu0_micro.c */
void pg_run_gte_compare_tests(void); /* test_gte_compare.c */
void pg_run_sio_tests(void);      /* test_sio.c    */

/* Master runner — calls all category runners above */
void pg_run_all_tests(void);

#endif /* PLAYGROUND_H */
