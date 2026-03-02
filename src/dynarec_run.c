/*
 * dynarec_run.c - Init, execution loop, scheduler callbacks, stats
 *
 * Contains Init_Dynarec(), Run_CPU() (the main dispatch loop),
 * scheduler callbacks (HBlank/VBlank), performance reporting, and
 * all runtime variable definitions for the dynarec subsystem.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "dynarec.h"
#include "spu.h"
#include "scheduler.h"
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"
#include "loader.h"
#include "config.h"
#include "profiler.h"

/* ================================================================
 *  Constants and Result Codes
 * ================================================================ */
#define HBLANK_BATCH_SIZE 32
#define ENABLE_PERF_REPORT /* Enabled by default for visible feedback */

/* Execution results for the JIT engine */
#define RUN_RES_NORMAL 0
#define RUN_RES_BREAK 1
#define RUN_RES_CONTINUE 2

/* ================================================================
 *  Module Variable Definitions
 * ================================================================ */

/* Code buffer / Memory (owned by this module) */
uint32_t *code_buffer;
uint32_t *code_ptr;
uint32_t *abort_trampoline_addr;
uint32_t *call_c_trampoline_addr = NULL;
uint32_t *call_c_trampoline_lite_addr = NULL;
uint32_t *mem_slow_trampoline_addr = NULL;

/* Hash table for fast JR/JALR dispatch */
JitHTEntry jit_ht[JIT_HT_SIZE] __attribute__((aligned(64)));
uint32_t *jump_dispatch_trampoline_addr = NULL;

/* Host log */
#ifdef ENABLE_HOST_LOG
int host_log_fd = -1;

/* Simple printf-to-fd helper (avoids stdio on host log) */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

void host_log_printf(const char *fmt, ...)
{
    if (host_log_fd < 0)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        ssize_t written = 0;
        while (written < n)
        {
            ssize_t w = write(host_log_fd, buf + written, n - written);
            if (w <= 0)
                break;
            written += w;
        }
    }
}

void host_log_putc(char c)
{
    if (host_log_fd < 0)
        return;
    write(host_log_fd, &c, 1);
}

void host_log_flush(void)
{
    /* no-op for fd writes */
}
#endif

/* Dynarec stats */
#ifdef ENABLE_DYNAREC_STATS
uint64_t stat_cache_hits = 0;
uint64_t stat_cache_misses = 0;
uint64_t stat_cache_collisions = 0;
uint64_t stat_blocks_executed = 0;
uint64_t stat_total_cycles = 0;
uint64_t stat_total_native_instrs = 0;
uint64_t stat_total_psx_instrs = 0;
#endif

/* Scheduler and Performance state */
static uint32_t hblank_scanline = 0;
static uint64_t hblank_ideal_deadline = 0;
static uint64_t perf_frame_count = 0;
static uint32_t cycles_per_hblank_runtime = CYCLES_PER_HBLANK_NTSC; /* Set at init based on region */
uint64_t hblank_frame_start_cycle = 0;                              /* Cycle at which current frame started (VBlank reset) */

/* Frame limiter: wall-clock target for next VBlank */
static uint32_t frame_limit_next_ms = 0;
static const uint32_t FRAME_TIME_NTSC_US = 16667; /* 1000000 / 60 */
static const uint32_t FRAME_TIME_PAL_US  = 20000; /* 1000000 / 50 */

#ifdef ENABLE_PERF_REPORT
static uint64_t perf_last_report_cycle = 0;
static uint32_t perf_last_report_tick = 0;
#endif

/* Main execution flow state */
static int binary_loaded = 0;
static uint32_t run_iterations = 0;
static uint32_t idle_skip_pc = 0;
static uint32_t idle_skip_count = 0;
static uint32_t poll_detect_pc = 0;

#ifdef ENABLE_VRAM_DUMP
static uint32_t next_vram_dump = 1000000;
#endif

#ifdef ENABLE_STUCK_DETECTION
static uint32_t stuck_pc = 0;
static uint32_t stuck_count = 0;
#endif

/* ================================================================
 *  Utility Functions
 * ================================================================ */

/* Get a millisecond-resolution wall-clock tick from PS2 hardware. */
static uint32_t get_wall_ms(void)
{
    return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC);
}

