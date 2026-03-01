/*
 * dynarec_emit.c - Low-level code emitters and register mapping
 *
 * Provides the core emit API for generating native R5900 instructions:
 * PSX register load/store, pinned register sync, C helper calls,
 * abort checks, and immediate loading.
 */
#include "dynarec.h"

/* PSX register → native pinned register (0 = not pinned) */
const int psx_pinned_reg[32] = {
    [2] = REG_S6,  /* PSX $v0 → native $s6 */
    [3] = REG_V1,  /* PSX $v1 → native $v1 */
    [4] = REG_T3,  /* PSX $a0 → native $t3 */
    [5] = REG_T4,  /* PSX $a1 → native $t4 */
    [6] = REG_T5,  /* PSX $a2 → native $t5 */
    [7] = REG_T6,  /* PSX $a3 → native $t6 */
    [8] = REG_T7,  /* PSX $t0 → native $t7 */
    [9] = REG_T8,  /* PSX $t1 → native $t8 */
    [10] = REG_T9, /* PSX $t2 → native $t9 */
    [28] = REG_FP, /* PSX $gp → native $fp */
    [29] = REG_S4, /* PSX $sp → native $s4 */
    [30] = REG_S7, /* PSX $s8 → native $s7 */
    [31] = REG_S5, /* PSX $ra → native $s5 */
};

RegStatus vregs[32];
uint32_t dirty_const_mask;

/* Scratch register cache: tracks which PSX GPR value is in T0/T1.
 * -1 means the register holds no cached PSX GPR value. */
int t0_cached_psx_reg = -1;
int t1_cached_psx_reg = -1;

void reg_cache_invalidate(void)
{
    t0_cached_psx_reg = -1;
    t1_cached_psx_reg = -1;
}

/* Load PSX register 'r' from cpu struct into hw reg 'hwreg' */
void emit_load_psx_reg(int hwreg, int r)
{
    if (r == 0)
    {
        EMIT_MOVE(hwreg, REG_ZERO); /* $0 is always 0 */
    }
    else if (vregs[r].is_const && vregs[r].is_dirty)
    {
        /* Lazy const: materialize into hwreg */
        emit_load_imm32(hwreg, vregs[r].value);
        /* Also update the pinned reg / cpu.regs[] so future uses are fast */
        if (psx_pinned_reg[r] && hwreg != psx_pinned_reg[r])
            EMIT_MOVE(psx_pinned_reg[r], hwreg);
        else if (!psx_pinned_reg[r])
            EMIT_SW(hwreg, CPU_REG(r), REG_S0);
        vregs[r].is_dirty = 0;
        dirty_const_mask &= ~(1u << r);
    }
    else if (psx_pinned_reg[r])
    {
        if (hwreg != psx_pinned_reg[r]) /* avoid self-move */
            EMIT_MOVE(hwreg, psx_pinned_reg[r]);
    }
    else
    {
        /* Non-pinned, non-dirty-const: use scratch cache */
        if (hwreg == REG_T0 && t0_cached_psx_reg == r) return;
        if (hwreg == REG_T1 && t1_cached_psx_reg == r) return;
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
        if (hwreg == REG_T0) t0_cached_psx_reg = r;
        else if (hwreg == REG_T1) t1_cached_psx_reg = r;
        return;
    }
    /* Non-cacheable paths (r=0, dirty const, pinned): invalidate scratch */
    if (hwreg == REG_T0) t0_cached_psx_reg = -1;
    else if (hwreg == REG_T1) t1_cached_psx_reg = -1;
}

