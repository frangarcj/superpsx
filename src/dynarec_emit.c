/*
 * dynarec_emit.c - Low-level code emitters and register mapping
 *
 * Provides the core emit API for generating native R5900 instructions:
 * PSX register load/store, pinned register sync, C helper calls,
 * abort checks, and immediate loading.
 */
#include "dynarec.h"

/* PSX register → native pinned register (0 = not pinned)
 *
 * Layout (Phase 1):
 *   Motor JIT (global):          S0=cpu, S1=ram, S2=cycles, S3=mask
 *   Callee-saved (survive C):    S4=$sp, S5=$ra, S6=$s0, S7=$s1, FP=$gp
 *   Caller-saved (flush in tramp): T3=$v0, T4=$v1, T5=$a0, T6=$a1, T7=$a2
 *   Scratch cache (JIT engine):  T8, T9  (+ AT helper)
 *   Dynamic slots (Phase 2):     T0, T1, T2
 *   C-reserved (no pins):        A0-A3, V0-V1
 */
const int psx_pinned_reg[32] = {
    [2]  = REG_T3,  /* PSX $v0 → native $t3 (caller-saved) */
    [3]  = REG_T4,  /* PSX $v1 → native $t4 (caller-saved) */
    [4]  = REG_T5,  /* PSX $a0 → native $t5 (caller-saved) */
    [5]  = REG_T6,  /* PSX $a1 → native $t6 (caller-saved) */
    [6]  = REG_T7,  /* PSX $a2 → native $t7 (caller-saved) */
    [16] = REG_S6,  /* PSX $s0 → native $s6 (callee-saved) */
    [17] = REG_S7,  /* PSX $s1 → native $s7 (callee-saved) */
    [28] = REG_FP,  /* PSX $gp → native $fp (callee-saved) */
    [29] = REG_S4,  /* PSX $sp → native $s4 (callee-saved) */
    [31] = REG_S5,  /* PSX $ra → native $s5 (callee-saved) */
};

RegStatus vregs[32];
uint32_t dirty_const_mask;

/* SMRV — Speculative Memory Region Validation.
 * Bitmask: bit r=1 means PSX reg r is known to hold a RAM address
 * (physical < PSX_RAM_SIZE after masking).  Used to skip the
 * 2-instruction range check in the memory hot path.
 * $sp (reg 29) is always marked.  Other regs get marked when set
 * via LUI/ADDIU/ORI to a RAM address, cleared on unknown writes. */
uint32_t smrv_known_ram;

/* Scratch register cache: tracks which PSX GPR value is in T8/T9.
 * -1 means the register holds no cached PSX GPR value. */
int t8_cached_psx_reg = -1;
int t9_cached_psx_reg = -1;

void reg_cache_invalidate(void)
{
    t8_cached_psx_reg = -1;
    t9_cached_psx_reg = -1;
}

/* ================================================================
 *  Dynamic Register Slots — Write-Through T0/T1/T2
 *
 *  Each slot maps a frequently-used non-pinned PSX GPR to one of
 *  T0/T1/T2 for the duration of a compiled block.  The write-through
 *  protocol ensures cpu.regs[] is always consistent: every store to
 *  a slot register also emits SW to memory.  This eliminates the need
 *  for writeback at block exits/abort trampolines.
 *
 *  Slots are suspended (disabled) during memory operations and C calls
 *  that commandeer T0/T1/T2 for their own purposes.  After suspension,
 *  a reload restores the slot registers from the always-current memory.
 * ================================================================ */
static const int dyn_slot_ee[DYN_SLOT_COUNT] = { REG_T0, REG_T1, REG_T2 };
int dyn_slot_psx[DYN_SLOT_COUNT] = { -1, -1, -1 };
int dyn_slots_active = 0;

/* Find the slot index holding PSX reg r, or -1 */
static inline int dyn_slot_find(int r)
{
    if (dyn_slot_psx[0] == r) return 0;
    if (dyn_slot_psx[1] == r) return 1;
    if (dyn_slot_psx[2] == r) return 2;
    return -1;
}