void dynarec_print_stats(void)
{
#ifdef ENABLE_DYNAREC_STATS
    uint64_t total_lookups = stat_cache_hits + stat_cache_misses;
    printf("[DYNAREC STATS]\n");
    printf("  Blocks executed : %llu\n", (unsigned long long)stat_blocks_executed);
    printf("  Total native R5900 instrs: %llu\n", (unsigned long long)stat_total_native_instrs);
    printf("  Total PSX R3000A instrs  : %llu\n", (unsigned long long)stat_total_psx_instrs);
    if (stat_total_psx_instrs > 0)
        printf("  Expansion Ratio : %.2f (R5900/PSX)\n",
               (double)stat_total_native_instrs / (double)stat_total_psx_instrs);
    printf("  Blocks compiled : %u\n", (unsigned)blocks_compiled);
    printf("  Cache hits      : %llu (%.1f%%)\n",
           (unsigned long long)stat_cache_hits,
           total_lookups ? (double)stat_cache_hits * 100.0 / total_lookups : 0.0);
    printf("  Cache misses    : %llu (compiles)\n", (unsigned long long)stat_cache_misses);
    printf("  Cache collisions: %llu\n", (unsigned long long)stat_cache_collisions);
    printf("  PSX cycles      : %llu\n", (unsigned long long)stat_total_cycles);
    printf("  DBL patches     : %llu\n", (unsigned long long)stat_dbl_patches);
    printf("  DBL pending     : %d\n", patch_sites_count);
    fflush(stdout);
#endif
}

/* ================================================================
 *  Dynarec Core Life Cycle
 * ================================================================ */

