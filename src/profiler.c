/**
 * profiler.c — Subsystem profiler implementation
 *
 * Accumulates per-subsystem wall-clock times and writes detailed
 * reports to "profile.log" every PROF_REPORT_INTERVAL frames.
 * Also prints a brief summary to stdout for each report.
 */
#include "profiler.h"
#include <stdio.h>

/* From dynarec_run.c — hotspot tracker */
extern void jit_hotspot_dump_and_reset(FILE *out);

/* ── Subsystem disable flags (always available) ──────────────────── */
int prof_disable_spu = 0;
int prof_disable_gpu_render = 0;

#ifdef ENABLE_SUBSYSTEM_PROFILER

#include <string.h>
#include <time.h>

const char *prof_category_names[PROF_NUM] = {
    "JIT Execution",
    "JIT Compile",
    "GPU DMA",
    "GPU Primitives",
    "GPU VRAM Upload",
    "GPU Flush(GIF)",
    "GPU TexCache",
    "SPU Mix",
    "SPU AudioFlush",
    "SIO",
    "CDROM",
    "Scheduler"
};

ProfState prof;

static FILE *prof_log_file = NULL;
static uint32_t prof_report_num = 0;
static uint64_t prof_total_frames = 0;

/* Grand totals across all reports */
static clock_t  prof_grand_ticks[PROF_NUM];
static uint32_t prof_grand_calls[PROF_NUM];
static clock_t  prof_grand_wall = 0;
static uint32_t prof_grand_frames = 0;
static uint64_t prof_grand_psx_cycles = 0;
static uint32_t prof_grand_jit_blocks = 0;
static uint32_t prof_grand_jit_compiles = 0;
static uint64_t prof_grand_gpu_pixels = 0;

#define PROF_REPORT_INTERVAL 60

/* ── Internal: write a formatted report table ────────────────────── */
static void write_report(FILE *out,
                         uint32_t nframes, clock_t total_wall,
                         clock_t *ticks, uint32_t *calls,
                         uint64_t psx_cycles, uint32_t jit_blocks,
                         uint32_t jit_compiles, uint64_t gpu_pixels)
{
    if (nframes == 0 || total_wall == 0)
        return;

    double total_ms  = (double)total_wall * 1000.0 / CLOCKS_PER_SEC;
    double avg_ms    = total_ms / nframes;
    double speed_pct = (avg_ms > 0) ? (16.667 / avg_ms * 100.0) : 0;

    fprintf(out, "Frame budget : 16.67ms (60fps NTSC)\n");
    fprintf(out, "Avg frame    : %.2f ms (%.1f%% speed)\n", avg_ms, speed_pct);
    fprintf(out, "CLOCKS_PER_SEC: %d\n\n", (int)CLOCKS_PER_SEC);

    clock_t accounted = 0;
    for (int i = 0; i < PROF_NUM; i++)
        accounted += ticks[i];
    clock_t other = (total_wall > accounted) ? (total_wall - accounted) : 0;

    fprintf(out, "%-20s %9s %7s %9s %9s\n",
            "Category", "Time(ms)", "%Total", "Calls/f", "Avg(us)");
    fprintf(out, "--------------------------------------------------------------\n");

    for (int i = 0; i < PROF_NUM; i++) {
        double ms  = (double)ticks[i] * 1000.0 / CLOCKS_PER_SEC;
        double pct = (double)ticks[i] * 100.0 / total_wall;
        double cpf = (double)calls[i] / nframes;
        double avg = (calls[i] > 0) ? (ms * 1000.0 / calls[i]) : 0;

        if (ticks[i] > 0 || calls[i] > 0) {
            fprintf(out, "%-20s %9.1f %6.1f%% %9.1f %9.1f\n",
                    prof_category_names[i], ms, pct, cpf, avg);
        }
    }

    /* Other / Unaccounted */
    {
        double ms  = (double)other * 1000.0 / CLOCKS_PER_SEC;
        double pct = (double)other * 100.0 / total_wall;
        fprintf(out, "%-20s %9.1f %6.1f%%       -         -\n",
                "Other/Unaccounted", ms, pct);
    }

    fprintf(out, "--------------------------------------------------------------\n");
    fprintf(out, "%-20s %9.1f %6.1f%%\n\n", "Total", total_ms, 100.0);

    /* Extra metrics */
    fprintf(out, "PSX cycles/frame  : %.0f\n",
            (double)psx_cycles / nframes);
    fprintf(out, "JIT blocks/frame  : %.1f\n",
            (double)jit_blocks / nframes);
    fprintf(out, "JIT compiles/frame: %.1f\n",
            (double)jit_compiles / nframes);
    fprintf(out, "GPU pixels/frame  : %.0f\n",
            (double)gpu_pixels / nframes);
}

/* ── Public API ──────────────────────────────────────────────────── */

void profiler_init(void)
{
    memset(&prof, 0, sizeof(prof));
    memset(prof_grand_ticks, 0, sizeof(prof_grand_ticks));
    memset(prof_grand_calls, 0, sizeof(prof_grand_calls));
    prof_grand_wall = 0;
    prof_grand_frames = 0;
    prof_grand_psx_cycles = 0;
    prof_grand_jit_blocks = 0;
    prof_grand_jit_compiles = 0;
    prof_grand_gpu_pixels = 0;
    prof_report_num = 0;
    prof_total_frames = 0;

    prof_log_file = fopen("profile.log", "w");
    if (prof_log_file) {
        fprintf(prof_log_file,
                "SuperPSX Subsystem Profiler Log\n"
                "===============================\n"
                "CLOCKS_PER_SEC: %d\n"
                "Disable SPU: %d  |  Disable GPU render: %d\n\n",
                (int)CLOCKS_PER_SEC,
                prof_disable_spu, prof_disable_gpu_render);
        fflush(prof_log_file);
    }

    prof.frame_start_tick = clock();
}

