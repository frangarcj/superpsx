/*
 * dynarec_run.c - Init, execution loop, scheduler callbacks, stats
 *
 * Contains Init_Dynarec(), Run_CPU() (the main dispatch loop),
 * scheduler callbacks (HBlank/VBlank), performance reporting, and
 * all runtime variable definitions for the dynarec subsystem.
 */
#include <time.h>
#include "dynarec.h"
#include "spu.h"
#undef LOG_TAG
#include "gpu_state.h"
#undef LOG_TAG
#define LOG_TAG "DYNAREC"
#include "loader.h"

/* ================================================================
 *  Variable definitions — code buffer (owned by this module)
 * ================================================================ */
uint32_t *code_buffer;
uint32_t *code_ptr;
uint32_t *abort_trampoline_addr;
uint32_t *call_c_trampoline_addr = NULL;

/* ================================================================
 *  Variable definitions — host log
 * ================================================================ */
#ifdef ENABLE_HOST_LOG
FILE *host_log_file = NULL;
#endif

/* ================================================================
 *  Variable definitions — dynarec stats
 * ================================================================ */
#ifdef ENABLE_DYNAREC_STATS
uint64_t stat_cache_hits = 0;
uint64_t stat_cache_misses = 0;
uint64_t stat_cache_collisions = 0;
uint64_t stat_blocks_executed = 0;
uint64_t stat_total_cycles = 0;
uint64_t stat_total_native_instrs = 0;
uint64_t stat_total_psx_instrs = 0;
#endif

