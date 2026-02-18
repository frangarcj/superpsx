/**
 * gpu_core.c — GPU initialisation, status queries and display update
 *
 * This file owns every shared GPU state variable (the definitions that
 * match the extern declarations in gpu_state.h) as well as the top-level
 * lifecycle functions: Init_Graphics, GPU_Read, GPU_ReadStatus, GPU_VBlank,
 * GPU_Flush, and Update_GS_Display.
 */
#include "gpu_state.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Global GPU state — definitions (declared extern in gpu_state.h)
 * ═══════════════════════════════════════════════════════════════════ */

/* GPU status / read registers */
uint32_t gpu_stat = 0x14802000;
uint32_t gpu_read = 0;

/* Framebuffer configuration */
int fb_address = 0;
int fb_width = 640;
int fb_height = 448;
int fb_psm = GS_PSM_16S;

/* GIF double-buffered packet buffers */
unsigned __int128 gif_packet_buf[2][GIF_BUFFER_SIZE] __attribute__((aligned(64)));
int current_buffer = 0;
int gif_packet_ptr = 0;

/* GS shadow drawing state */
int draw_offset_x = 0;
int draw_offset_y = 0;
int draw_clip_x1 = 0;
int draw_clip_y1 = 0;
int draw_clip_x2 = 640;
int draw_clip_y2 = 480;

/* PSX Display Range */
int disp_range_y1 = 0;
int disp_range_y2 = 0;

/* Texture page state (from GP0 E1) */
int tex_page_x = 0;
int tex_page_y = 0;
int tex_page_format = 2; /* 0=4bit, 1=8bit, 2=15bit */
int semi_trans_mode = 0;
int dither_enabled = 0;

/* Shadow PSX VRAM for CLUT texture decode — dynamically allocated */
uint16_t *psx_vram_shadow = NULL;

/* Debug log file */
FILE *gpu_debug_log = NULL;

/* VRAM transfer tracking for shadow writes */
int vram_tx_x = 0, vram_tx_y = 0, vram_tx_w = 0, vram_tx_h = 0, vram_tx_pixel = 0;

/* VRAM read state (GP0 C0h) */
int vram_read_x = 0, vram_read_y = 0, vram_read_w = 0, vram_read_h = 0;
int vram_read_remaining = 0;
int vram_read_pixel = 0;

/* Polyline accumulation state (GP0 48h-5Fh polylines) */
int polyline_active = 0;
int polyline_shaded = 0;
int polyline_semi_trans = 0;
uint32_t polyline_prev_color = 0;
uint32_t polyline_next_color = 0;
int16_t polyline_prev_x = 0, polyline_prev_y = 0;
int polyline_expect_color = 0;

/* Texture flip bits from GP0(E1) bits 12-13 */
int tex_flip_x = 0;
int tex_flip_y = 0;

/* Mask bit state from GP0(E6) */
int mask_set_bit = 0;
int mask_check_bit = 0;

/* GP1(09h) - Allow 2MB VRAM */
int gp1_allow_2mb = 0;

/* Texture window from GP0(E2) */
uint32_t tex_win_mask_x = 0;
uint32_t tex_win_mask_y = 0;
uint32_t tex_win_off_x = 0;
uint32_t tex_win_off_y = 0;

/* Immediate mode command buffer */
int gpu_cmd_remaining = 0;
uint32_t gpu_cmd_buffer[16];
int gpu_cmd_ptr = 0;
int gpu_transfer_words = 0;
int gpu_transfer_total = 0;

/* IMAGE transfer buffer */
unsigned __int128 buf_image[1024];
int buf_image_ptr = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  GPU_Read / GPU_ReadStatus / GPU_VBlank / GPU_Flush
 * ═══════════════════════════════════════════════════════════════════ */

