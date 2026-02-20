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
    [29] = REG_S4, /* PSX $sp → native $s4 */
    [30] = REG_S7, /* PSX $s8 → native $s7 */
    [31] = REG_S5, /* PSX $ra → native $s5 */
};

/* Load PSX register 'r' from cpu struct into hw reg 'hwreg' */
void emit_load_psx_reg(int hwreg, int r)
{
    if (r == 0)
    {
        EMIT_MOVE(hwreg, REG_ZERO); /* $0 is always 0 */
    }
    else if (psx_pinned_reg[r])
    {
        EMIT_MOVE(hwreg, psx_pinned_reg[r]); /* pinned register */
    }
    else
    {
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
    }
}

/* Store hw reg 'hwreg' to PSX register 'r' in cpu struct */
void emit_store_psx_reg(int r, int hwreg)
{
    if (r == 0)
        return; /* never write to $0 */
    if (psx_pinned_reg[r])
    {
        EMIT_MOVE(psx_pinned_reg[r], hwreg); /* pinned register */
        return;
    }
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
}

/* Flush pinned PSX registers to cpu struct before JAL to C helpers.
 * This ensures cpu.regs[] is consistent for C code and exception handlers. */
void emit_flush_pinned(void)
{
    EMIT_SW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_SW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
    EMIT_SW(REG_S6, CPU_REG(2), REG_S0);  /* PSX $v0 */
    EMIT_SW(REG_S7, CPU_REG(30), REG_S0); /* PSX $s8 */
}

/* Reload pinned PSX registers from cpu struct after JAL returns.
 * C functions may have modified cpu.regs[] directly. */
void emit_reload_pinned(void)
{
    EMIT_LW(REG_S4, CPU_REG(29), REG_S0); /* PSX $sp */
    EMIT_LW(REG_S5, CPU_REG(31), REG_S0); /* PSX $ra */
    EMIT_LW(REG_S6, CPU_REG(2), REG_S0);  /* PSX $v0 */
    EMIT_LW(REG_S7, CPU_REG(30), REG_S0); /* PSX $s8 */
}

/* Emit a JAL to a C helper function with pinned register sync.
 * Flushes pinned regs to cpu struct before call (for exception safety),
 * and reloads them after return (C code may have modified cpu.regs[]). */
void emit_call_c(uint32_t func_addr)
{
    emit_flush_pinned();
    EMIT_JAL_ABS((uint32_t)func_addr);
    EMIT_NOP();
    emit_reload_pinned();
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
void emit_abort_check(void)
{
    EMIT_LW(REG_T0, CPU_BLOCK_ABORTED, REG_S0); /* t0 = cpu.block_aborted */
    EMIT_BEQ(REG_T0, REG_ZERO, 3); /* skip next 2 instrs if zero */
    EMIT_NOP();
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
