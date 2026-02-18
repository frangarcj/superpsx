/*
 * SuperPSX Dynarec - MIPS-to-MIPS Block Compiler
 *
 * Since R3000A and R5900 share instruction encoding, most instructions
 * are handled by loading PSX registers from a struct, executing the
 * operation natively, and storing results back. Memory operations
 * go through C helper functions (ReadWord/WriteWord).
 *
 * Register convention in generated code:
 *   $s0 = pointer to R3000CPU struct
 *   $s1 = pointer to psx_ram
 *   $s2 = pointer to psx_bios
 *   $t0-$t9, $v0/$v1, $a0-$a3 = temporaries
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kernel.h>
#include "superpsx.h"
#include "scheduler.h"
#include "loader.h"

#define LOG_TAG "DYNAREC"

#ifdef ENABLE_HOST_LOG
FILE *host_log_file = NULL;
#endif

/* ---- Code buffer ---- */
#define CODE_BUFFER_SIZE (4 * 1024 * 1024)
static uint32_t *code_buffer;
static uint32_t *code_ptr;

/* ---- Block cache ---- */
#define BLOCK_CACHE_BITS 14
#define BLOCK_CACHE_SIZE (1 << BLOCK_CACHE_BITS)
#define BLOCK_CACHE_MASK (BLOCK_CACHE_SIZE - 1)
/* Overflow node pool for collision chaining (max extra nodes) */
#define BLOCK_NODE_POOL_SIZE 4096

typedef struct BlockEntry
{
    uint32_t psx_pc;
    uint32_t *native;
    uint32_t instr_count;    /* Number of PSX instructions in this block */
    uint32_t cycle_count;    /* Weighted R3000A cycle count for this block */
    struct BlockEntry *next; /* Collision chain pointer */
} BlockEntry;

static BlockEntry *block_cache;     /* Primary hash buckets [BLOCK_CACHE_SIZE] */
static BlockEntry *block_node_pool; /* Overflow pool for chaining */
static int block_node_pool_idx = 0; /* Next free node in pool */

/* ---- Direct Block Linking (Back-Patching) ---- */
/* Declarations only - definitions appear after the emit helpers */
#define PATCH_SITE_MAX 8192
typedef struct
{
    uint32_t *site_word; /* Address of the J instruction to overwrite */
    uint32_t target_psx_pc;
} PatchSite;

static PatchSite patch_sites[PATCH_SITE_MAX];
static int patch_sites_count = 0;
#ifdef ENABLE_DYNAREC_STATS
static uint64_t stat_dbl_patches = 0; /* # of back-patches applied */
#endif

/* Forward declarations (defined after emit helpers) */
static void emit_direct_link(uint32_t target_psx_pc);
static void apply_pending_patches(uint32_t target_psx_pc, uint32_t *native_addr);
static uint32_t *lookup_block_native(uint32_t psx_pc);

/* ---- Instruction encoding helpers ---- */
#define OP(x) (((x) >> 26) & 0x3F)
#define RS(x) (((x) >> 21) & 0x1F)
#define RT(x) (((x) >> 16) & 0x1F)
#define RD(x) (((x) >> 11) & 0x1F)
#define SA(x) (((x) >> 6) & 0x1F)
#define FUNC(x) ((x) & 0x3F)
#define IMM16(x) ((x) & 0xFFFF)
#define SIMM16(x) ((int16_t)((x) & 0xFFFF))
#define TARGET(x) ((x) & 0x03FFFFFF)

/* Emit a 32-bit instruction to code buffer */
static inline void emit(uint32_t inst)
{
    *code_ptr++ = inst;
}

/* MIPS instruction builders */
#define MK_R(op, rs, rt, rd, sa, fn) \
    ((((uint32_t)(op)) << 26) | (((uint32_t)(rs)) << 21) | (((uint32_t)(rt)) << 16) | (((uint32_t)(rd)) << 11) | (((uint32_t)(sa)) << 6) | ((uint32_t)(fn)))
#define MK_I(op, rs, rt, imm) \
    ((((uint32_t)(op)) << 26) | (((uint32_t)(rs)) << 21) | (((uint32_t)(rt)) << 16) | ((uint32_t)((imm) & 0xFFFF)))
#define MK_J(op, tgt) \
    ((((uint32_t)(op)) << 26) | ((uint32_t)((tgt) & 0x03FFFFFF)))

/* Common emitters using $t0-$t3 as temps */
#define EMIT_NOP() emit(0)
#define EMIT_LW(rt, off, base) emit(MK_I(0x23, (base), (rt), (off)))
#define EMIT_SW(rt, off, base) emit(MK_I(0x2B, (base), (rt), (off)))
#define EMIT_LH(rt, off, base) emit(MK_I(0x21, (base), (rt), (off)))
#define EMIT_LHU(rt, off, base) emit(MK_I(0x25, (base), (rt), (off)))
#define EMIT_LB(rt, off, base) emit(MK_I(0x20, (base), (rt), (off)))
#define EMIT_LBU(rt, off, base) emit(MK_I(0x24, (base), (rt), (off)))
#define EMIT_SH(rt, off, base) emit(MK_I(0x29, (base), (rt), (off)))
#define EMIT_SB(rt, off, base) emit(MK_I(0x28, (base), (rt), (off)))
#define EMIT_ADDIU(rt, rs, imm) emit(MK_I(0x09, (rs), (rt), (imm)))
#define EMIT_ADDU(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x21))
#define EMIT_OR(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x25))
#define EMIT_LUI(rt, imm) emit(MK_I(0x0F, 0, (rt), (imm)))
#define EMIT_ORI(rt, rs, imm) emit(MK_I(0x0D, (rs), (rt), (imm)))
#define EMIT_MOVE(rd, rs) EMIT_ADDU(rd, rs, 0)
#define EMIT_JR(rs) emit(MK_R(0, (rs), 0, 0, 0, 0x08))
#define EMIT_JAL_ABS(addr) emit(MK_J(3, (uint32_t)(addr) >> 2))
#define EMIT_J_ABS(addr) emit(MK_J(2, (uint32_t)(addr) >> 2))
#define EMIT_BEQ(rs, rt, off) emit(MK_I(4, (rs), (rt), (off)))
#define EMIT_BNE(rs, rt, off) emit(MK_I(5, (rs), (rt), (off)))

/* Hardware register IDs used in generated code:
 *   $s0 (16) = cpu struct ptr
 *   $s1 (17) = psx_ram ptr
 *   $s2 (18) = psx_bios ptr
 *   $t0 (8)  = temp0
 *   $t1 (9)  = temp1
 *   $t2 (10) = temp2
 *   $a0 (4)  = arg0 for function calls
 *   $a1 (5)  = arg1 for function calls
 *   $v0 (2)  = return value from functions
 */
#define REG_S0 16
#define REG_S1 17
#define REG_S2 18
#define REG_S3 19
#define REG_T0 8
#define REG_T1 9
#define REG_T2 10
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6
#define REG_V0 2
#define REG_RA 31
#define REG_SP 29
#define REG_ZERO 0

/* Load PSX register 'r' from cpu struct into hw reg 'hwreg' */
static void emit_load_psx_reg(int hwreg, int r)
{
    if (r == 0)
    {
        EMIT_MOVE(hwreg, REG_ZERO); /* $0 is always 0 */
    }
    else
    {
        EMIT_LW(hwreg, CPU_REG(r), REG_S0);
    }
}

/* Store hw reg 'hwreg' to PSX register 'r' in cpu struct */
static void emit_store_psx_reg(int r, int hwreg)
{
    if (r == 0)
        return; /* never write to $0 */
    EMIT_SW(hwreg, CPU_REG(r), REG_S0);
}

/* Load 32-bit immediate into hw register */
static void emit_load_imm32(int hwreg, uint32_t val)
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

/* ---- Direct Block Linking helper implementations ---- */
/*
 * lookup_block_native: returns native pointer for psx_pc or NULL.
 * Defined here (before it is used by emit_direct_link) even though the
 * full lookup_block (which updates stats) is defined later.
 */
static uint32_t *lookup_block_native(uint32_t psx_pc)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *e = &block_cache[idx];
    while (e)
    {
        if (e->native && e->psx_pc == psx_pc)
            return e->native;
        e = e->next;
    }
    return NULL;
}

/*
 * emit_direct_link: at the end of a block epilogue, emit a J to the
 * native code of target_psx_pc.  If not compiled yet, emit a J to the
 * slow-path trampoline (code_buffer[0]) and record a patch site.
 */
static void emit_direct_link(uint32_t target_psx_pc)
{
    uint32_t *native = lookup_block_native(target_psx_pc);
    if (native)
    {
        /* Target already compiled: jump directly, skipping C overhead */
        EMIT_J_ABS((uint32_t)native);
        EMIT_NOP();
        return;
    }

    /* Target not compiled yet: record patch site and J to slow-path trampoline */
    if (patch_sites_count < PATCH_SITE_MAX)
    {
        PatchSite *ps = &patch_sites[patch_sites_count++];
        ps->site_word = code_ptr;
        ps->target_psx_pc = target_psx_pc;
    }
    /* J to slow-path trampoline (code_buffer[0] = JR $ra) */
    EMIT_J_ABS((uint32_t)code_buffer);
    EMIT_NOP();
}

/* apply_pending_patches: back-patch all J stubs waiting for target_psx_pc. */
static void apply_pending_patches(uint32_t target_psx_pc, uint32_t *native_addr)
{
    int i, j;
    for (i = 0, j = 0; i < patch_sites_count; i++)
    {
        PatchSite *ps = &patch_sites[i];
        if (ps->target_psx_pc == target_psx_pc)
        {
            uint32_t j_target = ((uint32_t)native_addr >> 2) & 0x03FFFFFF;
            *ps->site_word = MK_J(2, j_target);
#ifdef ENABLE_DYNAREC_STATS
            stat_dbl_patches++;
#endif
        }
        else
        {
            patch_sites[j++] = patch_sites[i];
        }
    }
    patch_sites_count = j;
}

