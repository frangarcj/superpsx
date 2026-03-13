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
#ifdef PLATFORM_PSP
#include <psputils.h>  /* sceKernelDcacheWritebackAll, sceKernelIcacheInvalidateAll */
#else
#include <kernel.h>    /* FlushCache (PS2) */
#endif
#include "superpsx.h"
#include "scheduler.h"

#define LOG_TAG "DYNAREC"

/* ================================================================
 *  Constants
 * ================================================================ */
#define CODE_BUFFER_SIZE (4 * 1024 * 1024)

#define BLOCK_NODE_POOL_SIZE 32768

#define PATCH_SITE_MAX 8192

#define SCAN_MAX_INSNS 64 /* Max instructions analyzed per block scan */
#define DYN_SLOT_COUNT 8  /* Dynamic register slots: T0-T7 (dirty writeback to cpu.regs[]) */

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

/* Block scan result — filled by block_scan() pass 1, consumed by emit pass 2.
 * Enables multi-pass compilation: dirty bitmask, SMRV, future regalloc. */
typedef struct
{
    uint64_t dce_dead_mask;       /* bit i=1 → instruction[i] is dead (backward liveness) */
    uint32_t pinned_written_mask; /* bit r=1 → pinned PSX reg r is written in this block */
    uint32_t regs_written_mask;   /* bit r=1 → PSX reg r is written (any) */
    uint32_t regs_read_mask;      /* bit r=1 → PSX reg r is read */
    int insn_count;               /* Number of PSX instructions in block (incl. delay slot) */
    int has_mtc0_sr;              /* 1 if block contains MTC0 to COP0 reg 12 (SR) or RFE */
    int has_isc_write;            /* 1 if block has MTC0 to SR (can toggle ISC bit 16) — excludes RFE */
    int has_cop2;                 /* 1 if block contains COP2/LWC2/SWC2 instructions */
    uint32_t first_cop2_pc;       /* PC of first COP2/LWC2/SWC2 instruction (for hoisted CU2 exception) */
    uint8_t reg_access_count[32]; /* per-reg instruction access frequency (capped 255) */
} BlockScanResult;

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
#define REG_S6 22 /* Pinned: PSX $gp (28) */
#define REG_S7 23 /* Pinned: PSX $fp (30) */
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
#define REG_FP 30 /* $s8/$fp — pinned: jit_ht base addr for fast dispatch */
#define REG_RA 31
#define REG_SP 29
#define REG_ZERO 0

#define DYNAREC_PROLOGUE_WORDS 22

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

/* SMC handler: bumps page gen + flushes jit_ht for the affected page.
 * Called from the JIT const-address word-store fast path. */
void jit_smc_handler(uint32_t phys_addr);

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
} JitHTEntry;            /* 16 bytes: psx_pc[0], psx_pc[1], native[0], native[1] */

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
    if (jit_ht[h].psx_pc[0] == psx_pc)
    {
        jit_ht[h].native[0] = entry_native;
        return;
    }
    /* If already in slot 1, promote to slot 0 (MRU) */
    if (jit_ht[h].psx_pc[1] == psx_pc)
    {
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
    if (jit_ht[h].psx_pc[0] == psx_pc)
    {
        /* Promote slot 1 → slot 0 */
        jit_ht[h].psx_pc[0] = jit_ht[h].psx_pc[1];
        jit_ht[h].native[0] = jit_ht[h].native[1];
        jit_ht[h].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[h].native[1] = NULL;
    }
    else if (jit_ht[h].psx_pc[1] == psx_pc)
    {
        jit_ht[h].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[h].native[1] = NULL;
    }
}

/* ================================================================
 *  Shared state — compile-time
 * ================================================================ */
