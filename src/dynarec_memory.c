/*
 * dynarec_memory.c - Memory access emitters (LW/LH/LB/SW/SH/SB)
 *
 * Generates native code for PSX memory reads and writes with inline
 * fast-paths for aligned RAM access and slow-path C helper calls
 * for IO/BIOS/misaligned access.
 */
#include "dynarec.h"
#include "scheduler.h"
#include "psx_sio.h" /* SIO_Write/SIO_Read + extern sio_* vars */

/* gpu_state.h needed for const-address GPU_ReadStatus inline — silence LOG_TAG redef */
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"

/* GPU busy-until timestamp — when non-zero, GPU_ReadStatus needs to
 * fast-forward global_cycles.  JIT inline checks this for zero to
 * skip the expensive C call on the common "GPU idle" path. */
extern uint64_t gpu_busy_until;

/*
 * emit_flush_partial_cycles: Emit code to store the compile-time cycle
 * offset (emit_cycle_offset) into the partial_block_cycles global.
 * This allows timer reads/writes during C helper calls to see accurate
 * cycle counts instead of stale global_cycles.
 *
 * Uses REG_AT and REG_T1 (assembler temp, not pinned) as scratch.
 * Cost: 3 native instructions on the slow path.
 *
 * Note: LUI loads the upper half; SW/LW sign-extend the 16-bit offset.
 * We use the standard %hi/%lo split: hi = (addr + 0x8000) >> 16 to
 * compensate for sign extension of the lower half.
 */
void emit_flush_partial_cycles(void)
{
    uint32_t addr = (uint32_t)&partial_block_cycles;
    uint16_t lo = addr & 0xFFFF;
    uint16_t hi = (addr + 0x8000) >> 16;
    uint32_t val = emit_cycle_offset;
    EMIT_LUI(REG_AT, hi);
    EMIT_ADDIU(REG_T1, REG_ZERO, (int16_t)val);
    EMIT_SW(REG_T1, (int16_t)lo, REG_AT);
}

/* ================================================================
 *  Cold Slow Path Queue
 *
 *  Instead of emitting slow-path code inline (after the fast path),
 *  we defer it to the end of the block.  This allows the fast path to
 *  fall through without a branch, saving 2 instructions (b @done + nop)
 *  per memory access on the hot path (~95%+ of accesses hit the fast path).
 * ================================================================ */

#define MAX_COLD_SLOW 256

typedef struct {
    uint32_t *branches[4];  /* Branches to patch (alignment, range, ISC) */
    int       num_branches;
    uint32_t *return_point; /* Where to jump back after slow path */
    uint32_t  func_addr;    /* ReadWord/WriteWord/Helper_LWL/etc. */
    uint32_t  psx_pc;       /* PSX PC for cycle tracking */
    int16_t   cycle_offset; /* Cycle offset for flush */
    uint8_t   size;         /* 1/2/4 */
    uint8_t   is_signed;    /* Sign extension needed? */
    uint8_t   type;         /* 0=read, 1=write, 2=lwx, 3=swx */
    uint8_t   has_abort;    /* emit_abort_check after trampoline? */
    uint8_t   load_defer;   /* dynarec_load_defer at emission time */
    int       rt_psx;       /* PSX register for store result */
} ColdSlowEntry;

static ColdSlowEntry cold_queue[MAX_COLD_SLOW];
static int cold_count = 0;

/* Forward declaration — defined below with TLB backpatch infrastructure */
static int tlb_bp_count;

void cold_slow_reset(void)
{
    cold_count = 0;
    tlb_bp_count = 0;
}

/* ================================================================
 *  TLB Backpatch Slots
 *
 *  When TLB is active, each memory access emits a 3-insn fast path
 *  (and, addu, lw/sw).  If a TLB miss occurs at runtime, we backpatch
 *  the 'addu' to branch to a cold stub that does a range check first.
 *
 *  The stub tries the TLB fast path for RAM (range check pass) or
 *  falls through to the C helper for I/O.  Once patched, that JIT
 *  instruction never causes an exception again.
 *
 *  Layout of the global lookup table:
 *    tlb_bp_map[i].fault_insn  = EPC of the faulting lw/sw
 *    tlb_bp_map[i].addu_insn   = 'addu' to patch → 'b @stub'
 *    tlb_bp_map[i].stub        = address of the cold stub
 *
 *  The exception handler binary-searches this table by fault_insn.
 * ================================================================ */

/* Global lookup table (persists across blocks, grows monotonically
 * within the JIT code cache — reset when the cache is flushed). */
#define MAX_TLB_BP_MAP 4096
typedef struct {
    uint32_t fault_insn_addr;   /* EPC: address of the lw/sw instruction */
    uint32_t addu_insn_addr;    /* address of the 'addu' to patch */
    uint32_t stub_addr;         /* address of the cold patch stub */
} TLBBPMapEntry;
TLBBPMapEntry tlb_bp_map[MAX_TLB_BP_MAP];
int tlb_bp_map_count = 0;