/* Assign dynamic slots based on block scan register access frequency.
 * Picks the top-N (up to DYN_SLOT_COUNT) non-pinned PSX GPRs with
 * the highest access count (minimum 2 accesses to justify a slot). */
void dyn_assign_slots(BlockScanResult *scan)
{
    dyn_slot_psx[0] = dyn_slot_psx[1] = dyn_slot_psx[2] = -1;
    dyn_slots_active = 0;

    int slot = 0;
    while (slot < DYN_SLOT_COUNT)
    {
        int best = -1, best_count = 1; /* require >= 2 accesses */
        for (int r = 1; r < 32; r++)
        {
            if (psx_pinned_reg[r]) continue;
            if (scan->reg_access_count[r] <= best_count) continue;
            /* Check not already assigned */
            int already = 0;
            for (int s = 0; s < slot; s++)
                if (dyn_slot_psx[s] == r) { already = 1; break; }
            if (already) continue;
            best = r;
            best_count = scan->reg_access_count[r];
        }
        if (best < 0) break;
        dyn_slot_psx[slot++] = best;
    }
    if (slot > 0) dyn_slots_active = 1;
}

/* Emit LW instructions to load assigned dynamic slots from cpu.regs[].
 * Called at block entry point (after prologue, before instructions). */
void dyn_load_slots(void)
{
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        if (dyn_slot_psx[i] >= 0)
            EMIT_LW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
}

/* Emit LW instructions to reload dynamic slots from cpu.regs[].
 * Called after C calls and memory ops that clobber T0/T1/T2. */
void dyn_reload_slots(void)
{
    if (!dyn_slots_active) return;
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        if (dyn_slot_psx[i] >= 0)
            EMIT_LW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
}

void dyn_reset_slots(void)
{
    dyn_slot_psx[0] = dyn_slot_psx[1] = dyn_slot_psx[2] = -1;
    dyn_slots_active = 0;
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
        {
            EMIT_SW(hwreg, CPU_REG(r), REG_S0);
            /* Also update dynamic slot EE register if r is cached in one.
             * Without this, T0/T1/T2 desync from cpu.regs[] when a lazy
             * const is materialized into a different register (e.g. T8). */
            if (dyn_slots_active) {
                int si = dyn_slot_find(r);
                if (si >= 0 && dyn_slot_ee[si] != hwreg)
                    EMIT_MOVE(dyn_slot_ee[si], hwreg);
            }
        }
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
        /* Check dynamic slot first */
        if (dyn_slots_active) {
            int si = dyn_slot_find(r);
            if (si >= 0) {
                if (hwreg != dyn_slot_ee[si])
                    EMIT_MOVE(hwreg, dyn_slot_ee[si]);
                if (hwreg == REG_T8) t8_cached_psx_reg = -1;
                else if (hwreg == REG_T9) t9_cached_psx_reg = -1;
                return;
            }
        }
        /* Non-pinned, non-slot, non-dirty-const: use scratch cache */
        if (hwreg == REG_T8 && t8_cached_psx_reg == r) return;
        if (hwreg == REG_T9 && t9_cached_psx_reg == r) return;
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
        if (hwreg == REG_T8) t8_cached_psx_reg = r;
        else if (hwreg == REG_T9) t9_cached_psx_reg = r;
        return;
    }
    /* Non-cacheable paths (r=0, dirty const, pinned): invalidate scratch */
    if (hwreg == REG_T8) t8_cached_psx_reg = -1;
    else if (hwreg == REG_T9) t9_cached_psx_reg = -1;
}