extern uint32_t blocks_compiled;
extern int jit_flush_pending;            /* 1 = D/I-cache needs flush before execution */
extern uint32_t total_instructions;
extern uint32_t block_cycle_count;
extern uint32_t emit_cycle_offset;
extern uint32_t block_pinned_dirty_mask; /* Pinned regs written in current block */
extern int block_isc_cached;             /* 1 if ISC bit is cached in SP+0 for this block */
extern int block_has_isc_write;          /* 1 if block can toggle ISC via MTC0 to SR (not RFE) */
extern int block_cu2_hoisted;            /* 1 if CU2 check hoisted to block prologue */
extern int block_lite_calls;             /* # of emit_call_c_lite calls in current block */
extern int block_full_calls;             /* # of emit_call_c calls in current block */
extern uint32_t emit_current_psx_pc;
extern int dynarec_load_defer;
extern int dynarec_lwx_pending;
extern RegStatus vregs[32];
extern uint32_t dirty_const_mask; /* Bitmask of dirty const vregs */
extern uint32_t smrv_known_ram;   /* SMRV: bit r=1 → PSX reg r is known RAM address */
static inline int smrv_is_known_ram(int r) { return (smrv_known_ram >> r) & 1; }
static inline void smrv_set_ram(int r)
{
    if (r)
        smrv_known_ram |= (1u << r);
}
static inline void smrv_clear(int r) { smrv_known_ram &= ~(1u << r); }

/* Alignment tracking: bit r=1 → PSX reg r is known word-aligned (4-byte).
 * Word-aligned implies half-aligned, so LH/LHU/SH checks can also be elided.
 * $gp/$sp/$fp/$ra are always word-aligned by PSX ABI. */
#define ALIGN_PINNED_MASK ((1u << 28) | (1u << 29) | (1u << 30) | (1u << 31))
extern uint32_t align_known_mask;
static inline int align_is_known(int r) { return (align_known_mask >> r) & 1; }
static inline void align_set(int r)
{
    if (r)
        align_known_mask |= (1u << r);
}
static inline void align_clear(int r) { align_known_mask &= ~(1u << r); }

/* ================================================================
 *  Shared state — stats / perf
 * ================================================================ */
#ifdef ENABLE_DYNAREC_STATS
extern uint64_t stat_cache_hits;
extern uint64_t stat_cache_misses;
extern uint64_t stat_cache_collisions;
extern uint64_t stat_cache_flushes;
extern uint64_t stat_blocks_executed;
extern uint64_t stat_total_cycles;
extern uint64_t stat_total_native_instrs;
extern uint64_t stat_total_psx_instrs;
#endif

#ifdef ENABLE_HOST_LOG
extern int host_log_fd;
void host_log_printf(const char *fmt, ...);
void host_log_putc(char c);
void host_log_flush(void);
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
#define EMIT_SUBU(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x23))
#define EMIT_OR(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x25))
#define EMIT_AND(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x24))
#define EMIT_SLL(rd, rt, sa) emit(MK_R(0, 0, (rt), (rd), (sa), 0x00))
#define EMIT_SRA(rd, rt, sa) emit(MK_R(0, 0, (rt), (rd), (sa), 0x03))
#define EMIT_LUI(rt, imm) emit(MK_I(0x0F, 0, (rt), (imm)))
#define EMIT_ORI(rt, rs, imm) emit(MK_I(0x0D, (rs), (rt), (imm)))
#define EMIT_ANDI(rt, rs, imm) emit(MK_I(0x0C, (rs), (rt), (imm)))
#define EMIT_MOVE(rd, rs) EMIT_ADDU(rd, rs, 0)
#define EMIT_JR(rs) emit(MK_R(0, (rs), 0, 0, 0, 0x08))
#define EMIT_JAL_ABS(addr) emit(MK_J(3, (uint32_t)(addr) >> 2))
#define EMIT_J_ABS(addr) emit(MK_J(2, (uint32_t)(addr) >> 2))
#define EMIT_BEQ(rs, rt, off) emit(MK_I(4, (rs), (rt), (off)))
#define EMIT_BNE(rs, rt, off) emit(MK_I(5, (rs), (rt), (off)))
#define EMIT_MULT(rs, rt) emit(MK_R(0, (rs), (rt), 0, 0, 0x18))
#define EMIT_MFLO(rd) emit(MK_R(0, 0, 0, (rd), 0, 0x12))
#define EMIT_MFHI(rd) emit(MK_R(0, 0, 0, (rd), 0, 0x10))

