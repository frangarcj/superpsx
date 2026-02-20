/*
 * dynarec_compile.c - Block compiler, prologue/epilogue, analysis
 *
 * Contains the main compile_block() loop that translates PSX basic blocks
 * into native R5900 code, along with block prologue/epilogue generation,
 * cycle cost estimation, and load delay slot analysis.
 */
#include "dynarec.h"

/* ---- Compile-time state ---- */
uint32_t blocks_compiled = 0;
uint32_t total_instructions = 0;
uint32_t block_cycle_count = 0;
uint32_t emit_current_psx_pc = 0;
int dynarec_load_defer = 0;
int dynarec_lwx_pending = 0;

/* ---- R3000A Instruction Cycle Cost Table ----
 * Most instructions are 1 cycle. Exceptions:
 *   MULT/MULTU: ~6 cycles (data-dependent, 6 is average)
 *   DIV/DIVU:   ~36 cycles
 *   Load (LW/LB/LH/LBU/LHU/LWL/LWR): 2 cycles (1 + load delay)
 *   Store (SW/SB/SH/SWL/SWR): 1 cycle
 *   Branch taken: 2 cycles (1 + delay slot, but delay slot counted separately)
 *   COP2 (GTE): variable, approximate by opcode
 */
uint32_t r3000a_cycle_cost(uint32_t opcode)
{
    uint32_t op = OP(opcode);
    uint32_t func = FUNC(opcode);

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x18: return 6;  /* MULT  */
        case 0x19: return 6;  /* MULTU */
        case 0x1A: return 36; /* DIV   */
        case 0x1B: return 36; /* DIVU  */
        default:   return 1;
        }
    /* Loads: 2 cycles (includes load delay) */
    case 0x20: return 2; /* LB  */
    case 0x21: return 2; /* LH  */
    case 0x22: return 2; /* LWL */
    case 0x23: return 2; /* LW  */
    case 0x24: return 2; /* LBU */
    case 0x25: return 2; /* LHU */
    case 0x26: return 2; /* LWR */
    /* Stores: 1 cycle */
    case 0x28: return 1; /* SB  */
    case 0x29: return 1; /* SH  */
    case 0x2B: return 1; /* SW  */
    case 0x2A: return 1; /* SWL */
    case 0x2E: return 1; /* SWR */
    /* COP2 (GTE) commands */
    case 0x12: /* COP2 */
        if (opcode & 0x02000000)
        {
            uint32_t gte_op = opcode & 0x3F;
            switch (gte_op)
            {
            case 0x01: return 15; /* RTPS  */
            case 0x06: return 8;  /* NCLIP */
            case 0x0C: return 6;  /* OP    */
            case 0x10: return 8;  /* DPCS  */
            case 0x11: return 8;  /* INTPL */
            case 0x12: return 8;  /* MVMVA */
            case 0x13: return 19; /* NCDS  */
            case 0x14: return 13; /* CDP   */
            case 0x16: return 44; /* NCDT  */
            case 0x1B: return 17; /* NCCS  */
            case 0x1C: return 11; /* CC    */
            case 0x1E: return 14; /* NCS   */
            case 0x20: return 30; /* NCT   */
            case 0x28: return 5;  /* SQR   */
            case 0x29: return 8;  /* DCPL  */
            case 0x2A: return 17; /* DPCT  */
            case 0x2D: return 5;  /* AVSZ3 */
            case 0x2E: return 6;  /* AVSZ4 */
            case 0x30: return 23; /* RTPT  */
            case 0x3D: return 5;  /* GPF   */
            case 0x3E: return 5;  /* GPL   */
            case 0x3F: return 39; /* NCCT  */
            default:   return 8;  /* Unknown GTE */
            }
        }
        return 1; /* MFC2/MTC2/CFC2/CTC2 */
    /* Branches/Jumps */
    case 0x02: return 1; /* J    */
    case 0x03: return 1; /* JAL  */
    case 0x04: return 1; /* BEQ  */
    case 0x05: return 1; /* BNE  */
    case 0x06: return 1; /* BLEZ */
    case 0x07: return 1; /* BGTZ */
    case 0x01: return 1; /* REGIMM */
    /* COP0/COP1/COP3 */
    case 0x10: return 1; /* COP0 */
    case 0x11: return 1; /* COP1 */
    case 0x13: return 1; /* COP3 */
    /* LWC2/SWC2 */
    case 0x32: return 2; /* LWC2 */
    case 0x3A: return 1; /* SWC2 */
    default:   return 1;
    }
}