int emit_use_reg(int r, int scratch)
{
    if (r == 0)
        return REG_ZERO;
    if (vregs[r].is_const && vregs[r].is_dirty)
    {
        /* Lazy const: materialize into the canonical location */
        int dst = psx_pinned_reg[r] ? psx_pinned_reg[r] : scratch;
        emit_load_imm32(dst, vregs[r].value);
        if (!psx_pinned_reg[r])
            EMIT_SW(dst, CPU_REG(r), REG_S0);
        vregs[r].is_dirty = 0;
        dirty_const_mask &= ~(1u << r);
        /* Const materialized into scratch: invalidate cached entry */
        if (dst == REG_T0) t0_cached_psx_reg = -1;
        else if (dst == REG_T1) t1_cached_psx_reg = -1;
        return dst;
    }
    if (psx_pinned_reg[r])
        return psx_pinned_reg[r];
    /* Non-pinned: check scratch register cache */
    if (scratch == REG_T0 && t0_cached_psx_reg == r) return REG_T0;
    if (scratch == REG_T1 && t1_cached_psx_reg == r) return REG_T1;
    EMIT_LW(scratch, CPU_REG(r), REG_S0);
    if (scratch == REG_T0) t0_cached_psx_reg = r;
    else if (scratch == REG_T1) t1_cached_psx_reg = r;
    return scratch;
}

int emit_dst_reg(int r, int scratch)
{
    if (r == 0)
        return REG_T2; /* Junk register if writing to $0 */
    return psx_pinned_reg[r] ? psx_pinned_reg[r] : scratch;
}

/* Store hw reg 'hwreg' to PSX register 'r' in cpu struct */
void emit_store_psx_reg(int r, int hwreg)
{
    if (r == 0)
        return; /* never write to $0 */
    if (psx_pinned_reg[r])
    {
        if (psx_pinned_reg[r] != hwreg) /* avoid self-move */
            EMIT_MOVE(psx_pinned_reg[r], hwreg);
        return;
    }
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
    /* Update cache: hwreg now holds cpu.regs[r] */
    if (hwreg == REG_T0) {
        t0_cached_psx_reg = r;
        if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
    } else if (hwreg == REG_T1) {
        t1_cached_psx_reg = r;
        if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
    }
}

void emit_sync_reg(int r, int host_reg)
{
    if (r == 0 || psx_pinned_reg[r])
        return;
    EMIT_SW(host_reg, CPU_REG(r), REG_S0);
    /* Update cache: host_reg now holds cpu.regs[r] */
    if (host_reg == REG_T0) {
        t0_cached_psx_reg = r;
        if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
    } else if (host_reg == REG_T1) {
        t1_cached_psx_reg = r;
        if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
    }
}

/* Materialize all lazy (dirty) constants into native registers / cpu.regs[].
 * Must be called before any block exit, C function call, or register-indirect
 * jump to ensure the machine state is fully consistent. */
/* Note: uses REG_AT as scratch for non-pinned registers to avoid
 * clobbering REG_T0, which often holds the effective address in
 * memory slow paths when flush_dirty_consts is called. */
void flush_dirty_consts(void)
{
    if (dirty_const_mask == 0)
        return;
    uint32_t mask = dirty_const_mask;
    dirty_const_mask = 0;
    while (mask) {
        int r = __builtin_ctz(mask);
        mask &= mask - 1;
        if (vregs[r].is_const && vregs[r].is_dirty)
        {
            if (psx_pinned_reg[r])
            {
                emit_load_imm32(psx_pinned_reg[r], vregs[r].value);
            }
            else
            {
                emit_load_imm32(REG_AT, vregs[r].value);
                EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
                /* cpu.regs[r] changed; invalidate stale cache entry */
                if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
                if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
            }
            vregs[r].is_dirty = 0;
        }
    }
}


/* Flush pinned PSX registers to cpu struct before JAL to C helpers.
 * This ensures cpu.regs[] is consistent for C code and exception handlers. */
