/*
 * SuperPSX Dynarec - MIPS-to-MIPS Block Compiler
 *
 * Since R3000A and R5900 share instruction encoding, most instructions
 * are handled by loading PSX registers from a struct, executing the
 * operation natively, and storing results back. Memory operations
 * go through C helper functions (ReadWord/WriteWord).
 *
 * Register convention in generated code:
 *   $s0 = pointer to R3000CPU struct
 *   $s1 = pointer to psx_ram
 *   $s2 = pointer to psx_bios
 *   $t0-$t9, $v0/$v1, $a0-$a3 = temporaries
 */

#include <tamtypes.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <kernel.h>
#include "superpsx.h"
#include "loader.h"

#ifdef ENABLE_HOST_LOG
static FILE *host_log_file = NULL;
#endif

/* ---- Code buffer ---- */
#define CODE_BUFFER_SIZE (4 * 1024 * 1024)
static u32 *code_buffer;
static u32 *code_ptr;

/* ---- Block cache ---- */
#define BLOCK_CACHE_BITS 14
#define BLOCK_CACHE_SIZE (1 << BLOCK_CACHE_BITS)
#define BLOCK_CACHE_MASK (BLOCK_CACHE_SIZE - 1)

typedef struct
{
    u32 psx_pc;
    u32 *native;
    u32 instr_count; /* Number of PSX instructions in this block */
} BlockEntry;

static BlockEntry *block_cache;

/* ---- Instruction encoding helpers ---- */
#define OP(x) (((x) >> 26) & 0x3F)
#define RS(x) (((x) >> 21) & 0x1F)
#define RT(x) (((x) >> 16) & 0x1F)
#define RD(x) (((x) >> 11) & 0x1F)
#define SA(x) (((x) >> 6) & 0x1F)
#define FUNC(x) ((x) & 0x3F)
#define IMM16(x) ((x) & 0xFFFF)
#define SIMM16(x) ((s16)((x) & 0xFFFF))
#define TARGET(x) ((x) & 0x03FFFFFF)

/* Emit a 32-bit instruction to code buffer */
static inline void emit(u32 inst)
{
    *code_ptr++ = inst;
}

/* MIPS instruction builders */
#define MK_R(op, rs, rt, rd, sa, fn) \
    ((((u32)(op)) << 26) | (((u32)(rs)) << 21) | (((u32)(rt)) << 16) | (((u32)(rd)) << 11) | (((u32)(sa)) << 6) | ((u32)(fn)))
#define MK_I(op, rs, rt, imm) \
    ((((u32)(op)) << 26) | (((u32)(rs)) << 21) | (((u32)(rt)) << 16) | ((u32)((imm) & 0xFFFF)))
#define MK_J(op, tgt) \
    ((((u32)(op)) << 26) | ((u32)((tgt) & 0x03FFFFFF)))

/* Common emitters using $t0-$t3 as temps */
#define EMIT_NOP() emit(0)
#define EMIT_LW(rt, off, base) emit(MK_I(0x23, (base), (rt), (off)))
#define EMIT_SW(rt, off, base) emit(MK_I(0x2B, (base), (rt), (off)))
#define EMIT_LH(rt, off, base) emit(MK_I(0x21, (base), (rt), (off)))
#define EMIT_LHU(rt, off, base) emit(MK_I(0x25, (base), (rt), (off)))
#define EMIT_LB(rt, off, base) emit(MK_I(0x20, (base), (rt), (off)))
#define EMIT_LBU(rt, off, base) emit(MK_I(0x24, (base), (rt), (off)))
#define EMIT_SH(rt, off, base) emit(MK_I(0x29, (base), (rt), (off)))
#define EMIT_SB(rt, off, base) emit(MK_I(0x28, (base), (rt), (off)))
#define EMIT_ADDIU(rt, rs, imm) emit(MK_I(0x09, (rs), (rt), (imm)))
#define EMIT_ADDU(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x21))
#define EMIT_OR(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x25))
#define EMIT_LUI(rt, imm) emit(MK_I(0x0F, 0, (rt), (imm)))
#define EMIT_ORI(rt, rs, imm) emit(MK_I(0x0D, (rs), (rt), (imm)))
#define EMIT_MOVE(rd, rs) EMIT_ADDU(rd, rs, 0)
#define EMIT_JR(rs) emit(MK_R(0, (rs), 0, 0, 0, 0x08))
#define EMIT_JAL_ABS(addr) emit(MK_J(3, (u32)(addr) >> 2))
#define EMIT_J_ABS(addr) emit(MK_J(2, (u32)(addr) >> 2))
#define EMIT_BEQ(rs, rt, off) emit(MK_I(4, (rs), (rt), (off)))
#define EMIT_BNE(rs, rt, off) emit(MK_I(5, (rs), (rt), (off)))

/* Hardware register IDs used in generated code:
 *   $s0 (16) = cpu struct ptr
 *   $s1 (17) = psx_ram ptr
 *   $s2 (18) = psx_bios ptr
 *   $t0 (8)  = temp0
 *   $t1 (9)  = temp1
 *   $t2 (10) = temp2
 *   $a0 (4)  = arg0 for function calls
 *   $a1 (5)  = arg1 for function calls
 *   $v0 (2)  = return value from functions
 */
#define REG_S0 16
#define REG_S1 17
#define REG_S2 18
#define REG_S3 19
#define REG_T0 8
#define REG_T1 9
#define REG_T2 10
#define REG_A0 4
#define REG_A1 5
#define REG_V0 2
#define REG_RA 31
#define REG_SP 29
#define REG_ZERO 0

/* Load PSX register 'r' from cpu struct into hw reg 'hwreg' */
static void emit_load_psx_reg(int hwreg, int r)
{
    if (r == 0)
    {
        EMIT_MOVE(hwreg, REG_ZERO); /* $0 is always 0 */
    }
    else
    {
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
    }
}

/* Store hw reg 'hwreg' to PSX register 'r' in cpu struct */
static void emit_store_psx_reg(int r, int hwreg)
{
    if (r == 0)
        return; /* never write to $0 */
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
}

/* Load 32-bit immediate into hw register */
static void emit_load_imm32(int hwreg, u32 val)
{
    if (val == 0)
    {
        EMIT_MOVE(hwreg, REG_ZERO);
    }
    else if ((val & 0xFFFF0000) == 0)
    {
        EMIT_ORI(hwreg, REG_ZERO, val & 0xFFFF);
    }
    else if ((val & 0xFFFF) == 0)
    {
        EMIT_LUI(hwreg, val >> 16);
    }
    else
    {
        EMIT_LUI(hwreg, val >> 16);
        EMIT_ORI(hwreg, hwreg, val & 0xFFFF);
    }
}