void Init_Dynarec(void)
{
    printf("Initializing Dynarec...\n");

    /* Allocate buffers */
    code_buffer = (uint32_t *)memalign(64, CODE_BUFFER_SIZE);
    block_node_pool = (BlockEntry *)memalign(64, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));

    if (!code_buffer || !block_node_pool)
    {
        printf("  ERROR: Failed to allocate dynarec buffers!\n");
        return;
    }

    code_ptr = code_buffer;
    memset(code_buffer, 0, CODE_BUFFER_SIZE);
    memset(block_node_pool, 0, BLOCK_NODE_POOL_SIZE * sizeof(BlockEntry));
    memset(jit_l1_ram, 0, sizeof(jit_l1_ram));
    memset(jit_l1_bios, 0, sizeof(jit_l1_bios));
    memset(jit_page_gen, 0, sizeof(jit_page_gen));

    /* Clear hash table — set all entries to unmatchable */
    for (int i = 0; i < JIT_HT_SIZE; i++)
    {
        jit_ht[i].psx_pc[0] = 0xFFFFFFFF;
        jit_ht[i].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[i].native[0] = NULL;
        jit_ht[i].native[1] = NULL;
    }

    block_node_pool_idx = 0;
    blocks_compiled = 0;
    total_instructions = 0;

    /* ---- Slow-path trampoline at code_buffer[0] ---- */
    code_buffer[0] = MK_R(0, REG_RA, 0, 0, 0, 0x08); /* JR $ra */
    code_buffer[1] = 0;                              /* NOP */

    /* ---- Abort/Exit trampoline at code_buffer[2] ---- */
    abort_trampoline_addr = &code_buffer[2];
    {
        uint32_t *p = &code_buffer[2];
        *p++ = MK_R(0, REG_S2, 0, REG_V0, 0, 0x25); /* or v0, s2, zero */
        *p++ = MK_I(0x2B, REG_S0, REG_S6, CPU_REG(2));
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_I(0x2B, REG_S0, REG_FP, CPU_REG(28));
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31));
        *p++ = MK_I(0x23, REG_SP, REG_FP, 68);
        *p++ = MK_I(0x23, REG_SP, REG_S7, 60);
        *p++ = MK_I(0x23, REG_SP, REG_S6, 56);
        *p++ = MK_I(0x23, REG_SP, REG_S5, 52);
        *p++ = MK_I(0x23, REG_SP, REG_S4, 48);
        *p++ = MK_I(0x23, REG_SP, REG_S3, 28);
        *p++ = MK_I(0x23, REG_SP, REG_S2, 32);
        *p++ = MK_I(0x23, REG_SP, REG_S1, 36);
        *p++ = MK_I(0x23, REG_SP, REG_S0, 40);
        *p++ = MK_I(0x23, REG_SP, REG_RA, 44);
        *p++ = MK_I(0x09, REG_SP, REG_SP, 80);
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08);
        *p++ = 0;
    }

    /* ---- C-call trampoline at code_buffer[32] ---- */
    call_c_trampoline_addr = &code_buffer[32];
    {
        uint32_t *p = call_c_trampoline_addr;
        /* Flush ALL 13 pinned regs to cpu struct (exception safety) */
        *p++ = MK_I(0x2B, REG_S0, REG_S6, CPU_REG(2));
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_I(0x2B, REG_S0, REG_FP, CPU_REG(28));
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31));
        /* Call target function */
        *p++ = MK_I(0x09, REG_SP, REG_SP, (uint32_t)(int32_t)-32);
        *p++ = MK_I(0x2B, REG_SP, REG_RA, 28);
        *p++ = MK_R(0, REG_T0, 0, REG_RA, 0, 0x09); /* jalr t0 */
        *p++ = 0;
        *p++ = MK_I(0x23, REG_SP, REG_RA, 28);
        *p++ = MK_I(0x09, REG_SP, REG_SP, 32);
        /* Reload ALL 13 pinned regs: C helpers may write to cpu.regs[]
         * for any register (e.g., Helper_ADD writes to cpu.regs[rd]). */
        *p++ = MK_I(0x23, REG_S0, REG_S6, CPU_REG(2));
        *p++ = MK_I(0x23, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x23, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x23, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x23, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x23, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x23, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x23, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x23, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_I(0x23, REG_S0, REG_FP, CPU_REG(28));
        *p++ = MK_I(0x23, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x23, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x23, REG_S0, REG_S5, CPU_REG(31));
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08);
        *p++ = 0;
    }

    /* ---- Lightweight C-call trampoline at code_buffer[68] ----
     * For C helpers that do NOT read/write cpu.regs[] (memory R/W,
     * LWL/LWR, SWL/SWR).  Only saves/restores the caller-saved
     * pinned registers (V1, T3-T9 = 8 regs), skipping the 5
     * callee-saved S-regs (S4, S5, S6, S7, FP) which the C ABI
     * preserves automatically.  Saves 10 instructions per call. */
    call_c_trampoline_lite_addr = &code_buffer[68];
    {
        uint32_t *p = call_c_trampoline_lite_addr;
        /* Flush only caller-saved pinned regs to cpu struct */
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10));
        /* Call target function */
        *p++ = MK_I(0x09, REG_SP, REG_SP, (uint32_t)(int32_t)-32);
        *p++ = MK_I(0x2B, REG_SP, REG_RA, 28);
        *p++ = MK_R(0, REG_T0, 0, REG_RA, 0, 0x09); /* jalr t0 */
        *p++ = 0;
        *p++ = MK_I(0x23, REG_SP, REG_RA, 28);
        *p++ = MK_I(0x09, REG_SP, REG_SP, 32);
        /* Reload caller-saved pinned regs */
        *p++ = MK_I(0x23, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x23, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x23, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x23, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x23, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x23, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x23, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x23, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08);
        *p++ = 0;
    }

    /* ---- Jump dispatch trampoline at code_buffer[96] ----
     * Fast inline dispatch for JR/JALR.  Instead of returning to C,
     * do an inline hash table lookup and jump directly to the target
     * block if found.  Reduces dispatch overhead from ~50 to ~14 instr.
     *
     * Entry conditions (set by JR/JALR emission code):
     *   T0 = target PSX PC (already stored in cpu.pc)
     *   S2 = cycles_left (already decremented by block_cycle_count)
     *   S0 = &cpu (pinned)
     *
     * Exit: jump to target native block, or fall through to abort.
     */
    jump_dispatch_trampoline_addr = &code_buffer[96];
    {
        uint32_t *p = &code_buffer[96];

        /* 1. If cycles <= 0, abort to C scheduler */
        *p++ = MK_I(0x07, REG_S2, REG_ZERO, 0); /* bgtz s2, +0 (patched below) */
        uint32_t *cyc_branch = p - 1;
        *p++ = 0;                                             /* delay: nop */
        *p++ = MK_J(2, (uint32_t)abort_trampoline_addr >> 2); /* j abort */
        *p++ = 0;                                             /* delay: nop */

        /* Patch the bgtz to skip the abort (target = here) */
        uint32_t *cyc_ok = p;
        int32_t cyc_off = (int32_t)(cyc_ok - cyc_branch - 1);
        *cyc_branch = (*cyc_branch & 0xFFFF0000) | ((uint32_t)cyc_off & 0xFFFF);

        /* 2. Compute hash: t1 = ((t0 >> 12) ^ t0) & JIT_HT_MASK */
        *p++ = MK_R(0, 0, REG_T0, REG_T1, 12, 0x02);     /* srl  t1, t0, 12 */
        *p++ = MK_R(0, REG_T1, REG_T0, REG_T1, 0, 0x26); /* xor  t1, t1, t0 */
        *p++ = MK_I(0x0C, REG_T1, REG_T1, JIT_HT_MASK);  /* andi t1, t1, MASK */

        /* 3. Scale to byte offset: t1 <<= 4 (sizeof(JitHTEntry) = 16, 2-way) */
        *p++ = MK_R(0, 0, REG_T1, REG_T1, 4, 0x00); /* sll  t1, t1, 4 */

        /* 4. Load hash table base: t2 = &jit_ht */
        uint32_t ht_addr = (uint32_t)&jit_ht[0];
        *p++ = MK_I(0x0F, 0, REG_T2, (ht_addr >> 16) & 0xFFFF); /* lui t2, hi */
        *p++ = MK_I(0x0D, REG_T2, REG_T2, ht_addr & 0xFFFF);    /* ori t2, lo */

        /* 5. Index into table: t1 = &jit_ht[hash] */
        *p++ = MK_R(0, REG_T1, REG_T2, REG_T1, 0, 0x21); /* addu t1, t1, t2 */

        /* 6. Check slot 0: t2 = psx_pc[0], at = native[0]
         *    Struct layout: { psx_pc[0]=+0, psx_pc[1]=+4, native[0]=+8, native[1]=+12 }
         *    NOTE: use AT ($1) for native ptr — T3 ($11) is pinned to PSX $a0. */
        *p++ = MK_I(0x23, REG_T1, REG_T2, 0);  /* lw t2, 0(t1) = psx_pc[0] */
        *p++ = MK_I(0x23, REG_T1, REG_AT, 8);  /* lw at, 8(t1) = native[0] */

        /* 7. If slot 0 matches, jump to @hit */
        *p++ = MK_I(0x04, REG_T2, REG_T0, 0);  /* beq t2, t0, @hit (patched) */
        uint32_t *hit0_branch = p - 1;
        *p++ = 0;                                /* delay: nop */

        /* 8. Slot 0 miss — check slot 1 */
        *p++ = MK_I(0x23, REG_T1, REG_T2, 4);  /* lw t2, 4(t1) = psx_pc[1] */
        *p++ = MK_I(0x05, REG_T2, REG_T0, 0);  /* bne t2, t0, @miss (patched) */
        uint32_t *miss_branch = p - 1;
        *p++ = MK_I(0x23, REG_T1, REG_AT, 12); /* (delay) lw at, 12(t1) = native[1] */

        /* 9. @hit: jump to native block — at has native[0] or native[1] */
        uint32_t *hit_target = p;
        int32_t hit0_off = (int32_t)(hit_target - hit0_branch - 1);
        *hit0_branch = (*hit0_branch & 0xFFFF0000) | ((uint32_t)hit0_off & 0xFFFF);

        *p++ = MK_R(0, REG_AT, 0, 0, 0, 0x08); /* jr at */
        *p++ = 0;                                /* delay: nop */

        /* 10. @miss: fall through to abort trampoline */
        uint32_t *miss_target = p;
        int32_t miss_off = (int32_t)(miss_target - miss_branch - 1);
        *miss_branch = (*miss_branch & 0xFFFF0000) | ((uint32_t)miss_off & 0xFFFF);
        *p++ = MK_J(2, (uint32_t)abort_trampoline_addr >> 2); /* j abort */
        *p++ = 0;                                             /* delay: nop */
    }

    /* ---- Memory slow-path trampoline at code_buffer[128] ----
     * Shared by all non-const memory reads/writes.
     * Entry: A0 = addr (reads) or A0 = addr, A1 = data (writes)
     *        T0 = C function pointer (ReadWord/WriteHalf/etc.)
     *        T2 = psx_pc (to store in cpu.current_pc)
     *        T1 = cycle offset (for partial_block_cycles)
     * Saves block RA, stores psx_pc, flushes partial cycles,
     * saves cycles_left, calls lite trampoline, returns to block. */
    mem_slow_trampoline_addr = &code_buffer[128];
    {
        uint32_t *p = &code_buffer[128];
        uint32_t pbc_addr = (uint32_t)&partial_block_cycles;
        uint16_t pbc_lo = pbc_addr & 0xFFFF;
        uint16_t pbc_hi = (pbc_addr + 0x8000) >> 16;

        *p++ = MK_I(0x2B, REG_SP, REG_RA, 64);                      /* sw ra, 64(sp) */
        *p++ = MK_I(0x2B, REG_S0, REG_T2, CPU_CURRENT_PC);          /* sw t2, cpu.current_pc */
        *p++ = MK_I(0x0F, 0, REG_AT, pbc_hi);                       /* lui at, hi(&pbc) */
        *p++ = MK_I(0x2B, REG_AT, REG_T1, (int16_t)pbc_lo);         /* sw t1, lo(&pbc) */
        *p++ = MK_I(0x2B, REG_S0, REG_S2, CPU_CYCLES_LEFT);         /* sw s2, cpu.cycles_left */
        *p++ = MK_J(3, (uint32_t)call_c_trampoline_lite_addr >> 2); /* jal lite_tramp */
        *p++ = 0;                                                   /* delay: nop */
        *p++ = MK_I(0x23, REG_SP, REG_RA, 64);                      /* lw ra, 64(sp) */
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08);                      /* jr ra */
        *p++ = 0;                                                   /* delay: nop */
    }

    code_ptr = code_buffer + 144;

    printf("  Code buffer at %p (%u KB)\n", code_buffer, CODE_BUFFER_SIZE / 1024);
    printf("  Page Table (L1) initialized: %u + %u entries\n", JIT_L1_RAM_PAGES, JIT_L1_BIOS_PAGES);
    FlushCache(0);
    FlushCache(2);
}

