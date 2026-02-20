/*
 * dynarec_memory.c - Memory access emitters (LW/LH/LB/SW/SH/SB)
 *
 * Generates native code for PSX memory reads and writes with inline
 * fast-paths for aligned RAM access and slow-path C helper calls
 * for IO/BIOS/misaligned access.
 */
#include "dynarec.h"

/*
 * emit_memory_read: Emit native code for LW/LH/LHU/LB/LBU.
 *
 * For LW (size==4): inline fast-path for aligned RAM access:
 *   andi   t1, t0, 3          # alignment check
 *   bne    t1, zero, @slow
 *   nop
 *   lui    t1, 0x1FFF
 *   ori    t1, t1, 0xFFFF     # t1 = 0x1FFFFFFF (mask)
 *   and    t1, t0, t1         # t1 = phys_addr
 *   srl    t2, t1, 12
 *   sltiu  t2, t2, 0x200      # t2 = (phys < 2MB)
 *   beq    t2, zero, @slow
 *   nop
 *   addu   t1, t1, s1         # t1 = psx_ram + phys
 *   lw     v0, 0(t1)
 *   b      @done
 *   nop
 * @slow:
 *   move   a0, t0
 *   jal    ReadWord
 *   nop
 * @done:
 */
void emit_memory_read(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* Compute effective address into REG_T0 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);

    if (size == 4)
    {
        /* Alignment check */
        emit(MK_I(0x0C, REG_T0, REG_T1, 3)); /* andi  t1, t0, 3 */
        uint32_t *align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0)); /* bne   t1, zero, @slow */
        EMIT_NOP();

        /* Physical address mask */
        EMIT_LUI(REG_T1, 0x1FFF);
        EMIT_ORI(REG_T1, REG_T1, 0xFFFF);               /* t1 = 0x1FFFFFFF */
        emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and t1, t0, t1   t1=phys */

        /* RAM range check: phys < 2MB  (use srl+sltiu, no fancy delay slot tricks) */
        emit(MK_R(0, 0, REG_T1, REG_T2, 12, 0x02)); /* srl  t2, t1, 12 */
        emit(MK_I(0x0B, REG_T2, REG_T2, 0x0200));   /* sltiu t2, t2, 0x200 (1=RAM) */
        uint32_t *range_branch = code_ptr;
        emit(MK_I(0x04, REG_T2, REG_ZERO, 0)); /* beq  t2, zero, @slow */
        EMIT_NOP();                            /* delay slot: NOP (safe) */

        /* Fast path: t1 = phys (already masked), add psx_ram base here */
        EMIT_ADDU(REG_T1, REG_T1, REG_S1); /* t1 = psx_ram + phys */
        EMIT_LW(REG_V0, 0, REG_T1);        /* v0 = *(psx_ram+phys) */
        uint32_t *fast_done = code_ptr;
        emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
        EMIT_NOP();

        /* Slow path: store current_pc (needed by AdEL exception handler) */
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
        int32_t soff2 = (int32_t)(code_ptr - range_branch - 1);
        *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);
        emit_load_imm32(REG_T2, emit_current_psx_pc);
        EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);
        EMIT_MOVE(REG_A0, REG_T0);
        emit_call_c((uint32_t)ReadWord);
        emit_abort_check(); /* AdEL on misaligned addr */
        /* @done */
        int32_t doff = (int32_t)(code_ptr - fast_done - 1);
        *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);

        if (!dynarec_load_defer)
            emit_store_psx_reg(rt_psx, REG_V0);
        return;
    }

    /* Non-LW: slow path only */
    if (size >= 2)
    {
        /* Store current_pc for exception handler (ReadHalf can throw AdEL) */
        emit_load_imm32(REG_T2, emit_current_psx_pc);
        EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);
    }
    EMIT_MOVE(REG_A0, REG_T0);
    uint32_t func_addr;
    if (size == 2)
        func_addr = (uint32_t)ReadHalf;
    else
        func_addr = (uint32_t)ReadByte;
    emit_call_c((uint32_t)func_addr);
    if (size >= 2)
        emit_abort_check(); /* AdEL on misaligned halfword */
    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);
}