void emit_flush_pinned(void)
{
    EMIT_SW(REG_S6, CPU_REG(2), REG_S0);  /* PSX $v0 */
    EMIT_SW(REG_V1, CPU_REG(3), REG_S0);  /* PSX $v1 */
    EMIT_SW(REG_T3, CPU_REG(4), REG_S0);  /* PSX $a0 */
    EMIT_SW(REG_T4, CPU_REG(5), REG_S0);  /* PSX $a1 */
    EMIT_SW(REG_T5, CPU_REG(6), REG_S0);  /* PSX $a2 */
    EMIT_SW(REG_T6, CPU_REG(7), REG_S0);  /* PSX $a3 */
    EMIT_SW(REG_T7, CPU_REG(8), REG_S0);  /* PSX $t0 */
    EMIT_SW(REG_T8, CPU_REG(9), REG_S0);  /* PSX $t1 */
    EMIT_SW(REG_T9, CPU_REG(10), REG_S0); /* PSX $t2 */
    EMIT_SW(REG_FP, CPU_REG(28), REG_S0);  /* PSX $gp */
    EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_SW(REG_S7, CPU_REG(30), REG_S0); /* PSX $s8 */
    EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Reload pinned PSX registers from cpu struct after JAL returns.
 * C functions may have modified cpu.regs[] directly. */
void emit_reload_pinned(void)
{
    EMIT_LW(REG_S6, CPU_REG(2), REG_S0);  /* PSX $v0 */
    EMIT_LW(REG_V1, CPU_REG(3), REG_S0);  /* PSX $v1 */
    EMIT_LW(REG_T3, CPU_REG(4), REG_S0);  /* PSX $a0 */
    EMIT_LW(REG_T4, CPU_REG(5), REG_S0);  /* PSX $a1 */
    EMIT_LW(REG_T5, CPU_REG(6), REG_S0);  /* PSX $a2 */
    EMIT_LW(REG_T6, CPU_REG(7), REG_S0);  /* PSX $a3 */
    EMIT_LW(REG_T7, CPU_REG(8), REG_S0);  /* PSX $t0 */
    EMIT_LW(REG_T8, CPU_REG(9), REG_S0);  /* PSX $t1 */
    EMIT_LW(REG_T9, CPU_REG(10), REG_S0); /* PSX $t2 */
    EMIT_LW(REG_FP, CPU_REG(28), REG_S0);  /* PSX $gp */
    EMIT_LW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_LW(REG_S7, CPU_REG(30), REG_S0); /* PSX $s8 */
    EMIT_LW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Emit a JAL to a C helper function with pinned register sync.
 * Flushes pinned regs to cpu struct before call (for exception safety),
 * and reloads them after return (C code may have modified cpu.regs[]). */
void emit_call_c(uint32_t func_addr)
{
    /* Materialize any lazy constants before the C call */
    flush_dirty_consts();
    /* Flush S2 to memory so C code sees current cycles_left */
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0);

    /* Use the shared trampoline to flush/reload pinned registers and provide ABI shadow space
     * without emitting 24 instructions per C-call. Target is passed in REG_T0. */
    emit_load_imm32(REG_T0, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_addr);
    EMIT_NOP();
    reg_cache_invalidate();
}

void emit_call_c_lite(uint32_t func_addr)
{
    /* Materialize any lazy constants before the C call */
    flush_dirty_consts();
    /* Lightweight trampoline for C helpers that do NOT read/write cpu.regs[].
     * Only flushes/reloads caller-saved pinned regs (V1, T3-T9), saving 8
     * instructions vs the full trampoline.  Safe for memory R/W, LWL/LWR,
     * SWL/SWR helpers. */
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0);
    emit_load_imm32(REG_T0, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_lite_addr);
    EMIT_NOP();
    reg_cache_invalidate();
}

/*
 * Emit a mid-block abort check after a C helper that may trigger a PSX
 * exception (ADD/SUB/ADDI overflow, LW/LH/SH/SW alignment, CpU, etc.).
 *
 * cpu.block_aborted is at offset CPU_BLOCK_ABORTED from $s0 (cpu ptr).
 * The abort trampoline (emit_block_epilogue style) lives at a fixed offset
 * in code_buffer and is shared across all blocks.
 *
 * Generated code (5 instructions, 3 on normal path):
 *   lw   t0, CPU_BLOCK_ABORTED(s0) ; load abort flag from cpu struct
 *   beq  t0, zero, @skip           ; no abort -> continue
 *   nop
 *   j    abort_trampoline           ; abort -> shared epilogue
 *   nop
 * @skip:
 */
void emit_abort_check(uint32_t cycles)
{
    EMIT_LW(REG_T0, CPU_BLOCK_ABORTED, REG_S0); /* t0 = cpu.block_aborted */
    EMIT_BEQ(REG_T0, REG_ZERO, 4);              /* skip next 3 instrs if zero */
    EMIT_NOP();

    /* Inside abort path: subtract only the cycles consumed up to this
     * instruction, not the full block total.  For deferred cold/TLB paths
     * this is the per-instruction cycle_offset stored at emit time. */
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)cycles);
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    EMIT_NOP();
}