/* ================================================================
 *  dynarec_print_stats
 * ================================================================ */
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
 *  Init_Dynarec
 * ================================================================ */
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
     * We place a real trampoline here: JR $ra / NOP. */
    code_buffer[0] = MK_R(0, REG_RA, 0, 0, 0, 0x08); /* JR $ra */
    code_buffer[1] = 0;                              /* NOP (delay slot) */

    /* ---- Abort/Exit trampoline at code_buffer[2] ----
     * Shared exit path for mid-block exception aborts and DBL cache misses.
     * Flushes ALL pinned regs, restores callee-saved, returns to C dispatch. */
    abort_trampoline_addr = &code_buffer[2];
    {
        uint32_t *p = &code_buffer[2];
        /* Return remaining cycles */
        *p++ = MK_R(0, REG_S2, 0, REG_V0, 0, 0x25); /* or v0, s2, zero */
        /* Flush pinned PSX registers (12 registers) */
        *p++ = MK_I(0x2B, REG_S0, REG_S6, CPU_REG(2));  /* sw s6, CPU_REG(2)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));  /* sw v1, CPU_REG(3)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));  /* sw t3, CPU_REG(4)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));  /* sw t4, CPU_REG(5)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));  /* sw t5, CPU_REG(6)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));  /* sw t6, CPU_REG(7)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));  /* sw t7, CPU_REG(8)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));  /* sw t8, CPU_REG(9)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10)); /* sw t9, CPU_REG(10)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29)); /* sw s4, CPU_REG(29)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30)); /* sw s7, CPU_REG(30)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31)); /* sw s5, CPU_REG(31)(s0) */
        /* Restore callee-saved */
        *p++ = MK_I(0x23, REG_SP, REG_S7, 60); /* lw s7, 60(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S6, 56); /* lw s6, 56(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S5, 52); /* lw s5, 52(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S4, 48); /* lw s4, 48(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S3, 28); /* lw s3, 28(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S2, 32); /* lw s2, 32(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S1, 36); /* lw s1, 36(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_S0, 40); /* lw s0, 40(sp) */
        *p++ = MK_I(0x23, REG_SP, REG_RA, 44); /* lw ra, 44(sp) */
        *p++ = MK_I(0x09, REG_SP, REG_SP, 80); /* addiu sp, sp, 80 */
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08); /* jr ra */
        *p++ = 0;                              /* nop (delay slot) */
    }
    /* ---- C-call trampoline at code_buffer[32] ----
     * Helper for inline C calls to avoid emitting 24 flush/reload instructions
     * per memory access block. Target C function is passed in REG_T0. */
    call_c_trampoline_addr = &code_buffer[32];
    {
        uint32_t *p = call_c_trampoline_addr;
        /* Flush ALL pinned PSX registers (12 registers) */
        *p++ = MK_I(0x2B, REG_S0, REG_S6, CPU_REG(2));  /* sw s6, CPU_REG(2)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_V1, CPU_REG(3));  /* sw v1, CPU_REG(3)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T3, CPU_REG(4));  /* sw t3, CPU_REG(4)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T4, CPU_REG(5));  /* sw t4, CPU_REG(5)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T5, CPU_REG(6));  /* sw t5, CPU_REG(6)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T6, CPU_REG(7));  /* sw t6, CPU_REG(7)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T7, CPU_REG(8));  /* sw t7, CPU_REG(8)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T8, CPU_REG(9));  /* sw t8, CPU_REG(9)(s0)  */
        *p++ = MK_I(0x2B, REG_S0, REG_T9, CPU_REG(10)); /* sw t9, CPU_REG(10)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S4, CPU_REG(29)); /* sw s4, CPU_REG(29)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S7, CPU_REG(30)); /* sw s7, CPU_REG(30)(s0) */
        *p++ = MK_I(0x2B, REG_S0, REG_S5, CPU_REG(31)); /* sw s5, CPU_REG(31)(s0) */

        /* Save ra, provide ABI shadow space (32 bytes) */
        *p++ = MK_I(0x09, REG_SP, REG_SP, (uint32_t)(int32_t)-32); /* addiu sp, sp, -32 */
        *p++ = MK_I(0x2B, REG_SP, REG_RA, 28);                     /* sw ra, 28(sp) */

        /* Call the target function (address in REG_T0) */
        *p++ = MK_R(0, REG_T0, 0, REG_RA, 0, 0x09); /* jalr t0 */
        *p++ = 0;                                   /* nop */

        /* Restore ra & discard shadow space */
        *p++ = MK_I(0x23, REG_SP, REG_RA, 28); /* lw ra, 28(sp) */
        *p++ = MK_I(0x09, REG_SP, REG_SP, 32); /* addiu sp, sp, 32 */

        /* Reload ALL pinned PSX registers (12 registers) */
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

        /* Return to JIT block */
        *p++ = MK_R(0, REG_RA, 0, 0, 0, 0x08); /* jr ra */
        *p++ = 0;                              /* nop */
    }

    /* Reserve 128 words for trampolines; real compiled blocks start at [128] */
    code_ptr = code_buffer + 128;

    printf("  Code buffer at %p, size %d bytes\n", code_buffer, CODE_BUFFER_SIZE);
    printf("  Block cache at %p, %d entries\n", block_cache, BLOCK_CACHE_SIZE);
    printf("  Block node pool at %p, %d nodes\n", block_node_pool, BLOCK_NODE_POOL_SIZE);
    printf("  Slow-path trampoline at code_buffer[0] = %p\n", (void *)code_buffer);
    FlushCache(0);
    FlushCache(2);
}

/* ================================================================
 *  Scheduler callbacks
 * ================================================================ */
/* Forward declarations for scheduler callbacks */
static void Sched_VBlank_Callback(void);
static void Sched_HBlank_Callback(void);
void Timer_ScheduleAll(void);   /* Defined in hardware.c */
void CDROM_ScheduleEvent(void); /* Defined in cdrom.c */

/* HBlank scanline counter within the current frame (0..262 for NTSC) */
static uint32_t hblank_scanline = 0;

/* Ideal (drift-free) deadline for HBlank events.  We advance this by
 * exact multiples of CYCLES_PER_HBLANK so that block-overshoot in the
 * dynarec never accumulates into VBlank timing jitter. */
static uint64_t hblank_ideal_deadline = 0;

/* Emulation speed measurement */
static uint64_t perf_frame_count = 0;
static uint64_t perf_last_report_cycle = 0;
static uint32_t perf_last_report_tick = 0;

/* Get a millisecond-resolution wall-clock tick from PS2 hardware. */
static uint32_t get_wall_ms(void)
{
    return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC);
}

/* Number of scanlines to batch per HBlank event. */
#define HBLANK_BATCH_SIZE 32