/* ---- Get pointer to PSX code in EE memory ---- */
static u32 *get_psx_code_ptr(u32 psx_pc)
{
    u32 phys = psx_pc & 0x1FFFFFFF;
    if (phys < PSX_RAM_SIZE)
        return (u32 *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return (u32 *)(psx_bios + (phys - 0x1FC00000));
    return NULL;
}

/* ---- Forward declarations ---- */
static void emit_block_prologue(void);
static void emit_block_epilogue(void);
static void emit_instruction(u32 opcode, u32 psx_pc);
static void emit_branch_epilogue(u32 target_pc);
static void emit_memory_read(int size, int rt_psx, int rs_psx, s16 offset);
static void emit_memory_write(int size, int rt_psx, int rs_psx, s16 offset);

/* Track instruction count for logging */
static u32 blocks_compiled = 0;
static u32 total_instructions = 0;

/* Current PSX PC being emitted (used by memory emitters for exception EPC) */
static u32 emit_current_psx_pc = 0;

/* Load delay slot support:
 * On R3000A, loads have a 1-instruction delay - the loaded value isn't
 * available until 2 instructions after the load. The instruction immediately
 * following a load still sees the OLD register value.
 * dynarec_load_defer: set by compile_block before emit_instruction to tell
 * load emitters to leave the value in REG_V0 instead of storing to PSX reg.
 */
static int dynarec_load_defer = 0;

/* Check if instruction reads a given GPR as source operand */
static int instruction_reads_gpr(u32 opcode, int reg)
{
    if (reg == 0)
        return 0; /* r0 is always 0, never "read" in a delay-relevant way */
    int op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);

    /* RS is read by almost all instructions */
    if (rs == reg)
    {
        /* Exceptions where RS is NOT read */
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            /* SLL, SRL, SRA use shamt, not RS */
            if (func == 0x00 || func == 0x02 || func == 0x03)
                return 0;
            /* MFHI, MFLO don't read any GPR */
            if (func == 0x10 || func == 0x12)
                return 0;
            /* SYSCALL, BREAK don't read GPRs */
            if (func == 0x0C || func == 0x0D)
                return 0;
        }
        if (op == 0x02 || op == 0x03)
            return 0; /* J, JAL don't read RS */
        if (op == 0x0F)
            return 0; /* LUI doesn't read RS */
        return 1;
    }

    /* RT is read by some instructions */
    if (rt == reg)
    {
        if (op == 0x00)
        {
            /* R-type: most ALU instructions read RT */
            int func = FUNC(opcode);
            if (func == 0x08 || func == 0x09)
                return 0; /* JR/JALR don't read RT */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI/MFLO */
            if (func == 0x11 || func == 0x13)
                return 0; /* MTHI/MTLO read RS, not RT */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL/BREAK */
            return 1;     /* Most R-type read RT */
        }
        if (op == 0x04 || op == 0x05)
            return 1; /* BEQ/BNE read RT */
        if (op == 0x22 || op == 0x26)
            return 1; /* LWL/LWR read RT (merge) */
        if (op >= 0x28 && op <= 0x2E)
            return 1; /* Stores read RT */
        return 0;     /* I-type ALU/loads don't read RT */
    }

    return 0;
}

/* Check if instruction writes a given GPR as destination operand */
static int instruction_writes_gpr(u32 opcode, int reg)
{
    if (reg == 0)
        return 0; /* r0 is hardwired to 0 */
    int op = OP(opcode);
    if (op == 0x00)
    {
        /* R-type: writes to RD */
        int func = FUNC(opcode);
        /* MULT/MULTU/DIV/DIVU write HI/LO, not GPR */
        if (func >= 0x18 && func <= 0x1B)
            return 0;
        /* JR doesn't write GPR */
        if (func == 0x08)
            return 0;
        /* MTHI/MTLO don't write GPR (they write HI/LO) */
        if (func == 0x11 || func == 0x13)
            return 0;
        /* SYSCALL/BREAK don't write GPR */
        if (func == 0x0C || func == 0x0D)
            return 0;
        return (RD(opcode) == reg);
    }
    /* I-type ALU: writes to RT */
    if (op >= 0x08 && op <= 0x0F)
        return (RT(opcode) == reg);
    /* Loads: write to RT */
    if (op >= 0x20 && op <= 0x26)
        return (RT(opcode) == reg);
    /* JAL writes to r31 */
    if (op == 0x03)
        return (reg == 31);
    /* JALR: writes to RD */
    if (op == 0x00 && FUNC(opcode) == 0x09)
        return (RD(opcode) == reg);
    return 0;
}

