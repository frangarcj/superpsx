/**
 * gpu_gif.c — GIF buffer management and GS environment setup
 *
 * Handles the double-buffered GIF packet system that batches PSX GPU
 * commands into PS2 GS DMA transfers.  Also contains the one-time
 * GS register initialisation (Setup_GS_Environment).
 */
#include "gpu_state.h"

/* ── TEST register helpers ───────────────────────────────────────── */

/* Precomputed TEST value — updated only when mask_check_bit changes
 * (GP0 E6h or GPU reset).  Eliminates per-primitive function call. */
uint64_t cached_base_test = 0;

/* Get_Base_TEST() is now static inline in gpu_state.h */

/* ── GIF buffer management ───────────────────────────────────────── */

void Flush_GIF(void)
{
    gif_qword_t *base = (gif_qword_t *)&gif_packet_buf[current_buffer][0];
    int qwc = fast_gif_ptr - base;

    if (qwc > 0)
    {
        /* Targeted dcache writeback: only flush the GIF buffer region.
         * FlushCache(0) would invalidate the ENTIRE 8KB L1 dcache,
         * destroying hot JIT data (cpu struct, psx_ram, LUT) and
         * causing ~300+ cycles of dcache misses per call.
         * SyncDCache writes back only dirty lines in the range. */
        SyncDCache(base, (void *)((uintptr_t)base + (uint32_t)qwc * 16));

        /* Async double-buffer: wait for PREVIOUS DMA to finish, then
         * start THIS buffer's DMA and swap immediately.  The CPU can
         * fill the other buffer while this DMA runs in parallel.
         * On the first call the channel is idle → dma_wait_fast returns
         * instantly.  Saves ~85K×500 cycles/sec of idle CPU stalls. */
        dma_wait_fast();
        dma_channel_send_normal(DMA_CHANNEL_GIF, base, qwc, 0, 0);

        // Swap to other buffer — safe because dma_wait ensured it's done
        current_buffer ^= 1;
        fast_gif_ptr = (gif_qword_t *)&gif_packet_buf[current_buffer][0];
        gif_buffer_end_safe = fast_gif_ptr + (GIF_BUFFER_SIZE - 1024);
    }
}

/* Synchronous flush: drain GIF buffer AND wait for DMA completion.
 * Required before directly using the GIF DMA channel (e.g. VRAM readback)
 * or when GS must have processed all prior commands. */
void Flush_GIF_Sync(void)
{
    Flush_GIF();
    dma_wait_fast();
}

/* ── GS Environment Setup ────────────────────────────────────────── */

void Setup_GS_Environment(void)
{
    // Setup GIF pointer initially
    fast_gif_ptr = (gif_qword_t *)&gif_packet_buf[current_buffer][0];
    gif_buffer_end_safe = fast_gif_ptr + (GIF_BUFFER_SIZE - 1024);

    // Setup GS registers like draw_setup_environment does
    // This mimics what libdraw does

    // NLOOP=16, EOP=1, PRE=0, PRIM=0, FLG=PACKED, NREG=1, REGS=AD(GIF_REG_AD)
    Push_GIF_Tag(GIF_TAG_LO(16, 1, 0, 0, 0, 1), GIF_REG_AD);

    // FRAME_1 (Reg 0x4C) - Framebuffer address and settings
    Push_GIF_Data(GS_SET_FRAME((fb_address >> 11), PSX_VRAM_FBW, fb_psm, 0), GS_REG_FRAME_1);

    // ZBUF_1 (Reg 0x4E) - Disable ZBuffer (mask bit = 1)
    Push_GIF_Data(GS_SET_ZBUF(0, 0, 1), GS_REG_ZBUF_1);

    // PRMODECONT (Reg 0x1A) - ENABLE use of GIF tag PRIM field
    Push_GIF_Data(GS_SET_PRMODECONT(1), GS_REG_PRMODECONT);

    // XYOFFSET_1 (Reg 0x18) - Primitive coordinate offset
    // Set to (2048 << 4, 2048 << 4) = (32768, 32768)
    Push_GIF_Data(GS_SET_XYOFFSET(2048 << 4, 2048 << 4), GS_REG_XYOFFSET_1);

    // SCISSOR_1 (Reg 0x40) - Scissoring area (framebuffer space, post-XYOFFSET)
    // Cover full PSX VRAM initially; E3/E4 will narrow it
    Push_GIF_Data(GS_SET_SCISSOR(0, PSX_VRAM_WIDTH - 1, 0, PSX_VRAM_HEIGHT - 1), GS_REG_SCISSOR_1);

    // TEST_1 (Reg 0x47) - Alpha test, depth test, etc
    Push_GIF_Data(GS_SET_TEST(1, 1, 0, 0, 0, 0, 1, 1), GS_REG_TEST_1);

    // FOGCOL (Reg 0x3D) - Fog color
    Push_GIF_Data(GS_SET_FOGCOL(0, 0, 0), GS_REG_FOGCOL);

    // PABE (Reg 0x49) - Per-pixel alpha blending enable
    Push_GIF_Data(GS_SET_PABE(1), GS_REG_PABE);

    // ALPHA_1 (Reg 0x42) - Alpha blending settings
    // Default: PSX mode 0 with FIX=0x58 to match reference test screenshots
    Push_GIF_Data(GS_SET_ALPHA(0, 1, 2, 1, 0x58), GS_REG_ALPHA_1);

    // DTHE (Reg 0x45) - Dithering off
    Push_GIF_Data(GS_SET_DTHE(0), GS_REG_DTHE);

    // DIMX (Reg 0x44) - PSX Dithering matrix
    Push_GIF_Data(GS_SET_DIMX(4, 0, 5, 1, 2, 6, 3, 7, 5, 1, 4, 0, 3, 7, 2, 6), GS_REG_DIMX);

    // COLCLAMP (Reg 0x46) - Color clamp
    Push_GIF_Data(GS_SET_COLCLAMP(1), GS_REG_COLCLAMP);

    // FBA_1 (Reg 0x4A) - Alpha correction
    Push_GIF_Data(GS_SET_FBA(0), GS_REG_FBA_1);

    // TEX1_1 (Reg 0x14) - Texture filtering: nearest-neighbor
    Push_GIF_Data(GS_SET_TEX1(1, 0, 0, 0, 0, 0, 0), GS_REG_TEX1_1);

    // CLAMP_1 (Reg 0x08) - Texture clamping
    Push_GIF_Data(GS_SET_CLAMP(0,0,0,0,0,0), GS_REG_CLAMP_1);

    // TEXA (Reg 0x3B) - Texture alpha expansion for CT16S
    Push_GIF_Data(GS_SET_TEXA(0, 0, 0x80), GS_REG_TEXA);

    Flush_GIF();
}
