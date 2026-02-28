/*
 * dynarec.h - Internal shared header for the SuperPSX dynarec subsystem
 *
 * Provides instruction encoding macros, register IDs, shared types,
 * extern declarations for cross-module state, and function prototypes
 * used across the dynarec_*.c modules.
 */
#ifndef DYNAREC_H
#define DYNAREC_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kernel.h> /* FlushCache */
#include "superpsx.h"
#include "scheduler.h"

#define LOG_TAG "DYNAREC"

/* ================================================================
 *  Constants
 * ================================================================ */
#define CODE_BUFFER_SIZE (4 * 1024 * 1024)

#define BLOCK_NODE_POOL_SIZE 32768

#define PATCH_SITE_MAX 8192

/* ================================================================
 *  Shared types
 * ================================================================ */
typedef struct BlockEntry
{
    uint32_t psx_pc;
    uint32_t *native;
    uint32_t instr_count;    /* Number of PSX instructions in this block */
    uint32_t native_count;   /* Number of native R5900 instructions generated */
    uint32_t cycle_count;    /* Weighted R3000A cycle count for this block */
    uint32_t is_idle;        /* 1 = idle loop (self-jump, no side effects) */
    struct BlockEntry *next; /* Collision chain pointer */
    uint32_t code_hash;      /* XOR hash of PSX block opcodes (legacy, unused) */
    uint8_t page_gen;        /* Page generation at compile time (SMC fast check) */
} BlockEntry;

typedef struct
{
    uint32_t *site_word; /* Address of the J instruction to overwrite */
    uint32_t target_psx_pc;
} PatchSite;

typedef struct
{
    uint32_t value;    /* Current constant value if is_const is 1 */
    uint16_t is_const; /* 1 if this register holds a constant value we know */
    uint16_t is_dirty; /* 1 if the value is newer than what's in cpu.regs[] (unused for now) */
} RegStatus;

typedef int32_t (*block_func_t)(R3000CPU *cpu, uint8_t *ram, uint8_t *bios, int32_t cycles_left);

/* ================================================================
 *  Opcode field extraction macros
 * ================================================================ */
#define OP(x) (((x) >> 26) & 0x3F)
#define RS(x) (((x) >> 21) & 0x1F)
#define RT(x) (((x) >> 16) & 0x1F)
#define RD(x) (((x) >> 11) & 0x1F)
#define SA(x) (((x) >> 6) & 0x1F)
#define FUNC(x) ((x) & 0x3F)
#define IMM16(x) ((x) & 0xFFFF)
#define SIMM16(x) ((int16_t)((x) & 0xFFFF))
#define TARGET(x) ((x) & 0x03FFFFFF)

/* ================================================================
 *  Hardware register IDs used in generated code
 * ================================================================ */
#define REG_AT 1 /* Assembler temporary */
#define REG_S0 16
#define REG_S1 17
#define REG_S2 18
#define REG_S3 19 /* Pinned: physical address mask 0x1FFFFFFF */
#define REG_S4 20 /* Pinned: PSX $sp (29) */
#define REG_S5 21 /* Pinned: PSX $ra (31) */
#define REG_S6 22 /* Pinned: PSX $v0 (2)  */
#define REG_S7 23 /* Pinned: PSX $s8 (30) */
#define REG_T0 8
#define REG_T1 9
#define REG_T2 10
#define REG_T3 11
#define REG_T4 12
#define REG_T5 13
#define REG_T6 14
#define REG_T7 15
#define REG_T8 24
#define REG_T9 25
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6
#define REG_A3 7
#define REG_V0 2
#define REG_V1 3
#define REG_FP 30 /* $fp/$s8 — callee-saved, available for future use */
#define REG_RA 31
#define REG_SP 29
#define REG_ZERO 0

#define DYNAREC_PROLOGUE_WORDS 27

/* ================================================================
 *  MIPS instruction builders
 * ================================================================ */
#define MK_R(op, rs, rt, rd, sa, fn)                                                  \
    ((((uint32_t)(op)) << 26) | (((uint32_t)(rs)) << 21) | (((uint32_t)(rt)) << 16) | \
     (((uint32_t)(rd)) << 11) | (((uint32_t)(sa)) << 6) | ((uint32_t)(fn)))
#define MK_I(op, rs, rt, imm)                                                         \
    ((((uint32_t)(op)) << 26) | (((uint32_t)(rs)) << 21) | (((uint32_t)(rt)) << 16) | \
     ((uint32_t)((imm) & 0xFFFF)))