/* ---- Temp buffer for IO code execution ---- */
static uint32_t io_code_buffer[64];

/* ---- Get pointer to PSX code in EE memory ---- */
static uint32_t *get_psx_code_ptr(uint32_t psx_pc)
{
    uint32_t phys = psx_pc & 0x1FFFFFFF;
    if (phys < PSX_RAM_SIZE)
        return (uint32_t *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return (uint32_t *)(psx_bios + (phys - 0x1FC00000));

    /* IO regions that support instruction fetch:
     *   DMA registers  (0x1F801080-0x1F8010FF)
     *   SPU registers  (0x1F801C00-0x1F801FFF)
     * These preserve written values, so the CPU can execute from them.
     * Other IO regions (scratchpad, MDEC, interrupt controller, etc.)
     * trigger an Instruction Bus Error on the real PS1.
     */
    if ((phys >= 0x1F801080 && phys < 0x1F801100) ||
        (phys >= 0x1F801C00 && phys < 0x1F802000))
    {
        int i;
        memset(io_code_buffer, 0, sizeof(io_code_buffer));
        for (i = 0; i < 64; i++)
        {
            uint32_t addr = psx_pc + i * 4;
            uint32_t a_phys = addr & 0x1FFFFFFF;
            /* Stay within the same IO region */
            if (!((a_phys >= 0x1F801080 && a_phys < 0x1F801100) ||
                  (a_phys >= 0x1F801C00 && a_phys < 0x1F802000)))
                break;
            io_code_buffer[i] = ReadWord(addr);
        }
        return io_code_buffer;
    }

    return NULL;
}

/* ---- Forward declarations ---- */
static void emit_block_prologue(void);
static void emit_block_epilogue(void);
static void emit_instruction(uint32_t opcode, uint32_t psx_pc);
static void emit_branch_epilogue(uint32_t target_pc);
static void emit_memory_read(int size, int rt_psx, int rs_psx, int16_t offset);
static void emit_memory_write(int size, int rt_psx, int rs_psx, int16_t offset);

/* Track instruction count for logging */
static uint32_t blocks_compiled = 0;
static uint32_t total_instructions = 0;

/* ---- Dynarec Performance Counters (Baseline) ---- */
#ifdef ENABLE_DYNAREC_STATS
static uint64_t stat_cache_hits = 0;       /* lookup_block found a block */
static uint64_t stat_cache_misses = 0;     /* lookup_block returned NULL -> compile */
static uint64_t stat_cache_collisions = 0; /* hash slot had different psx_pc */
static uint64_t stat_blocks_executed = 0;  /* total block executions */
static uint64_t stat_total_cycles = 0;     /* accumulated PSX cycles */
#endif

static void dynarec_print_stats(void)
{
#ifdef ENABLE_DYNAREC_STATS
    uint64_t total_lookups = stat_cache_hits + stat_cache_misses;
    DLOG("[DYNAREC STATS]\n");
    DLOG_RAW("  Blocks executed : %llu\n", (unsigned long long)stat_blocks_executed);
    DLOG_RAW("  Cache hits      : %llu (%.1f%%)\n",
           (unsigned long long)stat_cache_hits,
           total_lookups ? (double)stat_cache_hits * 100.0 / total_lookups : 0.0);
    DLOG_RAW("  Cache misses    : %llu (compiles)\n", (unsigned long long)stat_cache_misses);
    DLOG_RAW("  Cache collisions: %llu\n", (unsigned long long)stat_cache_collisions);
    DLOG_RAW("  Blocks compiled : %u\n", (unsigned)blocks_compiled);
    DLOG_RAW("  PSX cycles      : %llu\n", (unsigned long long)stat_total_cycles);
    DLOG_RAW("  DBL patches     : %llu\n", (unsigned long long)stat_dbl_patches);
    DLOG_RAW("  DBL pending     : %d\n", patch_sites_count);
    fflush(stdout);
#endif
}

/* Accumulated cycle cost during compile_block */
static uint32_t block_cycle_count = 0;

/* ---- R3000A Instruction Cycle Cost Table ----
 * Most instructions are 1 cycle. Exceptions:
 *   MULT/MULTU: ~6 cycles (data-dependent, 6 is average)
 *   DIV/DIVU:   ~36 cycles
 *   Load (LW/LB/LH/LBU/LHU/LWL/LWR): 2 cycles (1 + load delay)
 *   Store (SW/SB/SH/SWL/SWR): 1 cycle
 *   Branch taken: 2 cycles (1 + delay slot, but delay slot counted separately)
 *   COP2 (GTE): variable, approximate by opcode
 */
static uint32_t r3000a_cycle_cost(uint32_t opcode)
{
    uint32_t op = OP(opcode);
    uint32_t func = FUNC(opcode);

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x18: /* MULT  */
            return 6;
        case 0x19: /* MULTU */
            return 6;
        case 0x1A: /* DIV   */
            return 36;
        case 0x1B: /* DIVU  */
            return 36;
        default:
            return 1;
        }
    /* Loads: 2 cycles (includes load delay) */
    case 0x20: /* LB  */
        return 2;
    case 0x21: /* LH  */
        return 2;
    case 0x22: /* LWL */
        return 2;
    case 0x23: /* LW  */
        return 2;
    case 0x24: /* LBU */
        return 2;
    case 0x25: /* LHU */
        return 2;
    case 0x26: /* LWR */
        return 2;
    /* Stores: 1 cycle */
    case 0x28: /* SB  */
        return 1;
    case 0x29: /* SH  */
        return 1;
    case 0x2B: /* SW  */
        return 1;
    case 0x2A: /* SWL */
        return 1;
    case 0x2E: /* SWR */
        return 1;
    /* COP2 (GTE) commands */
    case 0x12: /* COP2 */
        if (opcode & 0x02000000)
        {
            /* GTE command - approximate cycle cost by command */
            uint32_t gte_op = opcode & 0x3F;
            switch (gte_op)
            {
            case 0x01: /* RTPS  */
                return 15;
            case 0x06: /* NCLIP */
                return 8;
            case 0x0C: /* OP    */
                return 6;
            case 0x10: /* DPCS  */
                return 8;
            case 0x11: /* INTPL */
                return 8;
            case 0x12: /* MVMVA */
                return 8;
            case 0x13: /* NCDS  */
                return 19;
            case 0x14: /* CDP   */
                return 13;
            case 0x16: /* NCDT  */
                return 44;
            case 0x1B: /* NCCS  */
                return 17;
            case 0x1C: /* CC    */
                return 11;
            case 0x1E: /* NCS   */
                return 14;
            case 0x20: /* NCT   */
                return 30;
            case 0x28: /* SQR   */
                return 5;
            case 0x29: /* DCPL  */
                return 8;
            case 0x2A: /* DPCT  */
                return 17;
            case 0x2D: /* AVSZ3 */
                return 5;
            case 0x2E: /* AVSZ4 */
                return 6;
            case 0x30: /* RTPT  */
                return 23;
            case 0x3D: /* GPF   */
                return 5;
            case 0x3E: /* GPL   */
                return 5;
            case 0x3F: /* NCCT  */
                return 39;
            default:
                return 8; /* Unknown GTE, assume 8 */
            }
        }
        return 1; /* MFC2/MTC2/CFC2/CTC2 */
    /* Branches/Jumps: 1 cycle (delay slot counted separately) */
    case 0x02: /* J   */
        return 1;
    case 0x03: /* JAL */
        return 1;
    case 0x04: /* BEQ */
        return 1;
    case 0x05: /* BNE */
        return 1;
    case 0x06: /* BLEZ */
        return 1;
    case 0x07: /* BGTZ */
        return 1;
    case 0x01: /* REGIMM (BLTZ/BGEZ/BLTZAL/BGEZAL) */
        return 1;
    /* COP0/COP1/COP3 */
    case 0x10: /* COP0 */
        return 1;
    case 0x11: /* COP1 */
        return 1;
    case 0x13: /* COP3 */
        return 1;
    /* LWC2/SWC2 */
    case 0x32: /* LWC2 */
        return 2;
    case 0x3A: /* SWC2 */
        return 1;
    default:
        return 1;
    }
}

/* Current PSX PC being emitted (used by memory emitters for exception EPC) */
static uint32_t emit_current_psx_pc = 0;

/* Load delay slot support:
 * On R3000A, loads have a 1-instruction delay - the loaded value isn't
 * available until 2 instructions after the load. The instruction immediately
 * following a load still sees the OLD register value.
 * dynarec_load_defer: set by compile_block before emit_instruction to tell
 * load emitters to leave the value in REG_V0 instead of storing to PSX reg.
 */
static int dynarec_load_defer = 0;

/* LWL/LWR pending load forwarding:
 * On R3000A, when LWL/LWR is in the load delay slot of another load targeting
 * the same register, the merge source is the PENDING load value (hardware
 * forwarding), not the old register file value. This flag tells LWL/LWR
 * emission to read from CPU_LOAD_DELAY_VAL instead of the register bank.
 */
static int dynarec_lwx_pending = 0;

