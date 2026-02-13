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

/* ---- Code buffer ---- */
#define CODE_BUFFER_SIZE  (2 * 1024 * 1024)
static u32 *code_buffer;
static u32 *code_ptr;

/* ---- Block cache ---- */
#define BLOCK_CACHE_BITS 14
#define BLOCK_CACHE_SIZE (1 << BLOCK_CACHE_BITS)
#define BLOCK_CACHE_MASK (BLOCK_CACHE_SIZE - 1)

typedef struct {
    u32 psx_pc;
    u32 *native;
} BlockEntry;

static BlockEntry *block_cache;

/* ---- Instruction encoding helpers ---- */
#define OP(x)     (((x) >> 26) & 0x3F)
#define RS(x)     (((x) >> 21) & 0x1F)
#define RT(x)     (((x) >> 16) & 0x1F)
#define RD(x)     (((x) >> 11) & 0x1F)
#define SA(x)     (((x) >> 6)  & 0x1F)
#define FUNC(x)   ((x) & 0x3F)
#define IMM16(x)  ((x) & 0xFFFF)
#define SIMM16(x) ((s16)((x) & 0xFFFF))
#define TARGET(x) ((x) & 0x03FFFFFF)

/* Emit a 32-bit instruction to code buffer */
static inline void emit(u32 inst) {
    *code_ptr++ = inst;
}

/* MIPS instruction builders */
#define MK_R(op,rs,rt,rd,sa,fn) \
    ((((u32)(op))<<26)|(((u32)(rs))<<21)|(((u32)(rt))<<16)|(((u32)(rd))<<11)|(((u32)(sa))<<6)|((u32)(fn)))
#define MK_I(op,rs,rt,imm) \
    ((((u32)(op))<<26)|(((u32)(rs))<<21)|(((u32)(rt))<<16)|((u32)((imm)&0xFFFF)))
#define MK_J(op,tgt) \
    ((((u32)(op))<<26)|((u32)((tgt)&0x03FFFFFF)))

/* Common emitters using $t0-$t3 as temps */
#define EMIT_NOP()              emit(0)
#define EMIT_LW(rt,off,base)    emit(MK_I(0x23,(base),(rt),(off)))
#define EMIT_SW(rt,off,base)    emit(MK_I(0x2B,(base),(rt),(off)))
#define EMIT_LH(rt,off,base)    emit(MK_I(0x21,(base),(rt),(off)))
#define EMIT_LHU(rt,off,base)   emit(MK_I(0x25,(base),(rt),(off)))
#define EMIT_LB(rt,off,base)    emit(MK_I(0x20,(base),(rt),(off)))
#define EMIT_LBU(rt,off,base)   emit(MK_I(0x24,(base),(rt),(off)))
#define EMIT_SH(rt,off,base)    emit(MK_I(0x29,(base),(rt),(off)))
#define EMIT_SB(rt,off,base)    emit(MK_I(0x28,(base),(rt),(off)))
#define EMIT_ADDIU(rt,rs,imm)   emit(MK_I(0x09,(rs),(rt),(imm)))
#define EMIT_ADDU(rd,rs,rt)     emit(MK_R(0,(rs),(rt),(rd),0,0x21))
#define EMIT_OR(rd,rs,rt)       emit(MK_R(0,(rs),(rt),(rd),0,0x25))
#define EMIT_LUI(rt,imm)        emit(MK_I(0x0F,0,(rt),(imm)))
#define EMIT_ORI(rt,rs,imm)     emit(MK_I(0x0D,(rs),(rt),(imm)))
#define EMIT_MOVE(rd,rs)        EMIT_ADDU(rd,rs,0)
#define EMIT_JR(rs)             emit(MK_R(0,(rs),0,0,0,0x08))
#define EMIT_JAL_ABS(addr)      emit(MK_J(3, (u32)(addr) >> 2))
#define EMIT_J_ABS(addr)        emit(MK_J(2, (u32)(addr) >> 2))
#define EMIT_BEQ(rs,rt,off)     emit(MK_I(4,(rs),(rt),(off)))
#define EMIT_BNE(rs,rt,off)     emit(MK_I(5,(rs),(rt),(off)))

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
#define REG_T0  8
#define REG_T1  9
#define REG_T2 10
#define REG_A0  4
#define REG_A1  5
#define REG_V0  2
#define REG_RA 31
#define REG_SP 29
#define REG_ZERO 0