/* Per-block queue of pending TLB backpatch entries (emitted at block end) */
#define MAX_TLB_BP 256
typedef struct {
    uint32_t *addu_insn;    /* Address of 'addu t1, t1, s1' in fast path */
    uint32_t *fault_insn;   /* Address of lw/sw in fast path */
    uint32_t *return_point; /* Instruction after fast path store-result */
    uint32_t  func_addr;    /* ReadWord/WriteWord/etc. */
    uint32_t  psx_pc;       /* PSX PC for cycle tracking */
    int16_t   cycle_offset; /* emit_cycle_offset at emit time */
    uint8_t   type;         /* 0=read, 1=write */
    uint8_t   size;         /* 1/2/4 */
    uint8_t   is_signed;    /* Sign extension needed? */
    uint8_t   load_defer;   /* dynarec_load_defer at emit time */
    int       rt_psx;       /* PSX register for read result store */
} TLBBPEntry;
static TLBBPEntry tlb_bp_queue[MAX_TLB_BP];
static int tlb_bp_count = 0;

void tlb_patch_emit_all(void)
{
    int i;
    for (i = 0; i < tlb_bp_count; i++)
    {
        TLBBPEntry *e = &tlb_bp_queue[i];
        uint32_t *stub_start = code_ptr;

        /* Register the stub address in the global map */
        if (tlb_bp_map_count < MAX_TLB_BP_MAP)
        {
            TLBBPMapEntry *m = &tlb_bp_map[tlb_bp_map_count++];
            m->fault_insn_addr = (uint32_t)e->fault_insn;
            m->addu_insn_addr  = (uint32_t)e->addu_insn;
            m->stub_addr       = (uint32_t)stub_start;
        }

        /* --- Range-checked fast path (RAM via TLB) --- */
        /* t1 already has phys = vaddr & 0x1FFFFFFF (computed by 'and' still in the hot path) */
        emit(MK_R(0, 0, REG_T1, REG_A0, 21, 0x02));     /* srl  a0, t1, 21      */
        uint32_t *io_branch = code_ptr;
        emit(MK_I(0x05, REG_A0, REG_ZERO, 0));           /* bne  a0, zero, @io   */
        EMIT_ADDU(REG_T1, REG_T1, REG_S1);               /* [delay] addu t1,t1,s1 */

        /* TLB RAM fast path: inline load/store */
        if (e->type == 0) /* read */
        {
            if (e->size == 4)
                EMIT_LW(REG_V0, 0, REG_T1);
            else if (e->size == 2)
            {
                if (e->is_signed) EMIT_LH(REG_V0, 0, REG_T1);
                else              EMIT_LHU(REG_V0, 0, REG_T1);
            }
            else
            {
                if (e->is_signed) EMIT_LB(REG_V0, 0, REG_T1);
                else              EMIT_LBU(REG_V0, 0, REG_T1);
            }
            /* Store result same as fast path */
            if (!e->load_defer)
                emit_store_psx_reg(e->rt_psx, REG_V0);
        }
        else /* write */
        {
            if (e->size == 4)      EMIT_SW(REG_T2, 0, REG_T1);
            else if (e->size == 2) EMIT_SH(REG_T2, 0, REG_T1);
            else                   EMIT_SB(REG_T2, 0, REG_T1);
        }

        /* Branch back to return point */
        {
            int32_t ret_off = (int32_t)(e->return_point - code_ptr - 1);
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, (uint16_t)(ret_off & 0xFFFF)));
            EMIT_NOP();
        }

        /* --- I/O slow path: call C helper --- */
        {
            int32_t io_off = (int32_t)(code_ptr - io_branch - 1);
            *io_branch = (*io_branch & 0xFFFF0000) | ((uint32_t)io_off & 0xFFFF);
        }

        EMIT_MOVE(REG_A0, REG_T0);
        if (e->type == 1) /* write */
            EMIT_MOVE(REG_A1, REG_T2);

        /* Flush partial cycles */
        emit_load_imm32(REG_T0, e->func_addr);
        emit_load_imm32(REG_T2, e->psx_pc);
        EMIT_ADDIU(REG_T1, REG_ZERO, (int16_t)e->cycle_offset);
        EMIT_JAL_ABS((uint32_t)mem_slow_trampoline_addr);
        EMIT_NOP();

        if (e->type == 0 && e->size >= 2)
            emit_abort_check((uint32_t)e->cycle_offset);

        /* Sign extension for signed loads */
        if (e->type == 0 && e->is_signed && e->size < 4)
        {
            int shift = (e->size == 1) ? 24 : 16;
            emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x00)); /* SLL */
            emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x03)); /* SRA */
        }

        if (e->type == 0 && !e->load_defer)
            emit_store_psx_reg(e->rt_psx, REG_V0);

        /* Branch back to return point */
        {
            int32_t ret_off = (int32_t)(e->return_point - code_ptr - 1);
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, (uint16_t)(ret_off & 0xFFFF)));
            EMIT_NOP();
        }
    }

    tlb_bp_count = 0;
}