/* Check if instruction reads a given GPR as source operand */
int instruction_reads_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0;
    int op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);

    if (rs == reg)
    {
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            if (func == 0x00 || func == 0x02 || func == 0x03)
                return 0; /* SLL, SRL, SRA */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI, MFLO */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL, BREAK */
        }
        if (op == 0x02 || op == 0x03)
            return 0; /* J, JAL */
        if (op == 0x0F)
            return 0; /* LUI */
        return 1;
    }

    if (rt == reg)
    {
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            if (func == 0x08 || func == 0x09)
                return 0; /* JR/JALR */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI/MFLO */
            if (func == 0x11 || func == 0x13)
                return 0; /* MTHI/MTLO */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL/BREAK */
            return 1;
        }
        if (op == 0x04 || op == 0x05)
            return 1; /* BEQ/BNE */
        if (op == 0x22 || op == 0x26)
            return 1; /* LWL/LWR */
        if (op >= 0x28 && op <= 0x2E)
            return 1; /* Stores */
        return 0;
    }

    return 0;
}

/* Check if instruction writes a given GPR as destination operand */
int instruction_writes_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0;
    int op = OP(opcode);
    if (op == 0x00)
    {
        int func = FUNC(opcode);
        if (func >= 0x18 && func <= 0x1B)
            return 0; /* MULT/MULTU/DIV/DIVU */
        if (func == 0x08)
            return 0; /* JR */
        if (func == 0x11 || func == 0x13)
            return 0; /* MTHI/MTLO */
        if (func == 0x0C || func == 0x0D)
            return 0; /* SYSCALL/BREAK */
        return (RD(opcode) == reg);
    }
    if (op >= 0x08 && op <= 0x0F)
        return (RT(opcode) == reg);
    if (op >= 0x20 && op <= 0x26)
        return (RT(opcode) == reg);
    if (op == 0x03)
        return (reg == 31);
    if (op == 0x00 && FUNC(opcode) == 0x09)
        return (RD(opcode) == reg);
    return 0;
}

/* ---- Block prologue: save callee-saved regs, set up $s0-$s2, load pinned ---- */
void emit_block_prologue(void)
{
    EMIT_ADDIU(REG_SP, REG_SP, -80);
    EMIT_SW(REG_RA, 44, REG_SP);
    EMIT_SW(REG_S0, 40, REG_SP);
    EMIT_SW(REG_S1, 36, REG_SP);
    EMIT_SW(REG_S2, 32, REG_SP);
    EMIT_SW(REG_S3, 28, REG_SP);
    EMIT_SW(REG_S4, 48, REG_SP);
    EMIT_SW(REG_S5, 52, REG_SP);
    EMIT_SW(REG_S6, 56, REG_SP);
    EMIT_SW(REG_S7, 60, REG_SP);
    EMIT_MOVE(REG_S0, REG_A0);
    EMIT_MOVE(REG_S1, REG_A1);
    EMIT_MOVE(REG_S2, REG_A3); /* $s2 = cycles_left */
    emit_reload_pinned();
}

/* ---- Block epilogue: flush pinned, restore and return ---- */
void emit_block_epilogue(void)
{
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
    EMIT_MOVE(REG_V0, REG_S2);
    emit_flush_pinned();
    EMIT_LW(REG_S7, 60, REG_SP);
    EMIT_LW(REG_S6, 56, REG_SP);
    EMIT_LW(REG_S5, 52, REG_SP);
    EMIT_LW(REG_S4, 48, REG_SP);
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 80);
    EMIT_JR(REG_RA);
    EMIT_NOP();
}

void emit_branch_epilogue(uint32_t target_pc)
{
    /* Calculate remaining cycles after this block */
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
    
    /* Update cpu.pc IMMEDIATELY, before any potential abort check */
    emit_load_imm32(REG_T0, target_pc);
    EMIT_SW(REG_T0, CPU_PC, REG_S0);

    /* If remaining cycles <= 0, abort to C scheduler */
    emit(MK_I(0x07, REG_S2, REG_ZERO, 2)); /* BGTZ s2, +2 */
    EMIT_NOP(); /* Delay slot */
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    EMIT_NOP(); /* Delay slot */

    /* Direct link to target block (bypassing prologue, maintaining stack and pinned regs) */
    emit_direct_link(target_pc);
}

