/*
 * dynarec_insn.c - PSX instruction emitter (main switch)
 *
 * Contains emit_instruction() which generates native R5900 code for each
 * PSX R3000A instruction, plus BIOS HLE functions and debug helpers.
 */
#include "dynarec.h"
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"
#include "loader.h"

/* ---- Debug helpers ---- */
static int mtc0_sr_log_count = 0;
static uint32_t last_sr_logged = 0xDEAD;

void debug_mtc0_sr(uint32_t val)
{
    uint32_t interesting = val & 0x00000701;
    if (interesting || mtc0_sr_log_count < 10 || val != last_sr_logged)
    {
        if (mtc0_sr_log_count < 200)
        {
            mtc0_sr_log_count++;
        }
        last_sr_logged = val;
    }
    cpu.cop0[PSX_COP0_SR] = val;
}

/*=== BIOS HLE (High Level Emulation) ===*/
int BIOS_HLE_A(void)
{
    uint32_t func = cpu.regs[9];
    static int a_log_count = 0;
    if (a_log_count < 30)
    {
        a_log_count++;
    }

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
        cpu.regs[2] = cpu.regs[4];
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0;
}

int BIOS_HLE_B(void)
{
    uint32_t func = cpu.regs[9];
    static int b_log_count = 0;
    if (b_log_count < 30)
    {
        b_log_count++;
    }

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
        cpu.regs[2] = 1;
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0;
}

int BIOS_HLE_C(void)
{
    uint32_t func = cpu.regs[9];
    static int c_log_count = 0;
    if (c_log_count < 20)
    {
        c_log_count++;
    }
    (void)func;
    return 0;
}

