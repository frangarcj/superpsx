/**
 * gpu_ps2_state.h — PS2 GS-specific types, macros, and function declarations
 *
 * This header is included ONLY by PS2 GPU backend files (gpu_ps2_*.c).
 * Shared GPU state lives in gpu_state.h (platform-independent).
 */
#ifndef GPU_PS2_STATE_H
#define GPU_PS2_STATE_H

#include "gpu_state.h"

/* ── PS2 SDK includes ────────────────────────────────────────────── */
#include <kernel.h>
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <gs_psm.h>
#include <gs_gp.h>
#include <gs_privileged.h>
#include <gif_tags.h>

/* ── DMA Channel 1 (VIF1) registers ─────────────────────────────── */
#define D1_CHCR ((volatile uint32_t *)0x10009000)
#define D1_MADR ((volatile uint32_t *)0x10009010)
#define D1_QWC ((volatile uint32_t *)0x10009020)

/* ── GS pixel storage mode for PSX VRAM ──────────────────────────── */
#define PSX_VRAM_PSM GS_PSM_16S

/* ── GIF packet buffer ───────────────────────────────────────────── */
#define GIF_BUFFER_SIZE 16384

/* ── GIF Tag structure ───────────────────────────────────────────── */
typedef struct
{
    uint64_t NLOOP : 15;
    uint64_t EOP : 1;
    uint64_t pad1 : 30;
    uint64_t PRE : 1;
    uint64_t PRIM : 11;
    uint64_t FLG : 2;
    uint64_t NREG : 4;
    uint64_t REGS;
} GifTag __attribute__((aligned(16)));

typedef struct __attribute__((aligned(16)))
{
    uint64_t d0;
    uint64_t d1;
} gif_qword_t;

/* ── GIF double-buffered packet buffers ──────────────────────────── */
extern unsigned __int128 gif_packet_buf[2][GIF_BUFFER_SIZE];
extern gif_qword_t *fast_gif_ptr;
extern gif_qword_t *gif_buffer_end_safe;
extern int current_buffer;

/* ── IMAGE transfer buffer ───────────────────────────────────────── */
extern unsigned __int128 buf_image[1024];
extern int buf_image_ptr;

/* ── GIF buffer management (gpu_gif.c) ───────────────────────────── */
void Flush_GIF(void);
void Flush_GIF_Sync(void);

#define GIF_TAG_LO(nloop, eop, pre, prim, flg, nreg) \
    (((uint64_t)(nloop) & 0x7FFF) |                  \
     (((uint64_t)(eop) & 1) << 15) |                 \
     (((uint64_t)(pre) & 1) << 46) |                 \
     (((uint64_t)(prim) & 0x7FF) << 47) |            \
     (((uint64_t)(flg) & 3) << 58) |                 \
     (((uint64_t)(nreg) & 15) << 60))

static inline void Push_GIF_Tag(uint64_t tag_lo, uint64_t tag_hi)
{
    if (__builtin_expect(fast_gif_ptr >= gif_buffer_end_safe, 0))
        Flush_GIF();

    *(unsigned __int128 *)fast_gif_ptr = ((unsigned __int128)tag_hi << 64) | (unsigned __int128)tag_lo;
    fast_gif_ptr++;
}

static inline void Push_GIF_Data(uint64_t d0, uint64_t d1)
{
    *(unsigned __int128 *)fast_gif_ptr = ((unsigned __int128)d1 << 64) | (unsigned __int128)d0;
    fast_gif_ptr++;
}

void Setup_GS_Environment(void);

/* ── Alpha blending register helpers ─────────────────────────────── */
static inline uint64_t Get_Alpha_Reg(int mode)
{
    switch (mode & 3)
    {
    case 0:
        return GS_SET_ALPHA(0, 1, 2, 1, 0x58);
    case 1:
        return GS_SET_ALPHA(0, 2, 2, 1, 0x80);
    case 2:
        return GS_SET_ALPHA(1, 0, 2, 2, 0x80);
    default:
        return GS_SET_ALPHA(0, 2, 2, 1, 0x20);
    }
}

static inline uint64_t Get_Base_TEST(void)
{
    return GS_SET_TEST(0, 0, 0, 0, mask_check_bit, 0, 0, 0);
}

static inline uint64_t GS_PACK_PRIM_FROM_INT(uint64_t v)
{
    return GS_SET_PRIM((v) & 0x7, ((v) >> 3) & 0x1, ((v) >> 4) & 0x1, ((v) >> 5) & 0x1,
                       ((v) >> 6) & 0x1, ((v) >> 7) & 0x1, ((v) >> 8) & 0x1,
                       ((v) >> 9) & 0x1, ((v) >> 10) & 0x1);
}

/* ── GS State Tracking ───────────────────────────────────────────── */
typedef struct
{
    uint64_t tex0;
    uint64_t test;
    uint64_t alpha;
    uint64_t clamp;
    uint64_t texclut;
    int dthe;
    int valid;
    int last_cmd_key;
    int last_cache_slot;
} gs_state_t;

extern gs_state_t gs_state;

/* ── VRAM transfer operations (gpu_vram.c) ───────────────────────── */
void Start_VRAM_Transfer(int x, int y, int w, int h);
void Upload_Shadow_VRAM_Region(int x, int y, int w, int h);
uint16_t *GS_ReadbackRegion(int x, int y, int w_aligned, int h,
                            void *buf, int buf_qwc);
void GS_UploadRegion(int x, int y, int w, int h, const uint16_t *pixels);
void GS_UploadRegionFast(uint32_t coords, uint32_t dims, uint32_t *data_ptr, uint32_t word_count);
void DumpVRAM(const char *filename);

/* ── Display update (gpu_core.c) ─────────────────────────────────── */
void Update_GS_Display(void);

#endif /* GPU_PS2_STATE_H */