/* Load 32-bit immediate into hw register */
void emit_load_imm32(int hwreg, uint32_t val)
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

void mark_vreg_const(int r, uint32_t val)
{
    if (r == 0)
        return;
    vregs[r].is_const = 1;
    vregs[r].value = val;
    vregs[r].is_dirty = 0;
}

/* Mark a register as const but dirty (native reg / cpu.regs[] not yet updated).
 * The value will be materialized on demand by emit_use_reg/emit_load_psx_reg,
 * or flushed by flush_dirty_consts at block exit / C call boundary. */
void mark_vreg_const_lazy(int r, uint32_t val)
{
    if (r == 0)
        return;
    vregs[r].is_const = 1;
    vregs[r].value = val;
    vregs[r].is_dirty = 1;
    dirty_const_mask |= (1u << r);
}

void mark_vreg_var(int r)
{
    if (r == 0)
        return;
    /* If the register held a lazy (dirty) constant that was never
     * materialized into the native pinned register or cpu.regs[],
     * we must materialize it NOW before losing the value.  This
     * covers rd==rs overlaps like ADDU $t0, $t0, $t1 where the
     * destination is marked var before the source is read.
     * Uses REG_AT as scratch to avoid clobbering REG_T0. */
    if (vregs[r].is_const && vregs[r].is_dirty)
    {
        if (psx_pinned_reg[r])
            emit_load_imm32(psx_pinned_reg[r], vregs[r].value);
        else
        {
            emit_load_imm32(REG_AT, vregs[r].value);
            EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
            /* cpu.regs[r] changed; invalidate stale cache entry */
            if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
            if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
        }
    }
    vregs[r].is_const = 0;
    vregs[r].is_dirty = 0;
    dirty_const_mask &= ~(1u << r);
}

int is_vreg_const(int r)
{
    if (r == 0)
        return 1; /* Zero register is always constant 0 */
    return vregs[r].is_const;
}

uint32_t get_vreg_const(int r)
{
    if (r == 0)
        return 0;
    return vregs[r].value;
}

void reset_vregs(void)
{
    memset(vregs, 0, sizeof(vregs));
    dirty_const_mask = 0;
    reg_cache_invalidate();
    /* $0 is special, but memset already handles it by setting is_const=0.
     * However, is_vreg_const(0) handles it specifically. */
}

/* ---- Compile-loop helpers ----
 * These helpers use AT (or direct pinned regs) instead of T0/T1 for
 * non-GPR temporaries.  This keeps T0/T1 free for the scratch register
 * cache (Phase 2), and also saves 1 instruction when the destination
 * PSX register is pinned (direct load instead of load+move). */

/* Copy a CPU struct field (HI, LO, COP0, load_delay_val, etc.) into a
 * PSX general-purpose register.  Uses AT for non-pinned regs. */
void emit_cpu_field_to_psx_reg(int field_offset, int r)
{
    mark_vreg_var(r);
    if (r == 0) return;
    if (psx_pinned_reg[r])
    {
        EMIT_LW(psx_pinned_reg[r], field_offset, REG_S0);
    }
    else
    {
        EMIT_LW(REG_AT, field_offset, REG_S0);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        /* cpu.regs[r] changed via AT; invalidate stale scratch entries */
        if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
        if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
    }
}

/* Store an immediate value into a PSX register.  Uses AT for non-pinned. */
void emit_materialize_psx_imm(int r, uint32_t value)
{
    if (r == 0) return;
    if (psx_pinned_reg[r])
    {
        emit_load_imm32(psx_pinned_reg[r], value);
    }
    else
    {
        emit_load_imm32(REG_AT, value);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        /* cpu.regs[r] changed via AT; invalidate stale scratch entries */
        if (t0_cached_psx_reg == r) t0_cached_psx_reg = -1;
        if (t1_cached_psx_reg == r) t1_cached_psx_reg = -1;
    }
}

/* Store an immediate value into a cpu struct field.  Uses AT. */
void emit_imm_to_cpu_field(int field_offset, uint32_t value)
{
    emit_load_imm32(REG_AT, value);
    EMIT_SW(REG_AT, field_offset, REG_S0);
}
