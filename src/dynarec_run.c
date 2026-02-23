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
#include "dynarec.h"
#include "spu.h"
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"
#include "loader.h"
#include "config.h"

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

/* Host log */
#ifdef ENABLE_HOST_LOG
FILE *host_log_file = NULL;
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

#ifdef ENABLE_PERF_REPORT
static uint64_t perf_last_report_cycle = 0;
static uint32_t perf_last_report_tick = 0;
#endif

/* Main execution flow state */
static int binary_loaded = 0;
static uint32_t run_iterations = 0;
static uint32_t idle_skip_pc = 0;
static uint32_t idle_skip_count = 0;

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
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31));
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
        *p++ = MK_I(0x2B, REG_S0, REG_S6, CPU_REG(2));
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31));
        *p++ = MK_I(0x09, REG_SP, REG_SP, (uint32_t)(int32_t)-32);
        *p++ = MK_I(0x2B, REG_SP, REG_RA, 28);
        *p++ = MK_R(0, REG_T0, 0, REG_RA, 0, 0x09); /* jalr t0 */
        *p++ = 0;
        *p++ = MK_I(0x23, REG_SP, REG_RA, 28);
        *p++ = MK_I(0x09, REG_SP, REG_SP, 32);
        *p++ = MK_I(0x23, REG_S0, REG_S6, CPU_REG(2));
        *p++ = MK_I(0x23, REG_S0, REG_V1, CPU_REG(3));
        *p++ = MK_I(0x23, REG_S0, REG_T3, CPU_REG(4));
        *p++ = MK_I(0x23, REG_S0, REG_T4, CPU_REG(5));
        *p++ = MK_I(0x23, REG_S0, REG_T5, CPU_REG(6));
        *p++ = MK_I(0x23, REG_S0, REG_T6, CPU_REG(7));
        *p++ = MK_I(0x23, REG_S0, REG_T7, CPU_REG(8));
        *p++ = MK_I(0x23, REG_S0, REG_T8, CPU_REG(9));
        *p++ = MK_I(0x23, REG_S0, REG_T9, CPU_REG(10));
        *p++ = MK_I(0x23, REG_S0, REG_S4, CPU_REG(29));
        *p++ = MK_I(0x23, REG_S0, REG_S7, CPU_REG(30));
        *p++ = MK_I(0x23, REG_S0, REG_S5, CPU_REG(31));
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08);
        *p++ = 0;
    }

    code_ptr = code_buffer + 128;

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
    /* Level-triggered CD-ROM IRQ re-assertion */
    if (cdrom_irq_active)
        SignalInterrupt(2);

    /* Fallback SIO IRQ7 check */
    if (sio_irq_delay_cycle && global_cycles >= sio_irq_delay_cycle)
    {
        sio_irq_delay_cycle = 0;
        SignalInterrupt(7);
    }

    /* Check and dispatch hardware interrupts */
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
            host_log_file = fopen("output.log", "w");
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

static void Sched_VBlank_Callback(void)
{
    uint32_t cycles_per_frame = psx_config.region_pal ? CYCLES_PER_FRAME_PAL : CYCLES_PER_FRAME_NTSC;
    Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK, global_cycles + cycles_per_frame, Sched_VBlank_Callback);
}

static void Sched_HBlank_Callback(void)
{
    uint32_t remaining = SCANLINES_PER_FRAME - hblank_scanline;
    uint32_t batch = (remaining < HBLANK_BATCH_SIZE) ? remaining : HBLANK_BATCH_SIZE;

    hblank_scanline += batch;

    if (hblank_scanline >= SCANLINES_PER_FRAME)
    {
        hblank_scanline = 0;
        GPU_VBlank();
        gpu_pending_vblank_flush = 1;
        SignalInterrupt(0);
        SPU_GenerateSamples();

        perf_frame_count++;
        check_profiling_exit(perf_frame_count);
        handle_performance_report();
    }

    /* Re-schedule HBlank */
    uint32_t next_remaining = SCANLINES_PER_FRAME - hblank_scanline;
    uint32_t next_batch = (next_remaining < HBLANK_BATCH_SIZE) ? next_remaining : HBLANK_BATCH_SIZE;
    hblank_ideal_deadline += (uint64_t)next_batch * CYCLES_PER_HBLANK;

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

    if (!block)
    {
        block = compile_block(pc);
        if (!block)
        {
            DLOG("IBE at %08X\n", (unsigned)pc);
            cpu.pc = pc;
            PSX_Exception(6);
            return RUN_RES_NORMAL;
        }
        be = lookup_block(pc);
        apply_pending_patches(pc, block);
        FlushCache(0);
        FlushCache(2);
    }

    /* Execute block / chain */
    int32_t cycles_left = (int32_t)(deadline - global_cycles);
    if (cycles_left < 0)
        cycles_left = 0;

    psx_block_exception = 1;
    int32_t remaining = ((block_func_t)block)(&cpu, psx_ram, psx_bios, cycles_left);
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
        uint32_t threshold = (be->is_idle == 1) ? 1 : ((pc >= 0xBFC00000) ? 2048 : 0x7FFFFFFF);
        if (++idle_skip_count >= threshold)
        {
            if (deadline > global_cycles)
                global_cycles = deadline;
            return RUN_RES_BREAK;
        }
    }
    else
    {
        idle_skip_count = 0;
    }

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
    {
        uint32_t cycles_per_frame = psx_config.region_pal ? CYCLES_PER_FRAME_PAL : CYCLES_PER_FRAME_NTSC;
        Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK, global_cycles + cycles_per_frame, Sched_VBlank_Callback);
    }

    hblank_scanline = 0;
    perf_frame_count = 0;
    perf_last_report_cycle = 0;
    perf_last_report_tick = get_wall_ms();
    hblank_ideal_deadline = global_cycles + HBLANK_BATCH_SIZE * CYCLES_PER_HBLANK;
    Scheduler_ScheduleEvent(SCHED_EVENT_HBLANK, hblank_ideal_deadline, Sched_HBlank_Callback);

    Timer_ScheduleAll();
    binary_loaded = 0;

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
        }

        if (global_cycles >= scheduler_cached_earliest)
            Scheduler_DispatchEvents(global_cycles);

        sync_hardware_and_interrupts();
    }
}
