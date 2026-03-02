/*
 * dynarec_compile.c - Block compiler, prologue/epilogue, analysis
 *
 * Contains the main compile_block() loop that translates PSX basic blocks
 * into native R5900 code, along with block prologue/epilogue generation,
 * cycle cost estimation, and load delay slot analysis.
 */
#include "dynarec.h"
#include "scheduler.h"

/* ---- Compile-time state ---- */
uint32_t blocks_compiled = 0;
uint32_t total_instructions = 0;
uint32_t block_cycle_count = 0;
uint32_t emit_cycle_offset = 0;
uint32_t emit_current_psx_pc = 0;
uint32_t block_pinned_dirty_mask = 0;
int dynarec_load_defer = 0;
int dynarec_lwx_pending = 0;

/* ---- Super-block fall-through continuation ----
 * When a conditional branch is encountered, instead of emitting both
 * taken and not-taken epilogues, we continue compiling the fall-through
 * path inline and defer the taken-path epilogue to cold code at the
 * end of the super-block.  This saves ~12 native instructions per
 * conditional branch fall-through (no cycle deduction, pc update,
 * abort check, or direct link needed). */
#define MAX_CONTINUATIONS 3
#define MAX_SUPER_INSNS 200

typedef struct {
    uint32_t *branch_insn;       /* BNE instruction to patch (forward ref) */
    uint32_t target_pc;          /* Branch target PC */
    uint32_t cycle_count;        /* Accumulated cycles at this branch point */
    RegStatus saved_vregs[32];   /* vreg state at branch point */
    uint32_t saved_dirty_mask;   /* dirty_const_mask at branch point */
} DeferredTakenEntry;

static DeferredTakenEntry deferred_taken[MAX_CONTINUATIONS];
static int deferred_taken_count = 0;

/* Emit all deferred taken-path epilogues (cold code at end of super-block) */
static void emit_deferred_taken_all(void)
{
    int i;
    for (i = 0; i < deferred_taken_count; i++)
    {
        DeferredTakenEntry *e = &deferred_taken[i];

        /* Patch the BNE forward reference to jump here */
        int32_t off = (int32_t)(code_ptr - e->branch_insn - 1);
        *e->branch_insn = (*e->branch_insn & 0xFFFF0000)
                         | ((uint32_t)off & 0xFFFF);

        /* Restore vreg state for this branch point */
        memcpy(vregs, e->saved_vregs, sizeof(vregs));
        dirty_const_mask = e->saved_dirty_mask;

        /* Emit standard branch epilogue inline */
        flush_dirty_consts();
        emit(MK_I(0x09, REG_S2, REG_S2, (int16_t)(-(int)e->cycle_count)));
        emit_load_imm32(REG_T0, e->target_pc);
        EMIT_SW(REG_T0, CPU_PC, REG_S0);
        emit(MK_I(0x07, REG_S2, REG_ZERO, 2)); /* BGTZ s2, +2 */
        EMIT_NOP();
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
        emit_direct_link(e->target_pc);
    }
    deferred_taken_count = 0;
}

/* ---- R3000A Instruction Cycle Cost Table ----
 * Most instructions are 1 cycle. Exceptions:
 *   MULT/MULTU: ~6 cycles (data-dependent, 6 is average)
 *   DIV/DIVU:   ~36 cycles
 *   Load (LW/LB/LH/LBU/LHU/LWL/LWR): 2 cycles (1 + load delay)
 *   Store (SW/SB/SH/SWL/SWR): 1 cycle
 *   Branch taken: 2 cycles (1 + delay slot, but delay slot counted separately)
 *   COP2 (GTE): variable, approximate by opcode
 */
uint32_t r3000a_cycle_cost(uint32_t opcode)
{
    uint32_t op = OP(opcode);
    uint32_t func = FUNC(opcode);

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x18:
            return 6; /* MULT  */
        case 0x19:
            return 6; /* MULTU */
        case 0x1A:
            return 2; /* DIV (was 36) */
        case 0x1B:
            return 2; /* DIVU (was 36) */
        default:
            return 1;
        }
    /* Loads: 2 cycles (includes load delay) */
    case 0x20:
        return 2; /* LB  */
    case 0x21:
        return 2; /* LH  */
    case 0x22:
        return 2; /* LWL */
    case 0x23:
        return 2; /* LW  */
    case 0x24:
        return 2; /* LBU */
    case 0x25:
        return 2; /* LHU */
    case 0x26:
        return 2; /* LWR */
    /* Stores: 1 cycle */
    case 0x28:
        return 1; /* SB  */
    case 0x29:
        return 1; /* SH  */
    case 0x2B:
        return 1; /* SW  */
    case 0x2A:
        return 1; /* SWL */
    case 0x2E:
        return 1; /* SWR */
    /* COP2 (GTE) commands */
    case 0x12: /* COP2 */
        /* GTE compute commands (bit 25 set) cost 1 CPU cycle to issue;
         * the GTE executes in parallel.  Stall is applied when reading
         * results (MFC2/CFC2) — see gte_stall_remaining tracking in
         * the compile loop. */
        return 1; /* Both compute and transfer instructions: 1 CPU cycle */
    /* Branches/Jumps */
    case 0x02:
        return 1; /* J    */
    case 0x03:
        return 1; /* JAL  */
    case 0x04:
        return 1; /* BEQ  */
    case 0x05:
        return 1; /* BNE  */
    case 0x06:
        return 1; /* BLEZ */
    case 0x07:
        return 1; /* BGTZ */
    case 0x01:
        return 1; /* REGIMM */
    /* COP0/COP1/COP3 */
    case 0x10:
        return 1; /* COP0 */
    case 0x11:
        return 1; /* COP1 */
    case 0x13:
        return 1; /* COP3 */
    /* LWC2/SWC2 */
    case 0x32:
        return 2; /* LWC2 */
    case 0x3A:
        return 1; /* SWC2 */
    default:
        return 1;
    }
}

