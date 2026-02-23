/*
 * dynarec_memory.c - Memory access emitters (LW/LH/LB/SW/SH/SB)
 *
 * Generates native code for PSX memory reads and writes with inline
 * fast-paths for aligned RAM access and slow-path C helper calls
 * for IO/BIOS/misaligned access.
 */
#include "dynarec.h"

/* Only need gpu_state.h for GPU_ReadStatus inline — silence LOG_TAG redef */
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"

/* GPU busy-until timestamp — when non-zero, GPU_ReadStatus needs to
 * fast-forward global_cycles.  JIT inline checks this for zero to
 * skip the expensive C call on the common "GPU idle" path. */
extern uint64_t gpu_busy_until;

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

        /*
         * Const-address I/O whitelist (à la pcsx-redux pointerRead):
         * For known "passive" I/O registers whose values live in the cpu
         * struct, emit a direct load from S0+offset instead of a C call.
         */
        if (phys == 0x1F801070 && size == 4) /* I_STAT */
        {
            EMIT_LW(REG_V0, CPU_I_STAT, REG_S0);
            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            return;
        }
        if (phys == 0x1F801074 && size == 4) /* I_MASK */
        {
            EMIT_LW(REG_V0, CPU_I_MASK, REG_S0);
            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            return;
        }

        /*
         * GPU_ReadStatus fast-path (0x1F801814):
         * The hot path (GPU idle) is just  return gpu_stat | 0x14002000.
         * Only fall to the full C call when GPU is busy (gpu_busy_until != 0),
         * which triggers scheduler fast-forward logic.
         */
        if (phys == 0x1F801814 && size == 4)
        {
            /* Check gpu_busy_until == 0 (both halves) */
            emit_load_imm32(REG_T2, (uint32_t)&gpu_busy_until);
            EMIT_LW(REG_T1, 0, REG_T2);         /* t1 = low 32 bits  */
            EMIT_LW(REG_T2, 4, REG_T2);         /* t2 = high 32 bits */
            EMIT_OR(REG_T1, REG_T1, REG_T2);    /* t1 = low | high   */
            uint32_t *gbu_slow = code_ptr;
            EMIT_BNE(REG_T1, REG_ZERO, 0);      /* bne t1, $0, @slow */
            EMIT_NOP();

            /* Fast path: v0 = gpu_stat | 0x14002000 */
            emit_load_imm32(REG_T2, (uint32_t)&gpu_stat);
            EMIT_LW(REG_V0, 0, REG_T2);
            EMIT_LUI(REG_T2, 0x1400);
            EMIT_ORI(REG_T2, REG_T2, 0x2000);
            EMIT_OR(REG_V0, REG_V0, REG_T2);

            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            uint32_t *fast_skip = code_ptr;
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @after */
            EMIT_NOP();

            /* Slow path: full C call with trampoline */
            int32_t soff_gbu = (int32_t)(code_ptr - gbu_slow - 1);
            *gbu_slow = (*gbu_slow & 0xFFFF0000) | ((uint32_t)soff_gbu & 0xFFFF);

            emit_call_c((uint32_t)GPU_ReadStatus);

            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);

            /* Patch fast-path branch */
            int32_t soff_fast = (int32_t)(code_ptr - fast_skip - 1);
            *fast_skip = (*fast_skip & 0xFFFF0000) | ((uint32_t)soff_fast & 0xFFFF);
            return;
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
    {
        if (is_signed)
            EMIT_LH(REG_V0, 0, REG_T1);   /* native signed halfword load */
        else
            EMIT_LHU(REG_V0, 0, REG_T1);
    }
    else
    {
        if (is_signed)
            EMIT_LB(REG_V0, 0, REG_T1);   /* native signed byte load */
        else
            EMIT_LBU(REG_V0, 0, REG_T1);
    }

    uint32_t *fast_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
    EMIT_NOP();

    /*
     * Scratchpad inline check (LUT miss path):
     * When mem_lut[page] is NULL, check if this is a scratchpad access
     * (physical 0x1F800000-0x1F8003FF) before falling through to the
     * expensive C helper call.  Scratchpad shares the 0x1F80 64KB page
     * with I/O registers, so the LUT cannot map it directly.
     */
    /* @sp_check: lut_branch lands here */
    int32_t soff_lut = (int32_t)(code_ptr - lut_branch - 1);
    *lut_branch = (*lut_branch & 0xFFFF0000) | ((uint32_t)soff_lut & 0xFFFF);

    /* phys = vaddr & 0x1FFFFFFF */
    emit(MK_I(0x0F, 0, REG_T1, 0x1FFF));         /* lui  t1, 0x1FFF          */
    emit(MK_I(0x0D, REG_T1, REG_T1, 0xFFFF));    /* ori  t1, t1, 0xFFFF      */
    emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and  t1, t0, t1  (phys) */
    emit(MK_I(0x0F, 0, REG_T2, 0xE080));         /* lui  t2, 0xE080 (-0x1F800000) */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2);           /* addu t1, t1, t2 (phys - 0x1F800000) */
    emit(MK_I(0x0B, REG_T1, REG_T1, 0x400));     /* sltiu t1, t1, 0x400      */
    uint32_t *sp_miss = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));       /* beq  t1, zero, @slow     */
    EMIT_NOP();

    /* Scratchpad fast path: load from scratchpad_buf + (addr & 0x3FF) */
    emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf);
    emit(MK_I(0x0C, REG_T0, REG_T2, 0x3FF));    /* andi t2, t0, 0x3FF       */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2);
    if (size == 4)
        EMIT_LW(REG_V0, 0, REG_T1);
    else if (size == 2)
    {
        if (is_signed)
            EMIT_LH(REG_V0, 0, REG_T1);
        else
            EMIT_LHU(REG_V0, 0, REG_T1);
    }
    else
    {
        if (is_signed)
            EMIT_LB(REG_V0, 0, REG_T1);
        else
            EMIT_LBU(REG_V0, 0, REG_T1);
    }
    uint32_t *sp_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0));     /* b @done */
    EMIT_NOP();

    /* Slow path: C helper call (I/O, unmapped, etc.) */
    /* @slow: alignment branch and sp_miss land here */
    if (align_branch)
    {
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
    }
    int32_t soff_sp = (int32_t)(code_ptr - sp_miss - 1);
    *sp_miss = (*sp_miss & 0xFFFF0000) | ((uint32_t)soff_sp & 0xFFFF);

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

    /* Sign-extend slow-path result (C helpers return unsigned) */
    if (is_signed && size < 4)
    {
        int shift = (size == 1) ? 24 : 16;
        emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x00)); /* SLL */
        emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x03)); /* SRA */
    }

    /* @done: patch all forward branches */
    int32_t doff = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
    int32_t doff_sp = (int32_t)(code_ptr - sp_done - 1);
    *sp_done = (*sp_done & 0xFFFF0000) | ((uint32_t)doff_sp & 0xFFFF);

    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);
}