/*
 * TLB_Backpatch: Called from the TLB trampoline after handling an I/O access.
 *
 * Patches the JIT code so that the faulting instruction pair (addu + lw/sw)
 * is replaced by a branch to the range-checked cold stub.  After patching,
 * future executions of that instruction will use the stub instead of
 * causing another TLB exception.
 *
 * Patching:
 *   addu t1,t1,s1  →  b @stub   (branch to range-checked stub)
 *   lw/sw          →  nop       (becomes delay slot of the branch)
 *
 * Returns 1 if patched, 0 if entry not found.
 */
int __attribute__((used)) TLB_Backpatch(uint32_t epc)
{
    int i;
    for (i = 0; i < tlb_bp_map_count; i++)
    {
        if (tlb_bp_map[i].fault_insn_addr == epc)
        {
            uint32_t *addu_p = (uint32_t *)tlb_bp_map[i].addu_insn_addr;
            uint32_t *fault_p = (uint32_t *)epc;
            uint32_t stub = tlb_bp_map[i].stub_addr;

            /* Patch addu → b @stub */
            int32_t offset = (int32_t)((stub - (uint32_t)addu_p - 4) >> 2);
            *addu_p = 0x10000000 | ((uint32_t)offset & 0xFFFF);

            /* Patch fault insn → nop (becomes delay slot of the branch) */
            *fault_p = 0x00000000;

            /* Flush caches so patched code takes effect */
            FlushCache(0);  /* writeback D-cache */
            FlushCache(2);  /* invalidate I-cache */

            return 1;
        }
    }
    return 0;
}