/* ---- Compile a basic block ---- */
static u32 *compile_block(u32 psx_pc)
{
    u32 *psx_code = get_psx_code_ptr(psx_pc);
    if (!psx_code)
    {
        printf("DYNAREC: Cannot fetch code at PC=0x%08X\n", (unsigned)psx_pc);
        return NULL;
    }

    /* Check for code buffer overflow: reset if < 64KB remaining */
    u32 used = (u32)((u8 *)code_ptr - (u8 *)code_buffer);
    if (used > CODE_BUFFER_SIZE - 65536)
    {
        printf("DYNAREC: Code buffer nearly full (%u/%u), flushing cache\n",
               (unsigned)used, CODE_BUFFER_SIZE);
        code_ptr = code_buffer;
        memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
        blocks_compiled = 0;
    }

    u32 *block_start = code_ptr;
    u32 cur_pc = psx_pc;

    if (blocks_compiled < 20)
    {
        printf("DYNAREC: Compiling block at PC=0x%08X\n", (unsigned)psx_pc);
    }

    // Debug: Inspect hot loop
    if (psx_pc == 0x800509AC)
    {
        printf("DYNAREC: Hot Loop dump at %08X:\n", (unsigned)psx_pc);
        printf("  -4: %08X\n", psx_code[-1]);
        printf("   0: %08X (Hit)\n", psx_code[0]);
        printf("  +4: %08X\n", psx_code[1]);
        printf("  +8: %08X\n", psx_code[2]);
        printf(" +12: %08X\n", psx_code[3]);
    }

    emit_block_prologue();

    int block_ended = 0;
    int in_delay_slot = 0;
    u32 branch_target = 0;
    int branch_type = 0; /* 0=none, 1=unconditional, 2=conditional, 3=register */
    u32 branch_opcode = 0;

    /* Load delay slot tracking */
    int pending_load_reg = 0;       /* PSX register with pending load (0=none) */
    int pending_load_apply_now = 0; /* 1 = apply before this instruction */

    while (!block_ended)
    {
        u32 opcode = *psx_code++;

        if (in_delay_slot)
        {
            /* Apply any pending load delay before the delay slot instruction */
            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }
            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            /* Emit the delay slot instruction (no load deferral in branch delay slots) */
            dynarec_load_defer = 0;
            emit_instruction(opcode, cur_pc);
            cur_pc += 4;
            total_instructions++;

            /* Apply any remaining pending load before leaving block */
            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }

            /* Now emit the branch resolution */
            if (branch_type == 1)
            {
                /* Unconditional: J, JAL */
                emit_branch_epilogue(branch_target);
            }
            else if (branch_type == 4)
            {
                /* Deferred Conditional Branch (calculated in S3) */
                /* Emit BNE S3, ZERO, offset */
                u32 *bp = code_ptr;
                /* BNE s3, zero, 0 */
                emit(MK_I(0x05, REG_S3, REG_ZERO, 0));
                EMIT_NOP(); /* Native delay slot */

                /* Standard branch patching logic */
                branch_opcode = (u32)bp;

                /* Not taken: fall through PC */
                emit_load_imm32(REG_T0, cur_pc);
                EMIT_SW(REG_T0, CPU_PC, REG_S0);
                emit_block_epilogue();
                /* Taken path target */
                u32 *taken_addr = code_ptr;
                s32 offset = (s32)(taken_addr - bp - 1);
                *bp = (*bp & 0xFFFF0000) | (offset & 0xFFFF);
                emit_load_imm32(REG_T0, branch_target);
                EMIT_SW(REG_T0, CPU_PC, REG_S0);
                emit_block_epilogue();
            }
            else if (branch_type == 3)
            {
                /* Register jump (JR/JALR): target already in cpu.pc */
                emit_block_epilogue();
            }
            block_ended = 1;
            break;
        }

        u32 op = OP(opcode);

        /* Check for branch/jump instructions */
        if (op == 0x02 || op == 0x03)
        {
            /* J / JAL */
            if (op == 0x03)
            {
                /* JAL: store return address */
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(31, REG_T0);
            }
            branch_target = ((cur_pc + 4) & 0xF0000000) | (TARGET(opcode) << 2);
            branch_type = 1;
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x00 && (FUNC(opcode) == 0x08 || FUNC(opcode) == 0x09))
        {
            /* JR / JALR */
            int rs = RS(opcode);
            int rd = (FUNC(opcode) == 0x09) ? RD(opcode) : 0;
            /* Read rs FIRST (before link write that could clobber rd==rs) */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            if (FUNC(opcode) == 0x09 && rd != 0)
            {
                /* JALR: store return address in rd */
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(rd, REG_T1);
            }
            branch_type = 3;
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07)
        {
            /* BEQ, BNE, BLEZ, BGTZ */
            int rs = RS(opcode);
            int rt = RT(opcode);
            s32 offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            emit_load_psx_reg(REG_T0, rs);
            if (op == 0x04 || op == 0x05)
            { /* BEQ, BNE */
                emit_load_psx_reg(REG_T1, rt);
                /* XOR s3, t0, t1 */
                emit(MK_R(0, REG_T0, REG_T1, REG_S3, 0, 0x26));
                if (op == 0x04)
                { /* BEQ: taken if s3 == 0 -> set s3 = (s3 < 1) */
                    /* SLTIU s3, s3, 1 */
                    emit(MK_I(0x0B, REG_S3, REG_S3, 1));
                }
                /* BNE: taken if s3 != 0. Already correct. */
            }
            else if (op == 0x06)
            { /* BLEZ (rs <= 0) */
                /* Taken if rs <= 0 -> rs < 1 */
                /* SLTI s3, t0, 1 */
                emit(MK_I(0x0A, REG_T0, REG_S3, 1));
            }
            else if (op == 0x07)
            { /* BGTZ (rs > 0) */
                /* Taken if rs > 0 -> 0 < rs. SLT s3, zero, t0 */
                emit(MK_R(0, REG_ZERO, REG_T0, REG_S3, 0, 0x2A));
            }

            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x01)
        {
            /* REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL and unofficial variants.
             * R3000A only decodes bit 0 of rt (BLTZ vs BGEZ) and bit 4 (link).
             * All other rt values map to these 4 operations. */
            int rs = RS(opcode);
            int rt = RT(opcode);
            s32 offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            /* Read rs FIRST (before any link write that could clobber r31) */
            emit_load_psx_reg(REG_T0, rs);

            if (rt == 0x10 || rt == 0x11)
            {
                /* BLTZAL/BGEZAL only: store return address (always, even if not taken) */
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(31, REG_T1);
            }

            /* Compute branch condition using T0 (original rs value) */
            if ((rt & 1) == 0)
            {
                /* BLTZ / BLTZAL and unofficial even variants (rs < 0) */
                /* SLT s3, t0, zero */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
            }
            else
            {
                /* BGEZ / BGEZAL and unofficial odd variants (rs >= 0) */
                /* SLT s3, t0, zero (1 if <0). XORI s3, s3, 1 (1 if >= 0). */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
                emit(MK_I(0x0E, REG_S3, REG_S3, 1));
            }

            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        /* Not a branch - emit with load delay slot handling */
        {
            int this_is_load = 0;
            int load_target = 0;
            u32 op_check = OP(opcode);

            /* Check if this instruction is a load */
            if (op_check == 0x20 || op_check == 0x21 || op_check == 0x22 ||
                op_check == 0x23 || op_check == 0x24 || op_check == 0x25 ||
                op_check == 0x26)
            {
                load_target = RT(opcode);
                if (load_target != 0)
                {
                    /* Peek at next instruction */
                    u32 next_instr = *psx_code;
                    u32 next_op = OP(next_instr);
                    int next_rt = RT(next_instr);

                    /* Defer if: (a) next instruction reads our loaded register, OR
                     * (b) next instruction is ALSO a load to the same register
                     * Case (b) handles R3000A load cancellation:
                     * LB R,X; LB R,Y; READ R → READ gets original (neither X nor Y) */
                    if (instruction_reads_gpr(next_instr, load_target))
                    {
                        this_is_load = 1;
                    }
                    else if ((next_op >= 0x20 && next_op <= 0x26) &&
                             next_rt == load_target)
                    {
                        /* Next is another load to same register → defer to allow cancel */
                        this_is_load = 1;
                    }
                }
            }

            /* STEP 1: Apply old pending load if ready (1 instruction has passed) */
            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                /* If this is a load to the SAME register, CANCEL the old pending */
                if (this_is_load && load_target == pending_load_reg)
                {
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    /* Apply the old pending load */
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
            }

            /* STEP 2: Advance delay state */
            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            /* STEP 3: Emit the instruction (deferred loads leave value in REG_V0) */
            dynarec_load_defer = this_is_load;
            emit_instruction(opcode, cur_pc);
            dynarec_load_defer = 0;

            /* STEP 3.5: If the instruction just emitted WRITES to the pending
             * register, cancel the pending load (the write takes precedence). */
            if (pending_load_reg != 0 && !this_is_load &&
                instruction_writes_gpr(opcode, pending_load_reg))
            {
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }

            /* STEP 4: If this was a deferred load, save value to delay field */
            if (this_is_load)
            {
                /* If there's an old pending for a DIFFERENT register, apply it first */
                if (pending_load_reg != 0 && pending_load_reg != load_target)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                }
                /* Save new deferred value to delay field */
                EMIT_SW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
                pending_load_reg = load_target;
                pending_load_apply_now = 0;
            }
        }
        cur_pc += 4;
        total_instructions++;

        /* End block after N instructions to avoid huge blocks */
        if ((cur_pc - psx_pc) >= 256)
        {
            /* Apply any remaining pending load before leaving block */
            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }
            emit_load_imm32(REG_T0, cur_pc);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            emit_block_epilogue();
            block_ended = 1;
        }
    }

    if (blocks_compiled < 5)
    {
        int num_words = (int)(code_ptr - block_start);
        printf("DYNAREC: Block %u at %p, %d words:\n",
               (unsigned)blocks_compiled, block_start, num_words);
        int j;
        for (j = 0; j < num_words && j < 32; j++)
        {
            printf("  [%02d] %p: 0x%08X\n", j, &block_start[j], (unsigned)block_start[j]);
        }
        if (num_words > 32)
            printf("  ... (%d more)\n", num_words - 32);
    }
    /* Calculate instruction count for this block */
    u32 block_instr_count = (cur_pc - psx_pc) / 4;

    /* Flush caches */
    FlushCache(0); /* writeback dcache */
    FlushCache(2); /* invalidate icache */

    blocks_compiled++;

    /* Store instruction count in cache entry */
    {
        u32 idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
        block_cache[idx].instr_count = block_instr_count;
    }

    return block_start;
}