void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* emit_memory_read with is_signed=1 handles signed loads natively on the
     * LUT fast path (lb/lh) and adds SLL/SRA sign extension on the slow path.
     * Both const-address and LUT paths are fully handled internally. */
    emit_memory_read(size, rt_psx, rs_psx, offset, 1);
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

        /*
         * Const-address I/O write whitelist:
         * I_MASK (0x1F801074): cpu.i_mask = data & 0xFFFF07FF
         * Note: I_STAT (0x1F801070) has SIO_CheckIRQ side effect so it
         * stays on the C path.
         */
        if (phys == 0x1F801074 && size == 4) /* I_MASK */
        {
            emit_load_psx_reg(REG_T2, rt_psx);
            emit_load_imm32(REG_T1, 0xFFFF07FF);
            emit(MK_R(0, REG_T2, REG_T1, REG_T2, 0, 0x24)); /* and t2, t2, t1 */
            EMIT_SW(REG_T2, CPU_I_MASK, REG_S0);
            return;
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

    /*
     * Scratchpad inline check (LUT miss on write path):
     * Same logic as read path — check if physical address is in
     * scratchpad range before falling through to expensive C helper.
     * Registers: t0 = vaddr, t2 = data, t1 = 0 (LUT null), a0 = page offset
     */
    /* @sp_check: range_branch lands here */
    int32_t soff_range = (int32_t)(code_ptr - range_branch - 1);
    *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff_range & 0xFFFF);

    /* phys = vaddr & 0x1FFFFFFF */
    emit(MK_I(0x0F, 0, REG_T1, 0x1FFF));         /* lui  t1, 0x1FFF          */
    emit(MK_I(0x0D, REG_T1, REG_T1, 0xFFFF));    /* ori  t1, t1, 0xFFFF      */
    emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and  t1, t0, t1  (phys) */
    emit(MK_I(0x0F, 0, REG_A0, 0xE080));         /* lui  a0, 0xE080 (-0x1F800000) */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0);           /* addu t1, t1, a0 (phys - 0x1F800000) */
    emit(MK_I(0x0B, REG_T1, REG_T1, 0x400));     /* sltiu t1, t1, 0x400      */
    uint32_t *sp_miss = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));       /* beq  t1, zero, @slow     */
    EMIT_NOP();

    /* Scratchpad fast path: store to scratchpad_buf + (addr & 0x3FF) */
    emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf);
    emit(MK_I(0x0C, REG_T0, REG_A0, 0x3FF));    /* andi a0, t0, 0x3FF       */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0);
    if (size == 4)
        EMIT_SW(REG_T2, 0, REG_T1);
    else if (size == 2)
        EMIT_SH(REG_T2, 0, REG_T1);
    else
        EMIT_SB(REG_T2, 0, REG_T1);
    uint32_t *sp_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0));     /* b @done */
    EMIT_NOP();

    /* Slow path: C helper call (I/O, cache ctrl, etc.) */
    /* @slow: isc_branch, align_branch, and sp_miss land here */
    int32_t soff0 = (int32_t)(code_ptr - isc_branch - 1);
    *isc_branch = (*isc_branch & 0xFFFF0000) | ((uint32_t)soff0 & 0xFFFF);
    if (align_branch)
    {
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
    }
    int32_t soff_sp = (int32_t)(code_ptr - sp_miss - 1);
    *sp_miss = (*sp_miss & 0xFFFF0000) | ((uint32_t)soff_sp & 0xFFFF);

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

    /* @done: patch all forward branches */
    int32_t doff = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
    int32_t doff_sp = (int32_t)(code_ptr - sp_done - 1);
    *sp_done = (*sp_done & 0xFFFF0000) | ((uint32_t)doff_sp & 0xFFFF);
}

