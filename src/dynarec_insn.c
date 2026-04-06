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
    /* If ISC bit (16) changes, flush JIT: blocks compiled with/without
     * ISC checks must be recompiled with the correct assumption. */
    if ((val ^ cpu.cop0[PSX_COP0_SR]) & (1 << 16))
        jit_flush_pending = 1;
    cpu.cop0[PSX_COP0_SR] = val;
    cpu.irq_pending_fast = cpu.irq_pending & (val & 1);
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

/* ================================================================
 * P23: Overflow exception cold queue
 * ================================================================
 * Instead of inlining the full exception path (12 words) at each
 * ADD/SUB/ADDI overflow check, defer to a shared stub at block end.
 * Per-site cost: BLTZ+NOP (inline) + 3-5 words (cold entry).
 * Shared stub: ~10-16 words (once per block).
 * Saves ~8 words per additional overflow instruction in a block.
 */
#define MAX_OVERFLOW_COLD 32

typedef struct {
    uint32_t *branch_patch;   /* BLTZ placeholder to patch */
    uint32_t psx_pc;          /* PSX PC for cpu.current_pc */
    int16_t  cycle_offset;    /* Cycles consumed up to this point */
    int      restore_psx;     /* PSX register to restore (-1 = none) */
} OverflowColdEntry;

static OverflowColdEntry overflow_cold[MAX_OVERFLOW_COLD];
static int overflow_cold_count;

void overflow_cold_reset(void) { overflow_cold_count = 0; }

void overflow_cold_emit_all(void)
{
    if (overflow_cold_count == 0)
        return;

    /* Emit shared stub FIRST so per-site entries can J to it */
    uint32_t *shared_stub = code_ptr;

    /* AT = psx_pc (loaded by per-site code) */
    EMIT_SW(REG_AT, CPU_CURRENT_PC, REG_S0);

    /* Save -cycle_offset (T9) to stack — C call will clobber T9 */
    EMIT_SW(REG_T9, 76, REG_SP);

    /* Flush ALL assigned dynamic slots unconditionally.
     * Overflow always aborts, so non-dirty slots are harmless. */
    if (dyn_slots_active)
    {
        for (int s = 0; s < DYN_SLOT_COUNT; s++)
        {
            if (dyn_slot_psx[s] >= 0)
                EMIT_SW(REG_T0 + s, CPU_REG(dyn_slot_psx[s]), REG_S0);
        }
    }

    /* Call Helper_Overflow_Exception via full C-call trampoline */
    emit_load_imm32(REG_T8, (uint32_t)Helper_Overflow_Exception);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_addr);
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0); /* delay slot */

    /* Unconditional abort — overflow exception ALWAYS sets block_aborted */
    EMIT_LW(REG_T9, 76, REG_SP);              /* restore -cycle_offset */
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    emit(MK_R(0, REG_S2, REG_T9, REG_S2, 0, 0x21)); /* delay: ADDU S2, S2, T9 */

    /* Emit per-site cold entries (each Js backward to shared_stub) */
    for (int i = 0; i < overflow_cold_count; i++)
    {
        OverflowColdEntry *e = &overflow_cold[i];

        /* Patch BLTZ to jump here */
        int32_t off = (int32_t)(code_ptr - e->branch_patch - 1);
        *e->branch_patch = (*e->branch_patch & 0xFFFF0000) | ((uint32_t)off & 0xFFFF);

        /* Restore PSX register (pre-ALU value from cpu.regs[]) */
        if (e->restore_psx > 0)
        {
            if (psx_pinned_reg[e->restore_psx])
                EMIT_LW(psx_pinned_reg[e->restore_psx],
                         CPU_REG(e->restore_psx), REG_S0);
            else
                dyn_reload_one_slot(e->restore_psx);
        }

        /* Load psx_pc into AT */
        emit_load_imm32(REG_AT, e->psx_pc);

        /* J shared_stub with -cycle_offset in delay slot */
        emit(MK_J(2, (uint32_t)shared_stub >> 2));
        emit(MK_I(0x09, REG_ZERO, REG_T9,
                   (uint16_t)(-(int16_t)e->cycle_offset)));
    }

    overflow_cold_count = 0;
}