/* Check if instruction reads a given GPR as source operand */
static int instruction_reads_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0; /* r0 is always 0, never "read" in a delay-relevant way */
    int op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);

    /* RS is read by almost all instructions */
    if (rs == reg)
    {
        /* Exceptions where RS is NOT read */
        if (op == 0x00)
        {
            int func = FUNC(opcode);
            /* SLL, SRL, SRA use shamt, not RS */
            if (func == 0x00 || func == 0x02 || func == 0x03)
                return 0;
            /* MFHI, MFLO don't read any GPR */
            if (func == 0x10 || func == 0x12)
                return 0;
            /* SYSCALL, BREAK don't read GPRs */
            if (func == 0x0C || func == 0x0D)
                return 0;
        }
        if (op == 0x02 || op == 0x03)
            return 0; /* J, JAL don't read RS */
        if (op == 0x0F)
            return 0; /* LUI doesn't read RS */
        return 1;
    }

    /* RT is read by some instructions */
    if (rt == reg)
    {
        if (op == 0x00)
        {
            /* R-type: most ALU instructions read RT */
            int func = FUNC(opcode);
            if (func == 0x08 || func == 0x09)
                return 0; /* JR/JALR don't read RT */
            if (func == 0x10 || func == 0x12)
                return 0; /* MFHI/MFLO */
            if (func == 0x11 || func == 0x13)
                return 0; /* MTHI/MTLO read RS, not RT */
            if (func == 0x0C || func == 0x0D)
                return 0; /* SYSCALL/BREAK */
            return 1;     /* Most R-type read RT */
        }
        if (op == 0x04 || op == 0x05)
            return 1; /* BEQ/BNE read RT */
        if (op == 0x22 || op == 0x26)
            return 1; /* LWL/LWR read RT (merge) */
        if (op >= 0x28 && op <= 0x2E)
            return 1; /* Stores read RT */
        return 0;     /* I-type ALU/loads don't read RT */
    }

    return 0;
}

/* Check if instruction writes a given GPR as destination operand */
static int instruction_writes_gpr(uint32_t opcode, int reg)
{
    if (reg == 0)
        return 0; /* r0 is hardwired to 0 */
    int op = OP(opcode);
    if (op == 0x00)
    {
        /* R-type: writes to RD */
        int func = FUNC(opcode);
        /* MULT/MULTU/DIV/DIVU write HI/LO, not GPR */
        if (func >= 0x18 && func <= 0x1B)
            return 0;
        /* JR doesn't write GPR */
        if (func == 0x08)
            return 0;
        /* MTHI/MTLO don't write GPR (they write HI/LO) */
        if (func == 0x11 || func == 0x13)
            return 0;
        /* SYSCALL/BREAK don't write GPR */
        if (func == 0x0C || func == 0x0D)
            return 0;
        return (RD(opcode) == reg);
    }
    /* I-type ALU: writes to RT */
    if (op >= 0x08 && op <= 0x0F)
        return (RT(opcode) == reg);
    /* Loads: write to RT */
    if (op >= 0x20 && op <= 0x26)
        return (RT(opcode) == reg);
    /* JAL writes to r31 */
    if (op == 0x03)
        return (reg == 31);
    /* JALR: writes to RD */
    if (op == 0x00 && FUNC(opcode) == 0x09)
        return (RD(opcode) == reg);
    return 0;
}

/* ---- Compile a basic block ---- */
static uint32_t *compile_block(uint32_t psx_pc)
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
        /* Reset code ptr PAST the slow-path trampoline (first 8 words) */
        code_ptr = code_buffer + 8;
        memset(code_buffer + 8, 0, CODE_BUFFER_SIZE - 8 * sizeof(uint32_t));
        memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
        memset(block_node_pool, 0, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
        block_node_pool_idx = 0;
        /* All patch sites are now stale (native addrs invalidated) */
        patch_sites_count = 0;
        blocks_compiled = 0;
    }

    uint32_t *block_start = code_ptr;
    uint32_t cur_pc = psx_pc;
    block_cycle_count = 0; /* Reset cycle counter for this block */

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

    emit_block_prologue();

    int block_ended = 0;
    int in_delay_slot = 0;
    uint32_t branch_target = 0;
    int branch_type = 0; /* 0=none, 1=unconditional, 2=conditional, 3=register */
    uint32_t branch_opcode = 0;

    /* Load delay slot tracking */
    int pending_load_reg = 0;       /* PSX register with pending load (0=none) */
    int pending_load_apply_now = 0; /* 1 = apply before this instruction */

    while (!block_ended)
    {
        uint32_t opcode = *psx_code++;
        block_cycle_count += r3000a_cycle_cost(opcode);

        if (in_delay_slot)
        {
            /* Apply any pending load delay before the delay slot instruction */
            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }
            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            /* Emit the delay slot instruction (no load deferral in branch delay slots) */
            dynarec_load_defer = 0;
            /* LWL/LWR forwarding: use pending load value for merge */
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            emit_instruction(opcode, cur_pc);
            dynarec_lwx_pending = 0;
            cur_pc += 4;
            total_instructions++;

            /* Apply any remaining pending load before leaving block */
            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }

            /* Now emit the branch resolution */
            if (branch_type == 1)
            {
                /* Unconditional: J, JAL */
                emit_branch_epilogue(branch_target);
            }
            else if (branch_type == 4)
            {
                /* Deferred Conditional Branch (calculated in S3) */
                /* Emit BNE S3, ZERO, offset */
                uint32_t *bp = code_ptr;
                /* BNE s3, zero, 0 */
                emit(MK_I(0x05, REG_S3, REG_ZERO, 0));
                EMIT_NOP(); /* Native delay slot */

                /* Standard branch patching logic */
                branch_opcode = (uint32_t)bp;

                /* Not taken: fall through PC */
                emit_load_imm32(REG_T0, cur_pc);
                EMIT_SW(REG_T0, CPU_PC, REG_S0);
                emit_block_epilogue();
                /* Taken path target */
                uint32_t *taken_addr = code_ptr;
                int32_t offset = (int32_t)(taken_addr - bp - 1);
                *bp = (*bp & 0xFFFF0000) | (offset & 0xFFFF);
                emit_load_imm32(REG_T0, branch_target);
                EMIT_SW(REG_T0, CPU_PC, REG_S0);
                emit_block_epilogue();
            }
            else if (branch_type == 3)
            {
                /* Register jump (JR/JALR): target already in cpu.pc */
                emit_block_epilogue();
            }
            block_ended = 1;
            break;
        }

        uint32_t op = OP(opcode);

        /* Check for branch/jump instructions */
        if (op == 0x02 || op == 0x03)
        {
            /* J / JAL */
            if (op == 0x03)
            {
                /* JAL: store return address */
                emit_load_imm32(REG_T0, cur_pc + 8);
                emit_store_psx_reg(31, REG_T0);
            }
            branch_target = ((cur_pc + 4) & 0xF0000000) | (TARGET(opcode) << 2);
            branch_type = 1;
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
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
            /* JR / JALR */
            int rs = RS(opcode);
            int rd = (FUNC(opcode) == 0x09) ? RD(opcode) : 0;
            /* Read rs FIRST (before link write that could clobber rd==rs) */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            if (FUNC(opcode) == 0x09 && rd != 0)
            {
                /* JALR: store return address in rd */
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(rd, REG_T1);
            }
            branch_type = 3;
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
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
            /* BEQ, BNE, BLEZ, BGTZ */
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            emit_load_psx_reg(REG_T0, rs);
            if (op == 0x04 || op == 0x05)
            { /* BEQ, BNE */
                emit_load_psx_reg(REG_T1, rt);
                /* XOR s3, t0, t1 */
                emit(MK_R(0, REG_T0, REG_T1, REG_S3, 0, 0x26));
                if (op == 0x04)
                { /* BEQ: taken if s3 == 0 -> set s3 = (s3 < 1) */
                    /* SLTIU s3, s3, 1 */
                    emit(MK_I(0x0B, REG_S3, REG_S3, 1));
                }
                /* BNE: taken if s3 != 0. Already correct. */
            }
            else if (op == 0x06)
            { /* BLEZ (rs <= 0) */
                /* Taken if rs <= 0 -> rs < 1 */
                /* SLTI s3, t0, 1 */
                emit(MK_I(0x0A, REG_T0, REG_S3, 1));
            }
            else if (op == 0x07)
            { /* BGTZ (rs > 0) */
                /* Taken if rs > 0 -> 0 < rs. SLT s3, zero, t0 */
                emit(MK_R(0, REG_ZERO, REG_T0, REG_S3, 0, 0x2A));
            }

            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
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
            /* REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL and unofficial variants.
             * R3000A only decodes bit 0 of rt (BLTZ vs BGEZ) and bit 4 (link).
             * All other rt values map to these 4 operations. */
            int rs = RS(opcode);
            int rt = RT(opcode);
            int32_t offset = SIMM16(opcode) << 2;
            branch_target = cur_pc + 4 + offset;

            /* Read rs FIRST (before any link write that could clobber r31) */
            emit_load_psx_reg(REG_T0, rs);

            if (rt == 0x10 || rt == 0x11)
            {
                /* BLTZAL/BGEZAL only: store return address (always, even if not taken) */
                emit_load_imm32(REG_T1, cur_pc + 8);
                emit_store_psx_reg(31, REG_T1);
            }

            /* Compute branch condition using T0 (original rs value) */
            if ((rt & 1) == 0)
            {
                /* BLTZ / BLTZAL and unofficial even variants (rs < 0) */
                /* SLT s3, t0, zero */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
            }
            else
            {
                /* BGEZ / BGEZAL and unofficial odd variants (rs >= 0) */
                /* SLT s3, t0, zero (1 if <0). XORI s3, s3, 1 (1 if >= 0). */
                emit(MK_R(0, REG_T0, REG_ZERO, REG_S3, 0, 0x2A));
                emit(MK_I(0x0E, REG_S3, REG_S3, 1));
            }

            branch_type = 4; /* Deferred Conditional */
            in_delay_slot = 1;
            /* Advance load delay state (branch counts as one instruction) */
            if (pending_load_reg != 0)
            {
                if (pending_load_apply_now)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
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

            /* Check if this instruction is a load */
            if (op_check == 0x20 || op_check == 0x21 || op_check == 0x22 ||
                op_check == 0x23 || op_check == 0x24 || op_check == 0x25 ||
                op_check == 0x26)
            {
                load_target = RT(opcode);
                if (load_target != 0)
                {
                    /* Peek at next instruction */
                    uint32_t next_instr = *psx_code;
                    uint32_t next_op = OP(next_instr);
                    int next_rt = RT(next_instr);

                    /* Defer if: (a) next instruction reads our loaded register, OR
                     * (b) next instruction is ALSO a load to the same register
                     * Case (b) handles R3000A load cancellation:
                     * LB R,X; LB R,Y; READ R → READ gets original (neither X nor Y) */
                    if (instruction_reads_gpr(next_instr, load_target))
                    {
                        this_is_load = 1;
                    }
                    else if ((next_op >= 0x20 && next_op <= 0x26) &&
                             next_rt == load_target)
                    {
                        /* Next is another load to same register → defer to allow cancel */
                        this_is_load = 1;
                    }
                }
            }

            /* STEP 1: Apply old pending load if ready (1 instruction has passed) */
            if (pending_load_reg != 0 && pending_load_apply_now)
            {
                /* If this is a load to the SAME register, CANCEL the old pending */
                if (this_is_load && load_target == pending_load_reg)
                {
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
                else
                {
                    /* Apply the old pending load */
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                    pending_load_reg = 0;
                    pending_load_apply_now = 0;
                }
            }

            /* STEP 2: Advance delay state */
            if (pending_load_reg != 0 && !pending_load_apply_now)
                pending_load_apply_now = 1;

            /* STEP 3: Emit the instruction (deferred loads leave value in REG_V0) */
            dynarec_load_defer = this_is_load;
            /* LWL/LWR forwarding: use pending load value for merge */
            dynarec_lwx_pending = 0;
            if (pending_load_reg != 0 && (OP(opcode) == 0x22 || OP(opcode) == 0x26) &&
                pending_load_reg == RT(opcode))
                dynarec_lwx_pending = 1;
            emit_instruction(opcode, cur_pc);
            dynarec_lwx_pending = 0;
            dynarec_load_defer = 0;

            /* STEP 3.5: If the instruction just emitted WRITES to the pending
             * register, cancel the pending load (the write takes precedence). */
            if (pending_load_reg != 0 && !this_is_load &&
                instruction_writes_gpr(opcode, pending_load_reg))
            {
                pending_load_reg = 0;
                pending_load_apply_now = 0;
            }

            /* STEP 4: If this was a deferred load, save value to delay field */
            if (this_is_load)
            {
                /* If there's an old pending for a DIFFERENT register, apply it first */
                if (pending_load_reg != 0 && pending_load_reg != load_target)
                {
                    EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                    emit_store_psx_reg(pending_load_reg, REG_T0);
                }
                /* Save new deferred value to delay field */
                EMIT_SW(REG_V0, CPU_LOAD_DELAY_VAL, REG_S0);
                pending_load_reg = load_target;
                pending_load_apply_now = 0;
            }
        }
        cur_pc += 4;
        total_instructions++;

        /* End block after N instructions to avoid huge blocks */
        if ((cur_pc - psx_pc) >= 256)
        {
            /* Apply any remaining pending load before leaving block */
            if (pending_load_reg != 0)
            {
                EMIT_LW(REG_T0, CPU_LOAD_DELAY_VAL, REG_S0);
                emit_store_psx_reg(pending_load_reg, REG_T0);
                pending_load_reg = 0;
            }
            emit_load_imm32(REG_T0, cur_pc);
            EMIT_SW(REG_T0, CPU_PC, REG_S0);
            emit_block_epilogue();
            block_ended = 1;
        }
    }

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
    /* Calculate instruction count for this block */
    uint32_t block_instr_count = (cur_pc - psx_pc) / 4;

    /* Flush caches */
    FlushCache(0); /* writeback dcache */
    FlushCache(2); /* invalidate icache */

    blocks_compiled++;

    /* Store instruction count and weighted cycle count in cache entry */
    {
        uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
        block_cache[idx].instr_count = block_instr_count;
        block_cache[idx].cycle_count = block_cycle_count > 0 ? block_cycle_count : block_instr_count;
    }

    return block_start;
}

