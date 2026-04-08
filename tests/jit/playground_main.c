/*
 * JIT Playground — Main Entry Point
 *
 * Standalone ELF that initialises the JIT engine, then runs a suite of
 * micro-tests against it.  Each test injects R3000A opcodes into PSX
 * RAM, compiles + executes them through the real dynarec, and checks
 * the resulting CPU / memory state.
 *
 * Built as a separate CMake target that links the same dynarec, cpu
 * and memory modules as the main emulator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef PLATFORM_PS2
#include <kernel.h> /* FlushCache */
#elif defined(PLATFORM_PSP)
#include <pspkernel.h>
#include <pspdebug.h>
#include <psputils.h>
PSP_MODULE_INFO("JIT_Playground", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif

#include "playground.h"
#include "platform.h"
#include "interpreter.h"

/* ================================================================
 *  Globals used by the framework
 * ================================================================ */
PlaygroundResults pg_results = {0, 0, 0};
PGTestCtx pg_ctx;

/* ================================================================
 *  Minimal stubs for subsystems the dynarec references but
 *  the playground never exercises.
 * ================================================================ */

/* --- Hardware / IO --- */
/* Track HW accesses for SIO timing tests */
static uint32_t hw_read_log[64];
static uint32_t hw_write_log[64];  /* addr in [0], data in [1] pairs */
static int hw_read_count = 0;
static int hw_write_count = 0;
static uint32_t hw_read_return_val = 0; /* Value returned by ReadHardware */
static uint32_t hw_read_seq[64];       /* Sequence of return values */
static int hw_read_seq_len = 0;
static int hw_read_seq_idx = 0;

void pg_reset_hw_log(void)
{
    hw_read_count = 0;
    hw_write_count = 0;
    hw_read_return_val = 0;
    hw_read_seq_len = 0;
    hw_read_seq_idx = 0;
}

int pg_get_hw_read_count(void) { return hw_read_count; }
int pg_get_hw_write_count(void) { return hw_write_count / 2; }
uint32_t pg_get_hw_read_addr(int idx) { return (idx < hw_read_count) ? hw_read_log[idx] : 0; }
uint32_t pg_get_hw_write_addr(int idx) { return (idx*2 < hw_write_count) ? hw_write_log[idx*2] : 0; }
uint32_t pg_get_hw_write_data(int idx) { return (idx*2+1 < hw_write_count) ? hw_write_log[idx*2+1] : 0; }
void pg_set_hw_read_value(uint32_t val) { hw_read_return_val = val; }

void pg_set_hw_read_sequence(const uint32_t *values, int count)
{
    for (int i = 0; i < count && i < 64; i++)
        hw_read_seq[i] = values[i];
    hw_read_seq_len = count < 64 ? count : 64;
    hw_read_seq_idx = 0;
}

uint32_t ReadHardware(uint32_t addr)
{
    if (hw_read_count < 64)
        hw_read_log[hw_read_count++] = addr;
    if (hw_read_seq_len > 0 && hw_read_seq_idx < hw_read_seq_len)
        return hw_read_seq[hw_read_seq_idx++];
    return hw_read_return_val;
}
void WriteHardware(uint32_t addr,
                   uint32_t data, int width)
{
    (void)width;
    if (hw_write_count + 1 < 64) {
        hw_write_log[hw_write_count++] = addr;
        hw_write_log[hw_write_count++] = data;
    }
}
void SignalInterrupt(uint32_t irq) { (void)irq; }

/* --- GPU --- */
uint32_t gpu_stat = 0;
uint32_t gpu_busy_until = 0;
int gpu_pending_vblank_flush = 0;
volatile uint64_t gpu_irq_delay_cycle = 0;
void GPU_VBlank(void) {}
void GPU_Backend_VBlank(void) {}
uint32_t GPU_ReadStatus(void) { return gpu_stat; }
void GPU_DMA_Write(uint32_t a, int n)
{
    (void)a;
    (void)n;
}
void GPU_Write_GP0(uint32_t v) { (void)v; }
void GPU_Write_GP1(uint32_t v) { (void)v; }
uint32_t GPU_Read_GPUREAD(void) { return 0; }
void DumpVRAM(void) {}

/* --- SPU --- */
void SPU_GenerateSamples(void) {}
void SPU_FrameStart(void) {}
void SPU_Init(void) {}
void SPU_Shutdown(void) {}
int SPU_IsInitialized(void) { return 0; }
void SPU_WriteRegister(uint32_t a, uint16_t v)
{
    (void)a;
    (void)v;
}
uint16_t SPU_ReadRegister(uint32_t a)
{
    (void)a;
    return 0;
}
void SPU_DMA_Write(uint32_t a, int n)
{
    (void)a;
    (void)n;
}
void SPU_DMA_Read(uint32_t a, int n)
{
    (void)a;
    (void)n;
}

/* --- SIO --- */
uint32_t sio_data = 0;
int sio_tx_pending = 0;
int sio_selected = 0;
uint32_t SIO_Read(uint32_t a)
{
    (void)a;
    return 0;
}
void SIO_Write(uint32_t a, uint32_t v)
{
    (void)a;
    (void)v;
}

/* --- CDROM --- */
uint8_t cdrom_irq_active = 0;

/* --- Timers --- */
void Timer_ScheduleAll(void) {}

/* --- GTE --- (real implementations from gte.c; no stubs needed) */

/* --- Loader --- */
int Load_PSX_EXE(const char *p, uint32_t *pc)
{
    (void)p;
    (void)pc;
    return -1;
}
int Load_PSX_EXE_FromISO(const char *p, uint32_t *pc)
{
    (void)p;
    (void)pc;
    return -1;
}

/* --- DMA --- */
void DMA_WriteReg(uint32_t a, uint32_t v)
{
    (void)a;
    (void)v;
}
uint32_t DMA_ReadReg(uint32_t a)
{
    (void)a;
    return 0;
}
int DMA_IsPending(void) { return 0; }

/* --- Joystick --- */
void Joystick_Init(void) {}

/* --- Config --- */
#include "config.h"
PSXConfig psx_config; /* zero-initialised */

/* --- Profiler --- */
#include "profiler.h"
#ifdef ENABLE_SUBSYSTEM_PROFILER
int prof_disable_spu = 1;
int prof_disable_gpu_render = 1;
/* Minimal profiler stubs — the real profiler.c is not linked */
void profiler_init(void) {}
void profiler_frame_end(void) {}
#else
int prof_disable_spu = 1;
int prof_disable_gpu_render = 1;
#endif

/* --- Boot mode (run.c externs) --- */
int psx_boot_mode = 0;
const char *psx_exe_filename = NULL;

/* ================================================================
 *  JIT execution wrapper
 * ================================================================ */

/* Externs from dynarec */
extern uint32_t *code_buffer;
extern uint32_t *code_ptr;
extern BlockEntry *block_node_pool;
extern int block_node_pool_idx;
extern int patch_sites_count;
extern uint32_t blocks_compiled;
extern uint32_t total_instructions;
extern int tlb_bp_map_count;
extern JitHTEntry jit_ht[];
extern jit_l2_t jit_l1_ram[];
extern jit_l2_t jit_l1_bios[];
extern uint8_t jit_page_gen[];
extern uint32_t psx_tlb_base;

extern uint32_t *compile_block(uint32_t psx_pc);
extern void Free_PageTable(void);
extern void Init_Dynarec(void);
extern void Init_CPU(void);
extern void Init_Memory(void);
extern void Init_MemoryLUT(void);
extern volatile uint32_t psx_abort_pc;

/* Scheduler externs */
extern uint64_t global_cycles;

void pg_reset_jit_cache(void)
{
    /* Reset code pointer past trampolines */
    code_ptr = code_buffer + 144;

    /* Clear page tables */
    Free_PageTable();
    memset(jit_l1_ram, 0, sizeof(jit_l1_ram[0]) * 512);
    memset(jit_l1_bios, 0, sizeof(jit_l1_bios[0]) * 128);

    /* Clear block pool */
    block_node_pool_idx = 0;
    patch_sites_count = 0;
    blocks_compiled = 0;
    tlb_bp_map_count = 0;

    /* Clear hash table */
    for (int i = 0; i < JIT_HT_SIZE; i++)
    {
        jit_ht[i].psx_pc[0] = 0xFFFFFFFF;
        jit_ht[i].psx_pc[1] = 0xFFFFFFFF;
        jit_ht[i].native[0] = NULL;
        jit_ht[i].native[1] = NULL;
    }
    micro_cache_flush();

    jit_flush_pending = 1; /* Deferred: will flush before next block execution */
}

int pg_dump_next_block = 0; /* set to 1 to dump next compiled block */

void pg_run_jit(uint32_t pc, int32_t cycles)
{
    cpu.cycles_left = cycles;
    cpu.initial_cycles_left = cycles;
    cpu.block_aborted = 0;
    cpu.pc = pc;
    global_cycles = 0;

    int max_dispatches = 200;
    int first_block = 1;

    if (psx_config.interpreter)
    {
        run_interpreter_chain(cycles);
        return;
    }

    while (cpu.cycles_left > 0 && max_dispatches-- > 0)
    {
        uint32_t curr_pc = cpu.pc;

        /* Stop if we've left the test code / halt region */
        uint32_t phys = curr_pc & 0x1FFFFFFF;
        if (phys >= PSX_RAM_SIZE)
            break;

        BlockEntry *be;
        uint32_t *block = dynarec_ensure_block(curr_pc, &be);
        if (!block)
            break;

        /* Hex dump of the compiled native code */
        if (pg_dump_next_block && first_block)
        {
            first_block = 0;
            int nwords = (int)be->native_count;
            if (nwords > 200)
                nwords = 200;
            printf("  [DUMP] block PC=0x%08X nwords=%d\n",
                   curr_pc, nwords);
            for (int i = 0; i < nwords; i++)
            {
                printf("  [%3d] 0x%08X\n", i, block[i]);
            }
            pg_dump_next_block = 0;
        }

        /* Flush caches if any blocks were compiled since last execution */
        if (jit_flush_pending)
        {
            Platform_FlushDCache(NULL, NULL);
            Platform_FlushICache();
            jit_flush_pending = 0;
        }

        int32_t remaining = ((block_func_t)block)(&cpu, psx_ram, psx_bios, cpu.cycles_left);
        uint32_t cycles_taken = (uint32_t)(cpu.cycles_left - remaining);
        if (cycles_taken == 0)
            cycles_taken = 8;
        global_cycles += cycles_taken;
        cpu.cycles_left = remaining;

        /* Dispatch scheduler events that may have become due */
        if (global_cycles >= sched_cached_earliest)
            Sched_Tick(global_cycles);

        if (cpu.block_aborted)
        {
            cpu.pc = psx_abort_pc;
            cpu.block_aborted = 0;
        }
    }
}

/* ================================================================
 *  Main
 * ================================================================ */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("\n========================================\n");
    printf("   JIT Playground — SuperPSX\n");
    printf("========================================\n\n");

    /* 1. Initialise memory (allocates psx_ram, psx_bios, mem_lut) */
    Init_Memory();
    Init_MemoryLUT();

    /* Load config manually to pick up interpreter flag */
    FILE *f = fopen("superpsx.ini", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "interpreter=", 12) == 0) {
                psx_config.interpreter = atoi(line + 12);
            }
        }
        fclose(f);
    }

    /* 2. Initialise CPU struct */
    Init_CPU();

    /* 2b. Enable GTE fast paths (gte_use_vu0) for inline GTE tests */
#ifdef PLATFORM_PS2
    psx_config.gte_vu0 = 1;
    gte_use_vu0 = 1;
#elif defined(PLATFORM_PSP)
    psx_config.gte_vu0 = 1;
    gte_use_vu0 = 1;
#else
    psx_config.gte_vu0 = 0;
    gte_use_vu0 = 0;
#endif

    /* 3. Initialise dynarec (allocates code_buffer, builds trampolines) */
    Init_Dynarec();

    /* 4. Run all tests */
    pg_run_all_tests();

    /* 5. Summary */
    printf("\n========================================\n");
    printf("   Results: %d/%d passed",
           pg_results.passed, pg_results.total);
    if (pg_results.failed > 0)
        printf(", %d FAILED", pg_results.failed);
    printf("\n========================================\n\n");

    /* Return exit code for CI */
    return pg_results.failed > 0 ? 1 : 0;
}