/* ---- Block prologue: save callee-saved regs, set up $s0-$s2 ---- */
static void emit_block_prologue(void)
{
    /* addiu $sp, $sp, -48 */
    EMIT_ADDIU(REG_SP, REG_SP, -48);
    /* save $ra, $s0-$s3 */
    EMIT_SW(REG_RA, 44, REG_SP);
    EMIT_SW(REG_S0, 40, REG_SP);
    EMIT_SW(REG_S1, 36, REG_SP);
    EMIT_SW(REG_S2, 32, REG_SP);
    EMIT_SW(REG_S3, 28, REG_SP);
    /* $s0 = $a0 (cpu ptr), $s1 = $a1 (ram), $s2 = $a2 (bios) */
    EMIT_MOVE(REG_S0, REG_A0);
    EMIT_MOVE(REG_S1, REG_A1);
    EMIT_MOVE(REG_S2, 6); /* $a2 = register 6 */
}

/* ---- Block epilogue: restore and return ---- */
static void emit_block_epilogue(void)
{
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 48);
    EMIT_JR(REG_RA);
    EMIT_NOP();
}

static void emit_branch_epilogue(u32 target_pc)
{
    emit_load_imm32(REG_T0, target_pc);
    EMIT_SW(REG_T0, CPU_PC, REG_S0);
    emit_block_epilogue();
}

/* ---- Memory access emitters ---- */
static void emit_memory_read(int size, int rt_psx, int rs_psx, s16 offset)
{
    /* Store current PSX PC for exception handling */
    emit_load_imm32(REG_T2, emit_current_psx_pc);
    EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);

    /* $a0 = PSX address */
    emit_load_psx_reg(REG_A0, rs_psx);
    EMIT_ADDIU(REG_A0, REG_A0, offset);

    /* Call ReadWord/ReadHalf/ReadByte */
    u32 func_addr;
    if (size == 4)
        func_addr = (u32)ReadWord;
    else if (size == 2)
        func_addr = (u32)ReadHalf;
    else
        func_addr = (u32)ReadByte;

    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();

    /* Store result ($v0) to PSX reg - unless deferred for load delay */
    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);
    /* If deferred, result stays in REG_V0 for the caller to save */
}

static void emit_memory_read_signed(int size, int rt_psx, int rs_psx, s16 offset)
{
    emit_memory_read(size, rt_psx, rs_psx, offset);
    /* Sign extend for LB/LH */
    if (rt_psx == 0)
        return;
    if (dynarec_load_defer)
    {
        /* Sign extend REG_V0 directly (value not stored to PSX reg yet) */
        if (size == 1)
        {
            emit(MK_R(0, 0, REG_V0, REG_V0, 24, 0x00)); /* SLL $v0, $v0, 24 */
            emit(MK_R(0, 0, REG_V0, REG_V0, 24, 0x03)); /* SRA $v0, $v0, 24 */
        }
        else if (size == 2)
        {
            emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x00)); /* SLL $v0, $v0, 16 */
            emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x03)); /* SRA $v0, $v0, 16 */
        }
    }
    else
    {
        if (size == 1)
        {
            /* Sign extend byte: sll 24, sra 24 */
            EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x00)); /* SLL $t0, $t0, 24 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x03)); /* SRA $t0, $t0, 24 */
            EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
        }
        else if (size == 2)
        {
            EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x00)); /* SLL $t0, $t0, 16 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x03)); /* SRA $t0, $t0, 16 */
            EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
        }
    }
}

static void emit_memory_write(int size, int rt_psx, int rs_psx, s16 offset)
{
    /* Store current PSX PC for exception handling */
    emit_load_imm32(REG_T2, emit_current_psx_pc);
    EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);

    /* $a0 = PSX address, $a1 = data */
    emit_load_psx_reg(REG_A0, rs_psx);
    EMIT_ADDIU(REG_A0, REG_A0, offset);
    emit_load_psx_reg(REG_A1, rt_psx);

    u32 func_addr;
    if (size == 4)
        func_addr = (u32)WriteWord;
    else if (size == 2)
        func_addr = (u32)WriteHalf;
    else
        func_addr = (u32)WriteByte;

    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();
}