/* ---- Block prologue: save callee-saved regs, set up $s0-$s2 ---- */
static void emit_block_prologue(void)
{
    /* addiu $sp, $sp, -48 */
    EMIT_ADDIU(REG_SP, REG_SP, -48);
    /* save $ra, $s0-$s3 */
    EMIT_SW(REG_RA, 44, REG_SP);
    EMIT_SW(REG_S0, 40, REG_SP);
    EMIT_SW(REG_S1, 36, REG_SP);
    EMIT_SW(REG_S2, 32, REG_SP);
    EMIT_SW(REG_S3, 28, REG_SP);
    /* $s0 = $a0 (cpu ptr), $s1 = $a1 (ram), $s2 = $a2 (bios) */
    EMIT_MOVE(REG_S0, REG_A0);
    EMIT_MOVE(REG_S1, REG_A1);
    EMIT_MOVE(REG_S2, 6); /* $a2 = register 6 */
}

/* ---- Block epilogue: restore and return ---- */
static void emit_block_epilogue(void)
{
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 48);
    EMIT_JR(REG_RA);
    EMIT_NOP();
}

static void emit_branch_epilogue(uint32_t target_pc)
{
    /* Update cpu.pc (needed for C dispatch and exception handling) */
    emit_load_imm32(REG_T0, target_pc);
    EMIT_SW(REG_T0, CPU_PC, REG_S0);

    /* Prepare argument registers for the next block's prologue (which expects
     * $a0=cpu, $a1=ram, $a2=bios).  We copy from $s0-$s2 BEFORE restoring. */
    EMIT_MOVE(REG_A0, REG_S0); /* a0 = cpu ptr */
    EMIT_MOVE(REG_A1, REG_S1); /* a1 = ram ptr */
    EMIT_MOVE(REG_A2, REG_S2); /* a2 = bios ptr */

    /* Restore callee-saved and stack */
    EMIT_LW(REG_S3, 28, REG_SP);
    EMIT_LW(REG_S2, 32, REG_SP);
    EMIT_LW(REG_S1, 36, REG_SP);
    EMIT_LW(REG_S0, 40, REG_SP);
    EMIT_LW(REG_RA, 44, REG_SP);
    EMIT_ADDIU(REG_SP, REG_SP, 48);

    /* Direct link: J to next block's prologue (tail call).
     * $a0-$a2 are set up above so the prologue can configure $s0-$s2.
     * $ra is restored so the next block's epilogue returns to C dispatch. */
    emit_direct_link(target_pc);
}

/* ---- Memory access emitters ---- */
/*
 * Inline RAM fast-path for LW/SW (size==4).
 * Both paths converge with result in REG_V0.
 *
 * Code layout for LW:
 *   andi   t1, t0, 3          # t0 = effective addr
 *   bne    t1, zero, @slow
 *   nop
 *   lui    t1, 0x1FFF
 *   ori    t1, t1, 0xFFFF     # t1 = 0x1FFFFFFF (mask)
 *   and    t1, t0, t1         # t1 = phys_addr
 *   srl    t2, t1, 12
 *   sltiu  t2, t2, 0x200      # t2 = (phys < 2MB)
 *   beq    t2, zero, @slow
 *   addu   t1, t1, s1         # (delay slot) t1 = psx_ram + phys
 *   lw     v0, 0(t1)
 *   b      @done
 *   nop
 * @slow:
 *   move   a0, t0
 *   jal    ReadWord
 *   nop
 * @done:
 */
static void emit_memory_read(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* Store current PSX PC for exception handling */
    emit_load_imm32(REG_T2, emit_current_psx_pc);
    EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);

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

        /* Slow path */
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
        int32_t soff2 = (int32_t)(code_ptr - range_branch - 1);
        *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);
        EMIT_MOVE(REG_A0, REG_T0);
        EMIT_JAL_ABS((uint32_t)ReadWord);
        EMIT_NOP();

        /* @done */
        int32_t doff = (int32_t)(code_ptr - fast_done - 1);
        *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);

        if (!dynarec_load_defer)
            emit_store_psx_reg(rt_psx, REG_V0);
        return;
    }

    /* Non-LW: slow path only */
    EMIT_MOVE(REG_A0, REG_T0);
    uint32_t func_addr;
    if (size == 2)
        func_addr = (uint32_t)ReadHalf;
    else
        func_addr = (uint32_t)ReadByte;
    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();

    if (!dynarec_load_defer)
        emit_store_psx_reg(rt_psx, REG_V0);
}

static void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset)
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
            EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x00)); /* SLL $t0, $t0, 24 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 24, 0x03)); /* SRA $t0, $t0, 24 */
            EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
        }
        else if (size == 2)
        {
            EMIT_LW(REG_T0, CPU_REG(rt_psx), REG_S0);
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x00)); /* SLL $t0, $t0, 16 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 16, 0x03)); /* SRA $t0, $t0, 16 */
            EMIT_SW(REG_T0, CPU_REG(rt_psx), REG_S0);
        }
    }
}