void cold_slow_emit_all(void)
{
    int i;
    for (i = 0; i < cold_count; i++)
    {
        ColdSlowEntry *e = &cold_queue[i];
        int j;

        /* Patch all branches to point to this cold slow path entry */
        for (j = 0; j < e->num_branches; j++)
        {
            int32_t off = (int32_t)(code_ptr - e->branches[j] - 1);
            *e->branches[j] = (*e->branches[j] & 0xFFFF0000)
                             | ((uint32_t)off & 0xFFFF);
        }

        /* Emit slow path: set up args and call mem_slow_trampoline */
        EMIT_MOVE(REG_A0, REG_T0); /* a0 = PSX address (always in T0) */

        if (e->type == 1 || e->type == 3)
            EMIT_MOVE(REG_A1, REG_T2); /* a1 = data (writes/SWX) */
        else if (e->type == 2)
            EMIT_MOVE(REG_A1, REG_V0); /* a1 = current RT (LWX merge) */

        emit_load_imm32(REG_T0, e->func_addr);
        emit_load_imm32(REG_T2, e->psx_pc);
        EMIT_ADDIU(REG_T1, REG_ZERO, (int16_t)e->cycle_offset);
        EMIT_JAL_ABS((uint32_t)mem_slow_trampoline_addr);
        EMIT_NOP();

        if (e->has_abort)
            emit_abort_check((uint32_t)e->cycle_offset);

        /* Sign extension for signed byte/halfword reads */
        if (e->type == 0 && e->is_signed && e->size < 4)
        {
            int shift = (e->size == 1) ? 24 : 16;
            emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x00)); /* SLL */
            emit(MK_R(0, 0, REG_V0, REG_V0, shift, 0x03)); /* SRA */
        }

        /* Store result for reads (duplicate of fast-path store) */
        if ((e->type == 0 || e->type == 2) && !e->load_defer)
            emit_store_psx_reg(e->rt_psx, REG_V0);

        /* Branch back to return point */
        {
            int32_t ret_off = (int32_t)(e->return_point - code_ptr - 1);
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, (uint16_t)(ret_off & 0xFFFF)));
            EMIT_NOP();
        }
    }

    cold_count = 0;
}

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
    reg_cache_invalidate();
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
            /* Flush lazy consts before conditional fast/slow split
             * to prevent compile-time dirty-flag leak across paths */
            flush_dirty_consts();
            /* Check gpu_busy_until == 0 (both halves) */
            emit_load_imm32(REG_T2, (uint32_t)&gpu_busy_until);
            EMIT_LW(REG_T1, 0, REG_T2);      /* t1 = low 32 bits  */
            EMIT_LW(REG_T2, 4, REG_T2);      /* t2 = high 32 bits */
            EMIT_OR(REG_T1, REG_T1, REG_T2); /* t1 = low | high   */
            uint32_t *gbu_slow = code_ptr;
            EMIT_BNE(REG_T1, REG_ZERO, 0); /* bne t1, $0, @slow */
            EMIT_NOP();

            /* Fast path: v0 = gpu_stat | forced_bits (including dynamic bit 25) */
            emit_load_imm32(REG_T2, (uint32_t)&gpu_stat);
            EMIT_LW(REG_V0, 0, REG_T2);
            /* Compute bit 25 (DMA data request) from dma_dir (bits 29-30).
             * Per psx-spx: dir=0→0, dir=1/2→1, dir=3→bit27.
             * Simplified: (dma_dir != 0) → 1.  Games poll this before DMA. */
            emit(MK_R(0, 0, REG_V0, REG_T1, 29, 0x02));       /* srl t1, v0, 29  */
            emit(MK_I(0x0C, REG_T1, REG_T1, 3));              /* andi t1, t1, 3  */
            emit(MK_R(0, REG_ZERO, REG_T1, REG_T1, 0, 0x2B)); /* sltu t1, $0, t1 */
            emit(MK_R(0, 0, REG_T1, REG_T1, 25, 0x00));       /* sll t1, t1, 25  */
            EMIT_LUI(REG_T2, 0x1400);
            EMIT_ORI(REG_T2, REG_T2, 0x2000);
            EMIT_OR(REG_T2, REG_T2, REG_T1); /* t2 = 0x14002000 | bit25 */
            EMIT_OR(REG_V0, REG_V0, REG_T2);

            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            uint32_t *fast_skip = code_ptr;
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @after */
            EMIT_NOP();

            /* Slow path: full C call with trampoline */
            int32_t soff_gbu = (int32_t)(code_ptr - gbu_slow - 1);
            *gbu_slow = (*gbu_slow & 0xFFFF0000) | ((uint32_t)soff_gbu & 0xFFFF);

            emit_flush_partial_cycles();
            emit_call_c_lite((uint32_t)GPU_ReadStatus);

            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);

            /* Patch fast-path branch */
            int32_t soff_fast = (int32_t)(code_ptr - fast_skip - 1);
            *fast_skip = (*fast_skip & 0xFFFF0000) | ((uint32_t)soff_fast & 0xFFFF);
            return;
        }

        /*
         * SIO register read (0x1F801040-0x1F80105E):
         * Call SIO_Read directly, skipping ReadWord/ReadHalf → ReadHardware.
         * This eliminates two dispatch levels for the SIO polling hot path.
         */
        if (phys >= 0x1F801040 && phys <= 0x1F80105E)
        {
            emit_load_imm32(REG_A0, phys);
            emit_flush_partial_cycles();
            emit_call_c_lite((uint32_t)SIO_Read);
            if (!dynarec_load_defer)
                emit_store_psx_reg(rt_psx, REG_V0);
            return;
        }
    }

    /* Fallback to generic emitter if address is not constant or not in RAM/SP */
    /* Compute effective address into REG_T0 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);

    /* Flush lazy consts before conditional fast/slow split
     * to prevent compile-time dirty-flag leak across paths */
    flush_dirty_consts();

    /*
     * Direct address fast path (cold slow path):
     *   [alignment check if size > 1]
     *   and    t1, t0, s3          # phys = addr & 0x1FFFFFFF (S3 pinned)
     *   srl    t2, t1, 21          # 0 if phys < 2MB
     *   bne    t2, zero, @cold     # range miss → deferred slow path
     *   addu   t1, t1, s1          # [delay] host = psx_ram + phys
     *   lw/lhu/lbu v0, 0(t1)
     *   [store result]             # fast path falls through — no b @done!
     * @cold: <emitted at end of block>
     */

    uint32_t *align_branch = NULL;
    if (size > 1)
    {
        /* Alignment check — use delay slot for phys mask.
         * BNE reads T1 before the delay slot AND overwrites it. */
        emit(MK_I(0x0C, REG_T0, REG_T1, size - 1));         /* andi  t1, t0, size-1 */
        align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0));              /* bne   t1, zero, @cold */
        emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* [delay] and t1, t0, s3 (phys) */
    }
    else
    {
        /* No alignment check for byte loads */
        emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* and  t1, t0, s3  (phys) */
    }

    /* Range check: skip if TLB is active (TLB handles non-RAM via exception) */
    uint32_t *range_branch = NULL;
    if (!psx_tlb_base)
    {
        emit(MK_R(0, 0, REG_T1, REG_T2, 21, 0x02));  /* srl  t2, t1, 21      */
        range_branch = code_ptr;
        emit(MK_I(0x05, REG_T2, REG_ZERO, 0));        /* bne  t2, zero, @cold */
    }
    uint32_t *bp_addu = code_ptr;  /* record for TLB backpatch */
    EMIT_ADDU(REG_T1, REG_T1, REG_S1);            /* [delay/inline] addu t1, t1, s1 */
    uint32_t *bp_fault = code_ptr; /* record: the load that may TLB-miss */
    if (size == 4)
        EMIT_LW(REG_V0, 0, REG_T1);
    else if (size == 2)
    {
        if (is_signed)
            EMIT_LH(REG_V0, 0, REG_T1); /* native signed halfword load */
        else
            EMIT_LHU(REG_V0, 0, REG_T1);
    }
    else
    {
        if (is_signed)
            EMIT_LB(REG_V0, 0, REG_T1); /* native signed byte load */
        else
            EMIT_LBU(REG_V0, 0, REG_T1);
    }

    /* Fast path falls through — store result immediately */
    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);

    /* Queue TLB backpatch entry when TLB is active (for runtime patching on miss) */
    if (psx_tlb_base && tlb_bp_count < MAX_TLB_BP)
    {
        TLBBPEntry *p = &tlb_bp_queue[tlb_bp_count++];
        p->addu_insn   = bp_addu;
        p->fault_insn  = bp_fault;
        p->return_point = code_ptr;
        p->func_addr = (size == 4) ? (uint32_t)ReadWord
                     : (size == 2) ? (uint32_t)ReadHalf
                                   : (uint32_t)ReadByte;
        p->psx_pc       = emit_current_psx_pc;
        p->cycle_offset = (int16_t)emit_cycle_offset;
        p->type         = 0; /* read */
        p->size         = (uint8_t)size;
        p->is_signed    = (uint8_t)is_signed;
        p->load_defer   = (uint8_t)dynarec_load_defer;
        p->rt_psx       = rt_psx;
    }

    /* Defer slow path to end of block (alignment miss only when TLB active) */
    if (align_branch || range_branch)
    {
        ColdSlowEntry *e = &cold_queue[cold_count++];
        e->num_branches = 0;
        if (align_branch)
            e->branches[e->num_branches++] = align_branch;
        if (range_branch)
            e->branches[e->num_branches++] = range_branch;
        e->return_point = code_ptr;
        e->func_addr = (size == 4) ? (uint32_t)ReadWord
                     : (size == 2) ? (uint32_t)ReadHalf
                                   : (uint32_t)ReadByte;
        e->psx_pc = emit_current_psx_pc;
        e->cycle_offset = (int16_t)emit_cycle_offset;
        e->size = (uint8_t)size;
        e->is_signed = (uint8_t)is_signed;
        e->type = 0; /* read */
        e->has_abort = (size >= 2) ? 1 : 0;
        e->load_defer = (uint8_t)dynarec_load_defer;
        e->rt_psx = rt_psx;
    }
    reg_cache_invalidate();
}