/* GTE pipeline cycle count for COP2 compute commands.
 * Returns how many cycles the GTE hardware needs to produce results.
 * Used for stall tracking: if the CPU reads GTE results (MFC2/CFC2/LWC2)
 * before this many cycles have elapsed since the COP2 issue, the CPU
 * stalls for the remaining time. */
static uint32_t gte_pipeline_cycles(uint32_t opcode)
{
    uint32_t gte_op = opcode & 0x3F;
    switch (gte_op)
    {
    case 0x01:
        return 15; /* RTPS  */
    case 0x06:
        return 8; /* NCLIP */
    case 0x0C:
        return 6; /* OP    */
    case 0x10:
        return 8; /* DPCS  */
    case 0x11:
        return 8; /* INTPL */
    case 0x12:
        return 8; /* MVMVA */
    case 0x13:
        return 19; /* NCDS  */
    case 0x14:
        return 13; /* CDP   */
    case 0x16:
        return 44; /* NCDT  */
    case 0x1B:
        return 17; /* NCCS  */
    case 0x1C:
        return 11; /* CC    */
    case 0x1E:
        return 14; /* NCS   */
    case 0x20:
        return 30; /* NCT   */
    case 0x28:
        return 5; /* SQR   */
    case 0x29:
        return 8; /* DCPL  */
    case 0x2A:
        return 17; /* DPCT  */
    case 0x2D:
        return 5; /* AVSZ3 */
    case 0x2E:
        return 6; /* AVSZ4 */
    case 0x30:
        return 23; /* RTPT  */
    case 0x3D:
        return 5; /* GPF   */
    case 0x3E:
        return 5; /* GPL   */
    case 0x3F:
        return 39; /* NCCT  */
    default:
        return 8; /* Unknown GTE */
    }
}

/* Check if instruction reads a given GPR as source operand */
int instruction_reads_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0;
    int op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);

    if (rs == reg)
    {
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            if (func == 0x00 || func == 0x02 || func == 0x03)
                return 0; /* SLL, SRL, SRA */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI, MFLO */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL, BREAK */
        }
        if (op == 0x02 || op == 0x03)
            return 0; /* J, JAL */
        if (op == 0x0F)
            return 0; /* LUI */
        return 1;
    }

    if (rt == reg)
    {
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            if (func == 0x08 || func == 0x09)
                return 0; /* JR/JALR */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI/MFLO */
            if (func == 0x11 || func == 0x13)
                return 0; /* MTHI/MTLO */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL/BREAK */
            return 1;
        }
        if (op == 0x04 || op == 0x05)
            return 1; /* BEQ/BNE */
        if (op == 0x22 || op == 0x26)
            return 1; /* LWL/LWR */
        if (op >= 0x28 && op <= 0x2E)
            return 1; /* Stores */
        return 0;
    }

    return 0;
}

/* Check if instruction writes a given GPR as destination operand */
int instruction_writes_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0;
    int op = OP(opcode);
    if (op == 0x00)
    {
        int func = FUNC(opcode);
        if (func >= 0x18 && func <= 0x1B)
            return 0; /* MULT/MULTU/DIV/DIVU */
        if (func == 0x08)
            return 0; /* JR */
        if (func == 0x11 || func == 0x13)
            return 0; /* MTHI/MTLO */
        if (func == 0x0C || func == 0x0D)
            return 0; /* SYSCALL/BREAK */
        return (RD(opcode) == reg);
    }
    if (op >= 0x08 && op <= 0x0F)
        return (RT(opcode) == reg);
    if (op >= 0x20 && op <= 0x26)
        return (RT(opcode) == reg);
    if (op == 0x03)
        return (reg == 31);
    if (op == 0x00 && FUNC(opcode) == 0x09)
        return (RD(opcode) == reg);
    return 0;
}

/* ================================================================
 *  Dead Code Elimination (DCE) — backward liveness analysis
 * ================================================================ */

/* Get the destination GPR of an instruction (0 if none or writes $zero) */
static int dce_dest_gpr(uint32_t opcode)
{
    int op = OP(opcode);
    if (op == 0x00)
    {
        int func = FUNC(opcode);
        if (func >= 0x18 && func <= 0x1B) return 0; /* MULT/DIV: HI/LO only */
        if (func == 0x08) return 0;                  /* JR */
        if (func == 0x11 || func == 0x13) return 0;  /* MTHI/MTLO */
        if (func == 0x0C || func == 0x0D) return 0;  /* SYSCALL/BREAK */
        return RD(opcode);
    }
    if (op == 0x03) return 31;       /* JAL → $ra */
    if (op == 0x01)                  /* REGIMM */
    {
        int rt = RT(opcode);
        if (rt == 0x10 || rt == 0x11) return 31; /* BLTZAL/BGEZAL → $ra */
        return 0;
    }
    if (op >= 0x08 && op <= 0x0F) return RT(opcode); /* I-type ALU */
    /* Note: loads (0x20-0x26) are intentionally NOT listed here.
     * On PSX, loads write their dest via the load delay slot mechanism:
     * the value appears 1 instruction LATE.  If we reported loads as
     * killing the dest here, we would incorrectly mark the preceding
     * write as dead when the load delay read still needs the old value.
     * This is conservative (fewer DCE opportunities) but correct. */
    return 0;
}