static void emit_memory_write(int size, int rt_psx, int rs_psx, int16_t offset)
{
    /* Store current PSX PC for exception handling */
    emit_load_imm32(REG_T2, emit_current_psx_pc);
    EMIT_SW(REG_T2, CPU_CURRENT_PC, REG_S0);

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

        /* Slow path - back-patch all branch offsets pointing here */
        int32_t soff0 = (int32_t)(code_ptr - isc_branch - 1);
        *isc_branch = (*isc_branch & 0xFFFF0000) | ((uint32_t)soff0 & 0xFFFF);
        int32_t soff1 = (int32_t)(code_ptr - align_branch - 1);
        *align_branch = (*align_branch & 0xFFFF0000) | ((uint32_t)soff1 & 0xFFFF);
        int32_t soff2 = (int32_t)(code_ptr - range_branch - 1);
        *range_branch = (*range_branch & 0xFFFF0000) | ((uint32_t)soff2 & 0xFFFF);
        EMIT_MOVE(REG_A0, REG_T0); /* a0 = addr */
        EMIT_MOVE(REG_A1, REG_T2); /* a1 = data */
        EMIT_JAL_ABS((uint32_t)WriteWord);
        EMIT_NOP();

        /* @done */
        int32_t doff = (int32_t)(code_ptr - fast_done - 1);
        *fast_done = (*fast_done & 0xFFFF0000) | ((uint32_t)doff & 0xFFFF);
        return;
    }

    /* Non-SW: slow path only */
    EMIT_MOVE(REG_A0, REG_T0);
    EMIT_MOVE(REG_A1, REG_T2);
    uint32_t func_addr;
    if (size == 2)
        func_addr = (uint32_t)WriteHalf;
    else
        func_addr = (uint32_t)WriteByte;
    EMIT_JAL_ABS(func_addr);
    EMIT_NOP();
}

/* ---- Emit a non-branch instruction ---- */
/* Debug helper: log MTC0 writes to SR */
static int mtc0_sr_log_count = 0;
static uint32_t last_sr_logged = 0xDEAD;
void debug_mtc0_sr(uint32_t val)
{
    /* Log all SR writes that have IM or IEc bits, or first 10,
     * or when value changes significantly */
    uint32_t interesting = val & 0x00000701; /* IEc + IM bits */
    if (interesting || mtc0_sr_log_count < 10 || val != last_sr_logged)
    {
        if (mtc0_sr_log_count < 200)
        {
            //            DLOG("SR = %08X (IEc=%d IM=%02X BEV=%d CU0=%d)\n",
            //                   (unsigned)val,
            //                   (int)(val & 1),
            //                   (int)((val >> 8) & 0xFF),
            //                   (int)((val >> 22) & 1),
            //                   (int)((val >> 28) & 1));
            mtc0_sr_log_count++;
        }
        last_sr_logged = val;
    }
    cpu.cop0[PSX_COP0_SR] = val;
}

/*=== BIOS HLE (High Level Emulation) ===*/
/*
 * The PSX BIOS uses three function tables called via:
 *   A-table: jump to 0xA0 with function number in $t1 ($9)
 *   B-table: jump to 0xB0 with function number in $t1 ($9)
 *   C-table: jump to 0xC0 with function number in $t1 ($9)
 *
 * Some BIOS table entries (especially EnterCriticalSection/ExitCriticalSection)
 * may be incorrectly initialized to placeholder functions. We intercept these
 * calls and implement the critical ones in C code.
 */
static int hle_log_count = 0;

static int BIOS_HLE_A(void)
{
    uint32_t func = cpu.regs[9]; /* $t1 = function number */
    static int a_log_count = 0;
    if (a_log_count < 30)
    {
        // DLOG("A(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        a_log_count++;
    }

    /* A(0x3C) std_out_putchar */
    if (func == 0x3C)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = cpu.regs[4]; /* Return char */
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0; /* Let native code handle it */
}

static int BIOS_HLE_B(void)
{
    uint32_t func = cpu.regs[9]; /* $t1 = function number */
    static int b_log_count = 0;
    if (b_log_count < 30)
    {
        // DLOG("[BIOS] B(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        b_log_count++;
    }

    /* B(0x3B) putchar - useful for BIOS text output */
    if (func == 0x3B)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = cpu.regs[4];
        cpu.pc = cpu.regs[31];
        return 1;
    }
    /* B(0x3D) std_out_putchar */
    if (func == 0x3D)
    {
        char c = (char)(cpu.regs[4] & 0xFF);
        printf("%c", c);
#ifdef ENABLE_HOST_LOG
        if (host_log_file)
        {
            fputc(c, host_log_file);
            fflush(host_log_file);
        }
#endif
        cpu.regs[2] = 1; /* Return success/char */
        cpu.pc = cpu.regs[31];
        return 1;
    }
    return 0; /* Let native code handle everything else */
}

static int BIOS_HLE_C(void)
{
    uint32_t func = cpu.regs[9];
    static int c_log_count = 0;
    if (c_log_count < 20)
    {
        //        DLOG("[BIOS] C(%02X) ret=%08X\n", (unsigned)func, (unsigned)cpu.regs[31]);
        c_log_count++;
    }
    return 0; /* Let native code handle it */
}