/* ================================================================
 * COP Usable (CU) exception check — shared helper
 * ================================================================
 * Emit inline SR bit test for COPn.  If CUn is disabled, the cold
 * path calls Helper_CU_Exception and lets the abort check handle it.
 *
 * Critical invariant: emit_call_c() resets smrv_known_ram and
 * align_known_mask to defaults, and clears dyn_dirty_mask.  Those
 * changes only take effect at runtime when the exception fires, so
 * the compile-time mirrors must be restored for the normal path.
 */
static void emit_cu_check(int cop_num, uint32_t psx_pc)
{
    flush_dirty_consts();
    reg_cache_invalidate();

    /* Load SR and extract CUn bit (bit 28+n) */
    EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
    emit(MK_R(0, 0, REG_T8, REG_T8, 28 + cop_num, 0x02)); /* SRL */
    if (cop_num < 3)
        emit(MK_I(0x0C, REG_T8, REG_T8, 1));               /* ANDI */

    /* BNE T8, $zero, @skip — CUn enabled, no exception */
    uint32_t *skip = code_ptr;
    emit(MK_I(0x05, REG_T8, REG_ZERO, 0));
    EMIT_NOP();

    /* Cold path: trigger CU exception */
    emit_load_imm32(REG_A0, psx_pc);
    emit_load_imm32(REG_A1, cop_num);
    {
        uint8_t  saved_dirty  = dyn_dirty_mask;
        uint8_t  saved_loaded = dyn_slot_loaded_mask;
        uint32_t saved_smrv   = smrv_known_ram;
        uint32_t saved_align  = align_known_mask;
        emit_call_c((uint32_t)Helper_CU_Exception);
        dyn_dirty_mask       = saved_dirty;
        dyn_slot_loaded_mask = saved_loaded;
        smrv_known_ram       = saved_smrv;
        align_known_mask     = saved_align;
    }

    /* Patch BNE offset */
    *skip = (*skip & 0xFFFF0000) | ((uint32_t)(code_ptr - skip - 1) & 0xFFFF);
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
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s = emit_use_reg(rt, REG_T8);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, 0, s, d, sa, 0x03));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x04: /* SLLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s2, s1, d, 0, 0x04));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x06: /* SRLV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s2, s1, d, 0, 0x06));
            emit_sync_reg(rd, d);
            break;
        }
        case 0x07: /* SRAV */
        {
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rt, REG_T8);
            int s2 = emit_use_reg(rs, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            emit_load_psx_reg(REG_T8, rs);
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            break;
        case 0x12: /* MFLO */
            emit_cpu_field_to_psx_reg(CPU_LO, rd);
            break;
        case 0x13: /* MTLO */
            emit_load_psx_reg(REG_T8, rs);
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            break;
        case 0x18: /* MULT */
            emit_load_psx_reg(REG_T8, rs);
            emit_load_psx_reg(REG_T9, rt);
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULT1(REG_T8, REG_T9);
                EMIT_MFLO1(REG_T8);
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T8);
            }
            else
            {
                emit(MK_R(0, REG_T8, REG_T9, 0, 0, 0x18));
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x12));
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x10));
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T8, rs);
            emit_load_psx_reg(REG_T9, rt);
            if (((*mult_count)++ & 1) == 0)
            {
                EMIT_MULTU1(REG_T8, REG_T9);
                EMIT_MFLO1(REG_T8);
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                EMIT_MFHI1(REG_T8);
            }
            else
            {
                emit(MK_R(0, REG_T8, REG_T9, 0, 0, 0x19));
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x12));
                EMIT_SW(REG_T8, CPU_LO, REG_S0);
                emit(MK_R(0, 0, 0, REG_T8, 0, 0x10));
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        case 0x1A: /* DIV — branchless with MOVZ div-by-zero fixup */
        {
            int s1 = emit_use_reg(rs, REG_T8); /* s1 = EE reg holding rs */
            int s2 = emit_use_reg(rt, REG_T9); /* s2 = EE reg holding rt (divisor) */
            emit(MK_R(0, s1, s2, 0, 0, 0x1A)); /* div s1, s2 */
            /* Fill div latency: compute divz lo = (rs>=0)?-1:1 */
            emit(MK_R(0, 0, s1, REG_AT, 31, 0x03));           /* sra  AT, s1, 31  */
            emit(MK_R(0, 0, REG_AT, REG_AT, 1, 0x00));        /* sll  AT, AT, 1   */
            emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* nor  AT, AT, $0  */
            /* AT = divz lo result, s2 still valid (never clobbered) */
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x12)); /* mflo T8 = quotient */
            EMIT_MOVZ(REG_T8, REG_AT, s2);        /* if s2==0: T8 = divz lo */
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x10)); /* mfhi T8 = remainder */
#ifdef PLATFORM_PSP
            /* PSP/PPSSPP overflow fixup: INT_MIN / -1 gives hi=-1 instead
             * of hi=0.  Any x / -1 has remainder 0, so force it. */
            EMIT_ADDIU(REG_AT, s2, 1);           /* AT = rt+1 (0 if rt==-1) */
            EMIT_MOVZ(REG_T8, REG_ZERO, REG_AT); /* if rt==-1: T8 = 0 */