/* ================================================================
 *  Instruction / Execution Helpers (Internal Logic)
 * ================================================================ */

static inline void check_profiling_exit(uint64_t frame_count)
{
#ifdef ENABLE_PROFILING
    if (frame_count >= 200)
    {
        printf("[PROFILE] Exiting after %llu frames for profiling.\n", (unsigned long long)frame_count);
        exit(0);
    }
#endif
}

static inline void update_dynarec_stats(BlockEntry *be, uint32_t cycles_taken)
{
#ifdef ENABLE_DYNAREC_STATS
    stat_blocks_executed++;
    stat_total_cycles += cycles_taken;
    stat_total_native_instrs += be->native_count;
    stat_total_psx_instrs += be->instr_count;
#endif
}

/* ---- JIT chain hotspot tracker ----
 * Direct-mapped hash table: records entry PC + total cycles per
 * run_jit_chain call.  Dumped every profiler report interval. */
#ifdef ENABLE_SUBSYSTEM_PROFILER
#define HOTSPOT_SIZE 1024
#define HOTSPOT_MASK (HOTSPOT_SIZE - 1)
static struct {
    uint32_t pc;
    uint64_t total_cycles;
    uint32_t count;
} hotspot_table[HOTSPOT_SIZE];

static uint32_t hotspot_idle_skips = 0;
static uint64_t hotspot_idle_cycles_skipped = 0;