static void Sched_HBlank_Callback(void)
{
    uint32_t remaining = SCANLINES_PER_FRAME - hblank_scanline;
    uint32_t batch = (remaining < HBLANK_BATCH_SIZE) ? remaining : HBLANK_BATCH_SIZE;

    hblank_scanline += batch;

    if (hblank_scanline >= SCANLINES_PER_FRAME)
    {
        hblank_scanline = 0;
        GPU_VBlank();
        gpu_pending_vblank_flush = 1; /* Trigger GS flush on emulated VBlank */
        SignalInterrupt(0); /* PSX IRQ0 = VBLANK */
        SPU_GenerateSamples();

        perf_frame_count++;

#ifdef ENABLE_PROFILING
        if (perf_frame_count >= 200)
        {
            printf("[PROFILE] Exiting after %llu frames for profiling.\n",
                   (unsigned long long)perf_frame_count);
            exit(0);
        }
#endif

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
                       speed_pct,
                       (double)cycles_per_sec / 1000000.0,
                       emu_fps,
                       (unsigned long long)elapsed_cycles,
                       elapsed_ms);
            }

            perf_last_report_tick = now_ms;
            perf_last_report_cycle = global_cycles;

            dynarec_print_stats();
        }
    }

    /* Re-schedule next HBlank batch using the ideal (drift-free) deadline.
     * Advancing from the ideal rather than global_cycles prevents block-
     * overshoot from accumulating into VBlank timing jitter. */
    {
        uint32_t next_remaining = SCANLINES_PER_FRAME - hblank_scanline;
        uint32_t next_batch = (next_remaining < HBLANK_BATCH_SIZE) ? next_remaining : HBLANK_BATCH_SIZE;
        hblank_ideal_deadline += (uint64_t)next_batch * CYCLES_PER_HBLANK;

        /* Safety: if we've fallen behind (shouldn't happen normally),
         * snap forward so we don't schedule in the past. */
        if (hblank_ideal_deadline <= global_cycles)
            hblank_ideal_deadline = global_cycles + 1;

        Scheduler_ScheduleEvent(SCHED_EVENT_HBLANK,
                                hblank_ideal_deadline,
                                Sched_HBlank_Callback);
    }
}

static void Sched_VBlank_Callback(void)
{
    Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK,
                            global_cycles + CYCLES_PER_FRAME_NTSC,
                            Sched_VBlank_Callback);
}

/* ================================================================
 *  Run_CPU — main dispatch loop
 * ================================================================ */