void profiler_frame_end(uint64_t psx_cycles_this_frame)
{
    clock_t now = clock();
    clock_t frame_wall = now - prof.frame_start_tick;

    /* Snapshot the topmost stack entry only.
     *
     * profiler_frame_end is called from within Scheduler → HBlank, so the
     * stack may look like [JIT_EXEC, GPU_DMA] or [SCHEDULER].  Entries
     * BELOW the top are already "paused" — their exclusive time up to the
     * point they were paused was already accumulated by the prof_push that
     * created the entry above them.  Only the TOP entry is actively
     * running and needs its time flushed.
     *
     * We restart the top entry's clock so the next frame doesn't
     * double-count.  Entries below don't need touching — they'll be
     * correctly resumed when the entry above them is prof_pop'd. */
    if (prof.stack_depth > 0) {
        int top = prof.stack_depth - 1;
        int cat = prof.stack[top];
        prof.ticks[cat] += now - prof.stack_enter[top];
        prof.stack_enter[top] = now;
    }

    prof.total_wall_ticks += frame_wall;
    prof.psx_cycles += psx_cycles_this_frame;
    prof.frames++;
    prof_total_frames++;

    /* Reset frame start for next frame */
    prof.frame_start_tick = clock();

    /* ── Report every N frames ── */
    if (prof.frames >= PROF_REPORT_INTERVAL) {
        prof_report_num++;

        /* Accumulate grand totals */
        for (int i = 0; i < PROF_NUM; i++) {
            prof_grand_ticks[i] += prof.ticks[i];
            prof_grand_calls[i] += prof.calls[i];
        }
        prof_grand_wall        += prof.total_wall_ticks;
        prof_grand_frames      += prof.frames;
        prof_grand_psx_cycles  += prof.psx_cycles;
        prof_grand_jit_blocks  += prof.jit_blocks;
        prof_grand_jit_compiles+= prof.jit_compiles;
        prof_grand_gpu_pixels  += prof.gpu_pixels;

        /* ---- Console summary (one-liner) ---- */
        {
            double total_ms = (double)prof.total_wall_ticks * 1000.0 / CLOCKS_PER_SEC;
            double avg_ms   = total_ms / prof.frames;
            double speed    = (avg_ms > 0) ? (16.667 / avg_ms * 100.0) : 0;

            printf("[PROF #%lu] %.1f%% speed |", (unsigned long)prof_report_num, speed);
            for (int i = 0; i < PROF_NUM; i++) {
                if (prof.ticks[i] > 0) {
                    double pct = (double)prof.ticks[i] * 100.0 / prof.total_wall_ticks;
                    printf(" %s=%.1f%%", prof_category_names[i], pct);
                }
            }
            clock_t acc = 0;
            for (int i = 0; i < PROF_NUM; i++) acc += prof.ticks[i];
            clock_t oth = (prof.total_wall_ticks > acc)
                          ? (prof.total_wall_ticks - acc) : 0;
            if (oth > 0) {
                double opct = (double)oth * 100.0 / prof.total_wall_ticks;
                printf(" Other=%.1f%%", opct);
            }
            printf("\n");
        }

        /* ---- Detailed file report ---- */
        if (prof_log_file) {
            fprintf(prof_log_file,
                    "\n=== Report #%lu (frames %llu-%llu, %lu frames) ===\n",
                    (unsigned long)prof_report_num,
                    (unsigned long long)(prof_total_frames - prof.frames),
                    (unsigned long long)prof_total_frames,
                    (unsigned long)prof.frames);
            if (prof_disable_spu)
                fprintf(prof_log_file, "** SPU DISABLED **\n");
            if (prof_disable_gpu_render)
                fprintf(prof_log_file, "** GPU RENDER DISABLED **\n");

            write_report(prof_log_file,
                         prof.frames, prof.total_wall_ticks,
                         prof.ticks, prof.calls,
                         prof.psx_cycles, prof.jit_blocks,
                         prof.jit_compiles, prof.gpu_pixels);
            fprintf(prof_log_file, "\n");

            /* Grand totals every 5 reports */
            if ((prof_report_num % 5) == 0) {
                fprintf(prof_log_file,
                        "\n>>> GRAND TOTAL (frames 0-%llu, %lu frames) <<<\n",
                        (unsigned long long)prof_total_frames,
                        (unsigned long)prof_grand_frames);
                write_report(prof_log_file,
                             prof_grand_frames, prof_grand_wall,
                             prof_grand_ticks, prof_grand_calls,
                             prof_grand_psx_cycles, prof_grand_jit_blocks,
                             prof_grand_jit_compiles, prof_grand_gpu_pixels);
                fprintf(prof_log_file, "\n");
            }

            /* JIT hotspot report */
            jit_hotspot_dump_and_reset(prof_log_file);

            fflush(prof_log_file);
        }

        /* ---- Reset accumulators for next interval ---- */
        memset(prof.ticks, 0, sizeof(prof.ticks));
        memset(prof.calls, 0, sizeof(prof.calls));
        prof.total_wall_ticks = 0;
        prof.frames = 0;
        prof.psx_cycles = 0;
        prof.jit_blocks = 0;
        prof.jit_compiles = 0;
        prof.gpu_pixels = 0;
        /* Keep stack intact — callers still have pending PROF_POP.
         * Reset all entry times to prevent old-interval time
         * from bleeding into the new accumulator period. */
        {
            clock_t rst = clock();
            for (int i = 0; i < prof.stack_depth; i++)
                prof.stack_enter[i] = rst;
        }
    }
}

#endif /* ENABLE_SUBSYSTEM_PROFILER */
