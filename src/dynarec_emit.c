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
 * Layout:
 *   Motor JIT (global):          S0=cpu, S1=ram, S2=cycles, S3=mask
 *   Pinned (callee-saved, 4):    S4=$sp, S5=$ra, S6=$gp, S7=$fp
 *   Dynamic slots (8):           T0-T7 (frequency-based per-block, write-through)
 *   Scratch cache (JIT engine):  T8, T9  (+ AT helper)
 *   C-reserved (no pins):        A0-A3, V0-V1
 *   Free:                        FP ($s8)
 */
const int psx_pinned_reg[32] = {
    [28] = REG_S6, /* PSX $gp → native $s6 (callee-saved) */
    [29] = REG_S4, /* PSX $sp → native $s4 (callee-saved) */
    [30] = REG_S7, /* PSX $fp → native $s7 (callee-saved) */
    [31] = REG_S5, /* PSX $ra → native $s5 (callee-saved) */
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

/* Alignment tracking — compile-time bitmask: bit r=1 means PSX reg r
 * is known to be word-aligned (addr & 3 == 0).  Used to skip the
 * 2-instruction alignment check (ANDI+BNE) in LW/SW/LH/SH hot paths.
 * $gp/$sp/$fp/$ra (regs 28-31) are always marked (PSX ABI guarantees). */
uint32_t align_known_mask;

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
 *  Dynamic Register Slots — Dirty Writeback T0-T7
 *
 *  Each slot maps a frequently-used non-pinned PSX GPR to one of
 *  T0-T7 for the duration of a compiled block.  Dirty writeback:
 *  stores update the slot register and set a compile-time dirty bit;
 *  the actual SW to cpu.regs[] is deferred to sync points (block exit,
 *  C calls, abort).  This eliminates redundant memory writes in
 *  ALU-heavy and load-heavy hot paths.
 *
 *  Slots are suspended (disabled) during full C calls that clobber
 *  T0-T7.  After a full C call, dyn_reload_slots() restores them
 *  from cpu.regs[] and clears the dirty mask.
 * ================================================================ */
static const int dyn_slot_ee[DYN_SLOT_COUNT] = {
    REG_T0, REG_T1, REG_T2, REG_T3, REG_T4, REG_T5, REG_T6, REG_T7};
int dyn_slot_psx[DYN_SLOT_COUNT];
int dyn_slots_active = 0;
uint8_t dyn_dirty_mask = 0;

/* Find the slot index holding PSX reg r, or -1 */
static inline int dyn_slot_find(int r)
{
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        if (dyn_slot_psx[i] == r)
            return i;
    return -1;
}

/* Assign dynamic slots based on block scan register access frequency.
 * Picks the top-N (up to DYN_SLOT_COUNT) non-pinned PSX GPRs with
 * the highest access count (minimum 2 accesses to justify a slot). */
void dyn_assign_slots(BlockScanResult *scan)
{
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        dyn_slot_psx[i] = -1;
    dyn_slots_active = 0;

    int slot = 0;
    while (slot < DYN_SLOT_COUNT)
    {
        int best = -1, best_count = 1; /* require >= 2 accesses */
        for (int r = 1; r < 32; r++)
        {
            if (psx_pinned_reg[r])
                continue;
            if (scan->reg_access_count[r] <= best_count)
                continue;
            /* Check not already assigned */
            int already = 0;
            for (int s = 0; s < slot; s++)
                if (dyn_slot_psx[s] == r)
                {
                    already = 1;
                    break;
                }
            if (already)
                continue;
            best = r;
            best_count = scan->reg_access_count[r];
        }
        if (best < 0)
            break;
        dyn_slot_psx[slot++] = best;
    }
    if (slot > 0)
        dyn_slots_active = 1;
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
 * Called after full C calls that may modify cpu.regs[]. */
void dyn_reload_slots(void)
{
    if (!dyn_slots_active)
        return;
    dyn_dirty_mask = 0;
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        if (dyn_slot_psx[i] >= 0)
            EMIT_LW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
}

/* Emit SW for each dirty dynamic slot, then clear the dirty mask.
 * Must be called at every sync point: block exit, C call, abort. */
/* Emit SW for each dirty dynamic slot, then clear the dirty mask.
 * DIRTY-ONLY mode. */
void dyn_flush_dirty_slots(void)
{
    if (!dyn_slots_active || dyn_dirty_mask == 0)
        return;
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
    {
        if ((dyn_dirty_mask & (1u << i)) && dyn_slot_psx[i] >= 0)
            EMIT_SW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
    }
    dyn_dirty_mask = 0;
}

/* Always flush ALL assigned slots regardless of dirty mask */
void dyn_flush_all_slots(void)
{
    if (!dyn_slots_active)
        return;
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
    {
        if (dyn_slot_psx[i] >= 0)
            EMIT_SW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
    }
    dyn_dirty_mask = 0;
}

/* ---- Runtime invariant checker for dirty writeback ----
 * For each NON-DIRTY assigned slot, emits:
 *   LW AT, cpu.regs[psx](S0)
 *   BEQ AT, slot_ee, @ok
 *   NOP
 *   SW slot_ee, cpu.regs[psx](S0)   ; fix the mismatch
 *   <increment dyn_mismatch_count>
 * @ok:
 * For each DIRTY slot, normal SW.
 * This detects when the compile-time dirty mask doesn't match the
 * runtime state (non-dirty slot value != cpu.regs[]). */
uint32_t dyn_mismatch_count = 0;

void dyn_flush_verify_slots(void)
{
    if (!dyn_slots_active)
        return;
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
    {
        if (dyn_slot_psx[i] < 0)
            continue;
        if (dyn_dirty_mask & (1u << i))
        {
            /* Dirty: normal flush */
            EMIT_SW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
        }
        else
        {
            /* Non-dirty: verify slot == cpu.regs[], fix + count if not */
            EMIT_LW(REG_AT, CPU_REG(dyn_slot_psx[i]), REG_S0);
            uint32_t *beq = code_ptr;
            emit(MK_I(0x04, REG_AT, dyn_slot_ee[i], 0)); /* BEQ AT, T_i, @ok */
            EMIT_NOP();
            /* Mismatch: write correct value to cpu.regs[] and bump counter */
            EMIT_SW(dyn_slot_ee[i], CPU_REG(dyn_slot_psx[i]), REG_S0);
            {
                uint32_t addr = (uint32_t)&dyn_mismatch_count;
                uint16_t lo = addr & 0xFFFF;
                uint16_t hi = (addr + 0x8000) >> 16;
                EMIT_LUI(REG_AT, hi);
                EMIT_LW(REG_T9, (int16_t)lo, REG_AT);
                EMIT_ADDIU(REG_T9, REG_T9, 1);
                EMIT_SW(REG_T9, (int16_t)lo, REG_AT);
            }
            /* Patch BEQ to skip mismatch handler */
            int32_t skip = (int32_t)(code_ptr - beq - 1);
            *beq = (*beq & 0xFFFF0000) | ((uint32_t)skip & 0xFFFF);
        }
    }
    dyn_dirty_mask = 0;
}

/* Flush a single dynamic slot for PSX register 'r' IF it's dirty.
 * Used by overflow cold paths (ADD/SUB/ADDI) to save rd before clobbering. */
void dyn_flush_one_slot(int r)
{
    if (!dyn_slots_active || r <= 0)
        return;
    int si = dyn_slot_find(r);
    if (si >= 0 && (dyn_dirty_mask & (1u << si)))
        EMIT_SW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
}

/* Reload a single dynamic slot for PSX register 'r' from cpu.regs[].
 * Used by overflow cold paths to restore rd after the ALU op clobbered it. */
void dyn_reload_one_slot(int r)
{
    if (!dyn_slots_active || r <= 0)
        return;
    int si = dyn_slot_find(r);
    if (si >= 0)
        EMIT_LW(dyn_slot_ee[si], CPU_REG(r), REG_S0);
}

void dyn_reset_slots(void)
{
    for (int i = 0; i < DYN_SLOT_COUNT; i++)
        dyn_slot_psx[i] = -1;
    dyn_slots_active = 0;
    dyn_dirty_mask = 0;
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
        /* Also update the pinned reg or dynamic slot */
        if (psx_pinned_reg[r] && hwreg != psx_pinned_reg[r])
            EMIT_MOVE(psx_pinned_reg[r], hwreg);
        else if (!psx_pinned_reg[r])
        {
            if (dyn_slots_active)
            {
                int si = dyn_slot_find(r);
                if (si >= 0)
                {
                    if (dyn_slot_ee[si] != hwreg)
                        EMIT_MOVE(dyn_slot_ee[si], hwreg);
                    dyn_dirty_mask |= (1u << si);
                }
                else
                {
                    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
                }
            }
            else
            {
                EMIT_SW(hwreg, CPU_REG(r), REG_S0);
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
        if (dyn_slots_active)
        {
            int si = dyn_slot_find(r);
            if (si >= 0)
            {
                if (hwreg != dyn_slot_ee[si])
                    EMIT_MOVE(hwreg, dyn_slot_ee[si]);
                SCRATCH_INVALIDATE_HWREG(hwreg);
                return;
            }
        }
        /* Non-pinned, non-slot, non-dirty-const: use scratch cache */
        if (hwreg == REG_T8 && t8_cached_psx_reg == r)
            return;
        if (hwreg == REG_T9 && t9_cached_psx_reg == r)
            return;
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
        if (hwreg == REG_T8)
            t8_cached_psx_reg = r;
        else if (hwreg == REG_T9)
            t9_cached_psx_reg = r;
        return;
    }
    /* Non-cacheable paths (r=0, dirty const, pinned): invalidate scratch */
    SCRATCH_INVALIDATE_HWREG(hwreg);
}

int emit_use_reg(int r, int scratch)
{
    if (r == 0)
        return REG_ZERO;
    if (vregs[r].is_const && vregs[r].is_dirty)
    {
        /* Lazy const: materialize into the canonical location */
        int dst;
        int si = -1;
        if (psx_pinned_reg[r])
        {
            dst = psx_pinned_reg[r];
        }
        else if (dyn_slots_active && (si = dyn_slot_find(r)) >= 0)
        {
            dst = dyn_slot_ee[si];
        }
        else
        {
            dst = scratch;
        }
        emit_load_imm32(dst, vregs[r].value);
        if (!psx_pinned_reg[r])
        {
            if (si >= 0)
                dyn_dirty_mask |= (1u << si);
            else
                EMIT_SW(dst, CPU_REG(r), REG_S0);
        }
        vregs[r].is_dirty = 0;
        dirty_const_mask &= ~(1u << r);
        /* Const materialized into scratch: invalidate cached entry */
        SCRATCH_INVALIDATE_HWREG(dst);
        return dst;
    }
    if (psx_pinned_reg[r])
        return psx_pinned_reg[r];
    /* Check dynamic slot */
    if (dyn_slots_active)
    {
        int si = dyn_slot_find(r);
        if (si >= 0)
            return dyn_slot_ee[si];
    }
    /* Non-pinned: check scratch register cache */
    if (scratch == REG_T8 && t8_cached_psx_reg == r)
        return REG_T8;
    if (scratch == REG_T9 && t9_cached_psx_reg == r)
        return REG_T9;
    EMIT_LW(scratch, CPU_REG(r), REG_S0);
    if (scratch == REG_T8)
        t8_cached_psx_reg = r;
    else if (scratch == REG_T9)
        t9_cached_psx_reg = r;
    return scratch;
}

int emit_dst_reg(int r, int scratch)
{
    if (r == 0)
        return REG_T8; /* Junk register if writing to $0 (T8 is scratch, not a dyn slot) */
    if (psx_pinned_reg[r])
        return psx_pinned_reg[r];
    if (dyn_slots_active)
    {
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
    /* Dynamic slot: dirty writeback (update slot reg, defer SW) */
    if (dyn_slots_active)
    {
        int si = dyn_slot_find(r);
        if (si >= 0)
        {
            if (dyn_slot_ee[si] != hwreg)
                EMIT_MOVE(dyn_slot_ee[si], hwreg);
            dyn_dirty_mask |= (1u << si);
            SCRATCH_INVALIDATE_PSX(r);
            return;
        }
    }
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
    /* Update cache: hwreg now holds cpu.regs[r] */
    if (hwreg == REG_T8)
    {
        t8_cached_psx_reg = r;
        if (t9_cached_psx_reg == r)
            t9_cached_psx_reg = -1;
    }
    else if (hwreg == REG_T9)
    {
        t9_cached_psx_reg = r;
        if (t8_cached_psx_reg == r)
            t8_cached_psx_reg = -1;
    }
}

void emit_sync_reg(int r, int host_reg)
{
    if (r == 0 || psx_pinned_reg[r])
        return;
    /* Dynamic slot: dirty writeback (update slot reg, defer SW) */
    if (dyn_slots_active)
    {
        int si = dyn_slot_find(r);
        if (si >= 0)
        {
            if (dyn_slot_ee[si] != host_reg)
                EMIT_MOVE(dyn_slot_ee[si], host_reg);
            dyn_dirty_mask |= (1u << si);
            SCRATCH_INVALIDATE_PSX(r);
            return;
        }
    }
    EMIT_SW(host_reg, CPU_REG(r), REG_S0);
    /* Update cache: host_reg now holds cpu.regs[r] */
    if (host_reg == REG_T8)
    {
        t8_cached_psx_reg = r;
        if (t9_cached_psx_reg == r)
            t9_cached_psx_reg = -1;
    }
    else if (host_reg == REG_T9)
    {
        t9_cached_psx_reg = r;
        if (t8_cached_psx_reg == r)
            t8_cached_psx_reg = -1;
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
    while (mask)
    {
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
                dyn_dirty_mask |= (1u << si);
                SCRATCH_INVALIDATE_PSX(r);
            }
            else
            {
                emit_load_imm32(REG_AT, vregs[r].value);
                EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
                SCRATCH_INVALIDATE_PSX(r);
            }
            vregs[r].is_dirty = 0;
        }
    }
}

/* Flush pinned PSX registers to cpu struct before JAL to C helpers.
 * This ensures cpu.regs[] is consistent for C code and exception handlers. */
void emit_flush_pinned(void)
{
    EMIT_SW(REG_S6, CPU_REG(28), REG_S0); /* PSX $gp */
    EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_SW(REG_S7, CPU_REG(30), REG_S0); /* PSX $fp */
    EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Reload pinned PSX registers from cpu struct after JAL returns.
 * C functions may have modified cpu.regs[] directly. */
void emit_reload_pinned(void)
{
    EMIT_LW(REG_S6, CPU_REG(28), REG_S0); /* PSX $gp */
    EMIT_LW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_LW(REG_S7, CPU_REG(30), REG_S0); /* PSX $fp */
    EMIT_LW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Selectively flush only pinned PSX regs in the given mask.
 * mask bit r = 1 means PSX reg r (which is pinned) should be flushed.
 * Bits for non-pinned regs are ignored.  Used with block_scan()'s
 * pinned_written_mask to skip flushing regs that were never written. */
void emit_flush_pinned_selective(uint32_t mask)
{
    if (mask & (1u << 28))
        EMIT_SW(REG_S6, CPU_REG(28), REG_S0); /* PSX $gp */
    if (mask & (1u << 29))
        EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    if (mask & (1u << 30))
        EMIT_SW(REG_S7, CPU_REG(30), REG_S0); /* PSX $fp */
    if (mask & (1u << 31))
        EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
}

/* Emit a JAL to a C helper function with pinned register sync.
 * Flushes pinned regs to cpu struct before call (for exception safety),
 * and reloads them after return (C code may have modified cpu.regs[]).
 * T0-T7 (dynamic slots) are clobbered by C ABI and reloaded from
 * cpu.regs[] after the call. Dirty-only flush is safe because the
 * reload picks up any changes the C helper made. */
void emit_call_c(uint32_t func_addr)
{
    block_full_calls++;
    flush_dirty_consts();
    dyn_flush_dirty_slots(); /* A: dirty-only — safe with reload */
    emit_load_imm32(REG_T8, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_addr);
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0); /* delay slot (P9) */
    dyn_reload_slots();
    reg_cache_invalidate();
    smrv_known_ram = (1u << 29);
    align_known_mask = ALIGN_PINNED_MASK;
}

/* Emit a JAL to a C helper that does NOT modify cpu.regs[].
 * Uses the lite trampoline which saves/restores T0-T7 to the block's
 * stack frame.  Only dirty dynamic slots are flushed to cpu.regs[]
 * before the call; the trampoline preserves slot register values.
 * NOTE: the ISC cache is now at offset 80(sp), so the T0 save at
 * 0(sp) no longer conflicts with it. */
void emit_call_c_lite(uint32_t func_addr)
{
    block_lite_calls++;
    flush_dirty_consts();
    dyn_flush_dirty_slots(); /* B: dirty-only — safe; T0-T7 preserved by lite tramp */
    emit_load_imm32(REG_T8, func_addr);
    EMIT_JAL_ABS((uint32_t)call_c_trampoline_lite_addr);
    EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0); /* delay slot (P9) */
    /* T0-T7 (dynamic slots) preserved by lite trampoline save/restore.
     * Pinned regs (S4-S7) preserved by C ABI. */
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
 * Generated code (variable length due to dirty slot flush):
 *   lw   at, CPU_BLOCK_ABORTED(s0) ; load abort flag from cpu struct
 *   beq  at, zero, @skip           ; no abort -> continue (patched offset)
 *   nop
 *   [sw × N dirty dyn slots]       ; flush dirty slots to cpu.regs[]
 *   addiu s2, s2, -cycles          ; deduct partial cycles
 *   j    abort_trampoline           ; abort -> shared epilogue
 *   nop
 * @skip:
 */
void emit_abort_check(uint32_t cycles)
{
    /* Use AT instead of T0 to avoid clobbering dynamic slot 0 */
    EMIT_LW(REG_AT, CPU_BLOCK_ABORTED, REG_S0); /* at = cpu.block_aborted */
    uint32_t *beq_insn = code_ptr;
    emit(MK_I(0x04, REG_AT, REG_ZERO, 0)); /* BEQ at, zero, +? (patched) */
    EMIT_NOP();

    /* Abort path: flush dirty dyn slots so cpu.regs[] is consistent.
     * Save/restore dyn_dirty_mask because the flush only runs on the
     * abort path — the normal (non-abort) path still has dirty slots. */
    uint8_t saved_dyn_mask = dyn_dirty_mask;
    dyn_flush_dirty_slots(); /* C: emit_abort_check — dirty-only */
    dyn_dirty_mask = saved_dyn_mask;

    /* Inside abort path: subtract only the cycles consumed up to this
     * instruction, not the full block total.  For deferred cold/TLB paths
     * this is the per-instruction cycle_offset stored at emit time. */
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)cycles); /* delay slot (P9) */

    /* Patch BEQ to skip entire abort path */
    int32_t skip_off = (int32_t)(code_ptr - beq_insn - 2);
    *beq_insn = MK_I(0x04, REG_AT, REG_ZERO, skip_off & 0xFFFF);
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
    else if ((int32_t)val >= -32768 && (int32_t)val < 0)
    {
        /* Sign-extendable negative: ADDIU from $zero (1 insn vs LUI+ORI) */
        EMIT_ADDIU(hwreg, REG_ZERO, (int16_t)val);
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
    /* Alignment: auto-set if this constant is word-aligned */
    if ((val & 3) == 0)
        align_known_mask |= (1u << r);
    else
        align_known_mask &= ~(1u << r);
}

void mark_vreg_var(int r)
{
    if (r == 0)
        return;
    /* SMRV: clear by default; callers that preserve RAM-ness
     * (e.g., ADDIU from known-RAM base) re-set it after this call. */
    smrv_known_ram &= ~(1u << r);
    /* Alignment: clear by default; callers that preserve alignment
     * (e.g., ADDIU with aligned offset) re-set it after this call. */
    align_known_mask &= ~(1u << r);
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
            dyn_dirty_mask |= (1u << si);
            SCRATCH_INVALIDATE_PSX(r);
        }
        else
        {
            emit_load_imm32(REG_AT, vregs[r].value);
            EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
            SCRATCH_INVALIDATE_PSX(r);
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
    smrv_known_ram = (1u << 29);          /* $sp always RAM */
    align_known_mask = ALIGN_PINNED_MASK; /* $gp/$sp/$fp/$ra always word-aligned */
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
    if (r == 0)
        return;
    if (psx_pinned_reg[r])
    {
        EMIT_LW(psx_pinned_reg[r], field_offset, REG_S0);
    }
    else if (dyn_slots_active && dyn_slot_find(r) >= 0)
    {
        int si = dyn_slot_find(r);
        EMIT_LW(dyn_slot_ee[si], field_offset, REG_S0);
        dyn_dirty_mask |= (1u << si); /* defer writeback */
    }
    else
    {
        EMIT_LW(REG_AT, field_offset, REG_S0);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        SCRATCH_INVALIDATE_PSX(r);
    }
}

/* Store an immediate value into a PSX register.  Uses AT for non-pinned. */
void emit_materialize_psx_imm(int r, uint32_t value)
{
    if (r == 0)
        return;
    if (psx_pinned_reg[r])
    {
        emit_load_imm32(psx_pinned_reg[r], value);
    }
    else if (dyn_slots_active && dyn_slot_find(r) >= 0)
    {
        int si = dyn_slot_find(r);
        emit_load_imm32(dyn_slot_ee[si], value);
        dyn_dirty_mask |= (1u << si); /* defer writeback */
    }
    else
    {
        emit_load_imm32(REG_AT, value);
        EMIT_SW(REG_AT, CPU_REG(r), REG_S0);
        SCRATCH_INVALIDATE_PSX(r);
    }
}

/* Store an immediate value into a cpu struct field.  Uses AT. */
void emit_imm_to_cpu_field(int field_offset, uint32_t value)
{
    emit_load_imm32(REG_AT, value);
    EMIT_SW(REG_AT, field_offset, REG_S0);
}