/* R5900 specialized emitters (MOVZ/MOVN/MADD/MADDU: same encoding on Allegrex) */
#define EMIT_MOVZ(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x0A))
#define EMIT_MOVN(rd, rs, rt) emit(MK_R(0, (rs), (rt), (rd), 0, 0x0B))
#define EMIT_MADD(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x00))
#define EMIT_MADDU(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x01))

#ifdef PLATFORM_PSP
/* PSP (Allegrex) has only one HI/LO pipeline — redirect pipeline 1 to pipeline 0 */
#define EMIT_MULT1(rs, rt)  EMIT_MULT((rs), (rt))
#define EMIT_MULTU1(rs, rt) emit(MK_R(0, (rs), (rt), 0, 0, 0x19))
#define EMIT_DIV1(rs, rt)   emit(MK_R(0, (rs), (rt), 0, 0, 0x1A))
#define EMIT_DIVU1(rs, rt)  emit(MK_R(0, (rs), (rt), 0, 0, 0x1B))
#define EMIT_MFLO1(rd)      EMIT_MFLO((rd))
#define EMIT_MFHI1(rd)      EMIT_MFHI((rd))
#define EMIT_MTLO1(rs)      emit(MK_R(0, (rs), 0, 0, 0, 0x13))
#define EMIT_MTHI1(rs)      emit(MK_R(0, (rs), 0, 0, 0, 0x11))

/* PMAXW(rd,rs,rt) = MAX(rs,rt)→rd.  All callers have rd==rs.
 * PSP fallback: SLT AT,rs,rt; MOVN rd,rt,AT  (if rs<rt: rd=rt) */
#define EMIT_PMAXW(rd, rs, rt) do { \
    emit(MK_R(0, (rs), (rt), REG_AT, 0, 0x2A)); /* SLT AT, rs, rt */ \
    emit(MK_R(0, (rt), REG_AT, (rd), 0, 0x0B)); /* MOVN rd, rt, AT */ \
} while (0)

/* PMINW(rd,rs,rt) = MIN(rs,rt)→rd.  All callers have rd==rs.
 * PSP fallback: SLT AT,rt,rs; MOVN rd,rt,AT  (if rt<rs: rd=rt) */
#define EMIT_PMINW(rd, rs, rt) do { \
    emit(MK_R(0, (rt), (rs), REG_AT, 0, 0x2A)); /* SLT AT, rt, rs */ \
    emit(MK_R(0, (rt), REG_AT, (rd), 0, 0x0B)); /* MOVN rd, rt, AT */ \
} while (0)

#else /* PLATFORM_PS2 (R5900) */
#define EMIT_MULT1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x18))
#define EMIT_MULTU1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x19))
#define EMIT_DIV1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x1A))
#define EMIT_DIVU1(rs, rt) emit(MK_R(0x1C, (rs), (rt), 0, 0, 0x1B))
#define EMIT_MFLO1(rd) emit(MK_R(0x1C, 0, 0, (rd), 0, 0x12))
#define EMIT_MFHI1(rd) emit(MK_R(0x1C, 0, 0, (rd), 0, 0x10))
/* R5900 MMI packed min/max (P19: batch saturation) */
#define EMIT_PMAXW(rd, rs, rt) emit(MK_R(0x1C, (rs), (rt), (rd), 0x03, 0x08))
#define EMIT_PMINW(rd, rs, rt) emit(MK_R(0x1C, (rs), (rt), (rd), 0x03, 0x28))
#define EMIT_MTLO1(rs) emit(MK_R(0x1C, (rs), 0, 0, 0, 0x13))
#define EMIT_MTHI1(rs) emit(MK_R(0x1C, (rs), 0, 0, 0, 0x11))
#endif /* PLATFORM_PSP / PLATFORM_PS2 */

/* COP1 FPU emitters (single-precision float) — used for GTE inline division.
 * COP1 R-type layout: 0x11 | fmt(rs) | ft(rt) | fs(rd) | fd(sa) | func
 * Note: R5900 FPU only supports single-precision. CVT.W.S truncates toward zero. */
