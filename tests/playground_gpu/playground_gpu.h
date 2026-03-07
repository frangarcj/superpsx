/*
 * GPU Playground — Test DSL Header
 *
 * Provides macros and structures for testing the expansion and CPU cost
 * of PSX GPU commands.
 */
#ifndef PLAYGROUND_GPU_H
#define PLAYGROUND_GPU_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gpu_state.h"

/* ================================================================
 *  Test Framework Structures
 * ================================================================ */

/* Test result tracking */
typedef struct {
    int total;
    int passed;
    int failed;
} GPUPlaygroundResults;

extern GPUPlaygroundResults gp_results;

/* Current test context */
typedef struct {
    const char *name;
    int fail_count;
    
    /* Metrics collected by the mock framework */
    uint32_t qwords_generated;
    uint32_t eecycles_used;
    uint32_t eeinsns_used;
} GPTestCtx;

extern GPTestCtx gp_ctx;

/* ================================================================
 *  Mock Intercept Counters
 * ================================================================ */

/* External definition from gpu_core.c we'll need to reset */
extern uint32_t mock_gif_qwords_written;

/* Fast GIF pointer offset tracking */
extern gif_qword_t *fast_gif_ptr;
extern gif_qword_t *mock_gif_buffer_base;
#define MOCK_GIF_BUFFER_START mock_gif_buffer_base

/* ================================================================
 *  Performance Counters (Cop0 EE)
 * ================================================================ */
static inline void perf_start(void) {
    /* 
     * Configure Performance Counter 0 (Processor Cycle) and 1 (Single instruction issue)
     * PCR: 
     *   EXL0 = 1, K0 = 1, S0 = 1, U0 = 1, EVENT0 = 0x01 (CPU cycles)
     *   EXL1 = 1, K1 = 1, S1 = 1, U1 = 1, EVENT1 = 0x02 (Single instruction issue)
     * Enable counters = bit 31 (CTE)
     */
    uint32_t pccr = 0 | 
        (1 << 31) |       /* CTE: Enable counting */
        (1 << 4) | (1 << 3) | (1 << 2) | (1 << 1) | /* EXL, K, S, U for mode 0 */
        (0x01 << 5) |     /* Event 0: Processor cycle */
        (1 << 14) | (1 << 13) | (1 << 12) | (1 << 11) | /* EXL, K, S, U for mode 1 */
        (0x02 << 15);     /* Event 1: Single instruction issue */
        
    asm volatile (
        "mtpc $0, 0 \n"        // clear Counter 0 First
        "mtpc $0, 1 \n"        // clear Counter 1 First
        "mtc0 %0, $25 \n"      // set PCCR
        :: "r" (pccr)
    );
}

static inline void perf_stop(uint32_t *cycles, uint32_t *insns) {
    uint32_t pccr;
    uint32_t count0, count1;
    asm volatile (
        "mfc0 %0, $25 \n"
        "li %1, 0x7FFFFFFF \n"
        "and %0, %0, %1 \n"
        "mtc0 %0, $25 \n"      // disable CTE
        "mfpc %2, 0 \n"        // read PCR0 (cycles)
        "mfpc %3, 1 \n"        // read PCR1 (insns)
        : "=&r" (pccr), "=&r" (count1), "=&r" (count0), "=&r" (count1)
    );
    *cycles = count0;
    *insns = count1;
}

/* ================================================================
 *  Macros
 * ================================================================ */

/* Reset internal state before a test */
void gp_reset_state(void);

#define BEGIN_GPU_TEST(test_name) do { \
    gp_reset_state(); \
    gp_ctx.name = (test_name); \
    gp_ctx.fail_count = 0; \
    gp_ctx.qwords_generated = 0; \
    gp_ctx.eecycles_used = 0; \
    gp_ctx.eeinsns_used = 0; \
} while(0)


#define EMIT_GP0(word) do { \
    uint32_t _cycles, _insns; \
    perf_start(); \
    GPU_WriteGP0((uint32_t)(word)); \
    perf_stop(&_cycles, &_insns); \
    gp_ctx.eecycles_used += _cycles; \
    gp_ctx.eeinsns_used += _insns; \
} while(0)

#define EMIT_GP1(word) do { \
    uint32_t _cycles, _insns; \
    perf_start(); \
    GPU_WriteGP1((uint32_t)(word)); \
    perf_stop(&_cycles, &_insns); \
    gp_ctx.eecycles_used += _cycles; \
    gp_ctx.eeinsns_used += _insns; \
} while(0)

/* Setup commands that bypass metrics accumulation */
#define SETUP_GP0(word) do { \
    GPU_WriteGP0((uint32_t)(word)); \
} while(0)

#define SETUP_GP1(word) do { \
    GPU_WriteGP1((uint32_t)(word)); \
} while(0)

/* Validations */
#define EXPECT_QWORDS(max_qwords) do { \
    uint32_t _got = gp_ctx.qwords_generated; \
    uint32_t _max = (uint32_t)(max_qwords); \
    if (_got <= _max) { \
        printf("    %-16s: %4u QWORDs [max %u] OK\n", \
               gp_ctx.name, (unsigned int)_got, (unsigned int)_max); \
    } else { \
        printf("  [FAIL] %-16s: %4u QWORDs EXCEEDS max %u\n", \
               gp_ctx.name, (unsigned int)_got, (unsigned int)_max); \
        gp_ctx.fail_count++; \
    } \
} while(0)

#define EXPECT_CYCLES(max_cycles) do { \
    uint32_t _got = gp_ctx.eecycles_used; \
    uint32_t _max = (uint32_t)(max_cycles); \
    if (_got <= _max) { \
        printf("    %-16s: %4u CYCLES [max %u] OK\n", \
               gp_ctx.name, (unsigned int)_got, (unsigned int)_max); \
    } else { \
        printf("  [FAIL] %-16s: %4u CYCLES EXCEEDS max %u\n", \
               gp_ctx.name, (unsigned int)_got, (unsigned int)_max); \
        gp_ctx.fail_count++; \
    } \
} while(0)

#define END_GPU_TEST() do { \
    gp_results.total++; \
    if (gp_ctx.fail_count == 0) { \
        gp_results.passed++; \
    } else { \
        gp_results.failed++; \
    } \
} while(0)

/* ================================================================
 *  Test Runners
 * ================================================================ */
void gp_run_expansion_tests(void);
void gp_run_expansion_gp1_tests(void);
void gp_run_clut_tests(void);

#endif /* PLAYGROUND_GPU_H */
