/**
 * osd.h — On-Screen Display overlay for debug text
 *
 * Renders text on top of the emulated PSX output using the GS.
 * Works at any PSX display resolution.  The font is uploaded to an
 * unused area of GS VRAM at init time.
 *
 * Usage:
 *   osd_init();                                // once at startup
 *   osd_printf(10, 10, "FPS: %d", fps);        // queue text
 *   osd_draw();                                 // emit GIF commands
 */
#ifndef OSD_H
#define OSD_H

#include <stdint.h>

/* Call once after Init_Graphics to upload font to GS VRAM. */
void osd_init(void);

/* Queue a formatted string for overlay drawing.
 * Coordinates are in PSX VRAM pixels (absolute).
 * color: 0xAABBGGRR (GS byte order, AA ignored for IMAGE mode). */
void osd_printf(int x, int y, uint32_t color, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Boot-time log: prints a line at the next row of the boot log,
 * renders immediately to VRAM (0,0) and flushes.  Use BEFORE Run_CPU. */
void osd_boot_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Emit all queued overlay text into the GIF pipeline.
 * Should be called once per frame, after the PSX GPU commands
 * have been flushed but before the next frame begins. */
void osd_draw(void);

/* Clear all queued text (called automatically by osd_draw). */
void osd_clear(void);

/* Enable/disable overlay rendering globally. */
void osd_set_enabled(int enabled);
int  osd_is_enabled(void);

/* VBlank counter — incremented by GPU_VBlank(), read by FPS display. */
extern volatile uint32_t osd_vblank_count;

/* Font metrics */
#define OSD_CHAR_W 8
#define OSD_CHAR_H 8

/* Default text color: white, opaque */
#define OSD_COLOR_WHITE  0x80FFFFFF
#define OSD_COLOR_RED    0x800000FF
#define OSD_COLOR_GREEN  0x8000FF00
#define OSD_COLOR_YELLOW 0x8000FFFF

#endif /* OSD_H */