static inline void hotspot_record(uint32_t pc, uint32_t cycles)
{
    int idx = ((pc >> 2) ^ (pc >> 14)) & HOTSPOT_MASK;
    if (hotspot_table[idx].pc == pc || hotspot_table[idx].count == 0) {
        hotspot_table[idx].pc = pc;
        hotspot_table[idx].total_cycles += cycles;
        hotspot_table[idx].count++;
    }
    /* On collision, silently drop — acceptable for diagnostic use */
}

void jit_hotspot_dump_and_reset(FILE *out)
{
    /* Sort by total_cycles (simple selection of top 15) */
    int top_idx[15];
    uint64_t top_cycles[15];
    int i, j;
    for (i = 0; i < 15; i++) { top_idx[i] = -1; top_cycles[i] = 0; }

    for (i = 0; i < HOTSPOT_SIZE; i++) {
        if (hotspot_table[i].count == 0) continue;
        for (j = 0; j < 15; j++) {
            if (hotspot_table[i].total_cycles > top_cycles[j]) {
                /* Shift down */
                for (int k = 14; k > j; k--) {
                    top_idx[k] = top_idx[k-1];
                    top_cycles[k] = top_cycles[k-1];
                }
                top_idx[j] = i;
                top_cycles[j] = hotspot_table[i].total_cycles;
                break;
            }
        }
    }

    fprintf(out, "\nJIT Chain Hotspots (entry PC → total cycles, count):\n");
    fprintf(out, "  Idle skips: %u  (cycles skipped: %llu)\n",
            (unsigned)hotspot_idle_skips,
            (unsigned long long)hotspot_idle_cycles_skipped);
    for (i = 0; i < 15 && top_idx[i] >= 0; i++) {
        int idx = top_idx[i];
        fprintf(out, "  %2d. PC=%08X  cycles=%10llu  count=%6u  avg=%u\n",
                i + 1,
                (unsigned)hotspot_table[idx].pc,
                (unsigned long long)hotspot_table[idx].total_cycles,
                (unsigned)hotspot_table[idx].count,
                hotspot_table[idx].count ? (unsigned)(hotspot_table[idx].total_cycles / hotspot_table[idx].count) : 0);
    }

    memset(hotspot_table, 0, sizeof(hotspot_table));
    hotspot_idle_skips = 0;
    hotspot_idle_cycles_skipped = 0;
}
#else
static inline void hotspot_record(uint32_t pc, uint32_t cycles) { (void)pc; (void)cycles; }
void jit_hotspot_dump_and_reset(FILE *out) { (void)out; }
#endif

static inline void check_stuck_detection(uint32_t pc)
{
#ifdef ENABLE_STUCK_DETECTION
    if (pc == stuck_pc)
    {
        if (++stuck_count == 50000)
            DLOG("STUCK: Block at %08X ran 50000 times\n", (unsigned)pc);
    }
    else
    {
        stuck_pc = pc;
        stuck_count = 0;
    }
#endif
}

static inline void handle_vram_dump(uint32_t iterations)
{
#ifdef ENABLE_VRAM_DUMP
    if (iterations >= next_vram_dump)
    {
        char filename[64];
        sprintf(filename, "vram_%u.bin", (unsigned)iterations);
        extern void DumpVRAM(const char *);
        DumpVRAM(filename);
        next_vram_dump += 1000000;
    }
#endif
}

static inline void handle_performance_report(void)
{
#ifdef ENABLE_PERF_REPORT
    if ((perf_frame_count % 60) == 0)
    {
        uint32_t now_ms = get_wall_ms();
        uint32_t elapsed_ms = now_ms - perf_last_report_tick;
        uint64_t elapsed_cycles = global_cycles - perf_last_report_cycle;

        if (elapsed_ms > 0)
        {
            uint64_t cycles_per_sec = (elapsed_cycles * 1000ULL) / elapsed_ms;
            uint32_t speed_pct = (uint32_t)((cycles_per_sec * 100ULL) / PSX_CPU_FREQ);
            uint32_t emu_fps = (uint32_t)((perf_frame_count > 60 ? 60U : (uint32_t)perf_frame_count) * 1000U / elapsed_ms);

            printf("[EMU] Speed: %lu%% | %.1f MHz | ~%lu eFPS | %llu cycles in %lu ms\n",
                   speed_pct, (double)cycles_per_sec / 1000000.0, emu_fps,
                   (unsigned long long)elapsed_cycles, elapsed_ms);
        }

        perf_last_report_tick = now_ms;
        perf_last_report_cycle = global_cycles;
        dynarec_print_stats();
    }
#endif
}