uint32_t GPU_Read(void)
{
    /* If VRAM read transfer is active (GP0 C0h) */
    if (vram_read_remaining > 0)
    {
        uint16_t p0 = 0, p1 = 0;

        if (psx_vram_shadow && vram_read_w > 0)
        {
            int px0 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py0 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px0 < 1024 && py0 < 512)
                p0 = psx_vram_shadow[py0 * 1024 + px0];
            vram_read_pixel++;

            int px1 = vram_read_x + (vram_read_pixel % vram_read_w);
            int py1 = vram_read_y + (vram_read_pixel / vram_read_w);
            if (px1 < 1024 && py1 < 512)
                p1 = psx_vram_shadow[py1 * 1024 + px1];
            vram_read_pixel++;
        }

        vram_read_remaining--;

        if (vram_read_remaining == 0)
        {
            gpu_stat &= ~0x08000000; /* Clear bit 27 (ready to send VRAM to CPU) */
        }

        return (uint32_t)p0 | ((uint32_t)p1 << 16);
    }

    /* Otherwise return GPU info (GP1 10h responses) */
    return gpu_read;
}

uint32_t GPU_ReadStatus(void)
{
    /* Force bits: 28 (ready DMA), 26 (ready CMD), 13 (interlace field) */
    /* Bit 27 (ready VRAM-to-CPU) is dynamic, set only during C0h transfer */
    /* Bit 23 (display disable) must NOT be forced — it reflects GP1(03h) state */
    return gpu_stat | 0x14002000;
}

void GPU_VBlank(void)
{
    gpu_stat ^= 0x80000000;
}