#define EMIT_MTC1(rt, fs) emit(MK_R(0x11, 0x04, (rt), (fs), 0, 0))
#define EMIT_MFC1(rt, fs) emit(MK_R(0x11, 0x00, (rt), (fs), 0, 0))
#define EMIT_CVT_S_W(fd, fs) emit(MK_R(0x11, 0x14, 0, (fs), (fd), 0x20))
#define EMIT_CVT_W_S(fd, fs) emit(MK_R(0x11, 0x10, 0, (fs), (fd), 0x24))
#define EMIT_DIV_S(fd, fs, ft) emit(MK_R(0x11, 0x10, (ft), (fs), (fd), 0x03))
#define EMIT_MUL_S(fd, fs, ft) emit(MK_R(0x11, 0x10, (ft), (fs), (fd), 0x02))
#define EMIT_ADD_S(fd, fs, ft) emit(MK_R(0x11, 0x10, (ft), (fs), (fd), 0x00))
#define EMIT_LWC1(ft, off, base) emit(MK_I(0x31, (base), (ft), (off)))
#define EMIT_SWC1(ft, off, base) emit(MK_I(0x39, (base), (ft), (off)))

/* COP2 / VU0 macro mode emitters —— matrix multiply in JIT (PS2 only).
 * LQC2/SQC2: load/store 128-bit quadword (16-byte aligned).
 * VU0 compute: COP2 upper instructions (opcode=0x12, CO bit=1). */
#ifdef PLATFORM_PS2
#define EMIT_LQC2(vft, off, base) emit(MK_I(0x36, (base), (vft), (off)))
#define EMIT_SQC2(vft, off, base) emit(MK_I(0x3E, (base), (vft), (off)))

/* VU0 upper instruction builder: (0x12<<26)|(1<<25)|(dest<<21)|(ft<<16)|(fs<<11)|(fd<<6)|func */
#define MK_COP2(dest, ft, fs, fd, func) \
    ((0x12u << 26) | (1u << 25) | ((uint32_t)(dest) << 21) | \
     ((uint32_t)(ft) << 16) | ((uint32_t)(fs) << 11) | \
     ((uint32_t)(fd) << 6) | (uint32_t)(func))
#define VU_DEST_XYZ 0xE  /* dest mask: x|y|z (bits 24|23|22) */

/* VU0 matrix×vector sequence: VMULAX.xyz→VMADDAY.xyz→VMADDZ.xyz→VADD.xyz
 * ACC variants use fd as sub-opcode: 6=VMULA, 2=VMADDA. func=0x3C|bc. */
#define EMIT_VMULAX_XYZ(fs, ft) emit(MK_COP2(VU_DEST_XYZ, (ft), (fs), 6, 0x3C))
#define EMIT_VMADDAY_XYZ(fs, ft) emit(MK_COP2(VU_DEST_XYZ, (ft), (fs), 2, 0x3D))
#define EMIT_VMADDZ_XYZ(fd, fs, ft) emit(MK_COP2(VU_DEST_XYZ, (ft), (fs), (fd), 0x0A))
#define EMIT_VADD_XYZ(fd, fs, ft) emit(MK_COP2(VU_DEST_XYZ, (ft), (fs), (fd), 0x28))

#ifdef ENABLE_VU0_MICRO
/* VU0 micro mode: CTC2/CFC2 for CMSAR0 + status, VCALLMSR to launch.
 * CTC2 rt, rd: EE GPR → VU0 control reg  (rd=27 → CMSAR0)
 * CFC2 rt, rd: VU0 control reg → EE GPR  (rd=29 → VU0 status, bit 0 = VBS0)
 * VCALLMSR: launch micro at address in CMSAR0 */