/* Get bitmask of GPRs that an instruction reads (bit N = reads $N) */
static uint32_t dce_read_mask(uint32_t opcode)
{
    uint32_t m = 0;
    int op = OP(opcode);
    int rs = RS(opcode), rt = RT(opcode);

    if (op == 0x00)
    {
        int func = FUNC(opcode);
        /* Shifts by sa: only read rt */
        if (func <= 0x03) { if (rt) m |= (1u << rt); return m; }
        /* MFHI/MFLO: no GPR source */
        if (func == 0x10 || func == 0x12) return 0;
        /* SYSCALL/BREAK: no GPR source */
        if (func == 0x0C || func == 0x0D) return 0;
        /* JR/JALR: read rs only */
        if (func == 0x08 || func == 0x09) { if (rs) m |= (1u << rs); return m; }
        /* MTHI/MTLO: read rs only */
        if (func == 0x11 || func == 0x13) { if (rs) m |= (1u << rs); return m; }
        /* Everything else (ALU, MULT/DIV, SLLV etc.): read rs and rt */
        if (rs) m |= (1u << rs);
        if (rt) m |= (1u << rt);
        return m;
    }
    if (op == 0x02 || op == 0x03) return 0;  /* J/JAL */
    if (op == 0x0F) return 0;                 /* LUI */
    /* Branches: BEQ/BNE read rs+rt; BLEZ/BGTZ/REGIMM read rs */
    if (op == 0x04 || op == 0x05) { if (rs) m |= (1u << rs); if (rt) m |= (1u << rt); return m; }
    if (op >= 0x06 && op <= 0x07) { if (rs) m |= (1u << rs); return m; }
    if (op == 0x01) { if (rs) m |= (1u << rs); return m; }
    /* I-type ALU: read rs */
    if (op >= 0x08 && op <= 0x0E) { if (rs) m |= (1u << rs); return m; }
    /* Loads: read rs.  LWL/LWR also merge with rt */
    if (op >= 0x20 && op <= 0x26)
    {
        if (rs) m |= (1u << rs);
        if ((op == 0x22 || op == 0x26) && rt) m |= (1u << rt);
        return m;
    }
    /* Stores: read rs (base) + rt (data) */
    if ((op >= 0x28 && op <= 0x2E) || op == 0x3A)
    {
        if (rs) m |= (1u << rs);
        if (rt) m |= (1u << rt);
        return m;
    }
    /* COP0 MTC0: read rt */
    if (op == 0x10 && (RS(opcode) == 0x04)) { if (rt) m |= (1u << rt); return m; }
    /* COP2 MTC2/CTC2: read rt */
    if (op == 0x12 && !((opcode) & 0x02000000))
    {
        int cop_rs = RS(opcode);
        if (cop_rs == 0x04 || cop_rs == 0x06) { if (rt) m |= (1u << rt); }
        return m;
    }
    /* LWC2/SWC2: read rs */
    if (op == 0x32 || op == 0x3A) { if (rs) m |= (1u << rs); return m; }
    return m;
}

/* Returns 1 if instruction is a pure GPR-to-GPR operation with no side effects.
 * Only these can be safely eliminated by DCE. */
static int dce_is_pure(uint32_t opcode)
{
    int op = OP(opcode);
    if (op == 0x00)
    {
        int func = FUNC(opcode);
        if (func <= 0x07) return 1;                   /* SLL-SRAV */
        if (func == 0x10 || func == 0x12) return 1;   /* MFHI/MFLO */
        if (func >= 0x20 && func <= 0x2B) return 1;   /* ADD-SLTU */
        return 0;
    }
    if (op >= 0x08 && op <= 0x0F) return 1;  /* ADDI-LUI */
    return 0;
}

/* Returns bitmask of GPRs that 'opcode' may write.
 * More comprehensive than dce_dest_gpr — includes loads, MFC0/MFC2, etc.
 * Used for tracking which pinned regs need flush at block exit. */
static uint32_t scan_write_mask(uint32_t opcode)
{
    int op = OP(opcode);

    if (op == 0x00) { /* SPECIAL */
        int func = FUNC(opcode);
        if (func >= 0x18 && func <= 0x1B) return 0; /* MULT/DIV: HI/LO only */
        if (func == 0x08) return 0;                  /* JR */
        if (func == 0x11 || func == 0x13) return 0;  /* MTHI/MTLO */
        if (func == 0x0C || func == 0x0D) return 0;  /* SYSCALL/BREAK */
        int rd = RD(opcode);
        return rd ? (1u << rd) : 0;
    }
    if (op == 0x02) return 0;             /* J: no GPR write */
    if (op == 0x03) return (1u << 31);    /* JAL: writes $ra */
    if (op == 0x01) {                     /* REGIMM */
        int rt = RT(opcode);
        if (rt == 0x10 || rt == 0x11) return (1u << 31); /* BLTZAL/BGEZAL → $ra */
        return 0;
    }
    if (op >= 0x04 && op <= 0x07) return 0; /* Branches: no GPR write */
    /* I-type ALU: write rt */
    if (op >= 0x08 && op <= 0x0F) {
        int rt = RT(opcode);
        return rt ? (1u << rt) : 0;
    }
    /* Loads: write rt */
    if (op >= 0x20 && op <= 0x26) {
        int rt = RT(opcode);
        return rt ? (1u << rt) : 0;
    }
    /* Stores: no GPR write */
    if (op >= 0x28 && op <= 0x2E) return 0;
    /* COP0: MFC0 writes rt */
    if (op == 0x10) {
        if (RS(opcode) == 0x00) {
            int rt = RT(opcode);
            return rt ? (1u << rt) : 0;
        }
        return 0;
    }
    /* COP2: MFC2/CFC2 write rt */
    if (op == 0x12) {
        if (!(opcode & 0x02000000)) {
            int rs = RS(opcode);
            if (rs == 0x00 || rs == 0x02) {
                int rt = RT(opcode);
                return rt ? (1u << rt) : 0;
            }
        }
        return 0;
    }
    /* LWC2/SWC2: no GPR write */
    return 0;
}