/* Load PSX register 'r' from cpu struct into hw reg 'hwreg' */
static void emit_load_psx_reg(int hwreg, int r) {
    if (r == 0) {
        EMIT_MOVE(hwreg, REG_ZERO); /* $0 is always 0 */
    } else {
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
    }
}

/* Store hw reg 'hwreg' to PSX register 'r' in cpu struct */
static void emit_store_psx_reg(int r, int hwreg) {
    if (r == 0) return; /* never write to $0 */
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
}

/* Load 32-bit immediate into hw register */
static void emit_load_imm32(int hwreg, u32 val) {
    if (val == 0) {
        EMIT_MOVE(hwreg, REG_ZERO);
    } else if ((val & 0xFFFF0000) == 0) {
        EMIT_ORI(hwreg, REG_ZERO, val & 0xFFFF);
    } else if ((val & 0xFFFF) == 0) {
        EMIT_LUI(hwreg, val >> 16);
    } else {
        EMIT_LUI(hwreg, val >> 16);
        EMIT_ORI(hwreg, hwreg, val & 0xFFFF);
    }
}

/* ---- Get pointer to PSX code in EE memory ---- */
static u32 *get_psx_code_ptr(u32 psx_pc) {
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

/* ---- Compile a basic block ---- */
static u32 *compile_block(u32 psx_pc) {
    u32 *psx_code = get_psx_code_ptr(psx_pc);
    if (!psx_code) {
        printf("DYNAREC: Cannot fetch code at PC=0x%08X\n", (unsigned)psx_pc);
        return NULL;
    }

    u32 *block_start = code_ptr;
    u32 cur_pc = psx_pc;

    if (blocks_compiled < 20) {
        printf("DYNAREC: Compiling block at PC=0x%08X\n", (unsigned)psx_pc);
    }
    
    // Debug: Inspect hot loop
    if (psx_pc == 0x800509AC) {
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

    while (!block_ended) {
        u32 opcode = *psx_code++;

        if (in_delay_slot) {
            /* Emit the delay slot instruction */
            emit_instruction(opcode, cur_pc);
            cur_pc += 4;
            total_instructions++;

            /* Now emit the branch resolution */
            if (branch_type == 1) {
                /* Unconditional: J, JAL */
                emit_branch_epilogue(branch_target);
            } else if (branch_type == 4) {
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
            } else if (branch_type == 3) {
                /* Register jump (JR/JALR): target already in cpu.pc */
                emit_block_epilogue();
            }
            block_ended = 1;
            break;
        }

        u32 op = OP(opcode);

        /* Check for branch/jump instructions */
        if (op == 0x02 || op == 0x03) {
            /* J / JAL */
            if (op == 0x03) {
                /* JAL: store return address */
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(31, REG_T0);
            }
            branch_target = ((cur_pc + 4) & 0xF0000000) | (TARGET(opcode) << 2);
            branch_type = 1;
            in_delay_slot = 1;
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x00 && (FUNC(opcode) == 0x08 || FUNC(opcode) == 0x09)) {
            /* JR / JALR */
            int rs = RS(opcode);
            int rd = (FUNC(opcode) == 0x09) ? RD(opcode) : 0;
            if (FUNC(opcode) == 0x09 && rd != 0) {
                /* JALR: store return address */
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(rd, REG_T0);
            }
            /* Store target from register to cpu.pc */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            branch_type = 3;
            in_delay_slot = 1;
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07) {
            /* BEQ, BNE, BLEZ, BGTZ */
            int rs = RS(opcode);
            int rt = RT(opcode);
            s32 offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            emit_load_psx_reg(REG_T0, rs);
            if (op == 0x04 || op == 0x05) { /* BEQ, BNE */
                emit_load_psx_reg(REG_T1, rt);
                /* XOR s3, t0, t1 */
                emit(MK_R(0, REG_T0, REG_T1, REG_S3, 0, 0x26));
                if (op == 0x04) { /* BEQ: taken if s3 == 0 -> set s3 = (s3 < 1) */
                    /* SLTIU s3, s3, 1 */
                    emit(MK_I(0x0B, REG_S3, REG_S3, 1));
                }
                /* BNE: taken if s3 != 0. Already correct. */
            } else if (op == 0x06) { /* BLEZ (rs <= 0) */
               /* Taken if rs <= 0 -> rs < 1 */
               /* SLTI s3, t0, 1 */
               emit(MK_I(0x0A, REG_T0, REG_S3, 1));
            } else if (op == 0x07) { /* BGTZ (rs > 0) */
               /* Taken if rs > 0 -> 0 < rs. SLT s3, zero, t0 */
               emit(MK_R(0, REG_ZERO, REG_T0, REG_S3, 0, 0x2A));
            }
            
            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x01) {
            /* REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL */
            int rs = RS(opcode);
            int rt = RT(opcode);
            s32 offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            if (rt == 16 || rt == 17) {
                /* BLTZAL/BGEZAL: store return address */
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(31, REG_T0);
            }

            emit_load_psx_reg(REG_T0, rs);
            if (rt == 0 || rt == 16) {
                /* BLTZ / BLTZAL (rs < 0) */
                /* SLT s3, t0, zero */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
            } else {
                /* BGEZ / BGEZAL (rs >= 0) */
                /* Taken if rs >= 0. -> Not (rs < 0). */
                /* SLT s3, t0, zero (1 if <0). XORI s3, s3, 1 (1 if >= 0). */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
                emit(MK_I(0x0E, REG_S3, REG_S3, 1));
            }
            
            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        /* Not a branch - emit normally */
        emit_instruction(opcode, cur_pc);
        cur_pc += 4;
        total_instructions++;

        /* End block after N instructions to avoid huge blocks */
        if ((cur_pc - psx_pc) >= 256) {
            emit_load_imm32(REG_T0, cur_pc);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            emit_block_epilogue();
            block_ended = 1;
        }
    }

    if (blocks_compiled < 5) {
        int num_words = (int)(code_ptr - block_start);
        printf("DYNAREC: Block %u at %p, %d words:\n",
               (unsigned)blocks_compiled, block_start, num_words);
        int j;
        for (j = 0; j < num_words && j < 32; j++) {
            printf("  [%02d] %p: 0x%08X\n", j, &block_start[j], (unsigned)block_start[j]);
        }
        if (num_words > 32) printf("  ... (%d more)\n", num_words - 32);
    }
    /* Flush caches */
    FlushCache(0); /* writeback dcache */
    FlushCache(2); /* invalidate icache */

    blocks_compiled++;
    return block_start;
}

/* ---- Block prologue: save callee-saved regs, set up $s0-$s2 ---- */
static void emit_block_prologue(void) {
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
static void emit_block_epilogue(void) {
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 48);
    EMIT_JR(REG_RA);
    EMIT_NOP();
}

static void emit_branch_epilogue(u32 target_pc) {
    emit_load_imm32(REG_T0, target_pc);
    EMIT_SW(REG_T0, CPU_PC, REG_S0);
    emit_block_epilogue();
}

/* ---- Memory access emitters ---- */
static void emit_memory_read(int size, int rt_psx, int rs_psx, s16 offset) {
    /* $a0 = PSX address */
    emit_load_psx_reg(REG_A0, rs_psx);
    EMIT_ADDIU(REG_A0, REG_A0, offset);

    /* Call ReadWord/ReadHalf/ReadByte */
    u32 func_addr;
    if (size == 4) func_addr = (u32)ReadWord;
    else if (size == 2) func_addr = (u32)ReadHalf;
    else func_addr = (u32)ReadByte;

    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();

    /* Store result ($v0) to PSX reg */
    emit_store_psx_reg(rt_psx, REG_V0);
}

static void emit_memory_read_signed(int size, int rt_psx, int rs_psx, s16 offset) {
    emit_memory_read(size, rt_psx, rs_psx, offset);
    /* Sign extend for LB/LH */
    if (rt_psx == 0) return;
    if (size == 1) {
        /* Sign extend byte: sll 24, sra 24 */
        EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x00)); /* SLL $t0, $t0, 24 */
        emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x03)); /* SRA $t0, $t0, 24 */
        EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
    } else if (size == 2) {
        EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x00)); /* SLL $t0, $t0, 16 */
        emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x03)); /* SRA $t0, $t0, 16 */
        EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
    }
}