void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* emit_memory_read with is_signed=1 handles signed loads natively on the
     * Direct address fast path (lb/lh) and adds SLL/SRA sign extension on the slow path.
     * Both const-address and direct paths are fully handled internally. */
    emit_memory_read(size, rt_psx, rs_psx, offset, 1);
}

void emit_memory_write(int size, int rt_psx, int rs_psx, int16_t offset)
{
    reg_cache_invalidate();
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

            /* SMC detection: inline check of jit_l1_ram[page] before calling
             * the full handler.  Most pages have no compiled blocks, so the
             * inline NULL check (3-4 instrs) avoids the expensive trampoline
             * call (~30 instrs with reg save/restore) in the common case. */
            if (size == 4)
            {
                uint32_t page = phys >> 12;
                flush_dirty_consts();
                emit_load_imm32(REG_T0, (uint32_t)&jit_l1_ram[page]);
                EMIT_LW(REG_T0, 0, REG_T0);     /* t0 = jit_l1_ram[page] */
                uint32_t *beq_ptr = code_ptr;
                EMIT_BEQ(REG_T0, REG_ZERO, 0);   /* skip if NULL (placeholder) */
                EMIT_NOP();

                /* Only reached when page has compiled blocks */
                emit_load_imm32(REG_A0, phys);
                EMIT_SW(REG_S2, CPU_CYCLES_LEFT, REG_S0);
                emit_load_imm32(REG_T0, (uint32_t)jit_smc_handler);
                EMIT_JAL_ABS((uint32_t)call_c_trampoline_lite_addr);
                EMIT_NOP();

                /* Fixup BEQ target to skip the handler call */
                int32_t skip = (int32_t)(code_ptr - beq_ptr - 2);
                *beq_ptr = MK_I(4, REG_T0, REG_ZERO, skip & 0xFFFF);
            }
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
         * I_STAT (0x1F801070): cpu.i_stat &= data (acknowledge IRQs)
         * I_MASK (0x1F801074): cpu.i_mask = data & 0xFFFF07FF
         *
         * The SIO_CheckIRQ side effect for I_STAT writes is handled by
         * sio_irq_delay_cycle in the dynarec loop, so we can safely
         * inline the acknowledge-only logic here.
         */
        if (phys == 0x1F801070 && size == 4) /* I_STAT */
        {
            int data_reg = emit_use_reg(rt_psx, REG_T2);
            EMIT_LW(REG_T1, CPU_I_STAT, REG_S0);              /* t1 = cpu.i_stat */
            emit(MK_R(0, REG_T1, data_reg, REG_T1, 0, 0x24)); /* and t1, t1, data */
            EMIT_SW(REG_T1, CPU_I_STAT, REG_S0);              /* cpu.i_stat = t1 */
            return;
        }
        if (phys == 0x1F801074 && size == 4) /* I_MASK */
        {
            emit_load_psx_reg(REG_T2, rt_psx);
            emit_load_imm32(REG_T1, 0xFFFF07FF);
            emit(MK_R(0, REG_T2, REG_T1, REG_T2, 0, 0x24)); /* and t2, t2, t1 */
            EMIT_SW(REG_T2, CPU_I_MASK, REG_S0);
            return;
        }

        /*
         * SIO_DATA write fast-path (0x1F801040):
         * When !sio_selected (controller not asserted), the handler is
         * just sio_data=0xFF, sio_tx_pending=1.  Inline this to avoid
         * the full WriteByte→WriteHardware→SIO_Write call chain.
         * Fall to direct SIO_Write call when sio_selected (protocol active).
         */
        if (phys == 0x1F801040)
        {
            /* Flush lazy consts before conditional fast/slow split */
            flush_dirty_consts();
            /* Check sio_selected == 0 */
            emit_load_imm32(REG_T1, (uint32_t)&sio_selected);
            EMIT_LW(REG_T1, 0, REG_T1);
            uint32_t *sio_slow = code_ptr;
            EMIT_BNE(REG_T1, REG_ZERO, 0); /* bne t1, zero, @slow_sio */
            EMIT_NOP();

            /* Fast path: !sio_selected → 2 stores, no C call */
            emit_load_imm32(REG_T1, (uint32_t)&sio_data);
            EMIT_ORI(REG_T2, REG_ZERO, 0xFF);
            EMIT_SW(REG_T2, 0, REG_T1); /* sio_data = 0xFF */
            emit_load_imm32(REG_T1, (uint32_t)&sio_tx_pending);
            EMIT_ORI(REG_T2, REG_ZERO, 1);
            EMIT_SW(REG_T2, 0, REG_T1); /* sio_tx_pending = 1 */
            uint32_t *sio_fast_done = code_ptr;
            emit(MK_I(0x04, REG_ZERO, REG_ZERO, 0)); /* b @done */
            EMIT_NOP();

            /* @slow_sio: direct SIO_Write call (skip WriteWord+WriteHardware) */
            int32_t soff = (int32_t)(code_ptr - sio_slow - 1);
            *sio_slow = (*sio_slow & 0xFFFF0000) | ((uint32_t)soff & 0xFFFF);

            emit_load_imm32(REG_A0, phys);
            {
                int data_reg = emit_use_reg(rt_psx, REG_A1);
                if (data_reg != REG_A1)
                    EMIT_MOVE(REG_A1, data_reg);
            }
            emit_flush_partial_cycles();
            emit_call_c_lite((uint32_t)SIO_Write);

            /* @done: patch fast-path branch */
            int32_t doff = (int32_t)(code_ptr - sio_fast_done - 1);
            *sio_fast_done = (*sio_fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
            return;
        }

        /*
         * Other SIO register writes (0x1F801048-0x1F80105E):
         * Call SIO_Write directly, skipping WriteByte/WriteHalf→WriteHardware.
         */
        if (phys >= 0x1F801040 && phys <= 0x1F80105E)
        {
            emit_load_imm32(REG_A0, phys);
            {
                int data_reg = emit_use_reg(rt_psx, REG_A1);
                if (data_reg != REG_A1)
                    EMIT_MOVE(REG_A1, data_reg);
            }
            emit_flush_partial_cycles();
            emit_call_c_lite((uint32_t)SIO_Write);
            return;
        }
    }

    /* Compute effective address into REG_T0, data into REG_T2 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);
    emit_load_psx_reg(REG_T2, rt_psx); /* data value */

    /* Flush lazy consts before conditional fast/slow split
     * to prevent compile-time dirty-flag leak across paths */
    flush_dirty_consts();

    /* Cache Isolation check: if SR.IsC (bit 16) is set, writes to KUSEG/KSEG0
     * must be ignored (it's a cache invalidation, not a real RAM write).
     * Read SR, shift bit 16 to bit 0, test it; if set go to slow-path
     * (WriteWord handles kseg1 exception internally). */
    EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);      /* a0 = SR */
    emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
    emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
    uint32_t *isc_branch = code_ptr;
    emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne  a0, zero, @cold (IsC set) */
    EMIT_NOP();

    uint32_t *align_branch = NULL;
    if (size > 1)
    {
        emit(MK_I(0x0C, REG_T0, REG_T1, size - 1)); /* andi  t1, t0, size-1 */
        align_branch = code_ptr;
        emit(MK_I(0x05, REG_T1, REG_ZERO, 0));              /* bne   t1, zero, @cold */
        emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* [delay] and t1, t0, s3 (phys) */
    }
    else
    {
        emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* and  t1, t0, s3  (phys) */
    }

    /* Range check: skip if TLB active */
    uint32_t *range_branch = NULL;
    if (!psx_tlb_base)
    {
        emit(MK_R(0, 0, REG_T1, REG_A0, 21, 0x02)); /* srl  a0, t1, 21      */
        range_branch = code_ptr;
        emit(MK_I(0x05, REG_A0, REG_ZERO, 0));       /* bne  a0, zero, @cold */
    }
    uint32_t *bp_addu_w = code_ptr;  /* record for TLB backpatch */
    EMIT_ADDU(REG_T1, REG_T1, REG_S1);           /* [delay/inline] addu t1, t1, s1 */
    uint32_t *bp_fault_w = code_ptr; /* record: the store that may TLB-miss */
    if (size == 4)
        EMIT_SW(REG_T2, 0, REG_T1);
    else if (size == 2)
        EMIT_SH(REG_T2, 0, REG_T1);
    else
        EMIT_SB(REG_T2, 0, REG_T1);

    /* Fast path falls through — no b @done needed */

    /* Queue TLB backpatch entry when TLB is active */
    if (psx_tlb_base && tlb_bp_count < MAX_TLB_BP)
    {
        TLBBPEntry *p = &tlb_bp_queue[tlb_bp_count++];
        p->addu_insn   = bp_addu_w;
        p->fault_insn  = bp_fault_w;
        p->return_point = code_ptr;
        p->func_addr = (size == 4) ? (uint32_t)WriteWord
                     : (size == 2) ? (uint32_t)WriteHalf
                                   : (uint32_t)WriteByte;
        p->psx_pc       = emit_current_psx_pc;
        p->cycle_offset = (int16_t)emit_cycle_offset;
        p->type         = 1; /* write */
        p->size         = (uint8_t)size;
        p->is_signed    = 0;
        p->load_defer   = 0;
        p->rt_psx       = 0;
    }

    /* Defer slow path to end of block */
    {
        ColdSlowEntry *e = &cold_queue[cold_count++];
        e->num_branches = 0;
        e->branches[e->num_branches++] = isc_branch;
        if (align_branch)
            e->branches[e->num_branches++] = align_branch;
        if (range_branch)
            e->branches[e->num_branches++] = range_branch;
        e->return_point = code_ptr;
        e->func_addr = (size == 4) ? (uint32_t)WriteWord
                     : (size == 2) ? (uint32_t)WriteHalf
                                   : (uint32_t)WriteByte;
        e->psx_pc = emit_current_psx_pc;
        e->cycle_offset = (int16_t)emit_cycle_offset;
        e->size = (uint8_t)size;
        e->is_signed = 0;
        e->type = 1; /* write */
        e->has_abort = (size >= 2) ? 1 : 0;
        e->load_defer = 0;
        e->rt_psx = 0;
    }
    reg_cache_invalidate();
}