void GPU_Flush(void)
{
    Flush_GIF();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Update_GS_Display — reconfigure GS display registers
 * ═══════════════════════════════════════════════════════════════════ */

void Update_GS_Display(void)
{
    /* 1. Determine base PSX resolution */
    int psx_w;
    if (disp_hres368)
        psx_w = 368;
    else
    {
        int widths[] = {256, 320, 512, 640};
        psx_w = widths[disp_hres & 3];
    }
    int psx_h = (disp_range_y2 - disp_range_y1) * (disp_vres + 1);

    DLOG("Update_GS_Display: disp_range_y1=%d, disp_range_y2=%d, psx_h=%d\n, disp_vres=%d",
         disp_range_y1, disp_range_y2, psx_h, disp_vres);

    /* 2. Calculate MAGH to fill a TV line (~2560 cycles) */
    int magh;
    switch (psx_w)
    {
    case 256:
        magh = 9;
        break;
    case 320:
        magh = 7;
        break;
    case 368:
        magh = 6;
        break;
    case 512:
        magh = 4;
        break;
    case 640:
        magh = 3;
        break;
    default:
        magh = 3;
        break;
    }

    /* 3. DW (Display Width) in VCK units */
    int dw = (psx_w * (magh + 1)) - 1;

    /* 4. DH and MAGV (Vertical) */
    int dh = psx_h - 1;
    int magv = 0;

    /* 5. Centering (DX, DY) */
    int dx_start_ntsc = 650;
    int dx_start_pal = 680;
    int dy, dx;

    if (disp_pal)
    {
        dx = dx_start_pal;
        dy = disp_interlace ? (disp_range_y1 * 2) : disp_range_y1;
    }
    else
    {
        dx = dx_start_ntsc;
        dy = disp_interlace ? (disp_range_y1 * 2) + 18 : disp_range_y1 + 1;
    }

    int current_width_vck = dw + 1;
    int target_width = 2560;
    dx += (target_width - current_width_vck) / 2;

    DLOG("Update_GS_Display: PSX %dx%d -> GS MAGH=%d DW=%d (VCK) DX=%d\n DY=%d\n DR=%d\n",
         psx_w, psx_h, magh, dw, dx, dy, disp_range_y1);

    /* Build DISPLAY register value */
    uint64_t display = (uint64_t)(dx & 0xFFF) |
                       ((uint64_t)(dy & 0x7FF) << 12) |
                       ((uint64_t)(magh & 0xF) << 23) |
                       ((uint64_t)(magv & 0x3) << 27) |
                       ((uint64_t)(dw & 0xFFF) << 32) |
                       ((uint64_t)(dh & 0x7FF) << 44);

    *((volatile uint64_t *)0x12000080) = display; /* DISPLAY1 */
    *((volatile uint64_t *)0x120000A0) = display; /* DISPLAY2 */
}

/* ═══════════════════════════════════════════════════════════════════
 *  Init_Graphics — one-time GS / DMA initialization
 * ═══════════════════════════════════════════════════════════════════ */

void Init_Graphics(void)
{
    printf("Initializing Graphics (GS)...\n");

    /* Uncomment for GPU debug logging:
     * gpu_debug_log = fopen("host:gpu_debug.log", "w");
     * if (gpu_debug_log)
     *     fprintf(gpu_debug_log, "GPU Debug Log\n");
     */

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    if (!psx_vram_shadow)
    {
        psx_vram_shadow = (u16 *)malloc(1024 * 512 * 2);
        if (psx_vram_shadow)
            memset(psx_vram_shadow, 0, 1024 * 512 * 2);
    }

    /* Initialize graphics like libdraw does */
    graph_initialize(fb_address, fb_width, fb_height, fb_psm, 0, 0);

    /* Override DISPFB to use PSX VRAM width (1024) instead of display width (640)
     * Ensures the display reads from the same layout that FRAME_1 writes to */
    {
        uint64_t dispfb = 0;
        dispfb |= (uint64_t)0 << 0;                  /* FBP (Base 0) */
        dispfb |= (uint64_t)PSX_VRAM_FBW << 9;       /* FBW (1024 pixels) */
        dispfb |= (uint64_t)GS_PSM_16S << 15;        /* PSM (CT16S — matches PSX 15-bit VRAM) */
        dispfb |= (uint64_t)0 << 32;                 /* DBX */
        dispfb |= (uint64_t)0 << 43;                 /* DBY */
        *((volatile uint64_t *)0x12000070) = dispfb; /* DISPFB1 */
        *((volatile uint64_t *)0x12000090) = dispfb; /* DISPFB2 */
    }

    /* Allocate PSX VRAM shadow (1024x512 x 16-bit) for VRAM read-back */
    if (!psx_vram_shadow)
    {
        psx_vram_shadow = (uint16_t *)memalign(64, 1024 * 512 * sizeof(uint16_t));
        if (psx_vram_shadow)
            memset(psx_vram_shadow, 0, 1024 * 512 * sizeof(uint16_t));
        else
            printf("ERROR: Failed to allocate PSX VRAM shadow!\n");
    }

    /* Setup GS environment for rendering */
    Setup_GS_Environment();

    /* Clear the visible VRAM to black so nothing flashes before the PSX
     * BIOS/game draws its first frame.  We draw a full-screen sprite
     * covering the entire 1024×512 PSX VRAM area. */
    {
        /* GIF tag: NLOOP=3, EOP=1, PRE=1, PRIM=sprite(6), FLG=PACKED, NREG=1, REGS=AD */
        Push_GIF_Tag(3, 1, 1, 6, 0, 1, 0xE);
        /* RGBAQ = black, full alpha */
        Push_GIF_Data(GS_set_RGBAQ(0, 0, 0, 0x80, 0x3F800000), 0x01);
        /* XYZ2: top-left and bottom-right with 2048 offset already baked into GS coords */
        Push_GIF_Data(GS_set_XYZ(2048 << 4, 2048 << 4, 0), 0x05);
        Push_GIF_Data(GS_set_XYZ((2048 + PSX_VRAM_WIDTH) << 4, (2048 + PSX_VRAM_HEIGHT) << 4, 0), 0x05);
        Flush_GIF();
    }

    printf("Graphics Initialized. GS rendering state set.\n");
}