/* ================================================================
 *  Block Scan — Pass 1 of the 2-pass compilation pipeline.
 *
 *  Performs: block boundary detection, backward liveness (DCE),
 *  and register usage analysis (read/write/pinned-dirty masks).
 *  Results are consumed by the emit pass (pass 2) in compile_block().
 * ================================================================ */
void block_scan(const uint32_t *code, int max_insns, BlockScanResult *out)
{
    /* Phase 1: find block boundary (branch + delay slot or SYSCALL/BREAK) */
    int count = 0;
    int in_ds = 0;
    for (int i = 0; i < max_insns && i < SCAN_MAX_INSNS; i++)
    {
        count = i + 1;
        if (in_ds) break;
        int op = OP(code[i]);
        int func = (op == 0) ? FUNC(code[i]) : 0;
        if (op == 0x02 || op == 0x03 ||
            (op == 0 && (func == 0x08 || func == 0x09)) ||
            (op >= 0x04 && op <= 0x07) || op == 0x01)
            in_ds = 1;
        else if (op == 0 && (func == 0x0C || func == 0x0D))
            break;
    }
    out->insn_count = count;

    /* Phase 2: forward pass — compute reg read/write masks */
    uint32_t written = 0, read = 0;
    for (int i = 0; i < count; i++)
    {
        written |= scan_write_mask(code[i]);
        read |= dce_read_mask(code[i]);
    }
    out->regs_written_mask = written;
    out->regs_read_mask = read;

    /* Compute pinned_written_mask: which pinned regs are written */
    uint32_t pinned_set = 0;
    for (int r = 0; r < 32; r++)
        if (psx_pinned_reg[r]) pinned_set |= (1u << r);
    out->pinned_written_mask = written & pinned_set;

    /* Phase 3: backward liveness analysis (DCE) */
    out->dce_dead_mask = 0;
    uint32_t live = 0xFFFFFFFFu; /* conservative: all regs live at block exit */
    for (int i = count - 1; i >= 0; i--)
    {
        uint32_t insn = code[i];
        int dest = dce_dest_gpr(insn);

        /* If dest is not live AND instruction is pure → dead */
        if (dest != 0 && !(live & (1u << dest)) && dce_is_pure(insn))
        {
            out->dce_dead_mask |= (1ULL << i);
            /* Don't update liveness for dead instructions */
        }
        else
        {
            /* Update liveness: kill dest, add reads */
            if (dest != 0)
                live &= ~(1u << dest);
            live |= dce_read_mask(insn);
        }
    }
}

/* ---- Block prologue: save callee-saved regs, set up $s0-$s3, load pinned ---- */
void emit_block_prologue(void)
{
    EMIT_ADDIU(REG_SP, REG_SP, -80);
    EMIT_SW(REG_RA, 44, REG_SP);
    EMIT_SW(REG_S0, 40, REG_SP);
    EMIT_SW(REG_S1, 36, REG_SP);
    EMIT_SW(REG_S2, 32, REG_SP);
    EMIT_SW(REG_S3, 28, REG_SP);
    EMIT_SW(REG_S4, 48, REG_SP);
    EMIT_SW(REG_S5, 52, REG_SP);
    EMIT_SW(REG_S6, 56, REG_SP);
    EMIT_SW(REG_S7, 60, REG_SP);
    EMIT_SW(REG_FP, 68, REG_SP);
    EMIT_MOVE(REG_S0, REG_A0); /* S0 = &cpu           */
    /* S1 = TLB-mapped VA base (0x20000000) if TLB active, else psx_ram */
    if (psx_tlb_base)
    {
        EMIT_LUI(REG_S1, psx_tlb_base >> 16);
    }
    else
    {
        EMIT_MOVE(REG_S1, REG_A1); /* S1 = psx_ram */
    }
    EMIT_MOVE(REG_S2, REG_A3); /* S2 = cycles_left    */
    /* Load physical address mask into S3: 0x1FFFFFFF */
    EMIT_LUI(REG_S3, 0x1FFF);
    EMIT_ORI(REG_S3, REG_S3, 0xFFFF);
    emit_reload_pinned();
}

/* ---- Block epilogue: flush pinned, restore and return ---- */
void emit_block_epilogue(void)
{
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
    EMIT_MOVE(REG_V0, REG_S2);
    emit_flush_pinned();
    EMIT_LW(REG_FP, 68, REG_SP);
    EMIT_LW(REG_S7, 60, REG_SP);
    EMIT_LW(REG_S6, 56, REG_SP);
    EMIT_LW(REG_S5, 52, REG_SP);
    EMIT_LW(REG_S4, 48, REG_SP);
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 80);
    EMIT_JR(REG_RA);
    EMIT_NOP();
}

void emit_branch_epilogue(uint32_t target_pc)
{
    /* Materialize any lazy constants before leaving the block */
    flush_dirty_consts();

    /* Calculate remaining cycles after this block */
    EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);

    /* Update cpu.pc IMMEDIATELY, before any potential abort check */
    emit_load_imm32(REG_T0, target_pc);
    EMIT_SW(REG_T0, CPU_PC, REG_S0);

    /* If remaining cycles <= 0, abort to C scheduler */
    emit(MK_I(0x07, REG_S2, REG_ZERO, 2)); /* BGTZ s2, +2 */
    EMIT_NOP();                            /* Delay slot */
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    EMIT_NOP(); /* Delay slot */

    /* Direct link to target block (bypassing prologue, maintaining stack and pinned regs) */
    emit_direct_link(target_pc);
}

