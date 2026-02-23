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
void emit_memory_read(int size, int rt_psx, int rs_psx, int16_t offset, int is_signed)
{
    uint32_t const_addr = 0;
    int is_const = 0;

    if (is_vreg_const(rs_psx))
    {
        const_addr = get_vreg_const(rs_psx) + offset;
        is_const = 1;
    }

    if (is_const)
    {
        uint32_t phys = const_addr & 0x1FFFFFFF;
        /* Aligned RAM access? */
        if ((phys < PSX_RAM_SIZE) && (phys % size == 0))
        {
            /* Use T1 as scratch for large address */
            emit_load_imm32(REG_T1, (uint32_t)psx_ram + phys);
            if (size == 4)
                EMIT_LW(REG_V0, 0, REG_T1);
            else if (size == 2)
            {
                if (is_signed)
                    emit(MK_I(0x21, REG_T1, REG_V0, 0)); /* LH */
                else
                    EMIT_LHU(REG_V0, 0, REG_T1);
            }
            else
            {
                if (is_signed)
                    emit(MK_I(0x20, REG_T1, REG_V0, 0)); /* LB */
                else
                    EMIT_LBU(REG_V0, 0, REG_T1);
            }

            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            return;
        }
        /* Scratchpad access? */
        if (phys >= 0x1F800000 && phys < 0x1F800400)
        {
            uint32_t sp_off = phys & 0x3FF;
            if (sp_off % size == 0)
            {
                emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf + sp_off);
                if (size == 4)
                    EMIT_LW(REG_V0, 0, REG_T1);
                else if (size == 2)
                {
                    if (is_signed)
                        emit(MK_I(0x21, REG_T1, REG_V0, 0)); /* LH */
                    else
                        EMIT_LHU(REG_V0, 0, REG_T1);
                }
                else
                {
                    if (is_signed)
                        emit(MK_I(0x20, REG_T1, REG_V0, 0)); /* LB */
                    else
                        EMIT_LBU(REG_V0, 0, REG_T1);
                }

                if (!dynarec_load_defer)
                    emit_store_psx_reg(rt_psx, REG_V0);
                return;
            }
        }
    }

    /* Fallback to generic emitter if address is not constant or not in RAM/SP */
    /* Compute effective address into REG_T0 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);

    /*
     * LUT-based fast path (64KB virtual pages):
     *   [alignment check if size > 1]
     *   srl    t1, t0, 16         # page index
     *   sll    t1, t1, 2          # byte offset into LUT
     *   addu   t1, t1, s3         # &lut[page]
     *   lw     t1, 0(t1)          # host page base (or NULL)
     *   andi   t2, t0, 0xFFFF     # offset within 64KB page
     *   beq    t1, zero, @slow
     *   nop
     *   addu   t1, t1, t2         # host address
     *   lw/lhu/lbu v0, 0(t1)
     *   b      @done
     *   nop
     * @slow: <call C helper>
     * @done:
     */

    uint32_t *align_branch = NULL;
    if (size > 1)
    {
        /* Alignment check */
        emit(MK_I(0x0C, REG_T0, REG_T1, size - 1)); /* andi  t1, t0, size-1 */
        align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0)); /* bne   t1, zero, @slow */
        EMIT_NOP();
    }

    /* LUT lookup (64KB pages, virtual address based, S3 = mem_lut) */
    emit(MK_R(0, 0, REG_T0, REG_T1, 16, 0x02)); /* srl  t1, t0, 16       (page index)   */
    emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x00));  /* sll  t1, t1, 2        (byte offset)  */
    EMIT_ADDU(REG_T1, REG_T1, REG_S3);          /* addu t1, t1, s3       (&lut[page])   */
    EMIT_LW(REG_T1, 0, REG_T1);                 /* lw   t1, 0(t1)        (host base)    */
    emit(MK_I(0x0C, REG_T0, REG_T2, 0xFFFF));   /* andi t2, t0, 0xFFFF   (page offset)  */
    uint32_t *lut_branch = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0)); /* beq  t1, zero, @slow                 */
    EMIT_NOP();

    /* Fast path: direct access via LUT */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2); /* addu t1, t1, t2       (host addr)    */
    if (size == 4)
        EMIT_LW(REG_V0, 0, REG_T1);
    else if (size == 2)
        EMIT_LHU(REG_V0, 0, REG_T1);
    else
        EMIT_LBU(REG_V0, 0, REG_T1);

    uint32_t *fast_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
    EMIT_NOP();

    /* Slow path: store current_pc (needed by AdEL exception handler) */
    if (align_branch)
    {
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
    }
    int32_t soff2 = (int32_t)(code_ptr - lut_branch - 1);
    *lut_branch = (*lut_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);

    emit_load_imm32(REG_T2, emit_current_psx_pc);
    EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);
    EMIT_MOVE(REG_A0, REG_T0);

    uint32_t func_addr;
    if (size == 4)
        func_addr = (uint32_t)ReadWord;
    else if (size == 2)
        func_addr = (uint32_t)ReadHalf;
    else
        func_addr = (uint32_t)ReadByte;
    emit_call_c(func_addr);

    if (size >= 2)
        emit_abort_check(); /* AdEL on misaligned addr */

    /* @done */
    int32_t doff = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);

    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);
}