static inline void sync_hardware_and_interrupts(void)
{
    /* Check and dispatch hardware interrupts.
     * CD-ROM IRQ re-assertion is now handled in the I_STAT ack path
     * (hardware.c).  SIO IRQ delay is now a scheduler event. */
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
}

static inline bool handle_bios_boot_hook(uint32_t pc)
{
    if (__builtin_expect(pc == 0x80030000 || (pc >= 0x001A45A0 && pc <= 0x001A4620), 0))
    {
        DLOG("Reached BIOS Idle Loop (PC=%08X). Loading binary...\n", (unsigned)pc);
        int success = 0;
        if (psx_boot_mode == BOOT_MODE_ISO)
            success = (Load_PSX_EXE_FromISO(&cpu) == 0);
        else if (psx_exe_filename && psx_exe_filename[0] != '\0')
            success = (Load_PSX_EXE(psx_exe_filename, &cpu) == 0);

        if (success)
        {
            DLOG("Binary loaded. Start PC=0x%08X\n", (unsigned)cpu.pc);
#ifdef ENABLE_HOST_LOG
            {
                int hfd = open("output.log", O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (hfd >= 0)
                    host_log_fd = hfd;
            }
#endif
            binary_loaded = 1;
            FlushCache(0);
            FlushCache(2);
            return true;
        }
        else
        {
            printf("DYNAREC: Failed to load binary. Continuing BIOS.\n");
            binary_loaded = 1;
        }
    }
    return false;
}

/* ================================================================
 *  Scheduler Integration
 * ================================================================ */

static void Sched_HBlank_Callback(void)
{
    uint32_t remaining = SCANLINES_PER_FRAME - hblank_scanline;
    uint32_t batch = (remaining < HBLANK_BATCH_SIZE) ? remaining : HBLANK_BATCH_SIZE;

    hblank_scanline += batch;

    if (hblank_scanline >= SCANLINES_PER_FRAME)
    {
        hblank_scanline = 0;

        /* Subsystem profiler: compute PSX cycles for this frame BEFORE resetting */
        uint64_t frame_psx_cycles = global_cycles - hblank_frame_start_cycle;

        hblank_frame_start_cycle = global_cycles;
        GPU_VBlank();
        GTE_VBlankUpdate();
        gpu_pending_vblank_flush = 1;
        SignalInterrupt(0);
        Timer_ScheduleAll();   /* Reschedule timers after VBlank reset */
        SPU_GenerateSamples(); /* Generate all audio + submit to audio hw */

        /* Frame limiter: busy-wait until the wall-clock frame budget is met.
         * clock() on PS2 gives microsecond resolution via EE timer. */
        if (psx_config.frame_limit)
        {
            uint32_t frame_us = psx_config.region_pal ? FRAME_TIME_PAL_US : FRAME_TIME_NTSC_US;
            uint32_t now_us = (uint32_t)clock();
            if (frame_limit_next_ms == 0)
                frame_limit_next_ms = now_us + frame_us; /* first frame */
            else
            {
                while ((int32_t)(frame_limit_next_ms - (uint32_t)clock()) > 0)
                    ; /* busy-wait */
                now_us = (uint32_t)clock();
                /* If we overshot by more than 2 frames, resync to avoid catch-up burst */
                if ((int32_t)(now_us - frame_limit_next_ms) > (int32_t)(frame_us * 2))
                    frame_limit_next_ms = now_us + frame_us;
                else
                    frame_limit_next_ms += frame_us;
            }
        }

        perf_frame_count++;
        check_profiling_exit(perf_frame_count);
        handle_performance_report();

        profiler_frame_end(frame_psx_cycles);
    }

    /* Re-schedule HBlank */
    uint32_t next_remaining = SCANLINES_PER_FRAME - hblank_scanline;
    uint32_t next_batch = (next_remaining < HBLANK_BATCH_SIZE) ? next_remaining : HBLANK_BATCH_SIZE;
    hblank_ideal_deadline += (uint64_t)next_batch * cycles_per_hblank_runtime;

    if (hblank_ideal_deadline <= global_cycles)
        hblank_ideal_deadline = global_cycles + 1;

    Scheduler_ScheduleEvent(SCHED_EVENT_HBLANK, hblank_ideal_deadline, Sched_HBlank_Callback);
}

/* ================================================================
 *  The JIT Core
 * ================================================================ */

static inline int run_jit_chain(uint64_t deadline)
{
    uint32_t pc = cpu.pc;

    /* Dynamic polling skip: if this PC was seen as a self-loop last time,
     * skip immediately instead of executing another polling iteration. */
    if (__builtin_expect(pc == poll_detect_pc, 0))
    {
        poll_detect_pc = 0;
        if (deadline > global_cycles) {
#ifdef ENABLE_SUBSYSTEM_PROFILER
            hotspot_idle_skips++;
            hotspot_idle_cycles_skipped += (deadline - global_cycles);
#endif
            global_cycles = deadline;
        }
        return RUN_RES_BREAK;
    }

    /* Address Error on misaligned PC (AdEL — instruction fetch from bad addr).
     * cpu.current_pc holds the JR/JALR source instruction address. */
    if (__builtin_expect(pc & 3, 0))
    {
        cpu.cop0[PSX_COP0_BADVADDR] = pc;
        cpu.pc = cpu.current_pc; /* EPC = instruction that set the bad PC */
        PSX_Exception(4);        /* AdEL */
        return RUN_RES_NORMAL;
    }

    /* Block Lookup - SOTA Page Table */
    BlockEntry *be = lookup_block(pc);
    uint32_t *block = be ? be->native : NULL;

    /* Populate hash table for fast JR/JALR dispatch.
     * Skip if PC is already in slot 0 (common in hot loops). */
    if (block)
    {
        uint32_t h = jit_ht_hash(pc);
        if (__builtin_expect(jit_ht[h].psx_pc[0] != pc, 0))
            jit_ht_add(pc, block);
    }

    /* Two-tier SMC detection:
     * Tier 1: O(1) page generation check (fast reject for clean pages).
     * Tier 2: O(N) hash verification (only when page was written to).
     * If hash still matches, update block's page_gen to avoid repeated checks. */
    if (block && be)
    {
        uint32_t phys = pc & 0x1FFFFFFF;
        if (phys < PSX_RAM_SIZE && be->page_gen != jit_get_page_gen(phys))
        {
            /* Page was written to since compilation — verify opcodes */
            uint32_t *opcodes = get_psx_code_ptr(pc);
            if (opcodes)
            {
                uint32_t hash = 0;
                for (uint32_t i = 0; i < be->instr_count; i++)
                    hash = (hash << 5) + hash + opcodes[i];
                if (hash != be->code_hash)
                {
                    be->native = NULL;
                    /* Clear stale hash table entry to prevent dispatch
                     * trampoline from jumping to invalidated code */
                    jit_ht_remove(pc);
                    block = NULL;
                    be = NULL;
                }
                else
                {
                    /* Code unchanged — update page_gen to skip future hashes */
                    be->page_gen = jit_get_page_gen(phys);
                }
            }
        }
    }

    if (!block)
    {
        PROF_PUSH(PROF_JIT_COMPILE);
        block = compile_block(pc);
        PROF_POP(PROF_JIT_COMPILE);
        PROF_COUNT_COMPILE();
        if (!block)
        {
            DLOG("IBE at %08X\n", (unsigned)pc);
            cpu.pc = pc;
            PSX_Exception(6);
            return RUN_RES_NORMAL;
        }
        be = lookup_block(pc);
        apply_pending_patches(pc, block);
        jit_ht_add(pc, block);
        FlushCache(0);
        FlushCache(2);
    }

    /* Execute block / chain */
    int32_t cycles_left = (int32_t)(deadline - global_cycles);
    if (cycles_left < 0)
        cycles_left = 0;

    cpu.initial_cycles_left = cycles_left;
    cpu.cycles_left = cycles_left;

    psx_block_exception = 1;
    PROF_PUSH(PROF_JIT_EXEC);
    int32_t remaining = ((block_func_t)block)(&cpu, psx_ram, psx_bios, cycles_left);
    PROF_POP(PROF_JIT_EXEC);
    PROF_COUNT_BLOCK();
    psx_block_exception = 0;

    if (__builtin_expect(cpu.block_aborted, 0))
    {
        cpu.pc = psx_abort_pc;
        cpu.block_aborted = 0;
    }

    uint32_t cycles_taken = (uint32_t)(cycles_left - remaining);
    if (cycles_taken == 0)
        cycles_taken = 8;
    global_cycles += cycles_taken;
    partial_block_cycles = 0; /* Reset mid-block cycle offset */

    hotspot_record(pc, cycles_taken);

    if (be)
        update_dynarec_stats(be, cycles_taken);
    run_iterations++;
    check_stuck_detection(pc);
    handle_vram_dump(run_iterations);

    /* Idle skip logic (integrated execution control) */
    if (__builtin_expect(be && be->is_idle && cpu.pc == pc, 0))
    {
        if (pc != idle_skip_pc)
        {
            idle_skip_pc = pc;
            idle_skip_count = 0;
        }
        uint32_t threshold = (be->is_idle == 1) ? 1 : 2;
        if (++idle_skip_count >= threshold)
        {
            if (deadline > global_cycles) {
#ifdef ENABLE_SUBSYSTEM_PROFILER
                hotspot_idle_skips++;
                hotspot_idle_cycles_skipped += (deadline - global_cycles);
#endif
                global_cycles = deadline;
            }
            return RUN_RES_BREAK;
        }
    }
    else
    {
        idle_skip_count = 0;
    }

    /* Dynamic polling detection: if a chain exits to the same PC
     * it entered (cpu.pc == entry_pc), the block self-looped via DBL
     * until cycles exhausted.  Mark it so the NEXT entry to this PC
     * skips immediately (handled at top of run_jit_chain). */
    if (__builtin_expect(cpu.pc == pc, 0))
        poll_detect_pc = pc;
    else
        poll_detect_pc = 0;

    return RUN_RES_NORMAL;
}

/* ================================================================
 *  Main Entry Point
 * ================================================================ */

void Run_CPU(void)
{
    printf("Starting CPU Execution...\n");

    /* Initial state */
    cpu.pc = 0xBFC00000;
    cpu.cop0[PSX_COP0_SR] = 0x10400000;
    cpu.cop0[PSX_COP0_PRID] = 0x00000002;

    Scheduler_Init();

    hblank_scanline = 0;
    perf_frame_count = 0;
    perf_last_report_cycle = 0;
    perf_last_report_tick = get_wall_ms();
    cycles_per_hblank_runtime = psx_config.region_pal ? CYCLES_PER_HBLANK_PAL : CYCLES_PER_HBLANK_NTSC;
    hblank_frame_start_cycle = global_cycles;
    hblank_ideal_deadline = global_cycles + HBLANK_BATCH_SIZE * cycles_per_hblank_runtime;
    Scheduler_ScheduleEvent(SCHED_EVENT_HBLANK, hblank_ideal_deadline, Sched_HBlank_Callback);

    Timer_ScheduleAll();

    /* Subsystem profiler: apply config disable flags and init */
    prof_disable_spu = psx_config.disable_audio;
    prof_disable_gpu_render = psx_config.disable_gpu;
    profiler_init();

    binary_loaded = 0;
    static uint32_t bios_trace_count = 0;
    static uint32_t bios_last_pc = 0;
    static uint32_t bios_same_count = 0;

    /* Phase 1: BIOS */
    printf("DYNAREC: Phase 1 - BIOS Booting...\n");
    while (!binary_loaded)
    {
        uint64_t deadline = Scheduler_NextDeadlineFast();
        if (deadline == UINT64_MAX)
            deadline = global_cycles + 1024;

        while (global_cycles < deadline)
        {
            if (handle_bios_boot_hook(cpu.pc))
                break;
            bios_trace_count++;
            if (cpu.pc == bios_last_pc)
            {
                bios_same_count++;
                if (bios_same_count == 10000)
                {
                    printf("[BIOS-STUCK] PC=%08X stuck for 10000 iters at cycle %llu\n",
                           (unsigned)cpu.pc, (unsigned long long)global_cycles);
                    /* Dump some register state */
                    printf("[BIOS-STUCK] regs: v0=%08X v1=%08X a0=%08X a1=%08X sp=%08X ra=%08X\n",
                           (unsigned)cpu.regs[2], (unsigned)cpu.regs[3],
                           (unsigned)cpu.regs[4], (unsigned)cpu.regs[5],
                           (unsigned)cpu.regs[29], (unsigned)cpu.regs[31]);
                    fflush(stdout);
                }
            }
            else
            {
                bios_last_pc = cpu.pc;
                bios_same_count = 0;
            }
            if (run_jit_chain(deadline) == RUN_RES_BREAK)
                break;
        }

        if (global_cycles >= scheduler_cached_earliest)
            Scheduler_DispatchEvents(global_cycles);

        sync_hardware_and_interrupts();
    }

    /* Phase 2: Main Execution */
    printf("DYNAREC: Phase 2 - Main Execution...\n");
    while (true)
    {
        uint64_t deadline = Scheduler_NextDeadlineFast();
        if (deadline == UINT64_MAX)
            deadline = global_cycles + 1024;

        while (global_cycles < deadline)
        {
            if (run_jit_chain(deadline) == RUN_RES_BREAK)
                break;
            /* A hardware write (e.g. DMA CHCR) may have scheduled a new
             * event with a deadline earlier than our current batch deadline.
             * Break out so the outer loop re-reads scheduler_cached_earliest
             * and uses the closer deadline for the next batch. */
            if (scheduler_cached_earliest < deadline)
                break;
        }

        if (global_cycles >= scheduler_cached_earliest)
        {
            PROF_PUSH(PROF_SCHEDULER);
            Scheduler_DispatchEvents(global_cycles);
            PROF_POP(PROF_SCHEDULER);
        }

        sync_hardware_and_interrupts();
    }
}