#define MK_J(op, tgt) \
    ((((uint32_t)(op)) << 26) | ((uint32_t)((tgt) & 0x03FFFFFF)))

/* ================================================================
 *  Shared state — code buffer
 * ================================================================ */
extern uint32_t *code_buffer;
extern uint32_t *code_ptr;
extern uint32_t *abort_trampoline_addr;
extern uint32_t *call_c_trampoline_addr;
extern uint32_t *call_c_trampoline_lite_addr;
extern uint32_t *mem_slow_trampoline_addr;

/* ================================================================
 *  Shared state — Page Table (Lookup)
 * ================================================================ */
#define JIT_L1_RAM_PAGES 512  /* 2MB / 4KB */
#define JIT_L1_BIOS_PAGES 128 /* 512KB / 4KB */
#define JIT_L2_ENTRIES 1024   /* 4KB / 4 bytes */

typedef BlockEntry *(*jit_l2_t)[JIT_L2_ENTRIES];

extern jit_l2_t jit_l1_ram[JIT_L1_RAM_PAGES];
extern jit_l2_t jit_l1_bios[JIT_L1_BIOS_PAGES];

/* ================================================================
 *  SMC detection — Page generation counters
 *  Each 4KB RAM page has a generation counter. Incremented on writes.
 *  Blocks store the gen at compile time; mismatch = stale block.
 * ================================================================ */
extern uint8_t jit_page_gen[JIT_L1_RAM_PAGES];

static inline void jit_invalidate_page(uint32_t phys_addr)
{
    uint32_t page = phys_addr >> 12;
    if (page < JIT_L1_RAM_PAGES && jit_l1_ram[page] != NULL)
        jit_page_gen[page]++;
}

static inline uint8_t jit_get_page_gen(uint32_t phys_addr)
{
    uint32_t page = phys_addr >> 12;
    if (page < JIT_L1_RAM_PAGES)
        return jit_page_gen[page];
    return 0;
}

extern BlockEntry *block_node_pool;
extern int block_node_pool_idx;

/* ================================================================
 *  Shared state — direct block linking
 * ================================================================ */
extern PatchSite patch_sites[PATCH_SITE_MAX];
extern int patch_sites_count;
#ifdef ENABLE_DYNAREC_STATS
extern uint64_t stat_dbl_patches;
#endif

/* ================================================================
 *  Hash table for fast indirect jump dispatch (JR/JALR)
 *
 *  Instead of exiting to C on every JR $ra, the JIT does an inline
 *  hash lookup in ~14 R5900 instructions.  Inspired by pcsx-rearmed.
 * ================================================================ */
#define JIT_HT_SIZE 4096 /* Must be power of 2 */
#define JIT_HT_MASK (JIT_HT_SIZE - 1)

typedef struct
{
    uint32_t psx_pc[2];  /* PSX virtual address — 2-way set associative */
    uint32_t *native[2]; /* Pointer to compiled native code (past prologue) */
} JitHTEntry;  /* 16 bytes: psx_pc[0], psx_pc[1], native[0], native[1] */

extern JitHTEntry jit_ht[JIT_HT_SIZE];
extern uint32_t *jump_dispatch_trampoline_addr;

static inline uint32_t jit_ht_hash(uint32_t pc)
{
    return ((pc >> 12) ^ pc) & JIT_HT_MASK;
}

static inline void jit_ht_add(uint32_t psx_pc, uint32_t *native)
{
    uint32_t h = jit_ht_hash(psx_pc);
    uint32_t *entry_native = native + DYNAREC_PROLOGUE_WORDS;
    /* If already in slot 0, just update native pointer */
    if (jit_ht[h].psx_pc[0] == psx_pc) {
        jit_ht[h].native[0] = entry_native;
        return;
    }
    /* If already in slot 1, promote to slot 0 (MRU) */
    if (jit_ht[h].psx_pc[1] == psx_pc) {
        jit_ht[h].psx_pc[1] = jit_ht[h].psx_pc[0];
        jit_ht[h].native[1] = jit_ht[h].native[0];
        jit_ht[h].psx_pc[0] = psx_pc;
        jit_ht[h].native[0] = entry_native;
        return;
    }
    /* New entry: shift slot 0 → slot 1, insert at slot 0 */
    jit_ht[h].psx_pc[1] = jit_ht[h].psx_pc[0];
    jit_ht[h].native[1] = jit_ht[h].native[0];
    jit_ht[h].psx_pc[0] = psx_pc;
    jit_ht[h].native[0] = entry_native;
}