static void emit_instruction(uint32_t opcode, uint32_t psx_pc)
{
    uint32_t op = OP(opcode);
    int rs = RS(opcode);
    int rt = RT(opcode);
    int rd = RD(opcode);
    int sa = SA(opcode);
    int func = FUNC(opcode);
    int16_t imm = SIMM16(opcode);
    uint16_t uimm = IMM16(opcode);

    /* Track current PC for exception handling in memory accesses */
    emit_current_psx_pc = psx_pc;

    if (opcode == 0)
        return; /* NOP */

    switch (op)
    {
    case 0x00: /* SPECIAL */
        switch (func)
        {
        case 0x00: /* SLL */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x00));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x02: /* SRL */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x02));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x03: /* SRA */
            emit_load_psx_reg(REG_T0, rt);
            emit(MK_R(0, 0, REG_T0, REG_T0, sa, 0x03));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x04: /* SLLV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x04));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x06: /* SRLV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x06));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x07: /* SRAV */
            emit_load_psx_reg(REG_T0, rt);
            emit_load_psx_reg(REG_T1, rs);
            emit(MK_R(0, REG_T1, REG_T0, REG_T0, 0, 0x07));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x0C: /* SYSCALL */
            /* Trigger proper PSX exception (code 8) */
            emit_load_imm32(REG_A0, psx_pc);
            EMIT_JAL_ABS((uint32_t)Helper_Syscall_Exception);
            EMIT_NOP();
            break;
        case 0x0D: /* BREAK */
            /* Trigger proper PSX exception (code 9) */
            emit_load_imm32(REG_A0, psx_pc);
            EMIT_JAL_ABS((uint32_t)Helper_Break_Exception);
            EMIT_NOP();
            break;
        case 0x10: /* MFHI */
            EMIT_LW(REG_T0, CPU_HI, REG_S0);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x11: /* MTHI */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x12: /* MFLO */
            EMIT_LW(REG_T0, CPU_LO, REG_S0);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x13: /* MTLO */
            emit_load_psx_reg(REG_T0, rs);
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            break;
        case 0x18: /* MULT */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x18)); /* mult */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));      /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10)); /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x19: /* MULTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, 0, 0, 0x19)); /* multu */
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x12));      /* mflo */
            EMIT_SW(REG_T0, CPU_LO, REG_S0);
            emit(MK_R(0, 0, 0, REG_T0, 0, 0x10)); /* mfhi */
            EMIT_SW(REG_T0, CPU_HI, REG_S0);
            break;
        case 0x1A: /* DIV */
        {
            /* Call Helper_DIV(rs_val, rt_val, &cpu.lo, &cpu.hi) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            /* $a2 = &cpu.lo, $a3 = &cpu.hi */
            EMIT_ADDIU(6, REG_S0, CPU_LO); /* a2 = s0 + CPU_LO */
            EMIT_ADDIU(7, REG_S0, CPU_HI); /* a3 = s0 + CPU_HI */
            EMIT_JAL_ABS((uint32_t)Helper_DIV);
            EMIT_NOP();
            break;
        }
        case 0x1B: /* DIVU */
        {
            /* Call Helper_DIVU(rs_val, rt_val, &cpu.lo, &cpu.hi) */
            emit_load_psx_reg(REG_A0, rs);
            emit_load_psx_reg(REG_A1, rt);
            EMIT_ADDIU(6, REG_S0, CPU_LO); /* a2 = s0 + CPU_LO */
            EMIT_ADDIU(7, REG_S0, CPU_HI); /* a3 = s0 + CPU_HI */
            EMIT_JAL_ABS((uint32_t)Helper_DIVU);
            EMIT_NOP();
            break;
        }
        case 0x20:                         /* ADD (with overflow check) */
            emit_load_psx_reg(REG_A0, rs); /* a0 = rs_val */
            emit_load_psx_reg(REG_A1, rt); /* a1 = rt_val */
            emit_load_imm32(6, rd);        /* a2 = rd index */
            emit_load_imm32(7, psx_pc);    /* a3 = PC */
            EMIT_JAL_ABS((uint32_t)Helper_ADD);
            EMIT_NOP();
            break;
        case 0x21: /* ADDU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            EMIT_ADDU(REG_T0, REG_T0, REG_T1);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x22:                         /* SUB (with overflow check) */
            emit_load_psx_reg(REG_A0, rs); /* a0 = rs_val */
            emit_load_psx_reg(REG_A1, rt); /* a1 = rt_val */
            emit_load_imm32(6, rd);        /* a2 = rd index */
            emit_load_imm32(7, psx_pc);    /* a3 = PC */
            EMIT_JAL_ABS((uint32_t)Helper_SUB);
            EMIT_NOP();
            break;
        case 0x23: /* SUBU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x23)); /* subu */
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x24: /* AND */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x24));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x25: /* OR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            EMIT_OR(REG_T0, REG_T0, REG_T1);
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x26: /* XOR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x26));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x27: /* NOR */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x27));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x2A: /* SLT */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x2A));
            emit_store_psx_reg(rd, REG_T0);
            break;
        case 0x2B: /* SLTU */
            emit_load_psx_reg(REG_T0, rs);
            emit_load_psx_reg(REG_T1, rt);
            emit(MK_R(0, REG_T0, REG_T1, REG_T0, 0, 0x2B));
            emit_store_psx_reg(rd, REG_T0);
            break;
        default:
            if (total_instructions < 50)
                DLOG("Unknown SPECIAL func=0x%02X at PC=0x%08X\n", func, (unsigned)psx_pc);
            break;
        }
        break;

    /* I-type ALU */
    case 0x08: /* ADDI (with overflow check) */
    {
        emit_load_psx_reg(REG_A0, rs);                   /* a0 = rs_val */
        emit_load_imm32(REG_A1, (uint32_t)(int32_t)imm); /* a1 = sign-extended imm */
        emit_load_imm32(6, rt);                          /* a2 = rt index */
        emit_load_imm32(7, psx_pc);                      /* a3 = PC */
        EMIT_JAL_ABS((uint32_t)Helper_ADDI);
        EMIT_NOP();
        break;
    }
    case 0x09: /* ADDIU */
        emit_load_psx_reg(REG_T0, rs);
        EMIT_ADDIU(REG_T0, REG_T0, imm);
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0A: /* SLTI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0A, REG_T0, REG_T0, imm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0B: /* SLTIU */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0B, REG_T0, REG_T0, imm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0C: /* ANDI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0C, REG_T0, REG_T0, uimm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0D: /* ORI */
        emit_load_psx_reg(REG_T0, rs);
        EMIT_ORI(REG_T0, REG_T0, uimm);
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0E: /* XORI */
        emit_load_psx_reg(REG_T0, rs);
        emit(MK_I(0x0E, REG_T0, REG_T0, uimm));
        emit_store_psx_reg(rt, REG_T0);
        break;
    case 0x0F: /* LUI */
        emit_load_imm32(REG_T0, (uint32_t)uimm << 16);
        emit_store_psx_reg(rt, REG_T0);
        break;

    /* COP0 */
    case 0x10:
    {
        /* SWC0 CU check: COP0 is always accessible in kernel mode.
         * The CU0 bit (SR bit 28) controls USER mode access.
         * For simplicity, we skip the CU check for COP0 MFC0/MTC0/RFE
         * since those are privileged and the test only checks SWC0. */
        if (rs == 0x00)
        {
            /* MFC0 rt, rd */
            EMIT_LW(REG_T0, CPU_COP0(rd), REG_S0);
            emit_store_psx_reg(rt, REG_T0);
        }
        else if (rs == 0x04)
        {
            /* MTC0 rt, rd */
            emit_load_psx_reg(REG_T0, rt);
            if (rd == PSX_COP0_SR)
            {
                /* Call debug_mtc0_sr(val) for SR writes */
                EMIT_MOVE(REG_A0, REG_T0);
                EMIT_JAL_ABS((uint32_t)debug_mtc0_sr);
                EMIT_NOP();
            }
            else
            {
                EMIT_SW(REG_T0, CPU_COP0(rd), REG_S0);
            }
        }
        else if (rs == 0x10 && func == 0x10)
        {
            /* RFE - Return from exception
             * Pop mode stack: new_sr = (sr & ~0x0F) | ((sr >> 2) & 0x0F)
             * Bits 0-1 get values from bits 2-3 (IEp→IEc, KUp→KUc)
             * Bits 2-3 get values from bits 4-5 (IEo→IEp, KUo→KUp)
             * Bits 4-31 remain UNCHANGED (IM bits, BEV, CU0, etc.)
             */
            EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0); /* t0 = SR */
            EMIT_MOVE(REG_T1, REG_T0);                      /* t1 = SR */
            emit(MK_R(0, 0, REG_T1, REG_T1, 2, 0x02));      /* srl t1, t1, 2 */
            emit(MK_I(0x0C, REG_T1, REG_T1, 0x0F));         /* andi t1, t1, 0x0F */
            /* Clear bottom 4 bits of t0 using srl/sll (preserves bits 4-31) */
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x02));      /* srl t0, t0, 4 */
            emit(MK_R(0, 0, REG_T0, REG_T0, 4, 0x00));      /* sll t0, t0, 4 */
            EMIT_OR(REG_T0, REG_T0, REG_T1);                /* t0 = (sr & ~0x0F) | shifted */
            EMIT_SW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0); /* SR = t0 */
        }
        break;
    }

    /* COP1 */
    case 0x11:
    {
        /* Check CU1 bit (SR bit 29). If not set, fire CU exception */
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 1); /* cop_num = 1 */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_I(0x0C, REG_T0, REG_T0, 0x2000)); /* andi t0, t0, 0x2000_0000 >> 16... */
        /* Actually need to check bit 29. Use SRL + ANDI */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 29, 0x02)); /* srl t0, t0, 29 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        /* If t0 != 0, CU1 is set -> skip exception */
        uint32_t *skip_patch_1 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        /* CU1 not set -> fire exception */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        /* Patch skip target */
        *skip_patch_1 = (*skip_patch_1 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_patch_1 - 1) & 0xFFFF);
        /* COP1 doesn't exist on PSX, so if enabled we just NOP */
        break;
    }

    /* COP2 (GTE) */
    case 0x12:
    {
        /* Check CU2 bit (SR bit 30). If not set, fire CU exception */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02)); /* srl t0, t0, 30 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        uint32_t *skip_cu2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2); /* cop_num = 2 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_cu2 = (*skip_cu2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu2 - 1) & 0xFFFF);

        if (total_instructions < 20000000)
        { // Log COP2 instructions
            DLOG("Compiling COP2 Op %08" PRIX32 " at %08" PRIX32 "\n", opcode, psx_pc);
        }
        if ((opcode & 0x02000000) == 0)
        {
            /* Transfer Instructions (MFC2, CFC2, MTC2, CTC2) */
            // rs field tells us the op: 00=MFC2, 02=CFC2, 04=MTC2, 06=CTC2
            // wait, RS is bits 21-25. The instruction encoding is:
            // COP2(010010) 0(1) rt(5) rd(5) ...
            // Bit 25 is 0.
            // RS is bits 21-25.
            // 00000 -> MFC2
            // 00010 -> CFC2
            // 00100 -> MTC2
            // 00110 -> CTC2
            // These correspond to rs=0, rs=2, rs=4, rs=6.

            if (rs == 0x00)
            {                                /* MFC2 rt, rd - read GTE data register */
                EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
                emit_load_imm32(REG_A1, rd); /* a1 = reg index */
                EMIT_JAL_ABS((uint32_t)GTE_ReadData);
                EMIT_NOP();
                emit_store_psx_reg(rt, REG_V0); /* rt = result */
            }
            else if (rs == 0x02)
            {                                /* CFC2 rt, rd - read GTE control register */
                EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
                emit_load_imm32(REG_A1, rd); /* a1 = reg index */
                EMIT_JAL_ABS((uint32_t)GTE_ReadCtrl);
                EMIT_NOP();
                emit_store_psx_reg(rt, REG_V0); /* rt = result */
            }
            else if (rs == 0x04)
            {                                /* MTC2 rt, rd - write GTE data register */
                EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
                emit_load_imm32(REG_A1, rd); /* a1 = reg index */
                emit_load_psx_reg(6, rt);    /* a2 = value */
                EMIT_JAL_ABS((uint32_t)GTE_WriteData);
                EMIT_NOP();
            }
            else if (rs == 0x06)
            {                                /* CTC2 rt, rd - write GTE control register */
                EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
                emit_load_imm32(REG_A1, rd); /* a1 = reg index */
                emit_load_psx_reg(6, rt);    /* a2 = value */
                EMIT_JAL_ABS((uint32_t)GTE_WriteCtrl);
                EMIT_NOP();
            }
            else
            {
                if (total_instructions < 100)
                    DLOG("Unknown COP2 transfer rs=0x%X\n", rs);
            }
        }
        else
        {
            /* GTE Command (Bit 25 = 1) */
            /* Read opcode from PSX RAM at runtime to handle self-modifying code.
             * The COP2 opcode may change between calls (e.g. ps1-tests GTE suite
             * uses self-modifying code to patch COP2 instructions at runtime). */
            {
                uint32_t phys = psx_pc & 0x1FFFFFFF;
                emit_load_imm32(REG_T0, phys);     /* t0 = physical PSX address */
                EMIT_ADDU(REG_T0, REG_T0, REG_S1); /* t0 = psx_ram + phys */
                EMIT_LW(REG_A0, 0, REG_T0);        /* a0 = *(psx_ram + phys) = current opcode */
            }
            EMIT_MOVE(REG_A1, REG_S0);
            EMIT_JAL_ABS((uint32_t)GTE_Execute);
            EMIT_NOP();
        }
        break;
    }

    /* COP3 */
    case 0x13:
    {
        /* Check CU3 bit (SR bit 31). If not set, fire CU exception */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02)); /* srl t0, t0, 31 */
        /* t0 is now 0 or 1 */
        uint32_t *skip_cu3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3); /* cop_num = 3 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_cu3 = (*skip_cu3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_cu3 - 1) & 0xFFFF);
        /* COP3 doesn't exist on PSX, NOP if enabled */
        break;
    }

    /* Load instructions */
    case 0x20: /* LB */
        emit_memory_read_signed(1, rt, rs, imm);
        break;
    case 0x21: /* LH */
        emit_memory_read_signed(2, rt, rs, imm);
        break;
    case 0x23: /* LW */
        emit_memory_read(4, rt, rs, imm);
        break;
    case 0x24: /* LBU */
        emit_memory_read(1, rt, rs, imm);
        break;
    case 0x25: /* LHU */
        emit_memory_read(2, rt, rs, imm);
        break;

    /* Store instructions */
    case 0x28: /* SB */
        emit_memory_write(1, rt, rs, imm);
        break;
    case 0x29: /* SH */
        emit_memory_write(2, rt, rs, imm);
        break;
    case 0x2B: /* SW */
        emit_memory_write(4, rt, rs, imm);
        break;

    /* LWL/LWR/SWL/SWR - unaligned access via C helpers */
    case 0x22: /* LWL */
    {
        /* $a0 = address, $a1 = current rt value (or pending load value) */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        if (dynarec_lwx_pending)
            EMIT_LW(REG_A1, CPU_LOAD_DELAY_VAL, REG_S0);
        else
            emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((uint32_t)Helper_LWL);
        EMIT_NOP();
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x26: /* LWR */
    {
        /* $a0 = address, $a1 = current rt value (or pending load value) */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        if (dynarec_lwx_pending)
            EMIT_LW(REG_A1, CPU_LOAD_DELAY_VAL, REG_S0);
        else
            emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((uint32_t)Helper_LWR);
        EMIT_NOP();
        if (!dynarec_load_defer)
            emit_store_psx_reg(rt, REG_V0);
        break;
    }
    case 0x2A: /* SWL */
    {
        /* $a0 = address, $a1 = rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((uint32_t)Helper_SWL);
        EMIT_NOP();
        break;
    }
    case 0x2E: /* SWR */
    {
        /* $a0 = address, $a1 = rt value */
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        emit_load_psx_reg(REG_A1, rt);
        EMIT_JAL_ABS((uint32_t)Helper_SWR);
        EMIT_NOP();
        break;
    }

    /* LWC0 - CU check for COP0 (SWC0/LWC0 respect CU0 bit, unlike MFC0/MTC0) */
    case 0x30:
    {
        /* Check CU0 bit (SR bit 28) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 28, 0x02)); /* srl t0, t0, 28 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        uint32_t *skip_lwc0 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0); /* cop_num = 0 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_lwc0 = (*skip_lwc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc0 - 1) & 0xFFFF);
        /* COP0 has no actual coprocessor data registers to load into, so NOP if enabled */
        break;
    }

    /* LWC2 - Load Word to Cop2 */
    case 0x32:
    {
        /* Check CU2 bit (SR bit 30) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02)); /* srl t0, t0, 30 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        uint32_t *skip_lwc2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2); /* cop_num = 2 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_lwc2 = (*skip_lwc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc2 - 1) & 0xFFFF);

        // LWC2 rt, offset(base)
        // rt is destination in CP2 Data Registers
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        EMIT_JAL_ABS((uint32_t)ReadWord);
        EMIT_NOP();
        // Write result through GTE helper for proper register behavior
        EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
        emit_load_imm32(REG_A1, rt); /* a1 = reg index */
        EMIT_MOVE(6, REG_V0);        /* a2 = value from ReadWord */
        EMIT_JAL_ABS((uint32_t)GTE_WriteData);
        EMIT_NOP();
    }
    break;

    /* LWC3 */
    case 0x33:
    {
        /* Check CU3 bit (SR bit 31) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02)); /* srl t0, t0, 31 */
        uint32_t *skip_lwc3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3); /* cop_num = 3 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_lwc3 = (*skip_lwc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_lwc3 - 1) & 0xFFFF);
        /* COP3 doesn't exist, NOP if enabled */
        break;
    }

    /* SWC0 */
    case 0x38:
    {
        /* Check CU0 bit (SR bit 28) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 28, 0x02)); /* srl t0, t0, 28 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        uint32_t *skip_swc0 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 0); /* cop_num = 0 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_swc0 = (*skip_swc0 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc0 - 1) & 0xFFFF);
        /* COP0 store: NOP if enabled (no meaningful coprocessor data) */
        break;
    }

    /* SWC2 - Store Word from Cop2 */
    case 0x3A:
    {
        /* Check CU2 bit (SR bit 30) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 30, 0x02)); /* srl t0, t0, 30 */
        emit(MK_I(0x0C, REG_T0, REG_T0, 1));        /* andi t0, t0, 1 */
        uint32_t *skip_swc2 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 2); /* cop_num = 2 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_swc2 = (*skip_swc2 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc2 - 1) & 0xFFFF);

        // SWC2 rt, offset(base)
        // rt is source from CP2 Data Registers
        // First read the GTE data register through helper
        EMIT_MOVE(REG_A0, REG_S0);   /* a0 = cpu */
        emit_load_imm32(REG_A1, rt); /* a1 = reg index */
        EMIT_JAL_ABS((uint32_t)GTE_ReadData);
        EMIT_NOP();
        // Then write to memory
        emit_load_psx_reg(REG_A0, rs);
        EMIT_ADDIU(REG_A0, REG_A0, imm);
        EMIT_MOVE(REG_A1, REG_V0); /* a1 = value from GTE_ReadData */
        EMIT_JAL_ABS((uint32_t)WriteWord);
        EMIT_NOP();
    }
    break;

    /* SWC3 */
    case 0x3B:
    {
        /* Check CU3 bit (SR bit 31) */
        EMIT_LW(REG_T0, CPU_COP0(PSX_COP0_SR), REG_S0);
        emit(MK_R(0, 0, REG_T0, REG_T0, 31, 0x02)); /* srl t0, t0, 31 */
        uint32_t *skip_swc3 = code_ptr;
        emit(MK_I(0x05, REG_T0, REG_ZERO, 0)); /* bne t0, zero, skip */
        EMIT_NOP();
        emit_load_imm32(REG_A0, psx_pc);
        emit_load_imm32(REG_A1, 3); /* cop_num = 3 */
        EMIT_JAL_ABS((uint32_t)Helper_CU_Exception);
        EMIT_NOP();
        *skip_swc3 = (*skip_swc3 & 0xFFFF0000) | ((uint32_t)(code_ptr - skip_swc3 - 1) & 0xFFFF);
        /* COP3 doesn't exist, NOP if enabled */
        break;
    }

    default:
        // Always log unknown opcodes to catch missing instructions
        static int unknown_log_count = 0;
        if (unknown_log_count < 200)
        {
            DLOG("Unknown opcode 0x%02" PRIX32 " at PC=0x%08" PRIX32 "\n", op, psx_pc);
            unknown_log_count++;
        }
        break;
    }
}