/*
 * emit_memory_lwx: Emit LWL/LWR with direct address fast path.
 *
 * The R5900 (EE) has native lwl/lwr instructions identical to R3000A.
 * Fast path: phys mask + range check → native lwl/lwr on host pointer.
 * Slow path: C helper (Helper_LWL / Helper_LWR).
 *
 * is_left: 1 = LWL, 0 = LWR
 */
void emit_memory_lwx(int is_left, int rt_psx, int rs_psx, int16_t offset, int use_load_delay)
{
    reg_cache_invalidate();
    /* Load current rt value (merge target) */
    if (use_load_delay)
        EMIT_LW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
    else
        emit_load_psx_reg(REG_V0, rt_psx);

    /* Compute effective address */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);

    /* Flush lazy consts before conditional fast/slow split */
    flush_dirty_consts();

    /* Direct address fast path: S3 = 0x1FFFFFFF, S1 = psx_ram or TLB base */
    emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* and  t1, t0, s3 (phys) */
    uint32_t *range_branch = NULL;
    if (!psx_tlb_base)
    {
        emit(MK_R(0, 0, REG_T1, REG_T2, 21, 0x02));    /* srl  t2, t1, 21 (range) */
        range_branch = code_ptr;
        emit(MK_I(0x05, REG_T2, REG_ZERO, 0));          /* bne  t2, zero, @cold */
    }
    EMIT_ADDU(REG_T1, REG_T1, REG_S1);                  /* [delay/inline] addu t1, t1, s1 */

    /* Fast path: native lwl/lwr on host address */
    if (is_left)
        EMIT_LWL(REG_V0, 0, REG_T1);
    else
        EMIT_LWR(REG_V0, 0, REG_T1);

    /* Fast path falls through — store result immediately */
    if (!dynarec_load_defer)
    {
        if (is_left)
            mark_vreg_var(rt_psx);
        emit_store_psx_reg(rt_psx, REG_V0);
    }

    /* Defer slow path to end of block (only if range check exists) */
    if (range_branch)
    {
        ColdSlowEntry *e = &cold_queue[cold_count++];
        e->num_branches = 0;
        e->branches[e->num_branches++] = range_branch;
        e->return_point = code_ptr;
        e->func_addr = is_left ? (uint32_t)Helper_LWL : (uint32_t)Helper_LWR;
        e->psx_pc = emit_current_psx_pc;
        e->cycle_offset = (int16_t)emit_cycle_offset;
        e->size = 4;
        e->is_signed = 0;
        e->type = 2; /* lwx */
        e->has_abort = 0;
        e->load_defer = (uint8_t)dynarec_load_defer;
        e->rt_psx = rt_psx;
    }
    reg_cache_invalidate();
}