static void emit_memory_write(int size, int rt_psx, int rs_psx, s16 offset) {
    /* $a0 = PSX address, $a1 = data */
    emit_load_psx_reg(REG_A0, rs_psx);
    EMIT_ADDIU(REG_A0, REG_A0, offset);
    emit_load_psx_reg(REG_A1, rt_psx);

    u32 func_addr;
    if (size == 4) func_addr = (u32)WriteWord;
    else if (size == 2) func_addr = (u32)WriteHalf;
    else func_addr = (u32)WriteByte;

    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();
}

/* ---- Emit a non-branch instruction ---- */
static void emit_instruction(u32 opcode, u32 psx_pc) {
    u32 op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);
    int rd = RD(opcode);
    int sa = SA(opcode);
    int func = FUNC(opcode);
    s16 imm = SIMM16(opcode);
    u16 uimm = IMM16(opcode);

    if (opcode == 0) return; /* NOP */

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (func) {
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
            emit_load_imm32(REG_A0, psx_pc);
            EMIT_SW(REG_A0, CPU_PC, REG_S0);
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
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));       /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));       /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x19)); /* multu */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));       /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));       /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x1A: /* DIV */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x1A)); /* div */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));       /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));       /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x1B: /* DIVU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x1B)); /* divu */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));       /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));       /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
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
        if (rs == 0x00) {
            /* MFC0 rt, rd */
            EMIT_LW(REG_T0, CPU_COP0(rd), REG_S0);
            emit_store_psx_reg(rt, REG_T0);
        } else if (rs == 0x04) {
            /* MTC0 rt, rd */
            emit_load_psx_reg(REG_T0, rt);
            EMIT_SW(REG_T0, CPU_COP0(rd), REG_S0);
        } else if (rs == 0x10 && func == 0x10) {
            /* RFE - Return from exception */
            /* Shift Status register mode bits right by 2 */
            EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
            emit(MK_I(0x0C, REG_T0, REG_T1, 0x3C)); /* andi $t1, $t0, 0x3C */
            emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x02)); /* srl $t1, $t1, 2 */
            emit(MK_I(0x0C, REG_T0, REG_T0, 0xFFFFFFC0 & 0xFFFF)); /* andi $t0, $t0, 0xFFC0 ... */
            /* Actually simpler: */
            EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
            /* sr = (sr & 0xFFFFFFF0) | ((sr >> 2) & 0x0F) */
            EMIT_MOVE(REG_T1, REG_T0);
            emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x02)); /* srl $t1, 2 */
            emit(MK_I(0x0C, REG_T1, REG_T1, 0x0F));     /* andi $t1, 0x0F */
            emit(MK_I(0x0C, REG_T0, REG_T0, (u16)0xFFF0)); /* andi $t0, 0xFFF0 */
            EMIT_OR(REG_T0, REG_T0, REG_T1);
            EMIT_SW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        }
        break;

    /* COP2 (GTE) */
    case 0x12:
        if (total_instructions < 20000000) { // Log COP2 instructions
             printf("DYNAREC: Compiling COP2 Op %08X at %08X\n", opcode, (unsigned)psx_pc);
        }
        if ((opcode & 0x02000000) == 0) {
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
            
            if (rs == 0x00) { /* MFC2 rt, rd */
                // printf("MFC2 %d, %d\n", rt, rd);
                EMIT_LW(REG_T0, CPU_CP2_DATA(rd), REG_S0);
                emit_store_psx_reg(rt, REG_T0);
            } else if (rs == 0x02) { /* CFC2 rt, rd */
                // printf("CFC2 %d, %d\n", rt, rd);
                EMIT_LW(REG_T0, CPU_CP2_CTRL(rd), REG_S0);
                emit_store_psx_reg(rt, REG_T0);
            } else if (rs == 0x04) { /* MTC2 rt, rd */
                if (total_instructions < 1000) {
                    /* Emit log call? No, hard to emit printf call here easily without save/restore. 
                       Just rely on Compile log. */
                }
                emit_load_psx_reg(REG_T0, rt);
                EMIT_SW(REG_T0, CPU_CP2_DATA(rd), REG_S0);
            } else if (rs == 0x06) { /* CTC2 rt, rd */
                emit_load_psx_reg(REG_T0, rt);
                EMIT_SW(REG_T0, CPU_CP2_CTRL(rd), REG_S0);
            } else {
                 if (total_instructions < 100) printf("DYNAREC: Unknown COP2 transfer rs=0x%X\n", rs);
            }
        } else {
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

    /* LWL/LWR/SWL/SWR - unaligned access, handle via function calls */
    case 0x22: /* LWL */ {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        /* Read the word at aligned address */
        EMIT_MOVE(REG_T2, REG_A0); /* save addr */
        emit(MK_I(0x0C, REG_A0, REG_A0, (u16)0xFFFC)); /* andi $a0, 0xFFFC */
        EMIT_JAL_ABS((u32)ReadWord);
        EMIT_NOP();
        /* $v0 = word at aligned addr, $t2 = original addr */
        emit_load_psx_reg(REG_T0, rt); /* current rt value */
        /* shift = (addr & 3) * 8 */
        emit(MK_I(0x0C, REG_T2, REG_T1, 3)); /* andi $t1, $t2, 3 */
        emit(MK_R(0, 0, REG_T1, REG_T1, 3, 0x00)); /* sll $t1, 3 */
        /* For LWL: result = (mem << (24-shift)) | (rt & mask) */
        /* Simplified: just do full word read for now */
        emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x26: /* LWR */ {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit(MK_I(0x0C, REG_A0, REG_A0, (u16)0xFFFC));
        EMIT_JAL_ABS((u32)ReadWord);
        EMIT_NOP();
        emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x2A: /* SWL */
    case 0x2E: /* SWR */
        /* Simplified: write full word */
        emit_memory_write(4, rt, rs, imm);
        break;

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
        if (unknown_log_count < 200) {
            printf("DYNAREC: Unknown opcode 0x%02X at PC=0x%08X\n", op, (unsigned)psx_pc);
            unknown_log_count++;
        }
        break;
    }
}

/* ---- Block lookup ---- */
static u32 *lookup_block(u32 psx_pc) {
    u32 idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    if (block_cache[idx].psx_pc == psx_pc)
        return block_cache[idx].native;
    return NULL;
}

static void cache_block(u32 psx_pc, u32 *native) {
    u32 idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    block_cache[idx].psx_pc = psx_pc;
    block_cache[idx].native = native;
}

/* ---- Public API ---- */
typedef void (*block_func_t)(R3000CPU *cpu, u8 *ram, u8 *bios);

void Init_Dynarec(void) {
    printf("Initializing Dynarec...\n");

    /* Allocate code buffer dynamically (BSS is unmapped in PCSX2 TLB) */
    code_buffer = (u32 *)memalign(64, CODE_BUFFER_SIZE);
    if (!code_buffer) {
        printf("  ERROR: Failed to allocate code buffer!\n");
        return;
    }

    block_cache = (BlockEntry *)memalign(64, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    if (!block_cache) {
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

void Run_CPU(void) {
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
    for (j = 0; j < (int)(code_ptr - test_start); j++) {
        printf("  [%d] 0x%08X\n", j, (unsigned)test_start[j]);
    }

    /* Call it */
    cpu.pc = 0;
    printf("JIT test: calling block... cpu.pc before = 0x%08X\n", (unsigned)cpu.pc);
    ((block_func_t)test_start)(&cpu, psx_ram, psx_bios);
    printf("JIT test: returned! cpu.pc = 0x%08X (expect 0xDEADBEEF)\n", (unsigned)cpu.pc);

    if (cpu.pc != 0xDEADBEEF) {
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
    cpu.cop0[PSX_COP0_SR] = 0x10900000; /* Initial status: BEV=1, ISC=0 */
    cpu.cop0[PSX_COP0_PRID] = 0x00000002; /* R3000A */

    u32 iterations = 0;
    u32 max_iterations = 20000000; /* Increased limit */

    while (iterations < max_iterations) {
        u32 pc = cpu.pc;

        /* Look up compiled block */
        u32 *block = lookup_block(pc);
        if (!block) {
            block = compile_block(pc);
            if (!block) {
                printf("DYNAREC: Failed to compile at PC=0x%08X, stopping.\n", (unsigned)pc);
                break;
            }
            cache_block(pc, block);
        }

        /* Execute the block */
        u32 inst_count_before = total_instructions;
        ((block_func_t)block)(&cpu, psx_ram, psx_bios);
        u32 cycles = total_instructions - inst_count_before;
        
        /* Update Timers (approximate cycles) */
        UpdateTimers(cycles);

        /* Check for interrupts */
        if (CheckInterrupts()) {
            /* If interrupts are enabled in Status register (IEc=1, IM=something?)
             * For now, just assume widespread enable bit (IEc) is relevant
             */
             if (cpu.cop0[PSX_COP0_SR] & 1) {
                 PSX_Exception(0); /* Interrupt */
             }
        }

        iterations++;

        /* Periodic status */
        if (iterations % 1000000 == 0) {
            printf("DYNAREC: %u iterations, PC=0x%08X, blocks=%u\n",
                   (unsigned)iterations, (unsigned)cpu.pc, (unsigned)blocks_compiled);
        }
    }

    printf("DYNAREC: Stopped after %u iterations. Final PC=0x%08X\n",
           (unsigned)iterations, (unsigned)cpu.pc);
    printf("DYNAREC: Blocks compiled: %u, Total instructions: %u\n",
           (unsigned)blocks_compiled, (unsigned)total_instructions);
}
