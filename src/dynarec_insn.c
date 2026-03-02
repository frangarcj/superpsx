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

extern void emit_flush_partial_cycles(void);
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
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
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
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
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
        if (host_log_fd >= 0)
        {
            host_log_putc(c);
            host_log_flush();
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
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rt) << sa);
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x00));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x02: /* SRL */
        {
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rt) >> sa);
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x02));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x03: /* SRA */
        {
            if (is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, (uint32_t)((int32_t)get_vreg_const(rt) >> sa));
                break;
            }
            mark_vreg_var(rd);
            int s = emit_use_reg(rt, REG_T0);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, 0, s, d, sa, 0x03));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x04: /* SLLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T0);
            int s2 = emit_use_reg(rs, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s2, s1, d, 0, 0x04));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x06: /* SRLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T0);
            int s2 = emit_use_reg(rs, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s2, s1, d, 0, 0x06));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x07: /* SRAV */
        {
            mark_vreg_var(rd);
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
            emit_cpu_field_to_psx_reg(CPU_HI, rd);
            break;
        case 0x11: /* MTHI */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x12: /* MFLO */
            emit_cpu_field_to_psx_reg(CPU_LO, rd);
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
            reg_cache_invalidate();
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
            reg_cache_invalidate();
            break;
        case 0x1A: /* DIV — inline with div-by-zero handling */
        {
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            /* Branch over division if divisor == 0 */
            EMIT_BEQ(REG_T1, REG_ZERO, 7); /* skip 7 insns to @divz */
            EMIT_NOP();
            /* Common path: native signed divide */
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x1A)); /* div t0, t1 */
            emit(MK_R(0, 0, 0, REG_T2, 0, 0x12));      /* mflo t2 */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));      /* mfhi t0 */
            EMIT_SW(REG_T2, CPU_LO, REG_S0);
            uint32_t *b_end_div = code_ptr;
            emit(MK_I(4, REG_ZERO, REG_ZERO, 0)); /* beq zero,zero,@end (placeholder) */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);      /* delay slot */
            /* @divz: lo = (rs >= 0) ? -1 : 1, hi = rs */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);                  /* hi = rs (T0 still has rs) */
            emit(MK_R(0, 0, REG_T0, REG_T1, 31, 0x03));       /* sra t1, t0, 31 */
            emit(MK_R(0, 0, REG_T1, REG_T1, 1, 0x00));        /* sll t1, t1, 1  */
            emit(MK_R(0, REG_T1, REG_ZERO, REG_T1, 0, 0x27)); /* nor t1, t1, zero */
            EMIT_SW(REG_T1, CPU_LO, REG_S0);                  /* lo = result */
            /* @end: patch the branch */
            {
                int32_t off = (int32_t)(code_ptr - b_end_div - 1);
                *b_end_div = (*b_end_div & 0xFFFF0000) | (off & 0xFFFF);
            }
            reg_cache_invalidate();
            break;
        }
        case 0x1B: /* DIVU — inline with div-by-zero handling */
        {
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            /* Branch over division if divisor == 0 */
            EMIT_BEQ(REG_T1, REG_ZERO, 7); /* skip 7 insns to @divz */
            EMIT_NOP();
            /* Common path: native unsigned divide */
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x1B)); /* divu t0, t1 */
            emit(MK_R(0, 0, 0, REG_T2, 0, 0x12));      /* mflo t2 */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10));      /* mfhi t0 */
            EMIT_SW(REG_T2, CPU_LO, REG_S0);
            uint32_t *b_end_divu = code_ptr;
            emit(MK_I(4, REG_ZERO, REG_ZERO, 0)); /* beq zero,zero,@end (placeholder) */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);      /* delay slot */
            /* @divz: lo = 0xFFFFFFFF, hi = rs */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);  /* hi = rs (T0 still has rs) */
            EMIT_ADDIU(REG_T0, REG_ZERO, -1); /* t0 = 0xFFFFFFFF */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);  /* lo = 0xFFFFFFFF */
            /* @end: patch the branch */
            {
                int32_t off = (int32_t)(code_ptr - b_end_divu - 1);
                *b_end_divu = (*b_end_divu & 0xFFFF0000) | (off & 0xFFFF);
            }
            reg_cache_invalidate();
            break;
        }
        case 0x20: /* ADD — with overflow exception detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a + b;
                if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    /* Overflow at compile time — unconditional exception */
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_load_imm32(REG_A1, a);
                    emit_load_imm32(REG_A2, b);
                    emit_load_imm32(REG_A3, rd);
                    emit_call_c((uint32_t)Helper_ADD_JIT);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            emit_load_psx_reg(REG_A1, rs);
            emit_load_psx_reg(REG_A2, rt);
            emit_load_imm32(REG_A3, rd);
            emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
            emit_call_c((uint32_t)Helper_ADD_JIT);
            emit_abort_check(emit_cycle_offset);
            break;
        }
        case 0x21: /* ADDU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) + get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            EMIT_ADDU(d, s1, s2);
            emit_sync_reg(rd, d);
            break;
        }
        case 0x22: /* SUB — with overflow exception detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a - b;
                if (((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    /* Overflow at compile time — unconditional exception */
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_load_psx_reg(REG_A1, rs);
                    emit_load_psx_reg(REG_A2, rt);
                    emit_load_imm32(REG_A3, rd);
                    emit_call_c((uint32_t)Helper_SUB_JIT);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            emit_load_psx_reg(REG_A1, rs);
            emit_load_psx_reg(REG_A2, rt);
            emit_load_imm32(REG_A3, rd);
            emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
            emit_call_c((uint32_t)Helper_SUB_JIT);
            emit_abort_check(emit_cycle_offset);
            break;
        }
        case 0x23: /* SUBU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) - get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x23));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x24: /* AND */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) & get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x24));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x25: /* OR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) | get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            EMIT_OR(d, s1, s2);
            emit_sync_reg(rd, d);
            break;
        }
        case 0x26: /* XOR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) ^ get_vreg_const(rt));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x26));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x27: /* NOR */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, ~(get_vreg_const(rs) | get_vreg_const(rt)));
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x27));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2A: /* SLT */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, ((int32_t)get_vreg_const(rs) < (int32_t)get_vreg_const(rt)) ? 1 : 0);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T0);
            int s2 = emit_use_reg(rt, REG_T1);
            int d = emit_dst_reg(rd, REG_T0);
            emit(MK_R(0, s1, s2, d, 0, 0x2A));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x2B: /* SLTU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, (get_vreg_const(rs) < get_vreg_const(rt)) ? 1 : 0);
                break;
            }
            mark_vreg_var(rd);
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
    case 0x08: /* ADDI — with overflow exception detection */
    {
        if (is_vreg_const(rs))
        {
            uint32_t a = get_vreg_const(rs);
            uint32_t b = (uint32_t)imm; /* sign-extended immediate */
            uint32_t res = a + b;
            if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
            {
                /* Overflow at compile time */
                emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                emit_load_psx_reg(REG_A1, rs);
                emit_load_imm32(REG_A2, b);
                emit_load_imm32(REG_A3, rt);
                emit_call_c((uint32_t)Helper_ADDI_JIT);
                emit_abort_check(emit_cycle_offset);
                break;
            }
            mark_vreg_const_lazy(rt, res);
            break;
        }
        mark_vreg_var(rt);
        emit_load_psx_reg(REG_A1, rs);
        emit_load_imm32(REG_A2, (uint32_t)imm);
        emit_load_imm32(REG_A3, rt);
        emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
        emit_call_c((uint32_t)Helper_ADDI_JIT);
        emit_abort_check(emit_cycle_offset);
        break;
    }
    case 0x09: /* ADDIU */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) + imm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        EMIT_ADDIU(d, s, imm);
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0A: /* SLTI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, ((int32_t)get_vreg_const(rs) < (int32_t)imm) ? 1 : 0);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0A, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0B: /* SLTIU */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, (get_vreg_const(rs) < (uint32_t)(int32_t)imm) ? 1 : 0);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0B, s, d, imm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0C: /* ANDI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) & uimm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0C, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0D: /* ORI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) | uimm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        EMIT_ORI(d, s, uimm);
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0E: /* XORI */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) ^ uimm);
            break;
        }
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T0);
        int d = emit_dst_reg(rt, REG_T0);
        emit(MK_I(0x0E, s, d, uimm));
        emit_sync_reg(rt, d);
        break;
    }
    case 0x0F: /* LUI */
    {
        mark_vreg_const_lazy(rt, (uint32_t)uimm << 16);
        break;
    }

    /* COP0 */
    case 0x10:
    {
        if (rs == 0x00)
        {
            /* MFC0 rt, rd */
            emit_cpu_field_to_psx_reg(CPU_COP0(rd), rt);
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
            reg_cache_invalidate();
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
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 1);
        reg_cache_invalidate();
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
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
                mark_vreg_var(rt);
                if (rd == 15 || rd == 28 || rd == 29)
                {
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, rd);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_ReadData);
                    emit_store_psx_reg(rt, REG_V0);
                }
                else
                {
                    EMIT_LW(REG_V0, CPU_CP2_DATA(rd & 0x1F), REG_S0);
                    emit_store_psx_reg(rt, REG_V0);
                }
            }
            else if (rs == 0x02)
            {
                mark_vreg_var(rt);
                if (rd == 31)
                {
                    /* CFC2 $rt, $31 — FLAG register read.
                     * Call GTE_ReadCtrl so flag-read detection works
                     * (for VU0 fast-path gating). */
                    EMIT_MOVE(REG_A0, REG_S0);
                    emit_load_imm32(REG_A1, 31);
                    emit_flush_partial_cycles();
                    emit_call_c_lite((uint32_t)GTE_ReadCtrl);
                    emit_store_psx_reg(rt, REG_V0);
                }
                else
                {
                    EMIT_LW(REG_V0, CPU_CP2_CTRL(rd & 0x1F), REG_S0);
                    emit_store_psx_reg(rt, REG_V0);
                }
            }
            else if (rs == 0x04)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_load_psx_reg(6, rt);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_WriteData);
            }
            else if (rs == 0x06)
            {
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rd);
                emit_load_psx_reg(6, rt);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_WriteCtrl);
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
            int gte_sf = (opcode >> 19) & 1;
            int gte_lm = (opcode >> 10) & 1;
            switch (gte_func)
            {
            case 0x01: /* RTPS */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_RTPS);
                break;
            case 0x06: /* NCLIP */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCLIP);
                break;
            case 0x0C: /* OP */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_OP);
                break;
            case 0x10: /* DPCS */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_DPCS);
                break;
            case 0x11: /* INTPL */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_INTPL);
                break;
            case 0x12: /* MVMVA */
            {
                int mx = (opcode >> 17) & 3;
                int v = (opcode >> 15) & 3;
                int cv = (opcode >> 13) & 3;
                uint32_t packed = gte_sf | (gte_lm << 1) | (mx << 2) | (v << 4) | (cv << 6);
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, packed);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_MVMVA);
                break;
            }
            case 0x13: /* NCDS */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCDS);
                break;
            case 0x14: /* CDP */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_CDP);
                break;
            case 0x16: /* NCDT */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCDT);
                break;
            case 0x1B: /* NCCS */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCCS);
                break;
            case 0x1C: /* CC */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_CC);
                break;
            case 0x1E: /* NCS */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCS);
                break;
            case 0x20: /* NCT */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCT);
                break;
            case 0x28: /* SQR */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_SQR);
                break;
            case 0x29: /* DCPL */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_DCPL);
                break;
            case 0x2A: /* DPCT */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_DPCT);
                break;
            case 0x2D: /* AVSZ3 */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_AVSZ3);
                break;
            case 0x2E: /* AVSZ4 */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_AVSZ4);
                break;
            case 0x30: /* RTPT */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_RTPT);
                break;
            case 0x3D: /* GPF */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_GPF);
                break;
            case 0x3E: /* GPL */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_GPL);
                break;
            case 0x3F: /* NCCT */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, gte_sf);
                emit_load_imm32(REG_A2, gte_lm);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Inline_NCCT);
                break;
            default:
            {
                /* Unknown GTE op: fall back to generic dispatcher */
                uint32_t phys = psx_pc & 0x1FFFFFFF;
                reg_cache_invalidate();
                emit_load_imm32(REG_T0, phys);
                EMIT_ADDU(REG_T0, REG_T0, REG_S1);
                EMIT_LW(REG_A0, 0, REG_T0);
            }
                EMIT_MOVE(REG_A1, REG_S0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_Execute);
                break;
            }
        }
        break;
    }

    /* COP3 */
    case 0x13:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
    case 0x20:
        mark_vreg_var(rt);
        emit_memory_read_signed(1, rt, rs, imm);
        break; /* LB */
    case 0x21:
        mark_vreg_var(rt);
        emit_memory_read_signed(2, rt, rs, imm);
        break; /* LH */
    case 0x23:
        mark_vreg_var(rt);
        emit_memory_read(4, rt, rs, imm, 0);
        break; /* LW */
    case 0x24:
        mark_vreg_var(rt);
        emit_memory_read(1, rt, rs, imm, 0);
        break; /* LBU */
    case 0x25:
        mark_vreg_var(rt);
        emit_memory_read(2, rt, rs, imm, 0);
        break; /* LHU */

    /* Store instructions */
    case 0x28:
        emit_memory_write(1, rt, rs, imm);
        break; /* SB */
    case 0x29:
        emit_memory_write(2, rt, rs, imm);
        break; /* SH */
    case 0x2B:
        emit_memory_write(4, rt, rs, imm);
        break; /* SW */

    /* LWL/LWR/SWL/SWR */
    case 0x22: /* LWL */
    {
        mark_vreg_var(rt);
        emit_memory_lwx(1, rt, rs, imm, dynarec_lwx_pending);
        break;
    }
    case 0x26: /* LWR */
    {
        mark_vreg_var(rt);
        emit_memory_lwx(0, rt, rs, imm, dynarec_lwx_pending);
        break;
    }
    case 0x2A: /* SWL */
    {
        emit_memory_swx(1, rt, rs, imm);
        break;
    }
    case 0x2E: /* SWR */
    {
        emit_memory_swx(0, rt, rs, imm);
        break;
    }

    /* LWC0 */
    case 0x30:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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

        /* Memory read via LUT fast path (result in V0), then GTE write */
        {
            int saved_defer = dynarec_load_defer;
            dynarec_load_defer = 1;
            emit_memory_read(4, 0, rs, imm, 0); /* V0 = word from [rs+imm] */
            dynarec_load_defer = saved_defer;
        }
        EMIT_MOVE(REG_A0, REG_S0);
        emit_load_imm32(REG_A1, rt);
        EMIT_MOVE(6, REG_V0);
        emit_call_c((uint32_t)GTE_WriteData);
    }
    break;

    /* LWC3 */
    case 0x33:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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

        /* GTE read → V0 (data to store) */
        EMIT_MOVE(REG_A0, REG_S0);
        emit_load_imm32(REG_A1, rt);
        emit_call_c((uint32_t)GTE_ReadData);

        /* Memory write via LUT fast path (data in V0 → T2, addr in T0) */
        EMIT_MOVE(REG_T2, REG_V0); /* T2 = GTE data */
        emit_load_psx_reg(REG_T0, rs);
        EMIT_ADDIU(REG_T0, REG_T0, imm); /* T0 = effective addr */

        /* Cache Isolation check */
        EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);
        emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
        emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
        uint32_t *isc_swc2 = code_ptr;
        emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne → slow */
        EMIT_NOP();

        /* Alignment check */
        emit(MK_I(0x0C, REG_T0, REG_T1, 3)); /* andi t1, t0, 3 */
        uint32_t *align_swc2 = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0));          /* bne → slow */
        emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24)); /* [delay] and t1, t0, s3 (phys) */

        /* Range check: always present — non-RAM goes to SP/slow path */
        uint32_t *range_swc2 = NULL;
        {
            emit(MK_R(0, 0, REG_T1, REG_A0, 21, 0x02)); /* srl  a0, t1, 21 */
            range_swc2 = code_ptr;
            emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne → slow */
        }
        EMIT_ADDU(REG_T1, REG_T1, REG_S1); /* [delay/inline] host = base + phys */

        /* Fast path: direct store */
        EMIT_SW(REG_T2, 0, REG_T1);

        uint32_t *done_swc2 = code_ptr;
        emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
        EMIT_NOP();

        /* Scratchpad inline check for SWC2 */
        uint32_t *sp_miss_swc2 = NULL;
        uint32_t *sp_done_swc2 = NULL;
        {
            {
                int32_t s2 = (int32_t)(code_ptr - range_swc2 - 1);
                *range_swc2 = (*range_swc2 & 0xFFFF0000) | ((uint32_t)s2 & 0xFFFF);
            }
            /* phys = vaddr & 0x1FFFFFFF (already in S3); check (phys - 0x1F800000) < 0x400 */
            emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24)); /* and  t1, t0, s3     */
            emit(MK_I(0x0F, 0, REG_A0, 0xE080));            /* lui  a0, 0xE080     */
            EMIT_ADDU(REG_T1, REG_T1, REG_A0);              /* t1 = phys-0x1F800000 */
            emit(MK_I(0x0B, REG_T1, REG_T1, 0x400));        /* sltiu t1, 0x400     */
            sp_miss_swc2 = code_ptr;
            emit(MK_I(0x04, REG_T1, REG_ZERO, 0)); /* beq  → @slow        */
            EMIT_NOP();
            /* Scratchpad fast path */
            emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf);
            emit(MK_I(0x0C, REG_T0, REG_A0, 0x3FF)); /* andi a0, t0, 0x3FF  */
            EMIT_ADDU(REG_T1, REG_T1, REG_A0);
            EMIT_SW(REG_T2, 0, REG_T1);
            sp_done_swc2 = code_ptr;
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done             */
            EMIT_NOP();
        }

        /* Slow path */
        {
            int32_t s0 = (int32_t)(code_ptr - isc_swc2 - 1);
            *isc_swc2 = (*isc_swc2 & 0xFFFF0000) | ((uint32_t)s0 & 0xFFFF);
            int32_t s1 = (int32_t)(code_ptr - align_swc2 - 1);
            *align_swc2 = (*align_swc2 & 0xFFFF0000) | ((uint32_t)s1 & 0xFFFF);
            if (sp_miss_swc2)
            {
                int32_t s_sp = (int32_t)(code_ptr - sp_miss_swc2 - 1);
                *sp_miss_swc2 = (*sp_miss_swc2 & 0xFFFF0000) | ((uint32_t)s_sp & 0xFFFF);
            }
        }
        EMIT_MOVE(REG_A0, REG_T0);
        EMIT_MOVE(REG_A1, REG_T2);
        /* Flush partial cycle offset for accurate timer reads in WriteHardware */
        {
            uint32_t pbc_addr = (uint32_t)&partial_block_cycles;
            uint16_t pbc_lo = pbc_addr & 0xFFFF;
            uint16_t pbc_hi = (pbc_addr + 0x8000) >> 16;
            EMIT_LUI(REG_AT, pbc_hi);
            EMIT_ADDIU(REG_T1, REG_ZERO, (int16_t)emit_cycle_offset);
            EMIT_SW(REG_T1, (int16_t)pbc_lo, REG_AT);
        }
        emit_call_c_lite((uint32_t)WriteWord);

        /* @done: patch forward branches */
        {
            int32_t d0 = (int32_t)(code_ptr - done_swc2 - 1);
            *done_swc2 = (*done_swc2 & 0xFFFF0000) | ((uint32_t)d0 & 0xFFFF);
            if (sp_done_swc2)
            {
                int32_t d_sp = (int32_t)(code_ptr - sp_done_swc2 - 1);
                *sp_done_swc2 = (*sp_done_swc2 & 0xFFFF0000) | ((uint32_t)d_sp & 0xFFFF);
            }
        }
    }
    break;

    /* SWC3 */
    case 0x3B:
    {
        flush_dirty_consts(); /* Flush before COP-usable conditional */
        reg_cache_invalidate();
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