/*
 * emit_memory_swx: Emit SWL/SWR with direct address fast path.
 *
 * Fast path: cache-isolation check → phys mask + range check → native swl/swr.
 * Slow path: C helper (Helper_SWL / Helper_SWR).
 *
 * is_left: 1 = SWL, 0 = SWR
 */
void emit_memory_swx(int is_left, int rt_psx, int rs_psx, int16_t offset)
{
    reg_cache_invalidate();
    /* Compute effective address into T0, data into T2 */
    emit_load_psx_reg(REG_T0, rs_psx);
    EMIT_ADDIU(REG_T0, REG_T0, offset);
    emit_load_psx_reg(REG_T2, rt_psx);

    /* Flush lazy consts before conditional fast/slow split */
    flush_dirty_consts();

    /* Cache Isolation check */
    EMIT_LW(REG_A0, CPU_COP0(12), REG_S0);
    emit(MK_R(0, 0, REG_A0, REG_A0, 16, 0x02)); /* srl  a0, a0, 16 */
    emit(MK_I(0x0C, REG_A0, REG_A0, 1));        /* andi a0, a0, 1 */
    uint32_t *isc_branch = code_ptr;
    emit(MK_I(0x05, REG_A0, REG_ZERO, 0)); /* bne  a0, zero, @slow */
    EMIT_NOP();

    /* Direct address fast path (S3 = 0x1FFFFFFF, S1 = TLB base or psx_ram) */
    emit(MK_R(0, REG_T0, REG_S3, REG_T1, 0, 0x24));    /* and  t1, t0, s3 (phys) */
    uint32_t *range_branch = NULL;
    if (!psx_tlb_base)
    {
        emit(MK_R(0, 0, REG_T1, REG_A0, 21, 0x02));    /* srl  a0, t1, 21 (range) */
        range_branch = code_ptr;
        emit(MK_I(0x05, REG_A0, REG_ZERO, 0));          /* bne  a0, zero, @cold */
    }
    EMIT_ADDU(REG_T1, REG_T1, REG_S1);                  /* [delay/inline] addu t1, t1, s1 */

    /* Fast path: native swl/swr on host address */
    if (is_left)
        EMIT_SWL(REG_T2, 0, REG_T1);
    else
        EMIT_SWR(REG_T2, 0, REG_T1);

    /* Fast path falls through — no b @done needed */

    /* Defer slow path to end of block */
    {
        ColdSlowEntry *e = &cold_queue[cold_count++];
        e->num_branches = 0;
        e->branches[e->num_branches++] = isc_branch;
        if (range_branch)
            e->branches[e->num_branches++] = range_branch;
        e->return_point = code_ptr;
        e->func_addr = is_left ? (uint32_t)Helper_SWL : (uint32_t)Helper_SWR;
        e->psx_pc = emit_current_psx_pc;
        e->cycle_offset = (int16_t)emit_cycle_offset;
        e->size = 4;
        e->is_signed = 0;
        e->type = 3; /* swx */
        e->has_abort = 0;
        e->load_defer = 0;
        e->rt_psx = 0;
    }
    reg_cache_invalidate();
}
