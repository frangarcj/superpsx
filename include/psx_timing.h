#ifndef PSX_TIMING_H
#define PSX_TIMING_H

/*
 * PSX timing constants — shared across scheduler, timers, GPU, CDROM.
 *
 * PSX CPU: 33.868800 MHz (R3000A)
 * NTSC: 60 Hz → ~564480 cycles/frame
 * PAL:  50 Hz → ~677376 cycles/frame
 */

#define PSX_CPU_FREQ 33868800U /* 33.8688 MHz */

/* Per-scanline CPU cycle counts derived from psx-spx:
 * Video clock = 53222400 Hz. CPU clock / Video clock = 7/11 (exact).
 * NTSC: 3413 video cycles/scanline × 7/11 = 2172.27 → 2173 (round up)
 * PAL:  3406 video cycles/scanline × 7/11 = 2167.45 → 2168 (round up)  */
#define CYCLES_PER_HBLANK_NTSC 2173U /* 3413 × 7/11 ≈ 2172.27 */
#define CYCLES_PER_HBLANK_PAL 2168U  /* 3406 × 7/11 ≈ 2167.45 */
#define CYCLES_PER_HBLANK CYCLES_PER_HBLANK_NTSC
#define CYCLES_PER_SCANLINE CYCLES_PER_HBLANK
#define SCANLINES_PER_FRAME 263U        /* NTSC */
#define SCANLINES_PER_FRAME_PAL 314U    /* PAL  */
#define VBLANK_START_SCANLINE_NTSC 240U /* First VBlank scanline (NTSC) */
#define VBLANK_START_SCANLINE_PAL 288U  /* First VBlank scanline (PAL)  */
#define CYCLES_PER_FRAME_NTSC (SCANLINES_PER_FRAME * CYCLES_PER_HBLANK_NTSC)   /* 571399 */
#define CYCLES_PER_FRAME_PAL (SCANLINES_PER_FRAME_PAL * CYCLES_PER_HBLANK_PAL) /* 680752 */

/* Timer0 dotclock dividers — exact rational: CPU/dot = N × 7 / 11
 * where N = video-clocks-per-dot (10,8,7,5,4 for each resolution).
 * Integer approximations (DIV) are kept for backward compat / scheduling;
 * the fractional numerators (NUM) are used in Timer_SyncValue for
 * exact accumulation: ticks = elapsed_sub11 / NUM,  sub11 = cycles×11.
 *
 * 256-wide: N=10, CPU/dot = 70/11 = 6.3636...
 * 320-wide: N= 8, CPU/dot = 56/11 = 5.0909...
 * 368-wide: N= 7, CPU/dot = 49/11 = 4.4545...
 * 512-wide: N= 5, CPU/dot = 35/11 = 3.1818...
 * 640-wide: N= 4, CPU/dot = 28/11 = 2.5454...  */
#define DOTCLOCK_DIV_256 7U
#define DOTCLOCK_DIV_320 5U
#define DOTCLOCK_DIV_368 4U
#define DOTCLOCK_DIV_512 3U
#define DOTCLOCK_DIV_640 3U
#define DOTCLOCK_NUM_256 70U /* 10 × 7 */
#define DOTCLOCK_NUM_320 56U /*  8 × 7 */
#define DOTCLOCK_NUM_368 49U /*  7 × 7 */
#define DOTCLOCK_NUM_512 35U /*  5 × 7 */
#define DOTCLOCK_NUM_640 28U /*  4 × 7 */

/* CD-ROM read timing: PSX_CPU_FREQ / 75 sectors-per-second */
#define CDROM_READ_CYCLES_1X (PSX_CPU_FREQ / 75)  /* 1x speed: 451584 cycles */
#define CDROM_READ_CYCLES_2X (PSX_CPU_FREQ / 150) /* 2x speed: 225792 cycles */

#endif /* PSX_TIMING_H */