/*
 * emit_memory_lwx: Emit LWL/LWR with LUT fast path.
 *
 * The R5900 (EE) has native lwl/lwr instructions identical to R3000A.
 * Fast path: LUT lookup → native lwl/lwr on host pointer.
 * Slow path: C helper (Helper_LWL / Helper_LWR).
 *
 * is_left: 1 = LWL, 0 = LWR
 */
void emit_memory_lwx(int is_left, int rt_psx, int rs_psx, int16_t offset, int use_load_delay)
{
    /* Load current rt value (merge target) */
    if (use_load_delay)
        EMIT_LW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
    else
        emit_load_psx_reg(REG_V0, rt_psx);

    /* Compute effective address */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);

    /* LUT lookup (64KB virtual pages, S3 = mem_lut) */
    emit(MK_R(0, 0, REG_T0, REG_T1, 16, 0x02)); /* srl  t1, t0, 16       */
    emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x00));  /* sll  t1, t1, 2        */
    EMIT_ADDU(REG_T1, REG_T1, REG_S3);          /* addu t1, t1, s3       */
    EMIT_LW(REG_T1, 0, REG_T1);                 /* lw   t1, 0(t1)        */
    emit(MK_I(0x0C, REG_T0, REG_T2, 0xFFFF));   /* andi t2, t0, 0xFFFF   */
    uint32_t *lut_branch = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));      /* beq  t1, zero, @slow  */
    EMIT_NOP();

    /* Fast path: native lwl/lwr on host address */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2);
    if (is_left)
        EMIT_LWL(REG_V0, 0, REG_T1);
    else
        EMIT_LWR(REG_V0, 0, REG_T1);

    uint32_t *fast_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
    EMIT_NOP();

    /* Scratchpad inline check for LWL/LWR */
    int32_t soff_lut = (int32_t)(code_ptr - lut_branch - 1);
    *lut_branch = (*lut_branch & 0xFFFF0000) | ((uint32_t)soff_lut & 0xFFFF);

    emit(MK_I(0x0F, 0, REG_T1, 0x1FFF));             /* lui  t1, 0x1FFF          */
    emit(MK_I(0x0D, REG_T1, REG_T1, 0xFFFF));        /* ori  t1, t1, 0xFFFF      */
    emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and  t1, t0, t1  (phys)  */
    emit(MK_I(0x0F, 0, REG_T2, 0xE080));             /* lui  t2, 0xE080          */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2);               /* addu t1, phys-0x1F800000 */
    emit(MK_I(0x0B, REG_T1, REG_T1, 0x400));         /* sltiu t1, t1, 0x400      */
    uint32_t *sp_miss = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));           /* beq  t1, zero, @slow     */
    EMIT_NOP();

    /* Scratchpad fast path */
    emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf);
    emit(MK_I(0x0C, REG_T0, REG_T2, 0x3FF));        /* andi t2, t0, 0x3FF       */
    EMIT_ADDU(REG_T1, REG_T1, REG_T2);
    if (is_left)
        EMIT_LWL(REG_V0, 0, REG_T1);
    else
        EMIT_LWR(REG_V0, 0, REG_T1);
    uint32_t *sp_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0));         /* b @done */
    EMIT_NOP();

    /* Slow path: call C helper */
    int32_t soff_sp = (int32_t)(code_ptr - sp_miss - 1);
    *sp_miss = (*sp_miss & 0xFFFF0000) | ((uint32_t)soff_sp & 0xFFFF);

    EMIT_MOVE(REG_A0, REG_T0); /* a0 = addr */
    EMIT_MOVE(REG_A1, REG_V0); /* a1 = cur_rt */
    emit_call_c(is_left ? (uint32_t)Helper_LWL : (uint32_t)Helper_LWR);

    /* @done: patch forward branches */
    int32_t doff2 = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff2 & 0xFFFF);
    int32_t doff_sp = (int32_t)(code_ptr - sp_done - 1);
    *sp_done = (*sp_done & 0xFFFF0000) | ((uint32_t)doff_sp & 0xFFFF);

    if (!dynarec_load_defer)
    {
        if (is_left)
            mark_vreg_var(rt_psx);
        emit_store_psx_reg(rt_psx, REG_V0);
    }
}