int emit_use_reg(int r, int scratch)
{
    if (r == 0)
        return REG_ZERO;
    if (vregs[r].is_const && vregs[r].is_dirty)
    {
        /* Lazy const: materialize into the canonical location */
        int dst;
        if (psx_pinned_reg[r]) {
            dst = psx_pinned_reg[r];
        } else if (dyn_slots_active && dyn_slot_find(r) >= 0) {
            dst = dyn_slot_ee[dyn_slot_find(r)];
        } else {
            dst = scratch;
        }
        emit_load_imm32(dst, vregs[r].value);
        if (!psx_pinned_reg[r])
            EMIT_SW(dst, CPU_REG(r), REG_S0);
        vregs[r].is_dirty = 0;
        dirty_const_mask &= ~(1u << r);
        /* Const materialized into scratch: invalidate cached entry */
        if (dst == REG_T8) t8_cached_psx_reg = -1;
        else if (dst == REG_T9) t9_cached_psx_reg = -1;
        return dst;
    }
    if (psx_pinned_reg[r])
        return psx_pinned_reg[r];
    /* Check dynamic slot */
    if (dyn_slots_active) {
        int si = dyn_slot_find(r);
        if (si >= 0)
            return dyn_slot_ee[si];
    }
    /* Non-pinned: check scratch register cache */
    if (scratch == REG_T8 && t8_cached_psx_reg == r) return REG_T8;
    if (scratch == REG_T9 && t9_cached_psx_reg == r) return REG_T9;
    EMIT_LW(scratch, CPU_REG(r), REG_S0);
    if (scratch == REG_T8) t8_cached_psx_reg = r;
    else if (scratch == REG_T9) t9_cached_psx_reg = r;
    return scratch;
}

int emit_dst_reg(int r, int scratch)
{
    if (r == 0)
        return REG_T8; /* Junk register if writing to $0 (T8 is scratch, not a dyn slot) */
    if (psx_pinned_reg[r])
        return psx_pinned_reg[r];
    if (dyn_slots_active) {
        int si = dyn_slot_find(r);
        if (si >= 0)
            return dyn_slot_ee[si];
    }
    return scratch;
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
    /* Dynamic slot: write-through (update slot reg + memory) */
    if (dyn_slots_active) {
        int si = dyn_slot_find(r);
        if (si >= 0) {
            if (dyn_slot_ee[si] != hwreg)
                EMIT_MOVE(dyn_slot_ee[si], hwreg);
            EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
            /* Invalidate scratch cache if it held this PSX reg */
            if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
            if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
            return;
        }
    }
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
    /* Update cache: hwreg now holds cpu.regs[r] */
    if (hwreg == REG_T8) {
        t8_cached_psx_reg = r;
        if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
    } else if (hwreg == REG_T9) {
        t9_cached_psx_reg = r;
        if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
    }
}

void emit_sync_reg(int r, int host_reg)
{
    if (r == 0 || psx_pinned_reg[r])
        return;
    /* Dynamic slot: write-through (update slot reg + memory) */
    if (dyn_slots_active) {
        int si = dyn_slot_find(r);
        if (si >= 0) {
            if (dyn_slot_ee[si] != host_reg)
                EMIT_MOVE(dyn_slot_ee[si], host_reg);
            EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
            if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
            if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
            return;
        }
    }
    EMIT_SW(host_reg, CPU_REG(r), REG_S0);
    /* Update cache: host_reg now holds cpu.regs[r] */
    if (host_reg == REG_T8) {
        t8_cached_psx_reg = r;
        if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
    } else if (host_reg == REG_T9) {
        t9_cached_psx_reg = r;
        if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
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
            else if (dyn_slots_active && dyn_slot_find(r) >= 0)
            {
                int si = dyn_slot_find(r);
                emit_load_imm32(dyn_slot_ee[si], vregs[r].value);
                EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
                if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
                if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
            }
            else
            {
                emit_load_imm32(REG_AT, vregs[r].value);
                EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
                /* cpu.regs[r] changed; invalidate stale cache entry */
                if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
                if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
            }
            vregs[r].is_dirty = 0;
        }
    }
}


/* Flush pinned PSX registers to cpu struct before JAL to C helpers.
 * This ensures cpu.regs[] is consistent for C code and exception handlers. */