/* ---- Main instruction emitter ---- */
int emit_instruction(uint32_t opcode, uint32_t psx_pc, int *mult_count)
{
    uint32_t op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);
    int rd = RD(opcode);
    int sa = SA(opcode);
    int func = FUNC(opcode);
    int16_t imm = SIMM16(opcode);
    uint16_t uimm = IMM16(opcode);

    emit_current_psx_pc = psx_pc;

    if (opcode == 0)
        return 0; /* NOP */

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x00: /* SLL */
        {
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x00));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x02: /* SRL */
        {
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x02));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x03: /* SRA */
        {
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x03));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x04: /* SLLV */
        {
            int s1 = emit_use_reg(rt, REG_T0);
            int s2 = emit_use_reg(rs, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s2, s1, d, 0, 0x04));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x06: /* SRLV */
        {
            int s1 = emit_use_reg(rt, REG_T0);
            int s2 = emit_use_reg(rs, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s2, s1, d, 0, 0x06));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x07: /* SRAV */
        {
            int s1 = emit_use_reg(rt, REG_T0);
            int s2 = emit_use_reg(rs, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s2, s1, d, 0, 0x07));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x0C: /* SYSCALL */
            emit_load_imm32(REG_A0, psx_pc);
            emit_call_c((uint32_t)Helper_Syscall_Exception);
            emit_block_epilogue();
            return -1;
        case 0x0D: /* BREAK */
            emit_load_imm32(REG_A0, psx_pc);
            emit_call_c((uint32_t)Helper_Break_Exception);
            emit_block_epilogue();
            return -1;
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
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULT1(REG_T0, REG_T1);
                EMIT_MFLO1(REG_T0);
                EMIT_SW(REG_T0, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T0);
            }
            else
            {
                emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x18));
                emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));
                EMIT_SW(REG_T0, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));
            }
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULTU1(REG_T0, REG_T1);
                EMIT_MFLO1(REG_T0);
                EMIT_SW(REG_T0, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T0);
            }
            else
            {
                emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x19));
                emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));
                EMIT_SW(REG_T0, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));
            }
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x1A: /* DIV */
        {
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            EMIT_ADDIU(6, REG_S0, CPU_LO);
            EMIT_ADDIU(7, REG_S0, CPU_HI);
            emit_call_c((uint32_t)Helper_DIV);
            break;
        }
        case 0x1B: /* DIVU */
        {
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            EMIT_ADDIU(6, REG_S0, CPU_LO);
            EMIT_ADDIU(7, REG_S0, CPU_HI);
            emit_call_c((uint32_t)Helper_DIVU);
            break;
        }
        case 0x20: /* ADD (with overflow check) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            emit_load_imm32(6, rd);
            emit_load_imm32(7, psx_pc);
            emit_call_c((uint32_t)Helper_ADD);
            emit_abort_check();
            break;
        case 0x21: /* ADDU */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            EMIT_ADDU(d, s1, s2);
            emit_sync_reg(rd, d);
            break;
        }
        case 0x22: /* SUB (with overflow check) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            emit_load_imm32(6, rd);
            emit_load_imm32(7, psx_pc);
            emit_call_c((uint32_t)Helper_SUB);
            emit_abort_check();
            break;
        case 0x23: /* SUBU */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x23));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x24: /* AND */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x24));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x25: /* OR */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            EMIT_OR(d, s1, s2);
            emit_sync_reg(rd, d);
            break;
        }
        case 0x26: /* XOR */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x26));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x27: /* NOR */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x27));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2A: /* SLT */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x2A));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2B: /* SLTU */
        {
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x2B));
            emit_sync_reg(rd, d);
            break;
        }
        default:
            if (total_instructions < 50)
                DLOG("Unknown SPECIAL func=0x%02X at PC=0x%08X\n", func, (unsigned)psx_pc);
            break;
        }
        break;

    /* I-type ALU */
    case 0x08: /* ADDI (with overflow check) */
    {
        emit_load_psx_reg(REG_A0, rs);
        emit_load_imm32(REG_A1, (uint32_t)(int32_t)imm);
        emit_load_imm32(6, rt);
        emit_load_imm32(7, psx_pc);
        emit_call_c((uint32_t)Helper_ADDI);
        emit_abort_check();
        break;
    }
    case 0x09: /* ADDIU */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        EMIT_ADDIU(d, s, imm);
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0A: /* SLTI */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0A, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0B: /* SLTIU */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0B, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0C: /* ANDI */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0C, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0D: /* ORI */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        EMIT_ORI(d, s, uimm);
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0E: /* XORI */
    {
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0E, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0F: /* LUI */
    {
        int d = emit_dst_reg(rt, REG_T0);
        emit_load_imm32(d, (uint32_t)uimm << 16);
        emit_sync_reg(rt, d);
        break;
    }

    /* COP0 */
    case 0x10:
    {
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
                EMIT_MOVE(REG_A0, REG_T0);
                emit_call_c((uint32_t)debug_mtc0_sr);
            }
            else
            {
                EMIT_SW(REG_T0, CPU_COP0(rd), REG_S0);
            }
        }
        else if (rs == 0x10 && func == 0x10)
        {
            /* RFE */
            EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
            EMIT_MOVE(REG_T1, REG_T0);
            emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x02));
            emit(MK_I(0x0C, REG_T1, REG_T1, 0x0F));
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x02));
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x00));
            EMIT_OR(REG_T0, REG_T0, REG_T1);
            EMIT_SW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        }
        break;
    }

    /* COP1 */
    case 0x11:
    {
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 1);
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 29, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_patch_1 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_patch_1 = (*skip_patch_1 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_patch_1 - 1) & 0xFFFF);
        break;
    }

    /* COP2 (GTE) */
    case 0x12:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_cu2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_cu2 = (*skip_cu2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu2 - 1) & 0xFFFF);

        if (total_instructions < 20000000)
        {
            DLOG("Compiling COP2 Op %08" PRIX32 " at %08" PRIX32 "\n", opcode, psx_pc);
        }
        if ((opcode & 0x02000000) == 0)
        {
            if (rs == 0x00)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_call_c((uint32_t)GTE_ReadData);
                emit_store_psx_reg(rt, REG_V0);
            }
            else if (rs == 0x02)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_call_c((uint32_t)GTE_ReadCtrl);
                emit_store_psx_reg(rt, REG_V0);
            }
            else if (rs == 0x04)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_load_psx_reg(6, rt);
                emit_call_c((uint32_t)GTE_WriteData);
            }
            else if (rs == 0x06)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_load_psx_reg(6, rt);
                emit_call_c((uint32_t)GTE_WriteCtrl);
            }
            else
            {
                if (total_instructions < 100)
                    DLOG("Unknown COP2 transfer rs=0x%X\n", rs);
            }
        }
        else
        {
            uint32_t gte_func = opcode & 0x3F;
            switch (gte_func)
            {
            case 0x06: /* NCLIP */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_call_c((uint32_t)GTE_Inline_NCLIP);
                break;
            case 0x28: /* SQR */
            {
                int gte_sf = (opcode >> 19) & 1;
                int gte_lm = (opcode >> 10) & 1;
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_call_c((uint32_t)GTE_Inline_SQR);
                break;
            }
            case 0x2D: /* AVSZ3 */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_call_c((uint32_t)GTE_Inline_AVSZ3);
                break;
            case 0x2E: /* AVSZ4 */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_call_c((uint32_t)GTE_Inline_AVSZ4);
                break;
            default:
            {
                uint32_t phys = psx_pc & 0x1FFFFFFF;
                emit_load_imm32(REG_T0, phys);
                EMIT_ADDU(REG_T0, REG_T0, REG_S1);
                EMIT_LW(REG_A0, 0, REG_T0);
            }
                EMIT_MOVE(REG_A1, REG_S0);
                emit_call_c((uint32_t)GTE_Execute);
                break;
            }
        }
        break;
    }

    /* COP3 */
    case 0x13:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02));
        uint32_t *skip_cu3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_cu3 = (*skip_cu3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu3 - 1) & 0xFFFF);
        break;
    }

    /* Load instructions */
    case 0x20: emit_memory_read_signed(1, rt, rs, imm); break; /* LB */
    case 0x21: emit_memory_read_signed(2, rt, rs, imm); break; /* LH */
    case 0x23: emit_memory_read(4, rt, rs, imm); break;        /* LW */
    case 0x24: emit_memory_read(1, rt, rs, imm); break;        /* LBU */
    case 0x25: emit_memory_read(2, rt, rs, imm); break;        /* LHU */

    /* Store instructions */
    case 0x28: emit_memory_write(1, rt, rs, imm); break; /* SB */
    case 0x29: emit_memory_write(2, rt, rs, imm); break; /* SH */
    case 0x2B: emit_memory_write(4, rt, rs, imm); break; /* SW */

    /* LWL/LWR/SWL/SWR */
    case 0x22: /* LWL */
    {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        if (dynarec_lwx_pending)
            EMIT_LW(REG_A1, CPU_LOAD_DELAY_VAL, REG_S0);
        else
            emit_load_psx_reg(REG_A1, rt);
        emit_call_c((uint32_t)Helper_LWL);
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x26: /* LWR */
    {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        if (dynarec_lwx_pending)
            EMIT_LW(REG_A1, CPU_LOAD_DELAY_VAL, REG_S0);
        else
            emit_load_psx_reg(REG_A1, rt);
        emit_call_c((uint32_t)Helper_LWR);
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x2A: /* SWL */
    {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        emit_call_c((uint32_t)Helper_SWL);
        break;
    }
    case 0x2E: /* SWR */
    {
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        emit_call_c((uint32_t)Helper_SWR);
        break;
    }

    /* LWC0 */
    case 0x30:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 28, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_lwc0 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_lwc0 = (*skip_lwc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc0 - 1) & 0xFFFF);
        break;
    }

    /* LWC2 */
    case 0x32:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_lwc2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_lwc2 = (*skip_lwc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc2 - 1) & 0xFFFF);

        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_call_c((uint32_t)ReadWord);
        EMIT_MOVE(REG_A0, REG_S0);
        emit_load_imm32(REG_A1, rt);
        EMIT_MOVE(6, REG_V0);
        emit_call_c((uint32_t)GTE_WriteData);
    }
    break;

    /* LWC3 */
    case 0x33:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02));
        uint32_t *skip_lwc3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_lwc3 = (*skip_lwc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc3 - 1) & 0xFFFF);
        break;
    }

    /* SWC0 */
    case 0x38:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 28, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_swc0 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_swc0 = (*skip_swc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc0 - 1) & 0xFFFF);
        break;
    }

    /* SWC2 */
    case 0x3A:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02));
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));
        uint32_t *skip_swc2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_swc2 = (*skip_swc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc2 - 1) & 0xFFFF);

        EMIT_MOVE(REG_A0, REG_S0);
        emit_load_imm32(REG_A1, rt);
        emit_call_c((uint32_t)GTE_ReadData);
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        EMIT_MOVE(REG_A1, REG_V0);
        emit_call_c((uint32_t)WriteWord);
    }
    break;

    /* SWC3 */
    case 0x3B:
    {
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02));
        uint32_t *skip_swc3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0));
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3);
        emit_call_c((uint32_t)Helper_CU_Exception);
        *skip_swc3 = (*skip_swc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc3 - 1) & 0xFFFF);
        break;
    }

    default:
    {
        static int unknown_log_count = 0;
        if (unknown_log_count < 200)
        {
            DLOG("Unknown opcode 0x%02" PRIX32 " at PC=0x%08" PRIX32 "\n", op, psx_pc);
            unknown_log_count++;
        }
        break;
    }
    }
    return 0;
}