#define EMIT_CTC2(rt, rd) emit((0x12u << 26) | (0x06u << 21) | ((uint32_t)(rt) << 16) | ((uint32_t)(rd) << 11))
#define EMIT_CFC2(rt, rd) emit((0x12u << 26) | (0x02u << 21) | ((uint32_t)(rt) << 16) | ((uint32_t)(rd) << 11))
#define EMIT_VCALLMSR()   emit(0x4A000039u)
#define EMIT_SYNC_L()     emit(0x0000000Fu)
#endif /* ENABLE_VU0_MICRO */
#endif /* PLATFORM_PS2 — VU0/COP2 macro emitters */

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
void emit_flush_pinned_selective(uint32_t mask);
void emit_reload_pinned(void);
void emit_call_c(uint32_t func_addr);
void emit_call_c_lite(uint32_t func_addr);
void emit_abort_check(uint32_t cycles);
void emit_load_imm32(int hwreg, uint32_t val);

void mark_vreg_const(int r, uint32_t val);
void mark_vreg_const_lazy(int r, uint32_t val);
void mark_vreg_var(int r);
int is_vreg_const(int r);
uint32_t get_vreg_const(int r);
void reset_vregs(void);
void flush_dirty_consts(void);

/* Scratch register cache: tracks which PSX GPR is in T8/T9 */
extern int t8_cached_psx_reg;
extern int t9_cached_psx_reg;
void reg_cache_invalidate(void);

/* Dynamic register slots — dirty writeback T0-T7.
 * Per-block allocation: top-N non-pinned regs mapped to T0-T7 (8 slots).
 * Dirty writeback: stores update the slot reg and set a compile-time dirty
 * bit.  Actual SW to cpu.regs[] is deferred to sync points (block exit,
 * C calls, abort).  This eliminates redundant memory writes on hot paths.
 * T0-T7 are EXCLUSIVE slots — never used as inline scratch.
 * Scratch registers: T8, T9, AT. */
extern int dyn_slot_psx[DYN_SLOT_COUNT];
extern int dyn_slots_active;
extern uint8_t dyn_dirty_mask; /* compile-time: bit i = slot i needs writeback */
void dyn_assign_slots(BlockScanResult *scan);
void dyn_load_slots(void);
void dyn_reload_slots(void);
void dyn_reset_slots(void);
void dyn_flush_dirty_slots(void);
void dyn_flush_all_slots(void);
void dyn_flush_verify_slots(void);
extern uint32_t dyn_mismatch_count;
void dyn_flush_one_slot(int r);
void dyn_reload_one_slot(int r);

/* Compile-loop helpers: use AT instead of T8/T9 for non-GPR temporaries */
void emit_cpu_field_to_psx_reg(int field_offset, int r);
void emit_materialize_psx_imm(int r, uint32_t value);
void emit_imm_to_cpu_field(int field_offset, uint32_t value);

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
void cold_slow_push(uint32_t *branches[], int num_branches,
                    uint32_t *return_point, uint32_t func_addr,
                    uint32_t psx_pc, int16_t cycle_offset,
                    uint8_t size, uint8_t type, uint8_t has_abort,
                    uint8_t saved_dirty);
void tlb_patch_emit_all(void);
int TLB_Backpatch(uint32_t epc);
extern int tlb_bp_map_count;

/* ================================================================
 *  Function prototypes — dynarec_compile.c
 * ================================================================ */
void block_scan(const uint32_t *code, int max_insns, BlockScanResult *out);
void emit_block_prologue(void);
void emit_block_epilogue(void);
void emit_branch_epilogue(uint32_t target_pc);
uint32_t r3000a_cycle_cost(uint32_t opcode);
int instruction_reads_gpr(uint32_t opcode, int reg);
int instruction_writes_gpr(uint32_t opcode, int reg);
uint32_t *compile_block(uint32_t psx_pc);

/* ================================================================
 *  Shared dispatch primitive — dynarec_run.c
 *
 *  Ensures a compiled block exists for the given PC:
 *    lookup → compile (if miss) → patch → HT populate
 *  Returns the native block pointer, or NULL on compile failure.
 *  If out_be is non-NULL, *out_be is set to the BlockEntry (or NULL).
 * ================================================================ */
uint32_t *dynarec_ensure_block(uint32_t pc, BlockEntry **out_be);

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
void dynarec_print_jit_profile(void);

#endif /* DYNAREC_H */
