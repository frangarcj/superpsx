/**
 * profiler.h — Subsystem-level wall-clock profiler for SuperPSX
 *
 * Uses a small stack to track *exclusive* time per category:
 * when a nested category is entered (e.g. GPU_DMA inside JIT_EXEC),
 * the outer timer is paused automatically.
 *
 * Enable at compile time with -DENABLE_SUBSYSTEM_PROFILER.
 * Results are written to "profile.log" every 60 frames.
 */
#ifndef PROFILER_H
#define PROFILER_H

#include <stdint.h>

/* ── Profiler categories ─────────────────────────────────────────── */
enum ProfCategory {
    PROF_JIT_EXEC = 0,   /* Time executing JIT-compiled native code       */
    PROF_JIT_COMPILE,     /* Time compiling new JIT blocks                 */
    PROF_GPU_DMA,         /* GPU DMA processing (display list parsing)     */
    PROF_GPU_FLUSH,       /* Flush_GIF — DMA send to GS hardware          */
    PROF_GPU_TEXCACHE,    /* Texture cache lookups + invalidation          */
    PROF_SPU_MIX,         /* SPU sample generation (ADPCM+ADSR+mix)       */
    PROF_SPU_FLUSH,       /* Audio output (audsrv_wait + play_audio)      */
    PROF_SIO,             /* Serial I/O (controller read/write)            */
    PROF_CDROM,           /* CD-ROM register & data reads                  */
    PROF_SCHEDULER,       /* Scheduler dispatch overhead                   */
    PROF_NUM
};

/* ── Subsystem disable flags (always available, even without profiler) ── */
extern int prof_disable_spu;
extern int prof_disable_gpu_render;

#ifdef ENABLE_SUBSYSTEM_PROFILER

#include <time.h>
#include <stdio.h>

extern const char *prof_category_names[PROF_NUM];

/* ── Profiler state ──────────────────────────────────────────────── */
typedef struct {
    /* Exclusive-time accumulators (clock() ticks) */
    clock_t ticks[PROF_NUM];
    uint32_t calls[PROF_NUM];

    /* Frame-level wall clock */
    clock_t frame_start_tick;
    clock_t total_wall_ticks;
    uint32_t frames;

    /* Extra counters */
    uint64_t psx_cycles;
    uint32_t jit_blocks;
    uint32_t jit_compiles;
    uint64_t gpu_pixels;

    /* Exclusive-time tracking stack */
    int      stack[8];
    clock_t  stack_enter[8];
    int      stack_depth;
} ProfState;

extern ProfState prof;

/* ── Fast-path inline helpers ────────────────────────────────────── */

/**
 * Push a new profiler category onto the stack.
 * Pauses the currently-active outer category (if any).
 */
static inline void prof_push(int cat)
{
    clock_t now = clock();
    int d = prof.stack_depth;
    if (d >= 8) {
        printf("[PROF BUG] stack overflow! depth=%d pushing cat=%d\n", d, cat);
        return;
    }
    if (d > 0) {
        /* Pause outer category: accumulate its time so far */
        int outer = prof.stack[d - 1];
        clock_t delta = now - prof.stack_enter[d - 1];
        if (delta < 0) {
            printf("[PROF BUG] push: negative delta=%ld cat=%d outer=%d\n",
                   (long)delta, cat, outer);
        }
        prof.ticks[outer] += delta;
    }
    prof.stack[d] = cat;
    prof.stack_enter[d] = now;
    prof.stack_depth = d + 1;
    prof.calls[cat]++;
}

/**
 * Pop the current profiler category.
 * Accumulates exclusive time and resumes the outer category.
 */
static inline void prof_pop(int cat)
{
    clock_t now = clock();
    int d = prof.stack_depth - 1;
    if (d < 0) {
        printf("[PROF BUG] stack underflow! popping cat=%d\n", cat);
        return;
    }
    if (prof.stack[d] != cat) {
        printf("[PROF BUG] pop mismatch! expected cat=%d got stack[%d]=%d\n",
               cat, d, prof.stack[d]);
    }
    clock_t delta = now - prof.stack_enter[d];
    if (delta < 0) {
        printf("[PROF BUG] pop: negative delta=%ld cat=%d\n", (long)delta, cat);
    }
    prof.ticks[cat] += delta;
    prof.stack_depth = d;
    if (d > 0) {
        /* Resume outer category */
        prof.stack_enter[d - 1] = now;
    }
}

/* ── Public macros ───────────────────────────────────────────────── */
#define PROF_PUSH(cat)        prof_push(cat)
#define PROF_POP(cat)         prof_pop(cat)
#define PROF_COUNT_PIXELS(n)  (prof.gpu_pixels += (uint64_t)(n))
#define PROF_COUNT_BLOCK()    (prof.jit_blocks++)
#define PROF_COUNT_COMPILE()  (prof.jit_compiles++)

void profiler_init(void);
void profiler_frame_end(uint64_t psx_cycles_this_frame);

#else /* !ENABLE_SUBSYSTEM_PROFILER */

/* No-op stubs when profiler is disabled */
#define PROF_PUSH(cat)        ((void)0)
#define PROF_POP(cat)         ((void)0)
#define PROF_COUNT_PIXELS(n)  ((void)0)
#define PROF_COUNT_BLOCK()    ((void)0)
#define PROF_COUNT_COMPILE()  ((void)0)

static inline void profiler_init(void) {}
static inline void profiler_frame_end(uint64_t c) { (void)c; }

#endif /* ENABLE_SUBSYSTEM_PROFILER */

#endif /* PROFILER_H */