void Run_CPU(void)
{
    printf("Starting CPU Execution (Dynarec + Event Scheduler)...\n");

    /* ----- CPU Init ----- */
    cpu.pc = 0xBFC00000;
    cpu.cop0[PSX_COP0_SR] = 0x10400000;   /* Initial status: CU0=1, BEV=1 */
    cpu.cop0[PSX_COP0_PRID] = 0x00000002; /* R3000A */

    /* ----- Scheduler Init ----- */
    Scheduler_Init();

    Scheduler_ScheduleEvent(SCHED_EVENT_VBLANK,
                            global_cycles + CYCLES_PER_FRAME_NTSC,
                            Sched_VBlank_Callback);

    hblank_scanline = 0;
    perf_frame_count = 0;
    perf_last_report_cycle = 0;
    perf_last_report_tick = get_wall_ms();
    hblank_ideal_deadline = global_cycles + HBLANK_BATCH_SIZE * CYCLES_PER_HBLANK;
    Scheduler_ScheduleEvent(SCHED_EVENT_HBLANK,
                            hblank_ideal_deadline,
                            Sched_HBlank_Callback);

    Timer_ScheduleAll();

    uint32_t iterations = 0;
    uint32_t next_vram_dump = 1000000;
    int binary_loaded = 0;
    uint32_t idle_skip_pc = 0;
    uint32_t idle_skip_count = 0;
#define IDLE_SKIP_THRESHOLD 2048

#ifdef ENABLE_STUCK_DETECTION
    static uint32_t stuck_pc = 0;
    static uint32_t stuck_count = 0;
    static uint32_t heartbeat_counter = 0;
    static uint32_t last_heartbeat_pc = 0;
    static int heartbeat_dumped = 0;
#endif

    while (true)
    {
        uint64_t deadline = Scheduler_NextDeadlineFast();
        if (deadline == UINT64_MAX)
            deadline = global_cycles + 1024;

        while (global_cycles < deadline)
        {
            uint32_t pc = cpu.pc;

            /* === BIOS HLE Intercepts === */
            if (__builtin_expect((pc & 0x1FFFFFFF) <= 0xC0, 0))
            {
                uint32_t phys_pc = pc & 0x1FFFFFFF;
                if (phys_pc == 0xA0 && BIOS_HLE_A())
                    continue;
                if (phys_pc == 0xB0 && BIOS_HLE_B())
                    continue;
                if (phys_pc == 0xC0 && BIOS_HLE_C())
                    continue;
            }

            /* === BIOS Shell Hook === */
            if (__builtin_expect(!binary_loaded, 0) &&
                (pc == 0x80030000 || (pc >= 0x001A45A0 && pc <= 0x001A4620)))
            {
                if (!binary_loaded)
                {
                    DLOG("Reached BIOS Idle Loop (PC=%08X). Loading binary...\n", (unsigned)pc);
                    if (psx_boot_mode == BOOT_MODE_ISO)
                    {
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
            if (__builtin_expect(pc & 3, 0))
            {
                cpu.cop0[PSX_COP0_BADVADDR] = pc;
                cpu.pc = pc;
                PSX_Exception(4);
                continue;
            }

            /* Inline block lookup — fast path: direct hash hit */
            uint32_t cache_idx = (pc >> 2) & BLOCK_CACHE_MASK;
            BlockEntry *be = &block_cache[cache_idx];
            uint32_t *block;
            if (__builtin_expect(be->native != NULL && be->psx_pc == pc, 1))
            {
                block = be->native;
            }
            else
            {
                block = lookup_block(pc);
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
                    apply_pending_patches(pc, block);
                    FlushCache(0);
                    FlushCache(2);
                }
            }

            /* Calculate cycles left till deadline */
            int32_t cycles_left = (int32_t)(deadline - global_cycles);
            if (cycles_left < 0)
                cycles_left = 0;

            /* Execute the block */
            psx_block_exception = 1;
            int32_t remaining = ((block_func_t)block)(&cpu, psx_ram, psx_bios, cycles_left);
            psx_block_exception = 0;

            /* If an exception occurred mid-block, restore the correct PC */
            if (__builtin_expect(cpu.block_aborted, 0))
            {
                cpu.pc = psx_abort_pc;
                cpu.block_aborted = 0;
            }

            /* Advance global cycle counter */
            /* The real cycles taken is what we gave minus what's remaining */
            uint32_t cycles_taken = (uint32_t)(cycles_left - remaining);
            if (cycles_taken == 0)
                cycles_taken = 8;
            global_cycles += cycles_taken;

#ifdef ENABLE_DYNAREC_STATS
            stat_blocks_executed++;
            stat_total_cycles += cycles_taken;
            stat_total_native_instrs += be->native_count;
            stat_total_psx_instrs += be->instr_count;
#endif

            /* Level-triggered CD-ROM IRQ re-assertion */
            if (cdrom_irq_active)
                SignalInterrupt(2);

            /* Fallback SIO IRQ7: if the IO-operation trigger in WriteHardware
             * didn't fire (e.g., BIOS doesn't write I_STAT), fall back to the
             * cycle-based delay check here at C dispatch boundaries. */
            if (sio_irq_delay_cycle && global_cycles >= sio_irq_delay_cycle)
            {
                sio_irq_delay_cycle = 0;
                SignalInterrupt(7);
            }

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

            /* Idle/polling loop fast-forward — placed AFTER IRQ checks
             * so that pending interrupts (SIO IRQ7, CD-ROM) are delivered
             * before we skip ahead to the scheduler deadline. */
            {
                uint8_t idle_flag = be->is_idle;
                if (__builtin_expect(idle_flag && cpu.pc == pc, 0))
                {
                    if (pc != idle_skip_pc)
                    {
                        idle_skip_pc = pc;
                        idle_skip_count = 0;
                    }
                    idle_skip_count++;
                    uint32_t threshold;
                    if (idle_flag == 1)
                        threshold = 1;
                    else
                        threshold = (pc >= 0xBFC00000) ? 2048 : 0x7FFFFFFF;
                    if (idle_skip_count >= threshold)
                    {
                        if (deadline > global_cycles)
                            global_cycles = deadline;
                        break;
                    }
                }
                else
                {
                    idle_skip_count = 0;
                }
            }

            iterations++;

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
        if (global_cycles >= scheduler_cached_earliest)
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