/*
 * emit_memory_swx: Emit SWL/SWR with LUT fast path.
 *
 * Fast path: cache-isolation check → LUT lookup → native swl/swr.
 * Slow path: C helper (Helper_SWL / Helper_SWR).
 *
 * is_left: 1 = SWL, 0 = SWR
 */
void emit_memory_swx(int is_left, int rt_psx, int rs_psx, int16_t offset)
{
    /* Compute effective address into T0, data into T2 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);
    emit_load_psx_reg(REG_T2, rt_psx);

    /* Cache Isolation check */
    EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);
    emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
    emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
    uint32_t *isc_branch = code_ptr;
    emit(MK_I(0x05, REG_A0, REG_ZERO, 0));      /* bne  a0, zero, @slow */
    EMIT_NOP();

    /* LUT lookup */
    emit(MK_R(0, 0, REG_T0, REG_T1, 16, 0x02)); /* srl  t1, t0, 16       */
    emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x00));  /* sll  t1, t1, 2        */
    EMIT_ADDU(REG_T1, REG_T1, REG_S3);          /* addu t1, t1, s3       */
    EMIT_LW(REG_T1, 0, REG_T1);                 /* lw   t1, 0(t1)        */
    emit(MK_I(0x0C, REG_T0, REG_A0, 0xFFFF));   /* andi a0, t0, 0xFFFF   */
    uint32_t *lut_branch = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));      /* beq  t1, zero, @slow  */
    EMIT_NOP();

    /* Fast path: native swl/swr on host address */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0);
    if (is_left)
        EMIT_SWL(REG_T2, 0, REG_T1);
    else
        EMIT_SWR(REG_T2, 0, REG_T1);

    uint32_t *fast_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
    EMIT_NOP();

    /* Scratchpad inline check for SWL/SWR */
    int32_t soff_lut = (int32_t)(code_ptr - lut_branch - 1);
    *lut_branch = (*lut_branch & 0xFFFF0000) | ((uint32_t)soff_lut & 0xFFFF);

    /* phys = vaddr & 0x1FFFFFFF; check (phys - 0x1F800000) < 0x400 */
    emit(MK_I(0x0F, 0, REG_T1, 0x1FFF));             /* lui  t1, 0x1FFF          */
    emit(MK_I(0x0D, REG_T1, REG_T1, 0xFFFF));        /* ori  t1, t1, 0xFFFF      */
    emit(MK_R(0, REG_T0, REG_T1, REG_T1, 0, 0x24)); /* and  t1, t0, t1  (phys)  */
    emit(MK_I(0x0F, 0, REG_A0, 0xE080));             /* lui  a0, 0xE080          */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0);               /* addu t1, phys-0x1F800000 */
    emit(MK_I(0x0B, REG_T1, REG_T1, 0x400));         /* sltiu t1, t1, 0x400      */
    uint32_t *sp_miss = code_ptr;
    emit(MK_I(0x04, REG_T1, REG_ZERO, 0));           /* beq  t1, zero, @slow     */
    EMIT_NOP();

    /* Scratchpad fast path */
    emit_load_imm32(REG_T1, (uint32_t)scratchpad_buf);
    emit(MK_I(0x0C, REG_T0, REG_A0, 0x3FF));        /* andi a0, t0, 0x3FF       */
    EMIT_ADDU(REG_T1, REG_T1, REG_A0);
    if (is_left)
        EMIT_SWL(REG_T2, 0, REG_T1);
    else
        EMIT_SWR(REG_T2, 0, REG_T1);
    uint32_t *sp_done = code_ptr;
    emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0));         /* b @done */
    EMIT_NOP();

    /* Slow path */
    int32_t soff0 = (int32_t)(code_ptr - isc_branch - 1);
    *isc_branch = (*isc_branch & 0xFFFF0000) | ((uint32_t)soff0 & 0xFFFF);
    int32_t soff_sp = (int32_t)(code_ptr - sp_miss - 1);
    *sp_miss = (*sp_miss & 0xFFFF0000) | ((uint32_t)soff_sp & 0xFFFF);

    EMIT_MOVE(REG_A0, REG_T0); /* a0 = addr */
    EMIT_MOVE(REG_A1, REG_T2); /* a1 = rt_val */
    emit_call_c(is_left ? (uint32_t)Helper_SWL : (uint32_t)Helper_SWR);

    /* @done: patch forward branches */
    int32_t doff2 = (int32_t)(code_ptr - fast_done - 1);
    *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff2 & 0xFFFF);
    int32_t doff_sp = (int32_t)(code_ptr - sp_done - 1);
    *sp_done = (*sp_done & 0xFFFF0000) | ((uint32_t)doff_sp & 0xFFFF);
}