void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset)
{
    emit_memory_read(size, rt_psx, rs_psx, offset, 1);
    /* Sign extend for LB/LH (fallback for non-const path only) */
    if (rt_psx == 0)
        return;

    uint32_t cp = 0;
    if (is_vreg_const(rs_psx))
    {
        /* If it was constant, emit_memory_read already handled sign extension and returned */
        return;
    }

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
    uint32_t const_addr = 0;
    int is_const = 0;

    if (is_vreg_const(rs_psx))
    {
        const_addr = get_vreg_const(rs_psx) + offset;
        is_const = 1;
    }

    if (is_const)
    {
        uint32_t phys = const_addr & 0x1FFFFFFF;
        /* Aligned RAM access? */
        if ((phys < PSX_RAM_SIZE) && (phys % size == 0))
        {
            emit_load_psx_reg(REG_T2, rt_psx);
            emit_load_imm32(REG_T1, (uint32_t)psx_ram + phys);
            if (size == 4)
                EMIT_SW(REG_T2, 0, REG_T1);
            else if (size == 2)
                EMIT_SH(REG_T2, 0, REG_T1);
            else
                EMIT_SB(REG_T2, 0, REG_T1);
            return;
        }
        /* Scratchpad access? */
        if (phys >= 0x1F800000 && phys < 0x1F800400)
        {
            uint32_t sp_off = phys & 0x3FF;
            if (sp_off % size == 0)
            {
                emit_load_psx_reg(REG_T2, rt_psx);
                emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf + sp_off);
                if (size == 4)
                    EMIT_SW(REG_T2, 0, REG_T1);
                else if (size == 2)
                    EMIT_SH(REG_T2, 0, REG_T1);
                else
                    EMIT_SB(REG_T2, 0, REG_T1);
                return;
            }
        }
    }

    /* Compute effective address into REG_T0, data into REG_T2 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);
    emit_load_psx_reg(REG_T2, rt_psx); /* data value */

    /* Cache Isolation check: if SR.IsC (bit 16) is set, writes to KUSEG/KSEG0
     * must be ignored (it's a cache invalidation, not a real RAM write).
     * Read SR, shift bit 16 to bit 0, test it; if set go to slow-path
     * (WriteWord handles kseg1 exception internally). */
    EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);      /* a0 = SR */
    emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
    emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
    uint32_t *isc_branch = code_ptr;
    emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne  a0, zero, @slow (IsC set) */
    EMIT_NOP();

    uint32_t *align_branch = NULL;
    if (size > 1)
    {
        /* Alignment check */
        emit(MK_I(0x0C, REG_T0, REG_T1, size - 1)); /* andi  t1, t0, size-1 */
        align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0)); /* bne   t1, zero, @slow */
        EMIT_NOP();
    }

    /* LUT lookup (64KB virtual pages, S3 = mem_lut base) */
    emit(MK_R(0, 0, REG_T0, REG_T1, 16, 0x02)); /* srl  t1, t0, 16       (page index)   */
    emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x00));  /* sll  t1, t1, 2        (byte offset)  */
    EMIT_ADDU(REG_T1, REG_T1, REG_S3);          /* addu t1, t1, s3       (&lut[page])   */
    EMIT_LW(REG_T1, 0, REG_T1);                 /* lw   t1, 0(t1)        (host base)    */
    emit(MK_I(0x0C, REG_T0, REG_A0, 0xFFFF));   /* andi a0, t0, 0xFFFF   (page offset)  */
    uint32_t *range_branch = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0)); /* beq  t1, zero, @slow                 */
    EMIT_NOP();

    /* Fast path: direct store via LUT */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0); /* addu t1, t1, a0       (host addr)    */
    if (size == 4)
        EMIT_SW(REG_T2, 0, REG_T1);
    else if (size == 2)
        EMIT_SH(REG_T2, 0, REG_T1);
    else
        EMIT_SB(REG_T2, 0, REG_T1);

    uint32_t *fast_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b    @done */
    EMIT_NOP();

    /* Slow path: store current_pc (needed by AdES exception handler) */
    int32_t soff0 = (int32_t)(code_ptr - isc_branch - 1);
    *isc_branch = (*isc_branch & 0xFFFF0000) | ((uint32_t)soff0 & 0xFFFF);
    if (align_branch)
    {
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
    }
    int32_t soff2 = (int32_t)(code_ptr - range_branch - 1);
    *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);

    emit_load_imm32(REG_A1, emit_current_psx_pc); /* reuse a1 temp */
    EMIT_SW(REG_A1, CPU_CURRENT_PC, REG_S0);
    EMIT_MOVE(REG_A0, REG_T0); /* a0 = addr */
    EMIT_MOVE(REG_A1, REG_T2); /* a1 = data */

    uint32_t func_addr;
    if (size == 4)
        func_addr = (uint32_t)WriteWord;
    else if (size == 2)
        func_addr = (uint32_t)WriteHalf;
    else
        func_addr = (uint32_t)WriteByte;
    emit_call_c(func_addr);

    if (size >= 2)
        emit_abort_check(); /* AdES on misaligned addr */

    /* @done */
    int32_t doff = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
}