/* ---- Compile a basic block ---- */
uint32_t *compile_block(uint32_t psx_pc)
{
    uint32_t *psx_code = get_psx_code_ptr(psx_pc);
    if (!psx_code)
    {
        DLOG("Cannot fetch code at PC=0x%08X\n", (unsigned)psx_pc);
        return NULL;
    }

    /* Check for code buffer overflow: reset if < 64KB remaining */
    uint32_t used = (uint32_t)((uint8_t *)code_ptr - (uint8_t *)code_buffer);
    if (used > CODE_BUFFER_SIZE - 65536)
    {
        DLOG("Code buffer nearly full (%u/%u), flushing cache\n",
             (unsigned)used, CODE_BUFFER_SIZE);
        code_ptr = code_buffer + 144;
        memset(code_buffer + 144, 0, CODE_BUFFER_SIZE - 144 * sizeof(uint32_t));
        Free_PageTable();
        memset(jit_l1_ram, 0, sizeof(jit_l1_ram));
        memset(jit_l1_bios, 0, sizeof(jit_l1_bios));
        memset(block_node_pool, 0, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
        block_node_pool_idx = 0;
        patch_sites_count = 0;
        blocks_compiled = 0;
        tlb_bp_map_count = 0;
        /* Clear hash table — all native pointers are now stale */
        for (int i = 0; i < JIT_HT_SIZE; i++)
        {
            jit_ht[i].psx_pc[0] = 0xFFFFFFFF;
            jit_ht[i].psx_pc[1] = 0xFFFFFFFF;
            jit_ht[i].native[0] = NULL;
            jit_ht[i].native[1] = NULL;
        }
        /* Full flush on buffer reset: all old icache lines are stale */
        FlushCache(0);
        FlushCache(2);
    }

    uint32_t *block_start = code_ptr;
    uint32_t cur_pc = psx_pc;
    uint32_t sub_block_start_pc = psx_pc; /* Base PC for DCE indexing within current sub-block */
    int continuations = 0;                /* Fall-through continuations in this super-block */
    block_cycle_count = 0;
    emit_cycle_offset = 0;
    deferred_taken_count = 0;

    if (blocks_compiled < 20)
    {
        DLOG("Compiling block at PC=0x%08X\n", (unsigned)psx_pc);
    }

    // Debug: Inspect hot loop
    if (psx_pc == 0x800509AC)
    {
        DLOG("Hot Loop dump at %08" PRIX32 ":\n", psx_pc);
        DLOG_RAW("  -4: %08" PRIX32 "\n", psx_code[-1]);
        DLOG_RAW("   0: %08" PRIX32 " (Hit)\n", psx_code[0]);
        DLOG_RAW("  +4: %08" PRIX32 "\n", psx_code[1]);
        DLOG_RAW("  +8: %08" PRIX32 "\n", psx_code[2]);
        DLOG_RAW(" +12: %08" PRIX32 "\n", psx_code[3]);
    }

    reset_vregs();
    cold_slow_reset();
    BlockScanResult scan;
    block_scan(psx_code, SCAN_MAX_INSNS, &scan);
    block_pinned_dirty_mask = scan.pinned_written_mask;
    emit_block_prologue();

    /* Inject BIOS HLE hooks natively so that DBL jumps do not bypass them.
     * Charge a nominal 10 cycles for HLE overhead (the block's instructions
     * haven't been compiled yet so block_cycle_count is still 0). */
    uint32_t phys_pc = psx_pc & 0x1FFFFFFF;
    if (phys_pc == 0xA0)
    {
        emit_call_c((uint32_t)BIOS_HLE_A);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -10);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }
    else if (phys_pc == 0xB0)
    {
        emit_call_c((uint32_t)BIOS_HLE_B);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -10);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }
    else if (phys_pc == 0xC0)
    {
        emit_call_c((uint32_t)BIOS_HLE_C);
        EMIT_BEQ(REG_V0, REG_ZERO, 3);
        EMIT_NOP(); /* Delay slot */
        EMIT_ADDIU(REG_S2, REG_S2, -10);
        EMIT_J_ABS((uint32_t)abort_trampoline_addr);
        EMIT_NOP();
    }

    int block_ended = 0;
    int in_delay_slot = 0;
    uint32_t branch_target = 0;
    int branch_type = 0; /* 0=none, 1=unconditional, 3=register, 4=conditional */

    /* Load delay slot tracking */
    int pending_load_reg = 0;
    int pending_load_apply_now = 0;
    int block_mult_count = 0;
    int gte_stall_remaining = 0; /* GTE pipeline stall tracker */

    while (!block_ended)
    {
        uint32_t opcode = *psx_code++;

        /* GTE stall model (PSX R3000A COP2 interlock):
         *
         * COP2 compute issues in 1 CPU cycle; the GTE pipeline runs in
         * parallel.  If a subsequent instruction reads GTE results
         * (MFC2/CFC2/LWC2) before the pipeline completes, the CPU
         * interlocks (stalls).
         *
         * Stall formula (derived from psxtest_gte expected values):
         *   gte_stall_remaining = GTE_cost  (set at cop2 compute)
         *   Each instruction (including cop2 itself) decrements by 1.
         *   At a read instruction: if remaining > 0, add (remaining + 1)
         *   to block_cycle_count.  The "+1" models the minimum 2-cycle
         *   hardware interlock penalty when any stall occurs.
         */
        uint32_t op_for_stall = OP(opcode);
        if (op_for_stall == 0x12 && (opcode & 0x02000000))
        {
            /* COP2 compute: if GTE is still busy from a previous COP2
             * compute, the CPU interlocks until it completes. */
            if (gte_stall_remaining > 0)
            {
                block_cycle_count += (uint32_t)(gte_stall_remaining + 1);
                gte_stall_remaining = 0;
            }
            /* Start new GTE pipeline countdown */
            gte_stall_remaining = (int)gte_pipeline_cycles(opcode);
        }
        else if (op_for_stall == 0x12 && !(opcode & 0x02000000))
        {
            /* COP2 data transfer: any GTE register access (MFC2/CFC2/
             * MTC2/CTC2) while GTE is busy causes an interlock stall. */
            uint32_t rs = RS(opcode);
            if ((rs == 0x00 || rs == 0x02 || rs == 0x04 || rs == 0x06) && gte_stall_remaining > 0)
            {
                block_cycle_count += (uint32_t)(gte_stall_remaining + 1);
                gte_stall_remaining = 0;
            }
        }
        else if (op_for_stall == 0x32) /* LWC2: loads GTE data reg */
        {
            if (gte_stall_remaining > 0)
            {
                block_cycle_count += (uint32_t)(gte_stall_remaining + 1);
                gte_stall_remaining = 0;
            }
        }

        block_cycle_count += r3000a_cycle_cost(opcode);
        emit_cycle_offset = block_cycle_count;

        /* Decrement GTE pipeline countdown after EVERY instruction,
         * including the COP2 compute instruction itself (the cop2 issue
         * cycle counts as 1 GTE pipeline cycle). */
        if (gte_stall_remaining > 0)
            gte_stall_remaining--;

        if (in_delay_slot)
        {
            /* Apply any pending load delay before the delay slot instruction */
            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }
            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            dynarec_load_defer = 0;
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            {
                int dce_idx = (int)((cur_pc - sub_block_start_pc) >> 2);
                if (dce_idx < SCAN_MAX_INSNS && (scan.dce_dead_mask >> dce_idx) & 1)
                {
                    /* Dead instruction — clear vreg tracking without emitting flush code */
                    int dce_d = dce_dest_gpr(opcode);
                    if (dce_d) {
                        vregs[dce_d].is_const = 0;
                        vregs[dce_d].is_dirty = 0;
                        dirty_const_mask &= ~(1u << dce_d);
                    }
                }
                else if (emit_instruction(opcode, cur_pc, &block_mult_count) < 0)
                {
                    block_ended = 1;
                    break;
                }
            }
            dynarec_lwx_pending = 0;
            cur_pc += 4;
            total_instructions++;

            if (pending_load_reg != 0)
            {
                emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                pending_load_reg = 0;
            }

            if (branch_type == 1)
            {
                emit_branch_epilogue(branch_target);
            }
            else if (branch_type == 4)
            {
                /* Conditional branch: check if we can continue compiling
                 * the fall-through path inline (super-block continuation).
                 * The taken path is deferred to cold code at end of block. */
                int can_continue = (continuations < MAX_CONTINUATIONS) &&
                                   ((cur_pc - psx_pc) < (MAX_SUPER_INSNS * 4)) &&
                                   (deferred_taken_count < MAX_CONTINUATIONS);

                if (can_continue)
                {
                    /* Emit BNE cond → @taken (forward ref, patched later) */
                    EMIT_LW(REG_T2, 72, REG_SP);
                    DeferredTakenEntry *dt = &deferred_taken[deferred_taken_count++];
                    dt->target_pc = branch_target;
                    dt->cycle_count = block_cycle_count;
                    memcpy(dt->saved_vregs, vregs, sizeof(vregs));
                    dt->saved_dirty_mask = dirty_const_mask;
                    dt->branch_insn = code_ptr;
                    emit(MK_I(0x05, REG_T2, REG_ZERO, 0)); /* BNE t2, zero, @taken */
                    EMIT_NOP();

                    /* Fall-through: continue compiling next sub-block.
                     * vregs carry over — const propagation across fall-through. */
                    sub_block_start_pc = cur_pc;
                    block_scan(psx_code, SCAN_MAX_INSNS, &scan);
                    continuations++;

                    /* Reset branch state for next sub-block */
                    in_delay_slot = 0;
                    branch_type = 0;
                    continue; /* resume main while loop */
                }
                else
                {
                    /* Standard two-path epilogue (no more continuations) */
                    EMIT_LW(REG_T2, 72, REG_SP);
                    uint32_t *bp = code_ptr;
                    emit(MK_I(0x05, REG_T2, REG_ZERO, 0)); /* BNE t2, zero, 0 */
                    EMIT_NOP();

                    RegStatus saved_vregs[32];
                    uint32_t saved_dirty_mask = dirty_const_mask;
                    memcpy(saved_vregs, vregs, sizeof(vregs));

                    /* Not taken: fall through PC */
                    emit_branch_epilogue(cur_pc);

                    /* Taken path target */
                    uint32_t *taken_addr = code_ptr;
                    int32_t offset = (int32_t)(taken_addr - bp - 1);
                    *bp = (*bp & 0xFFFF0000) | (offset & 0xFFFF);

                    memcpy(vregs, saved_vregs, sizeof(vregs));
                    dirty_const_mask = saved_dirty_mask;
                    emit_branch_epilogue(branch_target);
                }
            }
            else if (branch_type == 3)
            {
                /* Register jump (JR/JALR): Inline hash dispatch. */
                flush_dirty_consts();
                EMIT_LW(REG_T0, CPU_PC, REG_S0);
                EMIT_ADDIU(REG_S2, REG_S2, -(int16_t)block_cycle_count);
                EMIT_J_ABS((uint32_t)jump_dispatch_trampoline_addr);
                EMIT_NOP();
            }
            block_ended = 1;
            break;
        }

        uint32_t op = OP(opcode);

        /* Check for branch/jump instructions */
        if (op == 0x02 || op == 0x03)
        {
            if (op == 0x03)
            {
                mark_vreg_const(31, cur_pc + 8);
                emit_materialize_psx_imm(31, cur_pc + 8);
            }
            branch_target = ((cur_pc + 4) & 0xF0000000) | (TARGET(opcode) << 2);
            branch_type = 1;
            in_delay_slot = 1;
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x00 && (FUNC(opcode) == 0x08 || FUNC(opcode) == 0x09))
        {
            int rs = RS(opcode);
            int rd = (FUNC(opcode) == 0x09) ? RD(opcode) : 0;
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            /* Save current_pc so AdEL exception can set EPC = JR/JALR instr */
            emit_imm_to_cpu_field(CPU_CURRENT_PC, cur_pc);
            if (FUNC(opcode) == 0x09 && rd != 0)
            {
                mark_vreg_const(rd, cur_pc + 8);
                emit_materialize_psx_imm(rd, cur_pc + 8);
            }
            branch_type = 3;
            in_delay_slot = 1;
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07)
        {
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            /* --- Compile-time branch folding --- */
            int folded = 0;
            if (op == 0x04 || op == 0x05)
            {
                /* BEQ / BNE: both rs and rt must be known */
                if (is_vreg_const(rs) && is_vreg_const(rt))
                {
                    uint32_t vs = get_vreg_const(rs), vt = get_vreg_const(rt);
                    int taken = (op == 0x04) ? (vs == vt) : (vs != vt);
                    if (!taken)
                        branch_target = cur_pc + 8; /* fall through */
                    folded = 1;
                }
            }
            else if (op == 0x06 || op == 0x07)
            {
                /* BLEZ / BGTZ: only rs */
                if (is_vreg_const(rs))
                {
                    int32_t vs = (int32_t)get_vreg_const(rs);
                    int taken = (op == 0x06) ? (vs <= 0) : (vs > 0);
                    if (!taken)
                        branch_target = cur_pc + 8;
                    folded = 1;
                }
            }

            if (folded)
            {
                /* Resolved at compile time → unconditional branch */
                branch_type = 1;
                in_delay_slot = 1;
                if (pending_load_reg != 0)
                {
                    if (pending_load_apply_now)
                    {
                        emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                        pending_load_reg = 0;
                        pending_load_apply_now = 0;
                    }
                    else
                    {
                        pending_load_apply_now = 1;
                    }
                }
                cur_pc += 4;
                total_instructions++;
                continue;
            }

            /* --- Runtime conditional branch --- */
            emit_load_psx_reg(REG_T0, rs);
            if (op == 0x04 || op == 0x05)
            {
                emit_load_psx_reg(REG_T1, rt);
                emit(MK_R(0, REG_T0, REG_T1, REG_T2, 0, 0x26)); /* XOR t2, t0, t1 */
                if (op == 0x04)
                {
                    emit(MK_I(0x0B, REG_T2, REG_T2, 1)); /* SLTIU t2, t2, 1 */
                }
            }
            else if (op == 0x06)
            {
                emit(MK_I(0x0A, REG_T0, REG_T2, 1)); /* SLTI t2, t0, 1 */
            }
            else if (op == 0x07)
            {
                emit(MK_R(0, REG_ZERO, REG_T0, REG_T2, 0, 0x2A)); /* SLT t2, zero, t0 */
            }
            EMIT_SW(REG_T2, 72, REG_SP); /* save cond to stack across delay slot */

            branch_type = 4;
            in_delay_slot = 1;
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        if (op == 0x01)
        {
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            /* --- Compile-time folding for BLTZ/BGEZ/BLTZAL/BGEZAL --- */
            if (is_vreg_const(rs))
            {
                int32_t vs = (int32_t)get_vreg_const(rs);
                int taken = ((rt & 1) == 0) ? (vs < 0) : (vs >= 0);
                if (!taken)
                    branch_target = cur_pc + 8;
                /* Link variants still write $ra */
                if (rt == 0x10 || rt == 0x11)
                {
                    mark_vreg_const(31, cur_pc + 8);
                    emit_materialize_psx_imm(31, cur_pc + 8);
                }
                branch_type = 1;
                in_delay_slot = 1;
                if (pending_load_reg != 0)
                {
                    if (pending_load_apply_now)
                    {
                        emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                        pending_load_reg = 0;
                        pending_load_apply_now = 0;
                    }
                    else
                    {
                        pending_load_apply_now = 1;
                    }
                }
                cur_pc += 4;
                total_instructions++;
                continue;
            }

            /* --- Runtime path --- */
            emit_load_psx_reg(REG_T0, rs);

            if (rt == 0x10 || rt == 0x11)
            {
                mark_vreg_const(31, cur_pc + 8);
                emit_materialize_psx_imm(31, cur_pc + 8);
            }

            if ((rt & 1) == 0)
            {
                emit(MK_R(0, REG_T0, REG_ZERO, REG_T2, 0, 0x2A)); /* SLT t2, t0, zero */
            }
            else
            {
                emit(MK_R(0, REG_T0, REG_ZERO, REG_T2, 0, 0x2A));
                emit(MK_I(0x0E, REG_T2, REG_T2, 1)); /* XORI t2, t2, 1 */
            }
            EMIT_SW(REG_T2, 72, REG_SP); /* save cond to stack across delay slot */

            branch_type = 4;
            in_delay_slot = 1;
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    pending_load_apply_now = 1;
                }
            }
            cur_pc += 4;
            total_instructions++;
            continue;
        }

        /* Not a branch - emit with load delay slot handling */
        {
            int this_is_load = 0;
            int load_target = 0;
            uint32_t op_check = OP(opcode);

            if (op_check == 0x20 || op_check == 0x21 || op_check == 0x22 ||
                op_check == 0x23 || op_check == 0x24 || op_check == 0x25 ||
                op_check == 0x26)
            {
                load_target = RT(opcode);
                if (load_target != 0)
                {
                    uint32_t next_instr = *psx_code;
                    uint32_t next_op = OP(next_instr);
                    int next_rt = RT(next_instr);

                    if (instruction_reads_gpr(next_instr, load_target))
                    {
                        this_is_load = 1;
                    }
                    else if ((next_op >= 0x20 && next_op <= 0x26) &&
                             next_rt == load_target)
                    {
                        this_is_load = 1;
                    }
                }
            }

            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                if (this_is_load && load_target == pending_load_reg)
                {
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
            }

            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            dynarec_load_defer = this_is_load;
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            {
                int dce_idx = (int)((cur_pc - sub_block_start_pc) >> 2);
                if (dce_idx < SCAN_MAX_INSNS && (scan.dce_dead_mask >> dce_idx) & 1)
                {
                    /* Dead instruction — clear vreg tracking without emitting flush code */
                    int dce_d = dce_dest_gpr(opcode);
                    if (dce_d) {
                        vregs[dce_d].is_const = 0;
                        vregs[dce_d].is_dirty = 0;
                        dirty_const_mask &= ~(1u << dce_d);
                    }
                }
                else if (emit_instruction(opcode, cur_pc, &block_mult_count) < 0)
                {
                    block_ended = 1;
                    break;
                }
            }
            dynarec_lwx_pending = 0;
            dynarec_load_defer = 0;

            if (pending_load_reg != 0 && !this_is_load &&
                instruction_writes_gpr(opcode, pending_load_reg))
            {
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }

            if (this_is_load)
            {
                if (pending_load_reg != 0 && pending_load_reg != load_target)
                {
                    emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                }
                EMIT_SW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
                pending_load_reg = load_target;
                pending_load_apply_now = 0;
            }
        }
        cur_pc += 4;
        total_instructions++;

        if ((cur_pc - sub_block_start_pc) >= 256 || (cur_pc - psx_pc) >= (MAX_SUPER_INSNS * 4))
        {
            if (pending_load_reg != 0)
            {
                emit_cpu_field_to_psx_reg(CPU_LOAD_DELAY_VAL, pending_load_reg);
                pending_load_reg = 0;
            }
            emit_branch_epilogue(cur_pc);
            block_ended = 1;
        }
    }

    /* Emit deferred taken-path epilogues (super-block continuations) */
    emit_deferred_taken_all();

    /* Emit all deferred (cold) slow paths at the end of the block */
    cold_slow_emit_all();

    /* Emit TLB backpatch stubs (range-checked cold paths for TLB misses) */
    if (psx_tlb_base)
        tlb_patch_emit_all();

    if (blocks_compiled < 5)
    {
        int num_words = (int)(code_ptr - block_start);
        DLOG("Block %u at %p, %d words:\n",
             (unsigned)blocks_compiled, block_start, num_words);
        int j;
        for (j = 0; j < num_words && j < 32; j++)
        {
            DLOG_RAW("  [%02d] %p: 0x%08X\n", j, &block_start[j], (unsigned)block_start[j]);
        }
        if (num_words > 32)
            DLOG_RAW("  ... (%d more)\n", num_words - 32);
    }

    /* Cache flush done in run_jit_chain after apply_pending_patches. */

    blocks_compiled++;

    /* Detect idle/polling loops */
    {
        int is_idle = 0;
        int branch_kind = 0;
        if (branch_type == 1 && branch_target == psx_pc)
            branch_kind = 1;
        else if (branch_type == 4 && branch_target == psx_pc)
            branch_kind = 2;

        if (branch_kind)
        {
            is_idle = branch_kind;
            uint32_t *scan = get_psx_code_ptr(psx_pc);
            uint32_t scan_end = cur_pc;
            uint32_t spc = psx_pc;
            while (scan && spc < scan_end)
            {
                uint32_t inst = *scan++;
                int sop = OP(inst);
                if (sop == 0x28 || sop == 0x29 || sop == 0x2A ||
                    sop == 0x2B || sop == 0x2E || sop == 0x3A)
                {
                    is_idle = 0;
                    break;
                }
                if ((sop == 0x10 || sop == 0x12) && (RS(inst) == 4 || RS(inst) == 6))
                {
                    is_idle = 0;
                    break;
                }
                if (sop == 0 && (FUNC(inst) == 0x0C || FUNC(inst) == 0x0D))
                {
                    is_idle = 0;
                    break;
                }
                spc += 4;
            }
        }

        BlockEntry *be = cache_block(psx_pc, block_start);
        if (be)
        {
            uint32_t block_instr_count = (cur_pc - psx_pc) / 4;
            be->instr_count = block_instr_count;
            be->native_count = (uint32_t)(code_ptr - block_start);
            be->cycle_count = block_cycle_count > 0 ? block_cycle_count : block_instr_count;
            be->is_idle = is_idle;
            /* Hash all PSX opcodes for self-modifying code detection */
            uint32_t *opcodes = get_psx_code_ptr(psx_pc);
            uint32_t hash = 0;
            if (opcodes)
            {
                for (uint32_t i = 0; i < block_instr_count; i++)
                    hash = (hash << 5) + hash + opcodes[i]; /* djb2 */
            }
            be->code_hash = hash;
        }
    }

    return block_start;
}