void emit_flush_pinned(void)
{
    /* Caller-saved pins (T3-T7) */
    EMIT_SW(REG_T3, CPU_REG(2),  REG_S0); /* PSX $v0 */
    EMIT_SW(REG_T4, CPU_REG(3),  REG_S0); /* PSX $v1 */
    EMIT_SW(REG_T5, CPU_REG(4),  REG_S0); /* PSX $a0 */
    EMIT_SW(REG_T6, CPU_REG(5),  REG_S0); /* PSX $a1 */
    EMIT_SW(REG_T7, CPU_REG(6),  REG_S0); /* PSX $a2 */
    /* Callee-saved pins (S4-S7, FP) */
    EMIT_SW(REG_S6, CPU_REG(16), REG_S0); /* PSX $s0 */
    EMIT_SW(REG_S7, CPU_REG(17), REG_S0); /* PSX $s1 */
    EMIT_SW(REG_FP, CPU_REG(28), REG_S0); /* PSX $gp */
    EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Reload pinned PSX registers from cpu struct after JAL returns.
 * C functions may have modified cpu.regs[] directly. */
void emit_reload_pinned(void)
{
    /* Caller-saved pins (T3-T7) */
    EMIT_LW(REG_T3, CPU_REG(2),  REG_S0); /* PSX $v0 */
    EMIT_LW(REG_T4, CPU_REG(3),  REG_S0); /* PSX $v1 */
    EMIT_LW(REG_T5, CPU_REG(4),  REG_S0); /* PSX $a0 */
    EMIT_LW(REG_T6, CPU_REG(5),  REG_S0); /* PSX $a1 */
    EMIT_LW(REG_T7, CPU_REG(6),  REG_S0); /* PSX $a2 */
    /* Callee-saved pins (S4-S7, FP) */
    EMIT_LW(REG_S6, CPU_REG(16), REG_S0); /* PSX $s0 */
    EMIT_LW(REG_S7, CPU_REG(17), REG_S0); /* PSX $s1 */
    EMIT_LW(REG_FP, CPU_REG(28), REG_S0); /* PSX $gp */
    EMIT_LW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_LW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Selectively flush only pinned PSX regs in the given mask.
 * mask bit r = 1 means PSX reg r (which is pinned) should be flushed.
 * Bits for non-pinned regs are ignored.  Used with block_scan()'s
 * pinned_written_mask to skip flushing regs that were never written. */
void emit_flush_pinned_selective(uint32_t mask)
{
    /* Caller-saved */
    if (mask & (1u <<  2)) EMIT_SW(REG_T3, CPU_REG(2),  REG_S0); /* PSX $v0 */
    if (mask & (1u <<  3)) EMIT_SW(REG_T4, CPU_REG(3),  REG_S0); /* PSX $v1 */
    if (mask & (1u <<  4)) EMIT_SW(REG_T5, CPU_REG(4),  REG_S0); /* PSX $a0 */
    if (mask & (1u <<  5)) EMIT_SW(REG_T6, CPU_REG(5),  REG_S0); /* PSX $a1 */
    if (mask & (1u <<  6)) EMIT_SW(REG_T7, CPU_REG(6),  REG_S0); /* PSX $a2 */
    /* Callee-saved */
    if (mask & (1u << 16)) EMIT_SW(REG_S6, CPU_REG(16), REG_S0); /* PSX $s0 */
    if (mask & (1u << 17)) EMIT_SW(REG_S7, CPU_REG(17), REG_S0); /* PSX $s1 */
    if (mask & (1u << 28)) EMIT_SW(REG_FP, CPU_REG(28), REG_S0); /* PSX $gp */
    if (mask & (1u << 29)) EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    if (mask & (1u << 31)) EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
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
     * without emitting 24 instructions per C-call. Target is passed in REG_T8. */
    emit_load_imm32(REG_T8, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_addr);
    EMIT_NOP();
    /* C helpers via call_c may write cpu.regs[] — reload dynamic slots
     * from cpu.regs[] to pick up any changes (T0/T1/T2 are clobbered). */
    dyn_reload_slots();
    reg_cache_invalidate();
    /* SMRV: C helper may change any cpu.regs[], invalidate all
     * known-RAM hints except $sp which is always RAM. */
    smrv_known_ram = (1u << 29);
}

void emit_call_c_lite(uint32_t func_addr)
{
    /* Materialize any lazy constants before the C call */
    flush_dirty_consts();
    /* Lightweight trampoline for C helpers that do NOT read/write cpu.regs[].
     * Only flushes/reloads caller-saved pinned regs (T3-T7 = 5 regs), skipping
     * callee-saved pins (S4/S5/S6/S7/FP) which C ABI preserves. */
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0);
    emit_load_imm32(REG_T8, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_lite_addr);
    EMIT_NOP();
    /* T0/T1/T2 preserved by lite trampoline save/restore */
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
    /* Use AT instead of T0 to avoid clobbering dynamic slot 0 */
    EMIT_LW(REG_AT, CPU_BLOCK_ABORTED, REG_S0); /* at = cpu.block_aborted */
    EMIT_BEQ(REG_AT, REG_ZERO, 4);              /* skip next 3 instrs if zero */
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
    /* SMRV: auto-set if this constant is a RAM address */
    if ((val & 0x1FFFFFFF) < PSX_RAM_SIZE)
        smrv_known_ram |= (1u << r);
    else
        smrv_known_ram &= ~(1u << r);
}

void mark_vreg_var(int r)
{
    if (r == 0)
        return;
    /* SMRV: clear by default; callers that preserve RAM-ness
     * (e.g., ADDIU from known-RAM base) re-set it after this call. */
    smrv_known_ram &= ~(1u << r);
    /* If the register held a lazy (dirty) constant that was never
     * materialized into the native pinned register or cpu.regs[],
     * we must materialize it NOW before losing the value.  This
     * covers rd==rs overlaps like ADDU $t0, $t0, $t1 where the
     * destination is marked var before the source is read.
     * Uses REG_AT as scratch to avoid clobbering REG_T8. */
    if (vregs[r].is_const && vregs[r].is_dirty)
    {
        if (psx_pinned_reg[r])
            emit_load_imm32(psx_pinned_reg[r], vregs[r].value);
        else if (dyn_slots_active && dyn_slot_find(r) >= 0)
        {
            int si = dyn_slot_find(r);
            emit_load_imm32(dyn_slot_ee[si], vregs[r].value);
            EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
            if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
            if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
        }
        else
        {
            emit_load_imm32(REG_AT, vregs[r].value);
            EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
            /* cpu.regs[r] changed; invalidate stale cache entry */
            if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
            if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
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
    smrv_known_ram = (1u << 29); /* $sp always RAM */
    reg_cache_invalidate();
    dyn_reset_slots();
    /* $0 is special, but memset already handles it by setting is_const=0.
     * However, is_vreg_const(0) handles it specifically. */
}

/* ---- Compile-loop helpers ----
 * These helpers use AT (or direct pinned regs) instead of T8/T9 for
 * non-GPR temporaries.  This keeps T8/T9 free for the scratch register
 * cache, and also saves 1 instruction when the destination
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
    else if (dyn_slots_active && dyn_slot_find(r) >= 0)
    {
        int si = dyn_slot_find(r);
        EMIT_LW(dyn_slot_ee[si], field_offset, REG_S0);
        EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0); /* write-through */
    }
    else
    {
        EMIT_LW(REG_AT, field_offset, REG_S0);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        /* cpu.regs[r] changed via AT; invalidate stale scratch entries */
        if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
        if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
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
    else if (dyn_slots_active && dyn_slot_find(r) >= 0)
    {
        int si = dyn_slot_find(r);
        emit_load_imm32(dyn_slot_ee[si], value);
        EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0); /* write-through */
    }
    else
    {
        emit_load_imm32(REG_AT, value);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        /* cpu.regs[r] changed via AT; invalidate stale scratch entries */
        if (t8_cached_psx_reg == r) t8_cached_psx_reg = -1;
        if (t9_cached_psx_reg == r) t9_cached_psx_reg = -1;
    }
}

/* Store an immediate value into a cpu struct field.  Uses AT. */
void emit_imm_to_cpu_field(int field_offset, uint32_t value)
{
    emit_load_imm32(REG_AT, value);
    EMIT_SW(REG_AT, field_offset, REG_S0);
}
