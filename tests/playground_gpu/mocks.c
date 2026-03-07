/*
 * GPU Playground — Mocks
 *
 * Provides dummy implementations for external dependencies (CPU, DMA, etc.)
 * so that gpu_commands.c and gpu_primitives.c can be compiled in isolation.
 *
 * This enables counting EE cycles and GS packet bytes accurately without
 * incurring the overhead of the actual hardware transfers or emulator
 * bookkeeping.
 */
#include "playground_gpu.h"
#include <malloc.h>

/* --- GPU State Globals required by gpu_*.c ---
 * These are normally defined in gpu_core.c, but since we exclude gpu_core.c
 * from the playground build, we must provide them here. */
uint32_t gpu_stat = 0x14802000;
uint32_t gpu_read = 0;
volatile uint64_t gpu_irq_delay_cycle = 0;

int fb_address = 0;
int fb_width = 640;
int fb_height = 448;
int fb_psm = 2; // GS_PSM_16S

unsigned __int128 gif_packet_buf[2][GIF_BUFFER_SIZE];
gif_qword_t *fast_gif_ptr = NULL;
gif_qword_t *gif_buffer_end_safe = NULL;
int current_buffer = 0;

/* Real buffer for mock GIF writes (avoids MMIO writes to VIF0 FIFO) */
static unsigned __int128 mock_gif_storage[GIF_BUFFER_SIZE];
gif_qword_t *mock_gif_buffer_base = (gif_qword_t *)mock_gif_storage;

int draw_offset_x = 0;
int draw_offset_y = 0;
int draw_clip_x1 = 0;
int draw_clip_y1 = 0;
int draw_clip_x2 = 640;
int draw_clip_y2 = 480;

int disp_range_y1 = 0;
int disp_range_y2 = 0;

int tex_page_x = 0;
int tex_page_y = 0;
int tex_page_format = 2;
int semi_trans_mode = 0;
int dither_enabled = 0;

uint16_t *psx_vram_shadow = NULL;

volatile int gpu_pending_vblank_flush = 0;

int vram_tx_x = 0, vram_tx_y = 0, vram_tx_w = 0, vram_tx_h = 0, vram_tx_pixel = 0;
int vram_read_x = 0, vram_read_y = 0, vram_read_w = 0, vram_read_h = 0;
int vram_read_remaining = 0;
int vram_read_pixel = 0;

int polyline_active = 0;
int polyline_shaded = 0;
int polyline_semi_trans = 0;
uint32_t polyline_prev_color = 0;
uint32_t polyline_next_color = 0;
int16_t polyline_prev_x = 0, polyline_prev_y = 0;
int polyline_expect_color = 0;

int tex_flip_x = 0;
int tex_flip_y = 0;

int mask_set_bit = 0;
int mask_check_bit = 0;
uint64_t cached_base_test = 0;

int gp1_allow_2mb = 0;

uint32_t tex_win_mask_x = 0;
uint32_t tex_win_mask_y = 0;
uint32_t tex_win_off_x = 0;
uint32_t tex_win_off_y = 0;

uint32_t raw_tex_window = 0;
uint32_t raw_draw_area_tl = 0;
uint32_t raw_draw_area_br = 0;
uint32_t raw_draw_offset = 0;

int gpu_cmd_remaining = 0;
uint32_t gpu_cmd_buffer[16];
int gpu_cmd_ptr = 0;
int gpu_transfer_words = 0;
int gpu_transfer_total = 0;

unsigned __int128 buf_image[1024];
int buf_image_ptr = 0;

uint64_t global_cycles = 0;
uint64_t scheduler_cached_earliest = 0;

/* Dummy RAM mapping */
uint8_t *psx_ram = NULL;

/* Scheduler stubs */
int scheduler_earliest_id = 0;
#include "scheduler.h"
SchedEvent sched_events[SCHED_EVENT_COUNT];

/* --- MOCKS --- */

/* 
 * The single most important mock: Flush_GIF
 * Instead of kicking off a DMA chain via ps2_drivers, we just measure
 * the length of the packet being flushed to determine the "Data Expansion".
 */
void Flush_GIF(void)
{
    if (fast_gif_ptr > MOCK_GIF_BUFFER_START) {
        /* Add to the current test context metrics */
        gp_ctx.qwords_generated += (fast_gif_ptr - MOCK_GIF_BUFFER_START);
    }
    /* "Flush" by resetting the pointer to the start of our dummy buffer */
    fast_gif_ptr = MOCK_GIF_BUFFER_START;
}

void SignalInterrupt(uint32_t source)
{
    // Mocked to do nothing. We don't want CPU IRQs firing during a GPU test.
}

void Start_VRAM_Transfer(int x, int y, int w, int h)
{
    // Update shadow VRAM pointer but skip hardware DMA.
}

void Upload_Shadow_VRAM_Region(int x, int y, int w, int h)
{
    // Skip hardware DMA.
}

void SetGsCrt(short inte, short mode, short ffmd)
{
    // Mocked PS2SDK API call
}

void Update_GS_Display(void)
{
    // In original code this updates the DISPLAY1/2 GS registers.
    // For pure translation testing, doing nothing is fine.
}

void scheduler_enqueue(int ticks, int event_id)
{
    // Empty
}

void Setup_GS_Environment(void)
{
    // Initialize common GS state required by the primitive builder, like
    // default Z buffer setup. Since we aren't executing the commands on GS,
    // we only need primitive structures to not crash.
}

void Timer0_RefreshDividerCache(void) {}

void Flush_GIF_Sync(void) {
    Flush_GIF();
}

int dma_channel_send_normal(int channel, void *data, int qwc, int flags, int mode) { return 0; }

void dma_wait_fast(void) {}

uint32_t GPU_Read(void) {
    return gpu_read;
}
void GS_UploadRegionFast(uint32_t coords, uint32_t dims, uint32_t *data_ptr, uint32_t word_count) {}
void GS_UploadRegion(int x, int y, int w, int h, const uint16_t *pixels) {}
uint16_t *GS_ReadbackRegion(int x, int y, int w_aligned, int h, void *buf, int buf_qwc) { return NULL; }