/* ---- Block lookup (with collision chain traversal) ---- */
static uint32_t *lookup_block(uint32_t psx_pc)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *e = &block_cache[idx];
    while (e)
    {
        if (e->native && e->psx_pc == psx_pc)
        {
#ifdef ENABLE_DYNAREC_STATS
            stat_cache_hits++;
#endif
            return e->native;
        }
        e = e->next;
    }
    /* Not found - check if bucket is occupied (collision) */
    if (block_cache[idx].native && block_cache[idx].psx_pc != psx_pc)
    {
#ifdef ENABLE_DYNAREC_STATS
        stat_cache_collisions++;
#endif
    }
#ifdef ENABLE_DYNAREC_STATS
    stat_cache_misses++;
#endif
    return NULL;
}

static void cache_block(uint32_t psx_pc, uint32_t *native)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *bucket = &block_cache[idx];

    /* Case 1: bucket is empty */
    if (!bucket->native)
    {
        bucket->psx_pc = psx_pc;
        bucket->native = native;
        return;
    }

    /* Case 2: same PC - update in place (recompile) */
    BlockEntry *e = bucket;
    while (e)
    {
        if (e->psx_pc == psx_pc)
        {
            e->native = native;
            return;
        }
        e = e->next;
    }

    /* Case 3: collision - allocate from pool and prepend to chain */
    if (block_node_pool_idx < BLOCK_NODE_POOL_SIZE)
    {
        BlockEntry *node = &block_node_pool[block_node_pool_idx++];
        node->psx_pc = psx_pc;
        node->native = native;
        node->instr_count = 0;
        node->cycle_count = 0;
        node->next = bucket->next;
        bucket->next = node;
    }
    else
    {
        /* Pool exhausted: fall back to overwrite (rare) */
        DLOG("Block node pool exhausted! Overwriting bucket at idx=%u\n", (unsigned)idx);
        bucket->psx_pc = psx_pc;
        bucket->native = native;
    }
}

/* ---- Public API ---- */
typedef void (*block_func_t)(R3000CPU *cpu, uint8_t *ram, uint8_t *bios);