#endif
            /* divz hi = rs: reload if s1 was clobbered by mflo */
            if (s1 == REG_T8)
            {
                EMIT_LW(REG_AT, CPU_REG(rs), REG_S0);
                EMIT_MOVZ(REG_T8, REG_AT, s2);
            }
            else
            {
                EMIT_MOVZ(REG_T8, s1, s2);
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        }
        case 0x1B: /* DIVU — branchless with MOVZ div-by-zero fixup */
        {
            int s1 = emit_use_reg(rs, REG_T8);    /* s1 = EE reg holding rs */
            int s2 = emit_use_reg(rt, REG_T9);    /* s2 = EE reg holding rt (divisor) */
            emit(MK_R(0, s1, s2, 0, 0, 0x1B));    /* divu s1, s2 */
            EMIT_ADDIU(REG_AT, REG_ZERO, -1);     /* AT = 0xFFFFFFFF (divz lo) */
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x12)); /* mflo T8 = quotient */
            EMIT_MOVZ(REG_T8, REG_AT, s2);        /* if s2==0: T8 = -1 */
            EMIT_SW(REG_T8, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T8, 0, 0x10)); /* mfhi T8 = remainder */
            /* divz hi = rs: reload if s1 was clobbered by mflo */
            if (s1 == REG_T8)
            {
                EMIT_LW(REG_AT, CPU_REG(rs), REG_S0);
                EMIT_MOVZ(REG_T8, REG_AT, s2);
            }
            else
            {
                EMIT_MOVZ(REG_T8, s1, s2);
            }
            EMIT_SW(REG_T8, CPU_HI, REG_S0);
            reg_cache_invalidate();
            break;
        }
        case 0x20: /* ADD — inline overflow detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a + b;
                if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    /* Overflow at compile time — unconditional exception */
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_call_c((uint32_t)Helper_Overflow_Exception);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);

            /* Flush current rd value to cpu.regs[] so the cold path
             * (overflow) can restore it.  Covers both pinned regs and
             * dirty dynamic slots that haven't been written back yet. */
            if (rd != 0 && psx_pinned_reg[rd])
                EMIT_SW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
            else
                dyn_flush_one_slot(rd);

            /* Pre-compute rs^rt; save rs via stack to avoid scratch conflict
             * when rd=0 (emit_dst_reg returns REG_T8 as junk dest). */
            emit(MK_R(0, s1, s2, REG_AT, 0, 0x26)); /* XOR  AT, s1, s2  (rs^rt) */
            EMIT_SW(s1, 76, REG_SP);                /* save rs to stack */

            int d = emit_dst_reg(rd, REG_T8);
            EMIT_ADDU(d, s1, s2); /* d = rs + rt */

            /* Overflow: ~(rs^rt) & (result^rs), bit31=1 => overflow.
             * Load saved rs into scratch (T8 or T9, avoiding d).
             * LW scheduled before NOR to hide load-use delay on R5900. */
            int sr = (d == REG_T8) ? REG_T9 : REG_T8;
            EMIT_LW(sr, 76, REG_SP);                          /* sr = saved rs */
            emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* NOR  AT, AT, $0 */
            emit(MK_R(0, d, sr, sr, 0, 0x26));                /* XOR  sr, d, sr  (result^rs) */
            emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24));       /* AND  AT, AT, sr */

            /* P23: BLTZ AT, @overflow_cold — deferred to block end */
            uint32_t *bltz_patch = code_ptr;
            emit(MK_I(0x01, REG_AT, 0x00, 0)); /* BLTZ AT, +? (placeholder) */
            EMIT_NOP();

            /* Queue cold entry for block-end emission */
            {
                OverflowColdEntry *e = &overflow_cold[overflow_cold_count++];
                e->branch_patch = bltz_patch;
                e->psx_pc = psx_pc;
                e->cycle_offset = emit_cycle_offset;
                e->restore_psx = rd;
            }

            /* @no_overflow: invalidate scratch cache (XOR/NOR clobbered them) */
            reg_cache_invalidate();
            emit_sync_reg(rd, d);
            break;
        }
        case 0x21: /* ADDU */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                mark_vreg_const_lazy(rd, get_vreg_const(rs) + get_vreg_const(rt));
                break;
            }
            /* SMRV: ADDU rd, rs, $0 (MOVE) or ADDU rd, $0, rt propagates RAM-ness */
            int src_ram = (rt == 0 && smrv_is_known_ram(rs)) ||
                          (rs == 0 && smrv_is_known_ram(rt));
            /* Alignment: MOVE propagates alignment */
            int src_aligned = (rt == 0 && align_is_known(rs)) ||
                              (rs == 0 && align_is_known(rt));
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            EMIT_ADDU(d, s1, s2);
            emit_sync_reg(rd, d);
            if (src_ram)
                smrv_set_ram(rd);
            if (src_aligned)
                align_set(rd);
            break;
        }
        case 0x22: /* SUB — inline overflow detection */
        {
            if (is_vreg_const(rs) && is_vreg_const(rt))
            {
                uint32_t a = get_vreg_const(rs), b = get_vreg_const(rt);
                uint32_t res = a - b;
                if (((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
                {
                    emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                    emit_call_c((uint32_t)Helper_Overflow_Exception);
                    emit_abort_check(emit_cycle_offset);
                    break;
                }
                mark_vreg_const_lazy(rd, res);
                break;
            }
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);

            /* Flush pinned/dynamic rd so cold path can restore from cpu.regs[] */
            if (rd != 0 && psx_pinned_reg[rd])
                EMIT_SW(psx_pinned_reg[rd], CPU_REG(rd), REG_S0);
            else
                dyn_flush_one_slot(rd);

            /* Pre-compute rs^rt; save rs via stack */
            emit(MK_R(0, s1, s2, REG_AT, 0, 0x26)); /* XOR  AT, s1, s2 */
            EMIT_SW(s1, 76, REG_SP);                /* save rs to stack */

            int d = emit_dst_reg(rd, REG_T8);
            emit(MK_R(0, s1, s2, d, 0, 0x23)); /* SUBU d, s1, s2 */

            /* Overflow: (rs^rt) & (result^rs), bit31=1 => overflow */
            int sr = (d == REG_T8) ? REG_T9 : REG_T8;
            EMIT_LW(sr, 76, REG_SP);                    /* sr = saved rs */
            EMIT_NOP();                                 /* load delay */
            emit(MK_R(0, d, sr, sr, 0, 0x26));          /* XOR  sr, d, sr */
            emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24)); /* AND  AT, AT, sr */

            /* P23: BLTZ AT, @overflow_cold — deferred to block end */
            uint32_t *bltz_patch = code_ptr;
            emit(MK_I(0x01, REG_AT, 0x00, 0));
            EMIT_NOP();

            {
                OverflowColdEntry *e = &overflow_cold[overflow_cold_count++];
                e->branch_patch = bltz_patch;
                e->psx_pc = psx_pc;
                e->cycle_offset = emit_cycle_offset;
                e->restore_psx = rd;
            }

            reg_cache_invalidate();
            emit_sync_reg(rd, d);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            /* SMRV: OR rd, rs, $0 (MOVE) propagates RAM-ness */
            int src_ram = (rt == 0 && smrv_is_known_ram(rs)) ||
                          (rs == 0 && smrv_is_known_ram(rt));
            mark_vreg_var(rd);
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
            EMIT_OR(d, s1, s2);
            emit_sync_reg(rd, d);
            if (src_ram)
                smrv_set_ram(rd);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
            int s1 = emit_use_reg(rs, REG_T8);
            int s2 = emit_use_reg(rt, REG_T9);
            int d = emit_dst_reg(rd, REG_T8);
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
    case 0x08: /* ADDI — inline overflow detection */
    {
        if (is_vreg_const(rs))
        {
            uint32_t a = get_vreg_const(rs);
            uint32_t b = (uint32_t)imm; /* sign-extended immediate */
            uint32_t res = a + b;
            if (!((a ^ b) & 0x80000000) && ((res ^ a) & 0x80000000))
            {
                emit_imm_to_cpu_field(CPU_CURRENT_PC, psx_pc);
                emit_call_c((uint32_t)Helper_Overflow_Exception);
                emit_abort_check(emit_cycle_offset);
                break;
            }
            mark_vreg_const_lazy(rt, res);
            break;
        }
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s1 = emit_use_reg(rs, REG_T8);

        /* Flush pinned/dynamic rt so cold path can restore from cpu.regs[] */
        if (rt != 0 && psx_pinned_reg[rt])
            EMIT_SW(psx_pinned_reg[rt], CPU_REG(rt), REG_S0);
        else
            dyn_flush_one_slot(rt);

        /* Load sign-extended immediate into T1 for overflow check */
        emit_load_imm32(REG_T9, (uint32_t)imm);

        /* Pre-compute rs^imm; save rs via stack */
        emit(MK_R(0, s1, REG_T9, REG_AT, 0, 0x26)); /* XOR  AT, s1, T1 */
        EMIT_SW(s1, 76, REG_SP);                    /* save rs to stack */

        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ADDU(d, s1, REG_T9); /* d = rs + imm */

        /* Overflow: ~(rs^imm) & (result^rs), bit31=1 => overflow */
        int sr = (d == REG_T8) ? REG_T9 : REG_T8;
        EMIT_LW(sr, 76, REG_SP);                          /* sr = saved rs */
        emit(MK_R(0, REG_AT, REG_ZERO, REG_AT, 0, 0x27)); /* NOR  AT, AT, $0 */
        emit(MK_R(0, d, sr, sr, 0, 0x26));                /* XOR  sr, d, sr */
        emit(MK_R(0, REG_AT, sr, REG_AT, 0, 0x24));       /* AND  AT, AT, sr */

        /* P23: BLTZ AT, @overflow_cold — deferred to block end */
        uint32_t *bltz_patch = code_ptr;
        emit(MK_I(0x01, REG_AT, 0x00, 0));
        EMIT_NOP();

        {
            OverflowColdEntry *e = &overflow_cold[overflow_cold_count++];
            e->branch_patch = bltz_patch;
            e->psx_pc = psx_pc;
            e->cycle_offset = emit_cycle_offset;
            e->restore_psx = rt;
        }

        reg_cache_invalidate();
        emit_sync_reg(rt, d);
        /* SMRV: ADDI from RAM base stays in RAM (if no overflow) */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ADDI preserves alignment when offset is word-aligned */
        if (rs_aligned && (imm & 3) == 0)
            align_set(rt);
        break;
    }
    case 0x09: /* ADDIU */
    {
        if (is_vreg_const(rs))
        {
            mark_vreg_const_lazy(rt, get_vreg_const(rs) + imm);
            break;
        }
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ADDIU(d, s, imm);
        emit_sync_reg(rt, d);
        /* SMRV: ADDIU from RAM base with small offset stays in RAM */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ADDIU preserves alignment when offset is word-aligned */
        if (rs_aligned && (imm & 3) == 0)
            align_set(rt);
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
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
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
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
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
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
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
        int rs_ram = smrv_is_known_ram(rs);
        int rs_aligned = align_is_known(rs);
        mark_vreg_var(rt);
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
        EMIT_ORI(d, s, uimm);
        emit_sync_reg(rt, d);
        /* SMRV: ORI from RAM base (e.g., LUI+ORI pattern) stays in RAM */
        if (rs_ram)
            smrv_set_ram(rt);
        /* Alignment: ORI preserves alignment when low bits don't break it */
        if (rs_aligned && (uimm & 3) == 0)
            align_set(rt);
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
        int s = emit_use_reg(rs, REG_T8);
        int d = emit_dst_reg(rt, REG_T8);
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
            emit_load_psx_reg(REG_T8, rt);
            if (rd == PSX_COP0_SR)
            {
                EMIT_MOVE(REG_A0, REG_T8);
                emit_call_c((uint32_t)debug_mtc0_sr);
            }
            else
            {
                EMIT_SW(REG_T8, CPU_COP0(rd), REG_S0);
            }
        }
        else if (rs == 0x10 && func == 0x10)
        {
            /* RFE */
            reg_cache_invalidate();
            EMIT_LW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            EMIT_MOVE(REG_T9, REG_T8);
            emit(MK_R(0, 0, REG_T9, REG_T9, 2, 0x02));
            emit(MK_I(0x0C, REG_T9, REG_T9, 0x0F));
            emit(MK_R(0, 0, REG_T8, REG_T8, 4, 0x02));
            emit(MK_R(0, 0, REG_T8, REG_T8, 4, 0x00));
            EMIT_OR(REG_T8, REG_T8, REG_T9);
            EMIT_SW(REG_T8, CPU_COP0(PSX_COP0_SR), REG_S0);
            /* Update irq_pending_fast = irq_pending & (new_SR & 1).
             * T8 still holds the new SR value after the SW. */
            EMIT_LW(REG_AT, CPU_IRQ_PENDING, REG_S0);
            EMIT_AND(REG_AT, REG_AT, REG_T8);
            EMIT_ANDI(REG_AT, REG_AT, 1);
            EMIT_SW(REG_AT, CPU_IRQ_PENDING_FAST, REG_S0);
        }
        break;
    }

    /* COP1 */
    case 0x11:
    {
        emit_cu_check(1, psx_pc);
        break;
    }

    /* COP2 (GTE) */
    case 0x12:
    {
        if (!block_cu2_hoisted)
            emit_cu_check(2, psx_pc);

        emit_gte_instruction(opcode, psx_pc);
        break;
    }
    /* COP3 */
    case 0x13:
    {
        emit_cu_check(3, psx_pc);
        break;
    }

    /* Load instructions */
    case 0x20:
        mark_vreg_var(rt);
        emit_memory_read(1, rt, rs, imm, 1);
        break; /* LB */
    case 0x21:
        mark_vreg_var(rt);
        emit_memory_read(2, rt, rs, imm, 1);
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
        emit_cu_check(0, psx_pc);
        break;
    }

    /* LWC2 */
    case 0x32:
    {
        if (!block_cu2_hoisted)
            emit_cu_check(2, psx_pc);

        /* Memory read via LUT fast path (result in V0), then GTE write */
        {
            int saved_defer = dynarec_load_defer;
            dynarec_load_defer = 1;
            emit_memory_read(4, 0, rs, imm, 0); /* V0 = word from [rs+imm] */
            dynarec_load_defer = saved_defer;
        }
        /* Inline GTE write for simple data registers (same as MTC2 P3) */
        {
            int rt5 = rt & 0x1F;
            switch (rt5)
            {
            case 15:
            case 28:
            case 30:
                /* Complex: SXY FIFO / IRGB / LZCS — keep C call (lite) */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rt5);
                EMIT_MOVE(6, REG_V0);
                emit_flush_partial_cycles();
                emit_call_c_lite((uint32_t)GTE_WriteData);
                break;
            case 29:
            case 31:
                /* Read-only: no-op (just discard loaded value) */
                break;
            case 1:
            case 3:
            case 5:
            case 8:
            case 9:
            case 10:
            case 11:
                /* Sign-extend 16 + store */
                emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x00)); /* sll v0, 16 */
                emit(MK_R(0, 0, REG_V0, REG_V0, 16, 0x03)); /* sra v0, 16 */
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            case 7:
            case 16:
            case 17:
            case 18:
            case 19:
                /* Zero-extend 16 + store */
                emit(MK_I(0x0C, REG_V0, REG_V0, 0xFFFF));
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            default:
                /* Simple store */
                EMIT_SW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            }
        }
    }
    break;

    /* LWC3 */
    case 0x33:
    {
        emit_cu_check(3, psx_pc);
        break;
    }

    /* SWC0 */
    case 0x38:
    {
        emit_cu_check(0, psx_pc);
        break;
    }

    /* SWC2 */
    case 0x3A:
    {
        if (!block_cu2_hoisted)
            emit_cu_check(2, psx_pc);

        /* Inline GTE read for simple data registers (same as MFC2 P3) */
        {
            int rt5 = rt & 0x1F;
            switch (rt5)
            {
            case 28:
            case 29:
                /* Complex: IRGB/ORGB clamp — keep C call (lite) */
                EMIT_MOVE(REG_A0, REG_S0);
                emit_load_imm32(REG_A1, rt5);
                emit_call_c_lite((uint32_t)GTE_ReadData);
                break;
            case 15:
                /* SXY2 always returns D(14) */
                EMIT_LW(REG_V0, CPU_CP2_DATA(14), REG_S0);
                break;
            default:
                /* Simple read */
                EMIT_LW(REG_V0, CPU_CP2_DATA(rt5), REG_S0);
                break;
            }
        }

        /* Memory write: data in V0, addr from base+offset.
         * Use cold_queue for slow path (P7: deferred to block end). */
        EMIT_MOVE(REG_T9, REG_V0); /* T9 = data (cold path protocol) */
        emit_load_psx_reg(REG_T8, rs);
        EMIT_ADDIU(REG_T8, REG_T8, imm); /* T8 = effective addr */

        /* Flush lazy consts before conditional fast/slow split */
        flush_dirty_consts();

        /* P22: Inline ISC skip for SWC2 when SMRV+aligned (same as SW) */
        if ((block_isc_cached || block_isc_skip) && smrv_is_known_ram(rs) && align_is_known(rs) && (imm % 4 == 0))
        {
            uint32_t *isc_skip_ptr = NULL;
            if (!block_isc_skip)
            {
                EMIT_LW(REG_AT, 80, REG_SP); /* at = cached ISC     */
                isc_skip_ptr = code_ptr;
                emit(MK_I(0x05, REG_AT, REG_ZERO, 0));          /* bne at,zero,@skip   */
            }
            emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* and at,t8,s3 */
            EMIT_ADDU(REG_AT, REG_AT, REG_S1);              /* addu at, at, s1     */
            EMIT_SW(REG_T9, 0, REG_AT);                     /* store               */
            if (isc_skip_ptr)
            {
                int32_t skip_off = (int32_t)(code_ptr - isc_skip_ptr - 1);
                *isc_skip_ptr = (*isc_skip_ptr & 0xFFFF0000) | ((uint32_t)skip_off & 0xFFFF);
            }
        }
        else
        {
            /* Cache Isolation check */
            uint32_t *isc_swc2 = NULL;
            if (block_isc_skip)
            {
                /* ISC compile-time 0 — no check needed */
            }
            else if (block_isc_cached)
            {
                EMIT_LW(REG_AT, 80, REG_SP);
                isc_swc2 = code_ptr;
                emit(MK_I(0x05, REG_AT, REG_ZERO, 0)); /* bne at,zero,@cold */
                EMIT_NOP();
            }
            else
            {
                EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);
                emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02));
                emit(MK_I(0x0C, REG_A0, REG_A0, 1));
                isc_swc2 = code_ptr;
                emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne → cold */
                EMIT_NOP();
            }

            /* Alignment check (word-aligned required) — P6: elide if base known aligned */
            uint32_t *align_swc2 = NULL;
            if (align_is_known(rs) && (imm % 4 == 0))
            {
                /* Alignment guaranteed — emit only the phys mask */
                emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* and at, t8, s3 */
            }
            else
            {
                emit(MK_I(0x0C, REG_T8, REG_AT, 3)); /* andi at, t8, 3 */
                align_swc2 = code_ptr;
                emit(MK_I(0x05, REG_AT, REG_ZERO, 0));          /* bne at, zero, @cold */
                emit(MK_R(0, REG_T8, REG_S3, REG_AT, 0, 0x24)); /* [delay] and at, t8, s3 */
            }

            /* Range check: skip if SMRV proves base is RAM */
            uint32_t *range_swc2 = NULL;
            if (!smrv_is_known_ram(rs))
            {
                emit(MK_R(0, 0, REG_AT, REG_A0, 21, 0x02)); /* srl a0, at, 21 */
                range_swc2 = code_ptr;
                emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne a0, zero, @cold */
            }
            EMIT_ADDU(REG_AT, REG_AT, REG_S1); /* [delay/inline] host addr */

            /* Fast path: direct store */
            EMIT_SW(REG_T9, 0, REG_AT);

            /* Defer slow path to end of block via cold_queue (P7) */
            {
                uint32_t *branches[4];
                int nb = 0;
                if (isc_swc2)
                    branches[nb++] = isc_swc2;
                if (align_swc2)
                    branches[nb++] = align_swc2;
                if (range_swc2)
                    branches[nb++] = range_swc2;
                cold_slow_push(branches, nb, code_ptr,
                               (uint32_t)WriteWord, psx_pc,
                               (int16_t)emit_cycle_offset, 4, 1, 1,
                               dyn_dirty_mask);
            }
        }
        reg_cache_invalidate();
    }
    break;

    /* SWC3 */
    case 0x3B:
    {
        emit_cu_check(3, psx_pc);
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