/* ---- Emit a non-branch instruction ---- */
/* Debug helper: log MTC0 writes to SR */
static int mtc0_sr_log_count = 0;
static u32 last_sr_logged = 0xDEAD;
void debug_mtc0_sr(u32 val)
{
    /* Log all SR writes that have IM or IEc bits, or first 10,
     * or when value changes significantly */
    u32 interesting = val & 0x00000701; /* IEc + IM bits */
    if (interesting || mtc0_sr_log_count < 10 || val != last_sr_logged)
    {
        if (mtc0_sr_log_count < 200)
        {
            //            printf("[MTC0] SR = %08X (IEc=%d IM=%02X BEV=%d CU0=%d)\n",
            //                   (unsigned)val,
            //                   (int)(val & 1),
            //                   (int)((val >> 8) & 0xFF),
            //                   (int)((val >> 22) & 1),
            //                   (int)((val >> 28) & 1));
            mtc0_sr_log_count++;
        }
        last_sr_logged = val;
    }
    cpu.cop0[PSX_COP0_SR] = val;
}

/*=== BIOS HLE (High Level Emulation) ===*/
/*
 * The PSX BIOS uses three function tables called via:
 *   A-table: jump to 0xA0 with function number in $t1 ($9)
 *   B-table: jump to 0xB0 with function number in $t1 ($9)
 *   C-table: jump to 0xC0 with function number in $t1 ($9)
 *
 * Some BIOS table entries (especially EnterCriticalSection/ExitCriticalSection)
 * may be incorrectly initialized to placeholder functions. We intercept these
 * calls and implement the critical ones in C code.
 */
static int hle_log_count = 0;

static int BIOS_HLE_A(void)
{
    u32 func = cpu.regs[9]; /* $t1 = function number */
    static int a_log_count = 0;
    if (a_log_count < 30)
    {
        // printf("[BIOS] A(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        a_log_count++;
    }

    /* A(0x3C) std_out_putchar */
    if (func == 0x3C)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = cpu.regs[4]; /* Return char */
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0; /* Let native code handle it */
}

static int BIOS_HLE_B(void)
{
    u32 func = cpu.regs[9]; /* $t1 = function number */
    static int b_log_count = 0;
    if (b_log_count < 30)
    {
        // printf("[BIOS] B(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        b_log_count++;
    }

    /* B(0x3B) putchar - useful for BIOS text output */
    if (func == 0x3B)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = cpu.regs[4];
        cpu.pc = cpu.regs[31];
        return 1;
    }
    /* B(0x3D) std_out_putchar */
    if (func == 0x3D)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = 1; /* Return success/char */
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0; /* Let native code handle everything else */
}

static int BIOS_HLE_C(void)
{
    u32 func = cpu.regs[9];
    static int c_log_count = 0;
    if (c_log_count < 20)
    {
        //        printf("[BIOS] C(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        c_log_count++;
    }
    return 0; /* Let native code handle it */
}