/* Remove a specific PC from the hash table (both slots) */
static inline void jit_ht_remove(uint32_t psx_pc)
{
    uint32_t h = jit_ht_hash(psx_pc);
    if (jit_ht[h].psx_pc[0] == psx_pc) {
        /* Promote slot 1 → slot 0 */
        jit_ht[h].psx_pc[0] = jit_ht[h].psx_pc[1];
        jit_ht[h].native[0] = jit_ht[h].native[1];
        jit_ht[h].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[h].native[1] = NULL;
    } else if (jit_ht[h].psx_pc[1] == psx_pc) {
        jit_ht[h].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[h].native[1] = NULL;
    }
}

/* ================================================================
 *  Shared state — compile-time
 * ================================================================ */
extern uint32_t blocks_compiled;
extern uint32_t total_instructions;
extern uint32_t block_cycle_count;
extern uint32_t emit_cycle_offset;
extern uint32_t emit_current_psx_pc;
extern int dynarec_load_defer;
extern int dynarec_lwx_pending;
extern RegStatus vregs[32];
extern uint32_t dirty_const_mask;  /* Bitmask of dirty const vregs */

/* ================================================================
 *  Shared state — stats / perf
 * ================================================================ */
#ifdef ENABLE_DYNAREC_STATS
extern uint64_t stat_cache_hits;
extern uint64_t stat_cache_misses;
extern uint64_t stat_cache_collisions;
extern uint64_t stat_blocks_executed;
extern uint64_t stat_total_cycles;
extern uint64_t stat_total_native_instrs;
extern uint64_t stat_total_psx_instrs;
#endif

#ifdef ENABLE_HOST_LOG
extern FILE *host_log_file;
#endif

/* ================================================================
 *  Inline emitter — emit() must be inlined for code-gen performance
 * ================================================================ */
static inline void emit(uint32_t inst)
{
    *code_ptr++ = inst;
}

/* ================================================================
 *  Common instruction emitters
 * ================================================================ */
#define EMIT_NOP() emit(0)
#define EMIT_LW(rt, off, base) emit(MK_I(0x23, (base), (rt), (off)))
#define EMIT_SW(rt, off, base) emit(MK_I(0x2B, (base), (rt), (off)))
#define EMIT_LH(rt, off, base) emit(MK_I(0x21, (base), (rt), (off)))
#define EMIT_LHU(rt, off, base) emit(MK_I(0x25, (base), (rt), (off)))
#define EMIT_LB(rt, off, base) emit(MK_I(0x20, (base), (rt), (off)))
#define EMIT_LBU(rt, off, base) emit(MK_I(0x24, (base), (rt), (off)))
#define EMIT_SH(rt, off, base) emit(MK_I(0x29, (base), (rt), (off)))
#define EMIT_SB(rt, off, base) emit(MK_I(0x28, (base), (rt), (off)))
#define EMIT_LWL(rt, off, base) emit(MK_I(0x22, (base), (rt), (off)))
#define EMIT_LWR(rt, off, base) emit(MK_I(0x26, (base), (rt), (off)))
#define EMIT_SWL(rt, off, base) emit(MK_I(0x2A, (base), (rt), (off)))
#define EMIT_SWR(rt, off, base) emit(MK_I(0x2E, (base), (rt), (off)))
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

/* R5900 specialized emitters */
#define EMIT_MOVZ(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x0A))
#define EMIT_MOVN(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x0B))
#define EMIT_MADD(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x00))
#define EMIT_MADDU(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x01))
#define EMIT_MULT1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x18))
#define EMIT_MULTU1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x19))
#define EMIT_DIV1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x1A))
#define EMIT_DIVU1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x1B))
#define EMIT_MFLO1(rd) emit(MK_R(0x1C, 0, 0, (rd), 0, 0x12))
#define EMIT_MFHI1(rd) emit(MK_R(0x1C, 0, 0, (rd), 0, 0x10))
#define EMIT_MTLO1(rs) emit(MK_R(0x1C, (rs), 0, 0, 0, 0x13))
#define EMIT_MTHI1(rs) emit(MK_R(0x1C, (rs), 0, 0, 0, 0x11))

/* ================================================================
 *  Function prototypes — dynarec_emit.c
 * ================================================================ */