void Init_Dynarec(void)
{
    printf("Initializing Dynarec...\n");

    /* Allocate code buffer dynamically (BSS is unmapped in PCSX2 TLB) */
    code_buffer = (uint32_t *)memalign(64, CODE_BUFFER_SIZE);
    if (!code_buffer)
    {
        printf("  ERROR: Failed to allocate code buffer!\n");
        return;
    }

    block_cache = (BlockEntry *)memalign(64, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    if (!block_cache)
    {
        printf("  ERROR: Failed to allocate block cache!\n");
        return;
    }

    block_node_pool = (BlockEntry *)memalign(64, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
    if (!block_node_pool)
    {
        printf("  ERROR: Failed to allocate block node pool!\n");
        return;
    }

    code_ptr = code_buffer;
    memset(code_buffer, 0, CODE_BUFFER_SIZE);
    memset(block_cache, 0, BLOCK_CACHE_SIZE * sizeof(BlockEntry));
    memset(block_node_pool, 0, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
    block_node_pool_idx = 0;
    blocks_compiled = 0;
    total_instructions = 0;

    /* ---- Slow-path trampoline at code_buffer[0] ----
     * When a direct-link J has not been patched yet (target not compiled),
     * the J word is 0 which on MIPS encodes SLL $0,$0,0 (NOP).
     * We place a real trampoline here: JR $ra / NOP.
     * This makes unpatched J stubs behave like a normal block return:
     * the C dispatch loop sees cpu.pc (already written before the J) and
     * compiles/runs the next block.
     */
    code_buffer[0] = MK_R(0, REG_RA, 0, 0, 0, 0x08); /* JR $ra */
    code_buffer[1] = 0;                              /* NOP (delay slot) */
    /* Reserve 8 words for trampoline; real compiled blocks start at [8] */
    code_ptr = code_buffer + 8;

    printf("  Code buffer at %p, size %d bytes\n", code_buffer, CODE_BUFFER_SIZE);
    printf("  Block cache at %p, %d entries\n", block_cache, BLOCK_CACHE_SIZE);
    printf("  Block node pool at %p, %d nodes\n", block_node_pool, BLOCK_NODE_POOL_SIZE);
    printf("  Slow-path trampoline at code_buffer[0] = %p\n", (void *)code_buffer);
    FlushCache(0);
    FlushCache(2);
}

/* ---- Forward declarations for scheduler callbacks ---- */
static void Sched_VBlank_Callback(void);
void Timer_ScheduleAll(void);   /* Defined in hardware.c */
void CDROM_ScheduleEvent(void); /* Defined in cdrom.c */

static void Sched_VBlank_Callback(void)
{
    /* Re-schedule next VBlank */
    Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK,
                            global_cycles + CYCLES_PER_FRAME_NTSC,
                            Sched_VBlank_Callback);
}

void Run_CPU(void)
{
    printf("Starting CPU Execution (Dynarec + Event Scheduler)...\n");

    /* ----- CPU Init ----- */
    cpu.pc = 0xBFC00000;
    cpu.cop0[PSX_COP0_SR] = 0x10400000;   /* Initial status: CU0=1, BEV=1 */
    cpu.cop0[PSX_COP0_PRID] = 0x00000002; /* R3000A */

    /* ----- Scheduler Init ----- */
    Scheduler_Init();

    /* Schedule initial VBlank event */
    Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK,
                            global_cycles + CYCLES_PER_FRAME_NTSC,
                            Sched_VBlank_Callback);

    /* Schedule initial timer events */
    Timer_ScheduleAll();

    /* CD-ROM will self-schedule when a read command starts */

    uint32_t iterations = 0;
    uint32_t next_vram_dump = 1000000;

#ifdef ENABLE_STUCK_DETECTION
    static uint32_t stuck_pc = 0;
    static uint32_t stuck_count = 0;
    static uint32_t heartbeat_counter = 0;
    static uint32_t last_heartbeat_pc = 0;
    static int heartbeat_dumped = 0;
#endif

    while (true)
    {
        /* ---- Determine how many cycles to run until next event ---- */
        uint64_t deadline = Scheduler_NextDeadline();
        if (deadline == UINT64_MAX)
        {
            /* No events scheduled - run a default chunk */
            deadline = global_cycles + 1024;
        }

        /* ---- Execute blocks until we reach the deadline ---- */
        while (global_cycles < deadline)
        {
            uint32_t pc = cpu.pc;

            /* === BIOS HLE Intercepts === */
            {
                uint32_t phys_pc = pc & 0x1FFFFFFF;
                if (phys_pc == 0xA0)
                {
                    if (BIOS_HLE_A())
                        continue;
                }
                else if (phys_pc == 0xB0)
                {
                    if (BIOS_HLE_B())
                        continue;
                }
                else if (phys_pc == 0xC0)
                {
                    if (BIOS_HLE_C())
                        continue;
                }
            }

            /* === BIOS Shell Hook === */
            /* PS2 ROM0 (TBIN/Boot) seems to idle in RAM around 0x001A45A0 - 0x001A4620. */
            if (pc == 0x80030000 || (pc >= 0x001A45A0 && pc <= 0x001A4620))
            {
                static int binary_loaded = 0;
                if (!binary_loaded)
                {
                    DLOG("Reached BIOS Idle Loop (PC=%08X). Loading binary...\n", (unsigned)pc);
                    if (psx_boot_mode == BOOT_MODE_ISO)
                    {
                        /* ISO mode: load the boot EXE directly from the mounted ISO */
                        DLOG("ISO boot mode: extracting EXE from ISO...\n");
                        if (Load_PSX_EXE_FromISO(&cpu) == 0)
                        {
                            DLOG("ISO EXE loaded. Jump to PC=0x%08X\n", (unsigned)cpu.pc);
#ifdef ENABLE_HOST_LOG
                            host_log_file = fopen("output.log", "w");
#endif
                            binary_loaded = 1;
                            FlushCache(0);
                            FlushCache(2);
#ifdef ENABLE_STUCK_DETECTION
                            stuck_pc = cpu.pc;
                            stuck_count = 0;
#endif
                            continue;
                        }
                        else
                        {
                            printf("DYNAREC: Failed to load EXE from ISO. Continuing BIOS.\n");
                        }
                    }
                    else if (psx_exe_filename && psx_exe_filename[0] != '\0')
                    {
                        if (Load_PSX_EXE(psx_exe_filename, &cpu) == 0)
                        {
                            DLOG("Binary loaded. Jump to PC=0x%08X\n", (unsigned)cpu.pc);
#ifdef ENABLE_HOST_LOG
                            host_log_file = fopen("output.log", "w");
#endif
                            binary_loaded = 1;
                            FlushCache(0);
                            FlushCache(2);
#ifdef ENABLE_STUCK_DETECTION
                            stuck_pc = cpu.pc;
                            stuck_count = 0;
#endif
                            continue;
                        }
                        else
                        {
                            printf("DYNAREC: Failed to load binary. Continuing BIOS.\n");
                        }
                    }
                    else
                    {
                        DLOG("No PSX EXE provided; continuing BIOS.\n");
                    }
                    binary_loaded = 1;
                }
            }

            /* === PC Alignment Check === */
            if (pc & 3)
            {
                cpu.cop0[PSX_COP0_BADVADDR] = pc;
                cpu.pc = pc;
                PSX_Exception(4);
                continue;
            }

            /* Look up compiled block */
            uint32_t *block = lookup_block(pc);
            if (!block)
            {
                block = compile_block(pc);
                if (!block)
                {
                    DLOG("IBE at PC=0x%08X\n", (unsigned)pc);
                    cpu.pc = pc;
                    PSX_Exception(6);
                    continue;
                }
                cache_block(pc, block);
                /* Back-patch any direct links waiting for this PC */
                apply_pending_patches(pc, block);
                FlushCache(0);
                FlushCache(2);
            }

            /* Execute the block */
            psx_block_exception = 1;
            if (setjmp(psx_block_jmp) == 0)
            {
                ((block_func_t)block)(&cpu, psx_ram, psx_bios);
            }
            psx_block_exception = 0;

            /* Advance global cycle counter using weighted cycle cost */
            uint32_t cache_idx = (pc >> 2) & BLOCK_CACHE_MASK;
            uint32_t cycles = block_cache[cache_idx].cycle_count;
            if (cycles == 0)
                cycles = block_cache[cache_idx].instr_count;
            if (cycles == 0)
                cycles = 8;
            global_cycles += cycles;
#ifdef ENABLE_DYNAREC_STATS
            stat_blocks_executed++;
            stat_total_cycles += cycles;
#endif

            /* Update CD-ROM deferred delivery + IRQ signal delay */
            CDROM_Update(cycles);

            /* Check for interrupts after each block */
            if (CheckInterrupts())
            {
                cpu.cop0[PSX_COP0_CAUSE] |= (1 << 10);
                uint32_t sr = cpu.cop0[PSX_COP0_SR];
                if ((sr & 1) && (sr & (1 << 10)))
                {
                    PSX_Exception(0);
                }
            }
            else
            {
                cpu.cop0[PSX_COP0_CAUSE] &= ~(1 << 10);
            }

            iterations++;

            /* ---- Periodic stats dump ---- */
            if ((iterations & 0x7FFFF) == 0) /* every ~512K iterations */
                dynarec_print_stats();

#ifdef ENABLE_STUCK_DETECTION
            if (pc == stuck_pc)
            {
                stuck_count++;
                if (stuck_count == 50000)
                {
                    DLOG("STUCK: Block at %08X ran 50000 times\n", (unsigned)pc);
                    DLOG("  SR=%08X I_STAT=%08X Cause=%08X\n",
                         (unsigned)cpu.cop0[PSX_COP0_SR],
                         (unsigned)CheckInterrupts(),
                         (unsigned)cpu.cop0[PSX_COP0_CAUSE]);
                }
            }
            else
            {
                stuck_pc = pc;
                stuck_count = 0;
            }
#endif
        } /* end inner while (blocks until deadline) */

        /* ---- Dispatch all events at or before current cycle ---- */
        Scheduler_DispatchEvents(global_cycles);

        /* ---- Periodic VRAM Dump ---- */
#ifdef ENABLE_VRAM_DUMP
        if (iterations >= next_vram_dump)
        {
            char filename[64];
            sprintf(filename, "vram_%u.bin", (unsigned)iterations);
            extern void DumpVRAM(const char *);
            DumpVRAM(filename);
            DLOG("VRAM Dumped to %s\n", filename);
            next_vram_dump += 1000000;
        }
#endif
    }
}
