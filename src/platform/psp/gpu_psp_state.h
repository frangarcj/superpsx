/**
 * gpu_psp_state.h — PSP-specific GPU state and types
 *
 * Minimal definitions to satisfy gpu_state.h's needs on PSP.
 * No GIF/GS types — PSP uses sceGu for rendering.
 */
#ifndef GPU_PSP_STATE_H
#define GPU_PSP_STATE_H

#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>

/* ── Stub types matching PS2 patterns for code sharing ─────────── */

/* GS state tracking equivalent for PSP (lazy state) */
typedef struct {
    int tex_format;
    int tex_page_x;
    int tex_page_y;
    int clut_x;
    int clut_y;
    int semi_mode;
    int dither;
    int valid;
} gs_state_t;

extern gs_state_t gs_state;

/* PSP VRAM layout (2MB GE EDRAM):
 * 0x000000 - 0x043FFF: FB0 (272KB)
 * 0x044000 - 0x087FFF: FB1 (272KB)
 * 0x088000 - 0x187FFF: PSX VRAM Mirror (1MB)
 * 0x188000 - 0x1FFFFF: Free (480KB)
 */
#define PSP_FB0_OFFSET    0x000000
#define PSP_FB1_OFFSET    0x044000
#define PSP_VRAM_OFFSET   0x088000

/* PSP native resolution */
#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272
#define PSP_BUF_W    512

extern int psx_active_width;
extern int psx_active_height;
extern void *vram_mirror;

/* ── Coordinate helper (used by both primitives and backend) ───── */
/* In the new scheme, draws go directly to PSX VRAM in EDRAM.
 * Vertices use PSX coordinates + draw_offset, NO scaling to PSP screen. */
static inline void PSX_to_PSP(int16_t vx, int16_t vy, int16_t *ox, int16_t *oy) {
    *ox = (int16_t)(vx + draw_offset_x);
    *oy = (int16_t)(vy + draw_offset_y);
}

/* PSP GU vertex formats */
typedef struct {
    uint32_t color;
    int16_t x, y, z;
} PspVertFlat;

typedef struct {
    float u, v;
    uint32_t color;
    int16_t x, y, z;
} PspVertTex;

/* ── Vertex pool for GU_SEND mode ──────────────────────────────── */
/* Replaces sceGuGetMemory (which doesn't work in non-DIRECT mode).
 * CPU writes to cached pool, one dcache writeback before sceGuSendList.
 * Double-buffered: CPU writes to [dl_active], GE reads from [1-dl_active]. */
#define VPOOL_SIZE (256 * 1024)
extern uint8_t vpool_buf[2][VPOOL_SIZE];
extern int vpool_offset;
extern int dl_active;
extern int sync_id[2];

/* Forward declare flush — implemented in gpu_psp_core.c */
void GPU_Backend_Flush(void);

/* Display list (defined in gpu_psp_core.c, used by vram.c too)
 * Double-buffered: indexed by dl_active. */
extern unsigned int display_list[2][262144];

/* Texture API (implemented in gpu_psp_texture.c) */
void Tex_SetupIfChanged(uint32_t clut_word);
void Tex_ApplyFuncReplace(void);
void Tex_InvalidateState(void);
#ifdef ENABLE_PSP_STRIDE_HACK
extern float tex_v_scale;  /* V coordinate multiplier for stride hack (1.0, 2.0, 4.0) */
#endif

static inline void *vpool_alloc(int size) {
    size = (size + 3) & ~3;  /* 4-byte align */
    if (vpool_offset + size > VPOOL_SIZE) {
        /* Pool exhausted — wrap to avoid memory corruption.
         * This overwrites stale data from earlier in the same list;
         * correctness depends on the GE executing list commands in order. */
        vpool_offset = 0;
    }
    void *ptr = &vpool_buf[dl_active][vpool_offset];
    vpool_offset += size;
    return ptr;
}

/* ── GIF packet stub types for code that references them ───────── */
/* PSP doesn't use GIF. These satisfy extern declarations in shared code. */
typedef struct { uint64_t lo, hi; } gif_qword_t;
#define GIF_BUFFER_SIZE 1

extern unsigned char gif_packet_buf[2][16]; /* minimal stub */
extern gif_qword_t *fast_gif_ptr;
extern gif_qword_t *gif_buffer_end_safe;
extern int current_buffer;

#endif /* GPU_PSP_STATE_H */