extern const int psx_pinned_reg[32];

void emit_load_psx_reg(int hwreg, int r);
void emit_store_psx_reg(int r, int hwreg);
int emit_use_reg(int r, int scratch);
int emit_dst_reg(int r, int scratch);
void emit_sync_reg(int r, int host_reg);
void emit_flush_pinned(void);
void emit_reload_pinned(void);
void emit_call_c(uint32_t func_addr);
void emit_call_c_lite(uint32_t func_addr);
void emit_abort_check(void);
void emit_load_imm32(int hwreg, uint32_t val);

void mark_vreg_const(int r, uint32_t val);
void mark_vreg_const_lazy(int r, uint32_t val);
void mark_vreg_var(int r);
int is_vreg_const(int r);
uint32_t get_vreg_const(int r);
void reset_vregs(void);
void flush_dirty_consts(void);

/* ================================================================
 *  Function prototypes — dynarec_cache.c
 * ================================================================ */
/* ================================================================
 *  Function prototypes — dynarec_cache.c
 * ================================================================ */
static inline BlockEntry *lookup_block(uint32_t psx_pc)
{
    uint32_t phys = psx_pc & 0x1FFFFFFF;
    uint32_t l1_idx, l2_idx;
    BlockEntry *be = NULL;

    if (phys < PSX_RAM_SIZE)
    {
        l1_idx = phys >> 12;
        if (__builtin_expect(jit_l1_ram[l1_idx] != NULL, 1))
        {
            l2_idx = (phys >> 2) & (JIT_L2_ENTRIES - 1);
            be = (*jit_l1_ram[l1_idx])[l2_idx];
        }
    }
    else if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
    {
        l1_idx = (phys - 0x1FC00000) >> 12;
        if (__builtin_expect(jit_l1_bios[l1_idx] != NULL, 1))
        {
            l2_idx = (phys >> 2) & (JIT_L2_ENTRIES - 1);
            be = (*jit_l1_bios[l1_idx])[l2_idx];
        }
    }

#ifdef ENABLE_DYNAREC_STATS
    if (be)
        stat_cache_hits++;
    else
        stat_cache_misses++;
#endif

    return be;
}

static inline uint32_t *lookup_block_native(uint32_t psx_pc)
{
    BlockEntry *be = lookup_block(psx_pc);
    return be ? be->native : NULL;
}

void emit_direct_link(uint32_t target_psx_pc);
void apply_pending_patches(uint32_t target_psx_pc, uint32_t *native_addr);
uint32_t *get_psx_code_ptr(uint32_t psx_pc);
BlockEntry *cache_block(uint32_t psx_pc, uint32_t *native);
void Free_PageTable(void);

/* ================================================================
 *  Function prototypes — dynarec_memory.c
 * ================================================================ */
void emit_memory_read(int size, int rt_psx, int rs_psx, int16_t offset, int is_signed);
void emit_memory_read_signed(int size, int rt_psx, int rs_psx, int16_t offset);
void emit_memory_write(int size, int rt_psx, int rs_psx, int16_t offset);
void emit_memory_lwx(int is_left, int rt_psx, int rs_psx, int16_t offset, int use_load_delay);
void emit_memory_swx(int is_left, int rt_psx, int rs_psx, int16_t offset);
void cold_slow_reset(void);
void cold_slow_emit_all(void);
void tlb_patch_emit_all(void);
int  TLB_Backpatch(uint32_t epc);
extern int tlb_bp_map_count;

/* ================================================================
 *  Function prototypes — dynarec_compile.c
 * ================================================================ */
void emit_block_prologue(void);
void emit_block_epilogue(void);
void emit_branch_epilogue(uint32_t target_pc);
uint32_t r3000a_cycle_cost(uint32_t opcode);
int instruction_reads_gpr(uint32_t opcode, int reg);
int instruction_writes_gpr(uint32_t opcode, int reg);
uint32_t *compile_block(uint32_t psx_pc);

/* ================================================================
 *  Function prototypes — dynarec_insn.c
 * ================================================================ */
int emit_instruction(uint32_t opcode, uint32_t psx_pc, int *mult_count);
void debug_mtc0_sr(uint32_t val);
int BIOS_HLE_A(void);
int BIOS_HLE_B(void);
int BIOS_HLE_C(void);

/* ================================================================
 *  Function prototypes — dynarec_run.c
 * ================================================================ */
void dynarec_print_stats(void);

#endif /* DYNAREC_H */
