/**
 * osd_psp.c — On-Screen Display for PSP (debug text via pspDebugScreen)
 */
#include "osd.h"
#include <pspdebug.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

volatile uint32_t osd_vblank_count = 0;
static int osd_enabled = 1;

#define OSD_MAX_LINES 8
#define OSD_LINE_LEN  64
static char osd_lines[OSD_MAX_LINES][OSD_LINE_LEN];
static int  osd_line_count = 0;

void osd_init(void) {
    pspDebugScreenInit();
}

void osd_printf(int x, int y, uint32_t color, const char *fmt, ...) {
    (void)x; (void)y; (void)color;
    if (osd_line_count >= OSD_MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(osd_lines[osd_line_count], OSD_LINE_LEN, fmt, args);
    va_end(args);
    osd_line_count++;
}

void osd_boot_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void osd_draw(void) {
    if (!osd_enabled) return;
    for (int i = 0; i < osd_line_count; i++) {
        pspDebugScreenSetXY(0, i);
        pspDebugScreenPrintf("%s", osd_lines[i]);
    }
    osd_line_count = 0;
}

void osd_clear(void) {
    osd_line_count = 0;
}

void osd_set_enabled(int enabled) { osd_enabled = enabled; }
int  osd_is_enabled(void)         { return osd_enabled; }
