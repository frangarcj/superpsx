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
 *  GIF Register Capture — parse GIF buffer for register writes
 *
 *  After GP0 commands execute, call gp_gif_scan() to extract all
 *  A+D register writes from the GIF buffer.  Then use EXPECT_GIF_REG,
 *  EXPECT_NO_GIF_REG, etc. to verify what the GPU actually emitted.
 * ================================================================ */

#define GIF_CAPTURE_MAX 256

typedef struct {
    uint64_t data;   /* Register value (d0) */
    uint16_t reg;    /* GS register address (d1 low 8 bits) */
} gif_reg_capture_t;

extern gif_reg_capture_t gif_captures[GIF_CAPTURE_MAX];
extern int gif_capture_count;

/*
 * Scan the GIF buffer from base to fast_gif_ptr, extracting all AD-mode
 * register writes into gif_captures[].  Returns the capture count.
 * This should be called AFTER GP0 commands but BEFORE gp_reset_state().
 */
int gp_gif_scan(void);

/*
 * Reset only the GIF pointer and QW counter without touching GPU state.
 * Useful for multi-draw tests where you want to measure each draw separately.
 */
void gp_gif_reset_counter(void);

/* ================================================================
 *  Test Runners
 * ================================================================ */
void gp_run_expansion_tests(void);
void gp_run_expansion_gp1_tests(void);
void gp_run_clut_tests(void);
void gp_run_state_tests(void);
void gp_run_vram_tests(void);
void gp_run_dma_block_tests(void);
void gp_run_deferred_tests(void);
void gp_run_texcache_tests(void);

/* ================================================================
 *  GIF Capture Validation Macros
 * ================================================================ */

/* Count occurrences of a register in captures */
static inline int gp_gif_reg_count(uint16_t reg) {
    int c = 0;
    for (int i = 0; i < gif_capture_count; i++)
        if (gif_captures[i].reg == reg) c++;
    return c;
}

/* Find first occurrence of a register and return its data value */
static inline int gp_gif_reg_find(uint16_t reg, uint64_t *out_data) {
    for (int i = 0; i < gif_capture_count; i++)
        if (gif_captures[i].reg == reg) {
            if (out_data) *out_data = gif_captures[i].data;
            return 1;
        }
    return 0;
}

/* Find last occurrence of a register */
static inline int gp_gif_reg_find_last(uint16_t reg, uint64_t *out_data) {
    for (int i = gif_capture_count - 1; i >= 0; i--)
        if (gif_captures[i].reg == reg) {
            if (out_data) *out_data = gif_captures[i].data;
            return 1;
        }
    return 0;
}

/* Expect register present at least N times */
#define EXPECT_GIF_REG_COUNT(reg_name, reg_val, expected) do { \
    int _cnt = gp_gif_reg_count(reg_val); \
    if (_cnt == (expected)) { \
        printf("    %-16s: " reg_name " count=%d OK\n", gp_ctx.name, _cnt); \
    } else { \
        printf("  [FAIL] %-16s: " reg_name " count=%d expected %d\n", \
               gp_ctx.name, _cnt, (expected)); \
        gp_ctx.fail_count++; \
    } \
} while(0)

/* Expect register present (at least once) */
#define EXPECT_GIF_REG(reg_name, reg_val) do { \
    if (gp_gif_reg_count(reg_val) > 0) { \
        printf("    %-16s: " reg_name " present OK\n", gp_ctx.name); \
    } else { \
        printf("  [FAIL] %-16s: " reg_name " NOT found\n", gp_ctx.name); \
        gp_ctx.fail_count++; \
    } \
} while(0)

/* Expect register absent */
#define EXPECT_NO_GIF_REG(reg_name, reg_val) do { \
    int _cnt = gp_gif_reg_count(reg_val); \
    if (_cnt == 0) { \
        printf("    %-16s: " reg_name " absent OK\n", gp_ctx.name); \
    } else { \
        printf("  [FAIL] %-16s: " reg_name " found %d times (expected 0)\n", \
               gp_ctx.name, _cnt); \
        gp_ctx.fail_count++; \
    } \
} while(0)

/* Expect register present with specific data value */
#define EXPECT_GIF_REG_VALUE(reg_name, reg_val, expected_data) do { \
    uint64_t _data = 0; \
    if (gp_gif_reg_find(reg_val, &_data) && _data == (uint64_t)(expected_data)) { \
        printf("    %-16s: " reg_name " value OK\n", gp_ctx.name); \
    } else if (!gp_gif_reg_find(reg_val, NULL)) { \
        printf("  [FAIL] %-16s: " reg_name " NOT found\n", gp_ctx.name); \
        gp_ctx.fail_count++; \
    } else { \
        printf("  [FAIL] %-16s: " reg_name " value=0x%llx expected 0x%llx\n", \
               gp_ctx.name, (unsigned long long)_data, (unsigned long long)(expected_data)); \
        gp_ctx.fail_count++; \
    } \
} while(0)

/* Expect VRAM shadow pixel value */
#define EXPECT_VRAM_PIXEL(px, py, expected_16) do { \
    uint16_t _got = psx_vram_shadow[(py) * 1024 + (px)]; \
    uint16_t _exp = (uint16_t)(expected_16); \
    if (_got == _exp) { \
        printf("    %-16s: VRAM(%d,%d)=0x%04x OK\n", gp_ctx.name, (px), (py), _got); \
    } else { \
        printf("  [FAIL] %-16s: VRAM(%d,%d)=0x%04x expected 0x%04x\n", \
               gp_ctx.name, (px), (py), _got, _exp); \
        gp_ctx.fail_count++; \
    } \
} while(0)

#endif /* PLAYGROUND_GPU_H */