/* ---- Compile a basic block ---- */
uint32_t *compile_block(uint32_t psx_pc)
{
    uint32_t *psx_code = get_psx_code_ptr(psx_pc);
    if (!psx_code)
    {
        DLOG("Cannot fetch code at PC=0x%08X\n", (unsigned)psx_pc);
        return NULL;
    }

    /* Check for code buffer overflow: reset if < 64KB remaining */
    uint32_t used = (uint32_t)((uint8_t *)code_ptr - (uint8_t *)code_buffer);
    if (used > CODE_BUFFER_SIZE - 65536)
    {
        DLOG("Code buffer nearly full (%u/%u), flushing cache\n",
             (unsigned)used, CODE_BUFFER_SIZE);
        code_ptr = code_buffer + 128;
        memset(code_buffer + 128, 0, CODE_BUFFER_SIZE - 128 * sizeof(uint32_t));
        memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
        memset(block_node_pool, 0, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
        block_node_pool_idx = 0;
        patch_sites_count = 0;
        blocks_compiled = 0;
    }

    uint32_t *block_start = code_ptr;
    uint32_t cur_pc = psx_pc;
    block_cycle_count = 0;

    if (blocks_compiled < 20)
    {
        DLOG("Compiling block at PC=0x%08X\n", (unsigned)psx_pc);
    }

    // Debug: Inspect hot loop
    if (psx_pc == 0x800509AC)
    {
        DLOG("Hot Loop dump at %08" PRIX32 ":\n", psx_pc);
        DLOG_RAW("  -4: %08" PRIX32 "\n", psx_code[-1]);
        DLOG_RAW("   0: %08" PRIX32 " (Hit)\n", psx_code[0]);
        DLOG_RAW("  +4: %08" PRIX32 "\n", psx_code[1]);
        DLOG_RAW("  +8: %08" PRIX32 "\n", psx_code[2]);
        DLOG_RAW(" +12: %08" PRIX32 "\n", psx_code[3]);
    }

    emit_block_prologue();

    /* Inject BIOS HLE hooks natively so that DBL jumps do not bypass them */
    uint32_t phys_pc = psx_pc & 0x1FFFFFFF;
    if (phys_pc == 0xA0)
    {
        emit_call_c((uint32_t)BIOS_HLE_A);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }
    else if (phys_pc == 0xB0)
    {
        emit_call_c((uint32_t)BIOS_HLE_B);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }
    else if (phys_pc == 0xC0)
    {
        emit_call_c((uint32_t)BIOS_HLE_C);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }

    int block_ended = 0;
    int in_delay_slot = 0;
    uint32_t branch_target = 0;
    int branch_type = 0; /* 0=none, 1=unconditional, 2=conditional, 3=register */
    uint32_t branch_opcode = 0;

    /* Load delay slot tracking */
    int pending_load_reg = 0;
    int pending_load_apply_now = 0;
    int block_mult_count = 0;

    while (!block_ended)
    {
        uint32_t opcode = *psx_code++;
        block_cycle_count += r3000a_cycle_cost(opcode);

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

            dynarec_load_defer = 0;
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            if (emit_instruction(opcode, cur_pc, &block_mult_count) < 0)
            {
                block_ended = 1;
                break;
            }
            dynarec_lwx_pending = 0;
            cur_pc += 4;
            total_instructions++;

            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }

            if (branch_type == 1)
            {
                emit_branch_epilogue(branch_target);
            }
            else if (branch_type == 4)
            {
                /* Deferred Conditional Branch (calculated in S3) */
                uint32_t *bp = code_ptr;
                emit(MK_I(0x05, REG_S3, REG_ZERO, 0)); /* BNE s3, zero, 0 */
                EMIT_NOP();

                branch_opcode = (uint32_t)bp;

                /* Not taken: fall through PC */
                emit_branch_epilogue(cur_pc);
                
                /* Taken path target */
                uint32_t *taken_addr = code_ptr;
                int32_t offset = (int32_t)(taken_addr - bp - 1);
                *bp = (*bp & 0xFFFF0000) | (offset & 0xFFFF);
                emit_branch_epilogue(branch_target);
            }
            else if (branch_type == 3)
            {
                /* Register jump (JR/JALR): try inline block cache lookup */
                
                /* 1. Calculate remaining cycles and check abort boundary */
                EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
                emit(MK_I(0x07, REG_S2, REG_ZERO, 2)); /* BGTZ s2, +2 */
                EMIT_NOP(); /* Delay slot */
                EMIT_J_ABS((uint32_t)abort_trampoline_addr);
                EMIT_NOP(); /* Delay slot */

                /* 2. Load target PC from CPU struct (it was saved there by JR opcode emitter) */
                EMIT_LW(REG_T0, CPU_PC, REG_S0);
                
                /* 3. Compute Hash Index: (target_pc >> 2) & BLOCK_CACHE_MASK */
                emit(MK_R(0, 0, REG_T0, REG_T1, 2, 0x02));          /* SRL t1, t0, 2 */
                emit(MK_I(0x0C, REG_T1, REG_T1, BLOCK_CACHE_MASK)); /* ANDI t1, t1, mask */
                
                /* 4. Multiply by 32 (sizeof BlockEntry is strictly 32 bytes) */
                emit(MK_R(0, 0, REG_T1, REG_T1, 5, 0x00));          /* SLL t1, t1, 5 */
                
                /* 5. Add base of block_cache */
                emit_load_imm32(REG_T2, (uint32_t)block_cache);
                EMIT_ADDU(REG_T1, REG_T1, REG_T2);                  /* t1 = &block_cache[idx] */
                
                /* 6. Verify psx_pc matches */
                EMIT_LW(REG_T2, 0, REG_T1);                         /* t2 = be->psx_pc */
                emit(MK_I(0x05, REG_T0, REG_T2, 7));                /* BNE t0, t2, MISS (+7) */
                EMIT_NOP();
                
                /* 7. Verify native matches */
                EMIT_LW(REG_T2, 4, REG_T1);                         /* t2 = be->native */
                emit(MK_I(0x04, REG_T2, REG_ZERO, 4));              /* BEQ t2, zero, MISS (+4) */
                EMIT_NOP();
                
                /* 8. Jump to target block bypassing prologue */
                EMIT_ADDIU(REG_T2, REG_T2, DYNAREC_PROLOGUE_WORDS * 4);
                EMIT_JR(REG_T2);
                EMIT_NOP();
                
                /* MISS target */
                EMIT_J_ABS((uint32_t)abort_trampoline_addr);
                EMIT_NOP();
            }
            block_ended = 1;
            break;
        }

        uint32_t op = OP(opcode);

        /* Check for branch/jump instructions */
        if (op == 0x02 || op == 0x03)
        {
            if (op == 0x03)
            {
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(31, REG_T0);
            }
            branch_target = ((cur_pc + 4) & 0xF0000000) | (TARGET(opcode) << 2);
            branch_type = 1;
            in_delay_slot = 1;
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
            int rs = RS(opcode);
            int rd = (FUNC(opcode) == 0x09) ? RD(opcode) : 0;
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            if (FUNC(opcode) == 0x09 && rd != 0)
            {
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(rd, REG_T1);
            }
            branch_type = 3;
            in_delay_slot = 1;
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
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            emit_load_psx_reg(REG_T0, rs);
            if (op == 0x04 || op == 0x05)
            {
                emit_load_psx_reg(REG_T1, rt);
                emit(MK_R(0, REG_T0, REG_T1, REG_S3, 0, 0x26)); /* XOR s3, t0, t1 */
                if (op == 0x04)
                {
                    emit(MK_I(0x0B, REG_S3, REG_S3, 1)); /* SLTIU s3, s3, 1 */
                }
            }
            else if (op == 0x06)
            {
                emit(MK_I(0x0A, REG_T0, REG_S3, 1)); /* SLTI s3, t0, 1 */
            }
            else if (op == 0x07)
            {
                emit(MK_R(0, REG_ZERO, REG_T0, REG_S3, 0, 0x2A)); /* SLT s3, zero, t0 */
            }

            branch_type = 4;
            in_delay_slot = 1;
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
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            emit_load_psx_reg(REG_T0, rs);

            if (rt == 0x10 || rt == 0x11)
            {
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(31, REG_T1);
            }

            if ((rt & 1) == 0)
            {
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A)); /* SLT s3, t0, zero */
            }
            else
            {
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
                emit(MK_I(0x0E, REG_S3, REG_S3, 1)); /* XORI s3, s3, 1 */
            }

            branch_type = 4;
            in_delay_slot = 1;
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
            uint32_t op_check = OP(opcode);

            if (op_check == 0x20 || op_check == 0x21 || op_check == 0x22 ||
                op_check == 0x23 || op_check == 0x24 || op_check == 0x25 ||
                op_check == 0x26)
            {
                load_target = RT(opcode);
                if (load_target != 0)
                {
                    uint32_t next_instr = *psx_code;
                    uint32_t next_op = OP(next_instr);
                    int next_rt = RT(next_instr);

                    if (instruction_reads_gpr(next_instr, load_target))
                    {
                        this_is_load = 1;
                    }
                    else if ((next_op >= 0x20 && next_op <= 0x26) &&
                             next_rt == load_target)
                    {
                        this_is_load = 1;
                    }
                }
            }

            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                if (this_is_load && load_target == pending_load_reg)
                {
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
            }

            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            dynarec_load_defer = this_is_load;
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            if (emit_instruction(opcode, cur_pc, &block_mult_count) < 0)
            {
                block_ended = 1;
                break;
            }
            dynarec_lwx_pending = 0;
            dynarec_load_defer = 0;

            if (pending_load_reg != 0 && !this_is_load &&
                instruction_writes_gpr(opcode, pending_load_reg))
            {
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }

            if (this_is_load)
            {
                if (pending_load_reg != 0 && pending_load_reg != load_target)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                }
                EMIT_SW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
                pending_load_reg = load_target;
                pending_load_apply_now = 0;
            }
        }
        cur_pc += 4;
        total_instructions++;

        if ((cur_pc - psx_pc) >= 256)
        {
            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }
            emit_branch_epilogue(cur_pc);
            block_ended = 1;
        }
    }

    if (blocks_compiled < 5)
    {
        int num_words = (int)(code_ptr - block_start);
        DLOG("Block %u at %p, %d words:\n",
             (unsigned)blocks_compiled, block_start, num_words);
        int j;
        for (j = 0; j < num_words && j < 32; j++)
        {
            DLOG_RAW("  [%02d] %p: 0x%08X\n", j, &block_start[j], (unsigned)block_start[j]);
        }
        if (num_words > 32)
            DLOG_RAW("  ... (%d more)\n", num_words - 32);
    }
    uint32_t block_instr_count = (cur_pc - psx_pc) / 4;

    FlushCache(0);
    FlushCache(2);

    blocks_compiled++;

    /* Detect idle/polling loops */
    {
        int is_idle = 0;
        int branch_kind = 0;
        if (branch_type == 1 && branch_target == psx_pc)
            branch_kind = 1;
        else if (branch_type == 4 && branch_target == psx_pc)
            branch_kind = 2;

        if (branch_kind)
        {
            is_idle = branch_kind;
            uint32_t *scan = get_psx_code_ptr(psx_pc);
            uint32_t scan_end = cur_pc;
            uint32_t spc = psx_pc;
            while (scan && spc < scan_end)
            {
                uint32_t inst = *scan++;
                int sop = OP(inst);
                if (sop == 0x28 || sop == 0x29 || sop == 0x2A ||
                    sop == 0x2B || sop == 0x2E || sop == 0x3A)
                {
                    is_idle = 0;
                    break;
                }
                if ((sop == 0x10 || sop == 0x12) && (RS(inst) == 4 || RS(inst) == 6))
                {
                    is_idle = 0;
                    break;
                }
                if (sop == 0 && (FUNC(inst) == 0x0C || FUNC(inst) == 0x0D))
                {
                    is_idle = 0;
                    break;
                }
                spc += 4;
            }
        }

        uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
        block_cache[idx].instr_count = block_instr_count;
        block_cache[idx].native_count = (uint32_t)(code_ptr - block_start);
        block_cache[idx].cycle_count = block_cycle_count > 0 ? block_cycle_count : block_instr_count;
        block_cache[idx].is_idle = is_idle;
    }

    return block_start;
}