static void emit_instruction(u32 opcode, u32 psx_pc)
{
    u32 op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);
    int rd = RD(opcode);
    int sa = SA(opcode);
    int func = FUNC(opcode);
    s16 imm = SIMM16(opcode);
    u16 uimm = IMM16(opcode);

    /* Track current PC for exception handling in memory accesses */
    emit_current_psx_pc = psx_pc;

    if (opcode == 0)
        return; /* NOP */

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x00: /* SLL */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x00));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x02: /* SRL */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x02));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x03: /* SRA */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x03));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x04: /* SLLV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x04));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x06: /* SRLV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x06));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x07: /* SRAV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x07));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x0C: /* SYSCALL */
            /* Use HLE handler for BIOS compatibility */
            emit_load_imm32(REG_T0, psx_pc);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            emit_load_imm32(REG_A0, 0);
            EMIT_JAL_ABS((u32)Handle_Syscall);
            EMIT_NOP();
            break;
        case 0x0D: /* BREAK */
            /* For now, just skip */
            break;
        case 0x10: /* MFHI */
            EMIT_LW(REG_T0, CPU_HI, REG_S0);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x11: /* MTHI */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x12: /* MFLO */
            EMIT_LW(REG_T0, CPU_LO, REG_S0);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x13: /* MTLO */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            break;
        case 0x18: /* MULT */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x18)); /* mult */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));      /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10)); /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x19)); /* multu */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));      /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10)); /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x1A: /* DIV */
        {
            /* Call Helper_DIV(rs_val, rt_val, &cpu.lo, &cpu.hi) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            /* $a2 = &cpu.lo, $a3 = &cpu.hi */
            EMIT_ADDIU(6, REG_S0, CPU_LO); /* a2 = s0 + CPU_LO */
            EMIT_ADDIU(7, REG_S0, CPU_HI); /* a3 = s0 + CPU_HI */
            EMIT_JAL_ABS((u32)Helper_DIV);
            EMIT_NOP();
            break;
        }
        case 0x1B: /* DIVU */
        {
            /* Call Helper_DIVU(rs_val, rt_val, &cpu.lo, &cpu.hi) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            EMIT_ADDIU(6, REG_S0, CPU_LO); /* a2 = s0 + CPU_LO */
            EMIT_ADDIU(7, REG_S0, CPU_HI); /* a3 = s0 + CPU_HI */
            EMIT_JAL_ABS((u32)Helper_DIVU);
            EMIT_NOP();
            break;
        }
        case 0x20: /* ADD */
        case 0x21: /* ADDU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            EMIT_ADDU(REG_T0, REG_T0, REG_T1);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x22: /* SUB */
        case 0x23: /* SUBU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x23)); /* subu */
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x24: /* AND */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x24));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x25: /* OR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            EMIT_OR(REG_T0, REG_T0, REG_T1);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x26: /* XOR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x26));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x27: /* NOR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x27));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x2A: /* SLT */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x2A));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x2B: /* SLTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x2B));
            emit_store_psx_reg(rd, REG_T0);
            break;
        default:
            if (total_instructions < 50)
                printf("DYNAREC: Unknown SPECIAL func=0x%02X at PC=0x%08X\n", func, (unsigned)psx_pc);
            break;
        }
        break;

    /* I-type ALU */
    case 0x08: /* ADDI */
    case 0x09: /* ADDIU */
        emit_load_psx_reg(REG_T0, rs);
        EMIT_ADDIU(REG_T0, REG_T0, imm);
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0A: /* SLTI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0A, REG_T0, REG_T0, imm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0B: /* SLTIU */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0B, REG_T0, REG_T0, imm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0C: /* ANDI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0C, REG_T0, REG_T0, uimm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0D: /* ORI */
        emit_load_psx_reg(REG_T0, rs);
        EMIT_ORI(REG_T0, REG_T0, uimm);
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0E: /* XORI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0E, REG_T0, REG_T0, uimm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0F: /* LUI */
        emit_load_imm32(REG_T0, (u32)uimm << 16);
        emit_store_psx_reg(rt, REG_T0);
        break;

    /* COP0 */
    case 0x10:
        if (rs == 0x00)
        {
            /* MFC0 rt, rd */
            EMIT_LW(REG_T0, CPU_COP0(rd), REG_S0);
            emit_store_psx_reg(rt, REG_T0);
        }
        else if (rs == 0x04)
        {
            /* MTC0 rt, rd */
            emit_load_psx_reg(REG_T0, rt);
            if (rd == PSX_COP0_SR)
            {
                /* Call debug_mtc0_sr(val) for SR writes */
                EMIT_MOVE(REG_A0, REG_T0);
                EMIT_JAL_ABS((u32)debug_mtc0_sr);
                EMIT_NOP();
            }
            else
            {
                EMIT_SW(REG_T0, CPU_COP0(rd), REG_S0);
            }
        }
        else if (rs == 0x10 && func == 0x10)
        {
            /* RFE - Return from exception
             * Pop mode stack: new_sr = (sr & ~0x0F) | ((sr >> 2) & 0x0F)
             * Bits 0-1 get values from bits 2-3 (IEp→IEc, KUp→KUc)
             * Bits 2-3 get values from bits 4-5 (IEo→IEp, KUo→KUp)
             * Bits 4-31 remain UNCHANGED (IM bits, BEV, CU0, etc.)
             */
            EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0); /* t0 = SR */
            EMIT_MOVE(REG_T1, REG_T0);                      /* t1 = SR */
            emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x02));      /* srl t1, t1, 2 */
            emit(MK_I(0x0C, REG_T1, REG_T1, 0x0F));         /* andi t1, t1, 0x0F */
            /* Clear bottom 4 bits of t0 using srl/sll (preserves bits 4-31) */
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x02));      /* srl t0, t0, 4 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x00));      /* sll t0, t0, 4 */
            EMIT_OR(REG_T0, REG_T0, REG_T1);                /* t0 = (sr & ~0x0F) | shifted */
            EMIT_SW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0); /* SR = t0 */
        }
        break;

    /* COP2 (GTE) */
    case 0x12:
        if (total_instructions < 20000000)
        { // Log COP2 instructions
            printf("DYNAREC: Compiling COP2 Op %08X at %08X\n", opcode, (unsigned)psx_pc);
        }
        if ((opcode & 0x02000000) == 0)
        {
            /* Transfer Instructions (MFC2, CFC2, MTC2, CTC2) */
            // rs field tells us the op: 00=MFC2, 02=CFC2, 04=MTC2, 06=CTC2
            // wait, RS is bits 21-25. The instruction encoding is:
            // COP2(010010) 0(1) rt(5) rd(5) ...
            // Bit 25 is 0.
            // RS is bits 21-25.
            // 00000 -> MFC2
            // 00010 -> CFC2
            // 00100 -> MTC2
            // 00110 -> CTC2
            // These correspond to rs=0, rs=2, rs=4, rs=6.

            if (rs == 0x00)
            { /* MFC2 rt, rd */
                // printf("MFC2 %d, %d\n", rt, rd);
                EMIT_LW(REG_T0, CPU_CP2_DATA(rd), REG_S0);
                emit_store_psx_reg(rt, REG_T0);
            }
            else if (rs == 0x02)
            { /* CFC2 rt, rd */
                // printf("CFC2 %d, %d\n", rt, rd);
                EMIT_LW(REG_T0, CPU_CP2_CTRL(rd), REG_S0);
                emit_store_psx_reg(rt, REG_T0);
            }
            else if (rs == 0x04)
            { /* MTC2 rt, rd */
                if (total_instructions < 1000)
                {
                    /* Emit log call? No, hard to emit printf call here easily without save/restore.
                       Just rely on Compile log. */
                }
                emit_load_psx_reg(REG_T0, rt);
                EMIT_SW(REG_T0, CPU_CP2_DATA(rd), REG_S0);
            }
            else if (rs == 0x06)
            { /* CTC2 rt, rd */
                emit_load_psx_reg(REG_T0, rt);
                EMIT_SW(REG_T0, CPU_CP2_CTRL(rd), REG_S0);
            }
            else
            {
                if (total_instructions < 100)
                    printf("DYNAREC: Unknown COP2 transfer rs=0x%X\n", rs);
            }
        }
        else
        {
            /* GTE Command (Bit 25 = 1) */
            // Call C helper: GTE_Execute(opcode, &cpu)
            // $a0 = opcode
            // $a1 = &cpu ($s0)
            emit_load_imm32(REG_A0, opcode);
            EMIT_MOVE(REG_A1, REG_S0);
            EMIT_JAL_ABS((u32)GTE_Execute);
            EMIT_NOP();
        }
        break;

    /* Load instructions */
    case 0x20: /* LB */
        emit_memory_read_signed(1, rt, rs, imm);
        break;
    case 0x21: /* LH */
        emit_memory_read_signed(2, rt, rs, imm);
        break;
    case 0x23: /* LW */
        emit_memory_read(4, rt, rs, imm);
        break;
    case 0x24: /* LBU */
        emit_memory_read(1, rt, rs, imm);
        break;
    case 0x25: /* LHU */
        emit_memory_read(2, rt, rs, imm);
        break;

    /* Store instructions */
    case 0x28: /* SB */
        emit_memory_write(1, rt, rs, imm);
        break;
    case 0x29: /* SH */
        emit_memory_write(2, rt, rs, imm);
        break;
    case 0x2B: /* SW */
        emit_memory_write(4, rt, rs, imm);
        break;

    /* LWL/LWR/SWL/SWR - unaligned access via C helpers */
    case 0x22: /* LWL */
    {
        /* $a0 = address, $a1 = current rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((u32)Helper_LWL);
        EMIT_NOP();
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x26: /* LWR */
    {
        /* $a0 = address, $a1 = current rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((u32)Helper_LWR);
        EMIT_NOP();
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x2A: /* SWL */
    {
        /* $a0 = address, $a1 = rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((u32)Helper_SWL);
        EMIT_NOP();
        break;
    }
    case 0x2E: /* SWR */
    {
        /* $a0 = address, $a1 = rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((u32)Helper_SWR);
        EMIT_NOP();
        break;
    }

    /* LWC2 - Load Word to Cop2 */
    case 0x32:
    {
        // LWC2 rt, offset(base)
        // rt is destination in CP2 Data Registers
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        EMIT_JAL_ABS((u32)ReadWord);
        EMIT_NOP();
        // Store result ($v0) to CP2_DATA[rt]
        EMIT_SW(REG_V0, CPU_CP2_DATA(rt), REG_S0);
    }
    break;

    /* SWC2 - Store Word from Cop2 */
    case 0x3A:
    {
        // SWC2 rt, offset(base)
        // rt is source from CP2 Data Registers
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        EMIT_LW(REG_A1, CPU_CP2_DATA(rt), REG_S0);
        EMIT_JAL_ABS((u32)WriteWord);
        EMIT_NOP();
    }
    break;

    default:
        // Always log unknown opcodes to catch missing instructions
        static int unknown_log_count = 0;
        if (unknown_log_count < 200)
        {
            printf("DYNAREC: Unknown opcode 0x%02X at PC=0x%08X\n", op, (unsigned)psx_pc);
            unknown_log_count++;
        }
        break;
    }
}

/* ---- Block lookup ---- */
static u32 *lookup_block(u32 psx_pc)
{
    u32 idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    if (block_cache[idx].psx_pc == psx_pc)
        return block_cache[idx].native;
    return NULL;
}

static void cache_block(u32 psx_pc, u32 *native)
{
    u32 idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    block_cache[idx].psx_pc = psx_pc;
    block_cache[idx].native = native;
}

/* ---- Public API ---- */
typedef void (*block_func_t)(R3000CPU *cpu, u8 *ram, u8 *bios);

void Init_Dynarec(void)
{
    printf("Initializing Dynarec...\n");

    /* Allocate code buffer dynamically (BSS is unmapped in PCSX2 TLB) */
    code_buffer = (u32 *)memalign(64, CODE_BUFFER_SIZE);
    if (!code_buffer)
    {
        printf("  ERROR: Failed to allocate code buffer!\n");
        return;
    }

    block_cache = (BlockEntry *)memalign(64, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    if (!block_cache)
    {
        printf("  ERROR: Failed to allocate block cache!\n");
        return;
    }

    code_ptr = code_buffer;
    memset(code_buffer, 0, CODE_BUFFER_SIZE);
    memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    blocks_compiled = 0;
    total_instructions = 0;
    printf("  Code buffer at %p, size %d bytes\n", code_buffer, CODE_BUFFER_SIZE);
    printf("  Block cache at %p, %d entries\n", block_cache, BLOCK_CACHE_SIZE);
}

void Run_CPU(void)
{
    printf("Starting CPU Execution (Dynarec)...\n");

    /* ----- JIT sanity test ----- */
    printf("JIT test: emitting trivial block...\n");
    u32 *test_start = code_ptr;
    /* Minimal block: store 0xDEADBEEF to cpu.pc, then return */
    /* addiu $sp, $sp, -16 */
    emit(MK_I(0x09, REG_SP, REG_SP, (u16)(-16)));
    /* sw $ra, 12($sp) */
    emit(MK_I(0x2B, REG_SP, REG_RA, 12));
    /* lui $t0, 0xDEAD */
    emit(MK_I(0x0F, 0, REG_T0, 0xDEAD));
    /* ori $t0, $t0, 0xBEEF */
    emit(MK_I(0x0D, REG_T0, REG_T0, 0xBEEF));
    /* sw $t0, CPU_PC($a0) */
    emit(MK_I(0x2B, REG_A0, REG_T0, CPU_PC));
    /* lw $ra, 12($sp) */
    emit(MK_I(0x23, REG_SP, REG_RA, 12));
    /* addiu $sp, $sp, 16 */
    emit(MK_I(0x09, REG_SP, REG_SP, 16));
    /* jr $ra */
    emit(MK_R(0, REG_RA, 0, 0, 0, 0x08));
    /* nop (delay slot) */
    emit(0);

    FlushCache(0);
    FlushCache(2);

    printf("JIT test: block at %p, %d words\n", test_start, (int)(code_ptr - test_start));
    int j;
    for (j = 0; j < (int)(code_ptr - test_start); j++)
    {
        printf("  [%d] 0x%08X\n", j, (unsigned)test_start[j]);
    }

    /* Call it */
    cpu.pc = 0;
    printf("JIT test: calling block... cpu.pc before = 0x%08X\n", (unsigned)cpu.pc);
    ((block_func_t)test_start)(&cpu, psx_ram, psx_bios);
    printf("JIT test: returned! cpu.pc = 0x%08X (expect 0xDEADBEEF)\n", (unsigned)cpu.pc);

    if (cpu.pc != 0xDEADBEEF)
    {
        printf("JIT TEST FAILED! FlushCache may not be working.\n");
        return;
    }
    printf("JIT TEST PASSED!\n");

    /* Reset code pointer for actual use */
    code_ptr = code_buffer;
    memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    blocks_compiled = 0;
    total_instructions = 0;

    /* ----- Real execution ----- */
    cpu.pc = 0xBFC00000;
    cpu.cop0[PSX_COP0_SR] = 0x10400000;   /* Initial status: CU0=1, BEV=1 */
    cpu.cop0[PSX_COP0_PRID] = 0x00000002; /* R3000A */

    u32 iterations = 0;
    u32 max_iterations = 200000000; /* 200M iterations for full BIOS boot */
    static u32 stuck_pc = 0;
    static u32 stuck_count = 0;

    while (iterations < max_iterations)
    {
        u32 pc = cpu.pc;

        /* === BIOS HLE Intercepts === */
        /* PSX BIOS uses calls to addresses 0xA0, 0xB0, 0xC0 for function dispatch.
         * Some function table entries may be incorrectly initialized. We intercept
         * key functions (especially EnterCriticalSection/ExitCriticalSection) here. */
        {
            u32 phys_pc = pc & 0x1FFFFFFF;
            if (phys_pc == 0xA0)
            {
                if (BIOS_HLE_A())
                    continue;
            }
            else if (phys_pc == 0xB0)
            {
                if (BIOS_HLE_B())
                    continue;
            }
            else if (phys_pc == 0xC0)
            {
                if (BIOS_HLE_C())
                    continue;
            }
        }

        /* === BIOS Shell Hook === */
        /* Hook the BIOS execution just before it enters the shell/logo sequence.
         * Tentative address: 0xBFC06FF0 (SCPH1001) */
        if (pc == 0xBFC06FF0)
        {
            static int binary_loaded = 0;
            if (!binary_loaded)
            {
                printf("DYNAREC: Reached BIOS Shell Entry (0xBFC06FF0). Loading binary...\n");
                if (Load_PSX_EXE("host:test.exe", &cpu) == 0)
                {
                    printf("DYNAREC: Binary loaded successfully. Jump to PC=0x%08X\n", (unsigned)cpu.pc);
#ifdef ENABLE_HOST_LOG
                    host_log_file = fopen("host:output.log", "w");
#endif
                    binary_loaded = 1;
                    /* Flush cache for new code */
                    FlushCache(0);
                    FlushCache(2);
                    /* Reset stuck count as we changed PC */
                    stuck_pc = cpu.pc;
                    stuck_count = 0;
                    continue; /* Resume execution at new PC */
                }
                else
                {
                    printf("DYNAREC: Failed to load binary. Continuing BIOS.\n");
                }
                binary_loaded = 1; /* Don't try again */
            }
        }

        /* === PC Alignment Check === */
        /* On R3000A, jumping to an unaligned address fires AdEL (cause 4) */
        if (pc & 3)
        {
            cpu.cop0[PSX_COP0_BADVADDR] = pc;
            cpu.pc = pc;      /* Ensure PC is set for the exception handler */
            PSX_Exception(4); /* AdEL - Address Error on instruction fetch */
            continue;
        }

        /* Look up compiled block */
        u32 *block = lookup_block(pc);
        if (!block)
        {
            block = compile_block(pc);
            if (!block)
            {
                printf("DYNAREC: Failed to compile at PC=0x%08X, stopping.\n", (unsigned)pc);
                break;
            }
            cache_block(pc, block);
        }

        /* Execute the block with exception support */
        /* setjmp returns 0 on first call, non-zero if longjmp fired from PSX_Exception */
        psx_block_exception = 1; /* Enable longjmp path in PSX_Exception */
        if (setjmp(psx_block_jmp) == 0)
        {
            ((block_func_t)block)(&cpu, psx_ram, psx_bios);
        }
        /* else: exception fired during block, cpu.pc already set by PSX_Exception */
        psx_block_exception = 0; /* Disable longjmp */

        /* Get instruction count from cache for this block */
        u32 cache_idx = (pc >> 2) & BLOCK_CACHE_MASK;
        u32 cycles = block_cache[cache_idx].instr_count;
        if (cycles == 0)
            cycles = 8; /* fallback estimate */

        /* Update Timers (approximate cycles) */
        UpdateTimers(cycles);

        /* Check for interrupts */
        if (CheckInterrupts())
        {
            /* Update Cause.IP2 to reflect pending interrupt */
            cpu.cop0[PSX_COP0_CAUSE] |= (1 << 10);

            u32 sr = cpu.cop0[PSX_COP0_SR];
            if ((sr & 1) && (sr & (1 << 10)))
            {
                PSX_Exception(0); /* Interrupt */
            }
            else if (iterations % 5000000 == 0)
            {
                printf("DYNAREC: INT pending but blocked: SR=%08X IEc=%d IM2=%d PC=%08X\n",
                       (unsigned)sr, (int)(sr & 1), (int)((sr >> 10) & 1),
                       (unsigned)cpu.pc);
            }
        }
        else
        {
            /* Clear Cause.IP2 when no pending interrupts */
            cpu.cop0[PSX_COP0_CAUSE] &= ~(1 << 10);
        }

        iterations++;

#ifdef ENABLE_STUCK_DETECTION
        /* Stuck loop detection */
        if (pc == stuck_pc)
        {
            stuck_count++;
            if (stuck_count == 50000)
            {
                printf("[STUCK] Block at %08X ran 50000 times (SR=%08X I_STAT=%08X Cause=%08X)\n",
                       (unsigned)pc, (unsigned)cpu.cop0[PSX_COP0_SR],
                       (unsigned)CheckInterrupts(),
                       (unsigned)cpu.cop0[PSX_COP0_CAUSE]);
                /* Dump instructions at stuck address */
                u32 phys = pc & 0x1FFFFF;
                if (phys < PSX_RAM_SIZE - 32)
                {
                    int di;
                    printf("[STUCK] Instructions at %08X:\n", (unsigned)pc);
                    for (di = 0; di < 8; di++)
                    {
                        printf("  %08X: %08X\n", (unsigned)(pc + di * 4),
                               (unsigned)(*(u32 *)(psx_ram + phys + di * 4)));
                    }
                }
                printf("[STUCK] Regs: v0=%08X a0=%08X a1=%08X t0=%08X t1=%08X ra=%08X sp=%08X\n",
                       (unsigned)cpu.regs[2], (unsigned)cpu.regs[4],
                       (unsigned)cpu.regs[5], (unsigned)cpu.regs[8],
                       (unsigned)cpu.regs[9], (unsigned)cpu.regs[31],
                       (unsigned)cpu.regs[29]);
                printf("[STUCK] s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X\n",
                       (unsigned)cpu.regs[16], (unsigned)cpu.regs[17],
                       (unsigned)cpu.regs[18], (unsigned)cpu.regs[19],
                       (unsigned)cpu.regs[20], (unsigned)cpu.regs[21]);
                /* Show what s1+4 contains */
                if (cpu.regs[17] != 0)
                {
                    u32 s1_phys = cpu.regs[17] & 0x1FFFFF;
                    if (s1_phys + 12 < PSX_RAM_SIZE)
                    {
                        printf("[STUCK] *s1: [0]=%08X [4]=%08X [8]=%08X\n",
                               (unsigned)(*(u32 *)(psx_ram + s1_phys)),
                               (unsigned)(*(u32 *)(psx_ram + s1_phys + 4)),
                               (unsigned)(*(u32 *)(psx_ram + s1_phys + 8)));
                    }
                }
            }
        }
        else
        {
            stuck_pc = pc;
            stuck_count = 0;
        }
#endif

        /* Periodic status */
        if (iterations % 1000000 == 0)
        {
            //            printf("DYNAREC: %u iterations, PC=0x%08X, blocks=%u SR=%08X\n",
            //                   (unsigned)iterations, (unsigned)cpu.pc, (unsigned)blocks_compiled,
            //                   (unsigned)cpu.cop0[PSX_COP0_SR]);
        }
    }

    printf("DYNAREC: Stopped after %u iterations. Final PC=0x%08X\n",
           (unsigned)iterations, (unsigned)cpu.pc);
    printf("DYNAREC: Blocks compiled: %u, Total instructions: %u\n",
           (unsigned)blocks_compiled, (unsigned)total_instructions);
}