void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset)
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
            emit_load_psx_reg(REG_T0, rt_psx);
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x00)); /* SLL $t0, $t0, 24 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x03)); /* SRA $t0, $t0, 24 */
            emit_store_psx_reg(rt_psx, REG_T0);
        }
        else if (size == 2)
        {
            emit_load_psx_reg(REG_T0, rt_psx);
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x00)); /* SLL $t0, $t0, 16 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x03)); /* SRA $t0, $t0, 16 */
            emit_store_psx_reg(rt_psx, REG_T0);
        }
    }
}

void emit_memory_write(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* Compute effective address into REG_T0, data into REG_T2 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);
    emit_load_psx_reg(REG_T2, rt_psx); /* data value */

    if (size == 4)
    {
        /* Cache Isolation check: if SR.IsC (bit 16) is set, writes to KUSEG/KSEG0
         * must be ignored (it's a cache invalidation, not a real RAM write).
         * Read SR, shift bit 16 to bit 0, test it; if set go to slow-path. */
        EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);      /* a0 = SR */
        emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
        emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
        uint32_t *isc_branch = code_ptr;
        emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne  a0, zero, @slow (IsC set) */
        EMIT_NOP();

        /* Alignment check */
        emit(MK_I(0x0C, REG_T0, REG_T1, 3)); /* andi  t1, t0, 3 */
        uint32_t *align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0)); /* bne   t1, zero, @slow */
        EMIT_NOP();

        /* Physical address mask */
        EMIT_LUI(REG_T1, 0x1FFF);
        EMIT_ORI(REG_T1, REG_T1, 0xFFFF);               /* t1 = 0x1FFFFFFF */
        emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and t1, t0, t1 */

        /* RAM range check */
        emit(MK_R(0, 0, REG_T1, REG_A0, 12, 0x02)); /* srl  a0, t1, 12 (use a0 as tmp) */
        emit(MK_I(0x0B, REG_A0, REG_A0, 0x0200));   /* sltiu a0, a0, 0x200 */
        uint32_t *range_branch = code_ptr;
        emit(MK_I(0x04, REG_A0, REG_ZERO, 0)); /* beq  a0, zero, @slow */
        EMIT_ADDU(REG_T1, REG_T1, REG_S1);     /* (delay slot) t1 = psx_ram+phys */

        /* Fast path: store to RAM */
        EMIT_SW(REG_T2, 0, REG_T1); /* sw   t2, 0(t1) */
        uint32_t *fast_done = code_ptr;
        emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b    @done */
        EMIT_NOP();

        /* Slow path: store current_pc (needed by AdES exception handler) */
        int32_t soff0 = (int32_t)(code_ptr - isc_branch - 1);
        *isc_branch = (*isc_branch & 0xFFFF0000) | ((uint32_t)soff0 & 0xFFFF);
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
        int32_t soff2 = (int32_t)(code_ptr - range_branch - 1);
        *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);
        emit_load_imm32(REG_A1, emit_current_psx_pc); /* reuse a1 temp */
        EMIT_SW(REG_A1, CPU_CURRENT_PC, REG_S0);
        EMIT_MOVE(REG_A0, REG_T0); /* a0 = addr */
        EMIT_MOVE(REG_A1, REG_T2); /* a1 = data */
        emit_call_c((uint32_t)WriteWord);
        emit_abort_check(); /* AdES on misaligned addr */
        /* @done */
        int32_t doff = (int32_t)(code_ptr - fast_done - 1);
        *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
        return;
    }

    /* Non-SW: slow path only */
    if (size >= 2)
    {
        /* Store current_pc for exception handler (WriteHalf can throw AdES) */
        emit_load_imm32(REG_A1, emit_current_psx_pc);
        EMIT_SW(REG_A1, CPU_CURRENT_PC, REG_S0);
    }
    EMIT_MOVE(REG_A0, REG_T0);
    EMIT_MOVE(REG_A1, REG_T2);
    uint32_t func_addr;
    if (size == 2)
        func_addr = (uint32_t)WriteHalf;
    else
        func_addr = (uint32_t)WriteByte;
    emit_call_c((uint32_t)func_addr);
    if (size >= 2)
        emit_abort_check(); /* AdES on misaligned halfword */
}
