#include "superpsx.h"
#include <tamtypes.h>
#include <kernel.h>
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <stdio.h>
#include <string.h>
#include <gs_psm.h>

// Global context for libgraph
// Assuming simple single buffered for now to just clear screen.

// -- GPU State --
static u32 gpu_stat = 0x14802000;
static u32 gpu_read = 0;

// Framebuffer configuration - must be global for GPU rendering
static int fb_address = 0;
static int fb_width = 640;
static int fb_height = 448;
static int fb_psm = GS_PSM_16S;
// PSX VRAM is 1024x512 - buffer width for ALL GS operations must match
#define PSX_VRAM_WIDTH 1024
#define PSX_VRAM_HEIGHT 512
#define PSX_VRAM_FBW (PSX_VRAM_WIDTH / 64) /* =16 in 64-pixel units */

// -- GIF Buffer --
#define GIF_BUFFER_SIZE 16384
static u128 gif_packet_buf[GIF_BUFFER_SIZE] __attribute__((aligned(16)));
static int gif_packet_ptr = 0;

// -- GS Registers (Shadow) --
static int draw_offset_x = 0;
static int draw_offset_y = 0;
static int draw_clip_x1 = 0;
static int draw_clip_y1 = 0;
static int draw_clip_x2 = 640;
static int draw_clip_y2 = 480;

// Texture page state (from GP0 E1)
static int tex_page_x = 0;      // Texture page X offset in pixels
static int tex_page_y = 0;      // Texture page Y offset in pixels
static int tex_page_format = 2; // 0=4bit, 1=8bit, 2=15bit
static int semi_trans_mode = 0; // Semi-transparency mode from E1 bits 5-6
static int dither_enabled = 0;  // Dithering enable from E1 bit 9

// Shadow PSX VRAM for CLUT texture decode - dynamically allocated

// Debug log file (enable for detailed GPU command tracing)
static FILE *gpu_debug_log = NULL;
static u16 *psx_vram_shadow = NULL;

// VRAM transfer tracking for shadow writes
static int vram_tx_x, vram_tx_y, vram_tx_w, vram_tx_h, vram_tx_pixel;

// VRAM read state (GP0 C0h)
static int vram_read_x, vram_read_y, vram_read_w, vram_read_h;
static int vram_read_remaining; // Words remaining to read
static int vram_read_pixel;     // Current pixel index

// Polyline accumulation state (GP0 48h-5Fh polylines)
static int polyline_active = 0;
static int polyline_shaded = 0;
static int polyline_semi_trans = 0;
static u32 polyline_prev_color;
static u32 polyline_next_color;
static s16 polyline_prev_x, polyline_prev_y;
static int polyline_expect_color = 0; // For shaded: 1=expecting color, 0=expecting vertex

// Texture flip bits from GP0(E1) bits 12-13
static int tex_flip_x = 0;
static int tex_flip_y = 0;

// Texture window from GP0(E2)
static u32 tex_win_mask_x = 0; // In 8-pixel steps
static u32 tex_win_mask_y = 0;
static u32 tex_win_off_x = 0;
static u32 tex_win_off_y = 0;

// -- GIF Tag --
typedef struct
{
    u64 NLOOP : 15;
    u64 EOP : 1;
    u64 pad1 : 30;
    u64 PRE : 1;
    u64 PRIM : 11;
    u64 FLG : 2;
    u64 NREG : 4;
    u64 REGS;
} GifTag __attribute__((aligned(16)));

// -- DMA Packet --
typedef struct
{
    GifTag tag;
    u64 data[2]; // Variable length normally
} GSPacket;

// Compute GS ALPHA_1 register value from PSX semi-transparency mode
// GS formula: ((A-B)*C >> 7) + D  (C=FIX divides by 128, so FIX=128=1.0, 64=0.5, 32=0.25)
static u64 Get_Alpha_Reg(int mode)
{
    switch (mode)
    {
    case 0: // 0.5*Cd + 0.5*Cs: (Cs-Cd)*FIX+Cd with FIX=0x40 (64/128=0.5)
        return (u64)0 | ((u64)1 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x40 << 32);
    case 1: // Cd + Cs: (Cs-0)*FIX+Cd with FIX=0x80 (128/128=1.0)
        return (u64)0 | ((u64)2 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x80 << 32);
    case 2: // Cd - Cs: (Cd-Cs)*FIX+0 with FIX=0x80 (128/128=1.0)
        return (u64)1 | ((u64)0 << 2) | ((u64)2 << 4) | ((u64)2 << 6) | ((u64)0x80 << 32);
    case 3: // Cd + 0.25*Cs: (Cs-0)*FIX+Cd with FIX=0x20 (32/128=0.25)
        return (u64)0 | ((u64)2 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x20 << 32);
    default:
        return (u64)0 | ((u64)1 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x40 << 32);
    }
}

// -- Helper Functions --

static void Flush_GIF(void)
{
    if (gif_packet_ptr > 0)
    {
        //        printf("[GIF] Sending %d qwords via DMA\n", gif_packet_ptr);
        // Send to GIF (Channel 2)
        dma_channel_send_normal(DMA_CHANNEL_GIF, gif_packet_buf, gif_packet_ptr, 0, 0);
        dma_wait_fast(); // Wait for completion for now to be safe
        gif_packet_ptr = 0;
    }
}

static void Push_GIF_Tag(u64 nloop, u64 eop, u64 pre, u64 prim, u64 flg, u64 nreg, u64 regs)
{
    if (gif_packet_ptr + 1 >= GIF_BUFFER_SIZE)
        Flush_GIF();

    GifTag *tag = (GifTag *)&gif_packet_buf[gif_packet_ptr];
    tag->NLOOP = nloop;
    tag->EOP = eop;
    tag->pad1 = 0;
    tag->PRE = pre;
    tag->PRIM = prim;
    tag->FLG = flg;
    tag->NREG = nreg;
    tag->REGS = regs;

    gif_packet_ptr++;
}

static void Push_GIF_Data(u64 d0, u64 d1)
{
    if (gif_packet_ptr + 1 >= GIF_BUFFER_SIZE)
        Flush_GIF();

    u64 *p = (u64 *)&gif_packet_buf[gif_packet_ptr];
    p[0] = d0;
    p[1] = d1;

    gif_packet_ptr++;
}

static u32 GPU_GetWord(u32 addr)
{
    addr &= 0x1FFFFC;
    return *(u32 *)&psx_ram[addr];
}

// Translate PSX coordinates to GS coordinates (12.4 fixed point)
// GS XYZ2 register:
// 0-15: X
// 16-31: Y
// 32-63: Z
// Standard GS center offset is 2048.0 (32768 in 12.4)
static u64 Wrap_Coord(s16 x, s16 y)
{
    // PSX coordinates are relative to the Drawing Offset.
    // GS requires absolute coordinates in 12.4 fixed point, plus the primitive offset.
    // We set XYOFFSET to (draw_offset_x + 2048, draw_offset_y + 2048).
    // So the primitive coordinates should just be (x << 4) + center_offset?
    // Actually, usually on PS2 we set XYOFFSET to the center (2048, 2048).
    // Then we add 2048 to the coordinates?
    // Let's stick to a simpler mapping:
    // XYOFFSET = (2048, 2048) in GS pixels.
    // PSX (0,0) with DrawOffset(DX, DY) should be GS (DX, DY).
    // To make this work:
    // GS_X = (X + DX) * 16 + (2048 * 16)
    // But since we can set XYOFFSET, let's set XYOFFSET = (2048 + DX, 2048 + DY).
    // Then GS_X = X * 16.

    // However, Wrap_Coord currently does: (x + draw_offset_x + 2048) << 4.
    // This is applying the offset TWICE if we also set XYOFFSET to include draw_offset.

    // Approach 2 (Recommended for dynamic offsets):
    // Set XYOFFSET to fixed (2048, 2048).
    // Calculate final position here: (x + draw_offset_x + 2048) << 4.

    // Sumar 2048 para ajustar al XYOFFSET del GS
    s32 gx = ((s32)x + 2048) << 4;
    s32 gy = ((s32)y + 2048) << 4;

    // Clamp to valid GS range (though GS usually handles this via scissor)
    if (gx < 0)
        gx = 0;
    if (gy < 0)
        gy = 0;

    u32 z = 0;

    return (u64)z << 32 | (u64)(gy & 0xFFFF) << 16 | (u64)(gx & 0xFFFF);
}

// Helper to bundle color (RGBA8)
static u64 Wrap_Color(u32 bgr)
{
    u64 r = bgr & 0xFF;
    u64 g = (bgr >> 8) & 0xFF;
    u64 b = (bgr >> 16) & 0xFF;
    u64 a = 0x80; // Default alpha (not transparent)
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void Setup_GS_Environment(void)
{
    // Setup GS registers like draw_setup_environment does
    // This mimics what libdraw does

    // NLOOP=16, EOP=1, PRE=0, PRIM=0, FLG=PACKED, NREG=1, REGS=AD(0xE)
    Push_GIF_Tag(16, 1, 0, 0, 0, 1, 0xE);

    // FRAME_1 (Reg 0x4C) - Framebuffer address and settings
    // FBP=0 (Base 0), FBW=16 (1024/64 - matches PSX VRAM width), PSM=0 (CT32)
    u64 frame_reg = ((u64)fb_address >> 11) | ((u64)PSX_VRAM_FBW << 16) | ((u64)fb_psm << 24);
    Push_GIF_Data(frame_reg, 0x4C);

    // ZBUF_1 (Reg 0x4E) - Disable ZBuffer (mask bit = 1)
    Push_GIF_Data(((u64)0 << 0) | ((u64)0 << 24) | ((u64)1 << 32), 0x4E);

    // PRMODECONT (Reg 0x1A) - ENABLE use of GIF tag PRIM field
    // When PRE=1 in GIF tag, the tag's PRIM value is used for the primitive
    Push_GIF_Data(1, 0x1A);

    // XYOFFSET_1 (Reg 0x18) - Primitive coordinate offset
    // Set to (2048 << 4, 2048 << 4) = (32768, 32768)
    u64 offset_x = (u64)2048 << 4;
    u64 offset_y = (u64)2048 << 4;
    Push_GIF_Data(offset_x | (offset_y << 32), 0x18);

    // SCISSOR_1 (Reg 0x40) - Scissoring area (framebuffer space, post-XYOFFSET)
    // Cover full PSX VRAM initially; E3/E4 will narrow it
    u64 scax0 = 0, scax1 = PSX_VRAM_WIDTH - 1, scay0 = 0, scay1 = PSX_VRAM_HEIGHT - 1;
    Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40);

    // TEST_1 (Reg 0x47) - Alpha test, depth test, etc
    // ALPHA TEST: enable=1, method=ALWAYS (pass all pixels)
    // DATE: disabled (DATE=0) - Don't gate draws on dest alpha bit
    //   DATE=1 would block semi-transparent draws on pixels where STP=1
    // DEPTH TEST: enable=1, method=ALLPASS
    u64 test_reg = ((u64)1 << 0) | ((u64)1 << 1) | ((u64)0 << 4) | ((u64)0 << 12) |
                   ((u64)0 << 13) | ((u64)1 << 16) | ((u64)1 << 17);
    Push_GIF_Data(test_reg, 0x47);

    // FOGCOL (Reg 0x3D) - Fog color
    Push_GIF_Data(0, 0x3D);

    // PABE (Reg 0x49) - Per-pixel alpha blending enable
    // PSX uses texture STP bit (bit 15) to control per-pixel semi-transparency.
    // With PABE=1, GS checks source alpha MSB: if 0→opaque, if 1→blended.
    // CT16S texture alpha: STP=0→alpha=0x00(MSB=0), STP=1→alpha=0x80(MSB=1).
    // This correctly maps PSX per-pixel STP behavior to GS.
    Push_GIF_Data(1, 0x49);

    // ALPHA_1 (Reg 0x42) - Alpha blending settings
    // Default: PSX mode 0 = 0.5*Cd + 0.5*Cs
    // GS: ((Cs-Cd)*FIX >> 7)+Cd with FIX=0x40 (64/128=0.5)
    u64 alpha_reg = ((u64)0 << 0) | ((u64)1 << 2) | ((u64)2 << 4) |
                    ((u64)1 << 6) | ((u64)0x40 << 32);
    Push_GIF_Data(alpha_reg, 0x42);

    // DTHE (Reg 0x45) - Dithering off
    Push_GIF_Data(0, 0x45);

    // DIMX (Reg 0x44) - PSX Dithering matrix
    // PSX: -4 +0 -3 +1 / +2 -2 +3 -1 / -3 +1 -4 +0 / +3 -1 +2 -2
    // GS DIMX stores 3-bit signed values (two's complement): -4=4, -3=5, -2=6, -1=7, 0=0, 1=1, 2=2, 3=3
    // DM(col,row): [2:0]=DM00, [6:4]=DM01, [10:8]=DM02, [14:12]=DM03, etc.
    u64 dimx_reg = ((u64)4 << 0) | ((u64)0 << 4) | ((u64)5 << 8) | ((u64)1 << 12) |    // Row 0: -4, 0, -3, +1
                   ((u64)2 << 16) | ((u64)6 << 20) | ((u64)3 << 24) | ((u64)7 << 28) | // Row 1: +2, -2, +3, -1
                   ((u64)5 << 32) | ((u64)1 << 36) | ((u64)4 << 40) | ((u64)0 << 44) | // Row 2: -3, +1, -4, 0
                   ((u64)3 << 48) | ((u64)7 << 52) | ((u64)2 << 56) | ((u64)6 << 60);  // Row 3: +3, -1, +2, -2
    Push_GIF_Data(dimx_reg, 0x44);

    // COLCLAMP (Reg 0x46) - Color clamp
    Push_GIF_Data(1, 0x46);

    // FBA_1 (Reg 0x4A) - Alpha correction
    Push_GIF_Data(0, 0x4A);

    // TEX1_1 (Reg 0x14) - Texture filtering: nearest-neighbor for PSX pixel-perfect textures
    // LCM=1 (fixed LOD), MXL=0, MMAG=0 (nearest), MMIN=0 (nearest), MTBA=0, L=0, K=0
    Push_GIF_Data((u64)1 << 0, 0x14);

    // CLAMP_1 (Reg 0x08) - Texture clamping
    Push_GIF_Data(0, 0x08);

    // TEXA (Reg 0x3B) - Texture alpha expansion for CT16S
    // TA0 (bits 0-7): Alpha for STP=0 texels = 0x00 → PABE won't blend these
    // AEM (bit 15): 0 = standard mode
    // TA1 (bits 32-39): Alpha for STP=1 texels = 0x80 → PABE will blend these
    // This enables per-pixel semi-transparency matching PSX STP behavior
    Push_GIF_Data(((u64)0x00 << 0) | ((u64)0 << 15) | ((u64)0x80 << 32), 0x3B);

    Flush_GIF();
}

// -- Implementation --

u32 GPU_Read(void)
{
    // If VRAM read transfer is active (GP0 C0h)
    if (vram_read_remaining > 0)
    {
        // Read 2 pixels from shadow VRAM and pack into 32-bit word
        u16 p0 = 0, p1 = 0;

        if (psx_vram_shadow && vram_read_w > 0)
        {
            // Calculate current pixel position
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

        // If transfer complete, clear GPUSTAT ready bit
        if (vram_read_remaining == 0)
        {
            gpu_stat &= ~0x08000000; // Clear bit 27 (ready to send VRAM to CPU)
        }

        return (u32)p0 | ((u32)p1 << 16);
    }

    // Otherwise return GPU info (GP1 10h responses)
    return gpu_read;
}

u32 GPU_ReadStatus(void)
{
    // Force Bit 27 (Ready to Send DMA) to 1 to unblock BIOS
    return gpu_stat | 0x1C802000;
}

void GPU_VBlank(void)
{
    gpu_stat ^= 0x80000000;
}

// -- Immediate Mode State --
static int gpu_cmd_remaining = 0;
static u32 gpu_cmd_buffer[16];
static int gpu_cmd_ptr = 0;
static int gpu_transfer_words = 0;
static int gpu_transfer_total = 0;

// IMAGE transfer buffer (file scope for use by Upload_Shadow_VRAM_Region)
static u128 buf_image[1024];
static int buf_image_ptr = 0;

static void Start_VRAM_Transfer(int x, int y, int w, int h)
{
    // 1. Set BITBLTBUF (Buffer Address)
    // DBP=0 (Base 0), DBW=10 (640px), PSM=0 (CT32)
    // 2. Set TRXPOS (Dst X,Y)
    // 3. Set TRXREG (W, H)
    // 4. Set TRXDIR (0 = Host -> Local)

    // Using simple GIF tags to set registers
    Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE); // NLOOP=4, EOP=1, A+D

    // BITBLTBUF (0x50): DBP=0 (Base 0), DBW=16 (1024px), DPSM=CT16S
    Push_GIF_Data(((u64)GS_PSM_16S << 56) | ((u64)PSX_VRAM_FBW << 48), 0x50);

    // TRXPOS (0x51): SSAX=0, SSAY=0, DSAX=x, DSAY=y, DIR=0
    Push_GIF_Data(((u64)y << 48) | ((u64)x << 32), 0x51);

    // TRXREG (0x52): RRW=w, RRH=h
    Push_GIF_Data(((u64)h << 32) | (u64)w, 0x52);

    // TRXDIR (0x53): XDIR=0 (Host -> Local)
    Push_GIF_Data(0, 0x53);

    Flush_GIF();

    // Now prepare for IMAGE transfer
    // We will send REGS as we receive them.
    // NOTE: Sending small IMAGE packets is inefficient.
    // We should buffer a few.
}

// Upload a region from shadow VRAM to GS VRAM (used for VRAM wrap fixup)
static void Upload_Shadow_VRAM_Region(int x, int y, int w, int h)
{
    if (!psx_vram_shadow || w <= 0 || h <= 0)
        return;

    // Set up GS IMAGE transfer for the region
    Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
    Push_GIF_Data(((u64)GS_PSM_16S << 56) | ((u64)PSX_VRAM_FBW << 48), 0x50);
    Push_GIF_Data(((u64)y << 48) | ((u64)x << 32), 0x51);
    Push_GIF_Data(((u64)h << 32) | (u64)w, 0x52);
    Push_GIF_Data(0, 0x53); // Host -> Local
    Flush_GIF();

    // Send pixel data from shadow VRAM
    // Pack rows of w pixels into 128-bit qwords (8 pixels each per qword)
    for (int row = 0; row < h; row++)
    {
        int sy = y + row;
        if (sy >= 512) sy -= 512;
        u32 words_in_row = (w + 1) / 2; // 2 pixels per word
        u32 pending[4];
        int pc = 0;
        int qw_count = 0;

        for (u32 col = 0; col < (u32)w; col += 2)
        {
            int sx = x + col;
            if (sx >= 1024) sx -= 1024;
            u16 p0 = psx_vram_shadow[sy * 1024 + sx];
            u16 p1 = (col + 1 < (u32)w) ? psx_vram_shadow[sy * 1024 + ((sx + 1) & 0x3FF)] : 0;
            pending[pc++] = (u32)p0 | ((u32)p1 << 16);

            if (pc >= 4)
            {
                u64 lo = (u64)pending[0] | ((u64)pending[1] << 32);
                u64 hi = (u64)pending[2] | ((u64)pending[3] << 32);
                u128 q = (u128)lo | ((u128)hi << 64);
                buf_image[buf_image_ptr++] = q;
                pc = 0;
                qw_count++;

                if (buf_image_ptr >= 1000)
                {
                    Push_GIF_Tag(buf_image_ptr, 0, 0, 0, 2, 0, 0);
                    for (int i = 0; i < buf_image_ptr; i++)
                    {
                        u64 *pp = (u64 *)&buf_image[i];
                        Push_GIF_Data(pp[0], pp[1]);
                    }
                    buf_image_ptr = 0;
                }
            }
        }

        // Pad and flush partial qword at end of row
        if (pc > 0)
        {
            while (pc < 4)
                pending[pc++] = 0;
            u64 lo = (u64)pending[0] | ((u64)pending[1] << 32);
            u64 hi = (u64)pending[2] | ((u64)pending[3] << 32);
            u128 q = (u128)lo | ((u128)hi << 64);
            buf_image[buf_image_ptr++] = q;
            pc = 0;
        }
    }

    // Final flush
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 2, 0, 0);
        for (int i = 0; i < buf_image_ptr; i++)
        {
            u64 *pp = (u64 *)&buf_image[i];
            Push_GIF_Data(pp[0], pp[1]);
        }
        buf_image_ptr = 0;
    }
    Flush_GIF();
}

// Helper Macros for GS Data Generation
#define GS_set_RGBAQ(r, g, b, a, q) \
    ((u64)(r) | ((u64)(g) << 8) | ((u64)(b) << 16) | ((u64)(a) << 24) | ((u64)(q) << 32))

// Fixed: Added masking
#define GS_set_XYZ(x, y, z) \
    ((u64)((x) & 0xFFFF) | ((u64)((y) & 0xFFFF) << 16) | ((u64)((z) & 0xFFFFFFFF) << 32))

// Lookup table for Primitive Sizes (0x00-0xFF)
// Could be useful, but for now we calculate it.

// -- Helper: Emit a single line segment using A+D mode GIF packets --
static void Emit_Line_Segment_AD(s16 x0, s16 y0, u32 color0,
                                 s16 x1, s16 y1, u32 color1,
                                 int is_shaded, int is_semi_trans)
{
    u64 prim_reg = 1; // LINE
    if (is_shaded)
        prim_reg |= (1 << 3); // IIP=1 (Gouraud)
    if (is_semi_trans)
        prim_reg |= (1 << 6); // ABE=1

    int nregs = 5; // PRIM + 2*(RGBAQ + XYZ2)
    if (is_semi_trans)
        nregs++;

    Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE); // A+D mode

    if (is_semi_trans)
    {
        Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42); // ALPHA_1
    }

    Push_GIF_Data(prim_reg, 0x00); // PRIM register

    // Vertex 0
    Push_GIF_Data(GS_set_RGBAQ(color0 & 0xFF, (color0 >> 8) & 0xFF,
                               (color0 >> 16) & 0xFF, 0x80, 0x3F800000),
                  0x01);
    s32 gx0 = ((s32)x0 + draw_offset_x + 2048) << 4;
    s32 gy0 = ((s32)y0 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05);

    // Vertex 1
    Push_GIF_Data(GS_set_RGBAQ(color1 & 0xFF, (color1 >> 8) & 0xFF,
                               (color1 >> 16) & 0xFF, 0x80, 0x3F800000),
                  0x01);
    s32 gx1 = ((s32)x1 + draw_offset_x + 2048) << 4;
    s32 gy1 = ((s32)y1 + draw_offset_y + 2048) << 4;
    Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05);
}

// Apply PSX texture window formula to a texture coordinate
// texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
static u32 Apply_Tex_Window_U(u32 u)
{
    if (tex_win_mask_x == 0)
        return u;
    u32 mask = tex_win_mask_x * 8;
    u32 off = (tex_win_off_x & tex_win_mask_x) * 8;
    return (u & ~mask) | off;
}

static u32 Apply_Tex_Window_V(u32 v)
{
    if (tex_win_mask_y == 0)
        return v;
    u32 mask = tex_win_mask_y * 8;
    u32 off = (tex_win_off_y & tex_win_mask_y) * 8;
    return (v & ~mask) | off;
}

// Translate a single GP0 command to GS GIF packets
// Writes directly to the GIF buffer cursor
void Translate_GP0_to_GS(u32 *psx_cmd, u128 **gif_cursor)
{
    u32 cmd_word = psx_cmd[0];
    u32 cmd = (cmd_word >> 24) & 0xFF;

    // Polygon (0x20-0x3F)
    if ((cmd & 0xE0) == 0x20)
    {
        // ... (Existing Polygon Code) ...
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;

        // ... (Keep existing Polygon implementation logic here using the vars above)
        // RE-IMPLEMENTING POLYGON LOGIC TO BE SAFE + ADDING RECTS IN SAME FUNCTION

        // GS Primitive Type: Triangle (3)
        int prim_type = 3;

        u64 prim_reg = prim_type;
        if (is_shaded)
            prim_reg |= (1 << 3); // IIP=1 (Gouraud)
        if (is_textured)
        {
            prim_reg |= (1 << 4); // TME=1 (Texture On)
            prim_reg |= (1 << 8); // FST=1 (use UV register, not STQ)
        }
        if (cmd & 0x02)
            prim_reg |= (1 << 6); // ABE=1

        // Registers Sequence
        // Textured: UV, RGBA, XYZ2 (Regs: 0x513)
        // Flat: RGBA, XYZ2 (Regs: 0x51)
        int nreg = is_textured ? 3 : 2;
        u64 regs = is_textured ? 0x513 : 0x51;

        // Vertices
        int num_psx_verts = is_quad ? 4 : 3;

        u32 color = cmd_word & 0xFFFFFF;
        int idx = 1;

        struct Vertex
        {
            s16 x, y;
            u32 color;
            u32 uv;
        } verts[4];

        // Per-polygon texture page: for textured polygons, the PSX reads
        // the texpage from the 2nd vertex's UV word (bits 16-31).
        int poly_tex_page_x = tex_page_x;
        int poly_tex_page_y = tex_page_y;

        for (int i = 0; i < num_psx_verts; i++)
        {
            if (i == 0)
                verts[i].color = color;
            else if (is_shaded)
            {
                verts[i].color = psx_cmd[idx++] & 0xFFFFFF;
            }
            else
                verts[i].color = color;

            u32 xy = psx_cmd[idx++];
            verts[i].x = (s16)(xy & 0xFFFF);
            verts[i].y = (s16)(xy >> 16);

            if (is_textured)
            {
                verts[i].uv = psx_cmd[idx++];
                // 2nd vertex (i==1) UV word bits 16-31 contain texpage
                if (i == 1)
                {
                    u32 tpage = verts[i].uv >> 16;
                    poly_tex_page_x = (tpage & 0xF) * 64;
                    poly_tex_page_y = ((tpage >> 4) & 0x1) * 256;
                }
            }
        }

        if (is_quad)
        {
            // Check if we can use GS SPRITE for axis-aligned textured quads.
            // SPRITE gives linear UV interpolation matching PSX quad rasterization,
            // avoiding the seam artifact from triangle decomposition.
            int use_sprite = 0;
            if (is_textured && !is_shaded)
            {
                // PSX quad vertex order: 0=TL, 1=TR, 2=BL, 3=BR
                int quad_w = verts[1].x - verts[0].x;
                int quad_h = verts[2].y - verts[0].y;
                if (quad_w > 0 && quad_h > 0 &&
                    verts[0].y == verts[1].y && verts[2].y == verts[3].y &&
                    verts[0].x == verts[2].x && verts[1].x == verts[3].x)
                {
                    use_sprite = 1;
                }
            }

            if (use_sprite)
            {
                gif_packet_ptr = *gif_cursor - gif_packet_buf;

                u64 sprite_prim = 6; // SPRITE
                sprite_prim |= (1 << 4); // TME
                sprite_prim |= (1 << 8); // FST=1 (UV register mode)
                if (cmd & 0x02)
                    sprite_prim |= (1 << 6); // ABE

                u32 u0 = Apply_Tex_Window_U(verts[0].uv & 0xFF) + poly_tex_page_x;
                u32 v0 = Apply_Tex_Window_V((verts[0].uv >> 8) & 0xFF) + poly_tex_page_y;
                // Exclusive end: +1 past the last texel
                u32 u1 = Apply_Tex_Window_U(verts[3].uv & 0xFF) + 1 + poly_tex_page_x;
                u32 v1 = Apply_Tex_Window_V((verts[3].uv >> 8) & 0xFF) + 1 + poly_tex_page_y;

                s32 gx0 = ((s32)verts[0].x + draw_offset_x + 2048) << 4;
                s32 gy0 = ((s32)verts[0].y + draw_offset_y + 2048) << 4;
                s32 gx1 = ((s32)(verts[3].x + 1) + draw_offset_x + 2048) << 4;
                s32 gy1 = ((s32)(verts[3].y + 1) + draw_offset_y + 2048) << 4;

                u32 c = verts[0].color;
                u64 rgbaq = GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000);

                // PRIM + 2*(UV+RGBAQ+XYZ2) = 7 AD registers
                Push_GIF_Tag(7, 1, 0, 0, 0, 1, 0xE);
                Push_GIF_Data(sprite_prim, 0x00);
                Push_GIF_Data(GS_set_XYZ(u0 << 4, v0 << 4, 0), 0x03); // UV TL
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05); // XYZ2 TL
                Push_GIF_Data(GS_set_XYZ(u1 << 4, v1 << 4, 0), 0x03); // UV BR
                Push_GIF_Data(rgbaq, 0x01);
                Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05); // XYZ2 BR (kick)

                *gif_cursor = &gif_packet_buf[gif_packet_ptr];
            }
            else
            {
            // Emit two triangles: 0-1-2 and 1-3-2 using A+D mode
            int tris[2][3] = {{0, 1, 2}, {1, 3, 2}};
            // Sync cursor to gif_packet_ptr first
            gif_packet_ptr = *gif_cursor - gif_packet_buf;

            for (int t = 0; t < 2; t++)
            {
                // For non-textured: 1 PRIM + 3*(RGBAQ+XYZ2) = 7 registers
                // For textured: 1 PRIM + 3*(UV+RGBAQ+XYZ2) = 10 registers
                int ndata = is_textured ? 10 : 7;
                Push_GIF_Tag(ndata, (t == 1) ? 1 : 0, 0, 0, 0, 1, 0xE);
                Push_GIF_Data(prim_reg, 0x00); // PRIM register

                for (int v = 0; v < 3; v++)
                {
                    int i = tris[t][v];
                    if (is_textured)
                    {
                        u32 u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                        u32 v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                        Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03); // ST
                    }
                    u32 c = verts[i].color;
                    Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                    s32 gx = ((s32)verts[i].x + draw_offset_x + 2048) << 4;
                    s32 gy = ((s32)verts[i].y + draw_offset_y + 2048) << 4;
                    Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
                }
            }
            *gif_cursor = &gif_packet_buf[gif_packet_ptr];
            } // end else (triangle decomposition fallback)
        }
        else
        {
            // Triangle using A+D mode (most reliable for Gouraud)
            // Sync cursor to gif_packet_ptr
            gif_packet_ptr = *gif_cursor - gif_packet_buf;

            // 1 PRIM + 3*(RGBAQ+XYZ2) = 7 registers
            int ndata = is_textured ? 10 : 7;
            Push_GIF_Tag(ndata, 1, 0, 0, 0, 1, 0xE);
            Push_GIF_Data(prim_reg, 0x00); // PRIM register

            for (int i = 0; i < 3; i++)
            {
                if (is_textured)
                {
                    u32 u = Apply_Tex_Window_U(verts[i].uv & 0xFF) + poly_tex_page_x;
                    u32 v_coord = Apply_Tex_Window_V((verts[i].uv >> 8) & 0xFF) + poly_tex_page_y;
                    Push_GIF_Data(GS_set_XYZ(u << 4, v_coord << 4, 0), 0x03); // ST
                }
                u32 c = verts[i].color;
                Push_GIF_Data(GS_set_RGBAQ(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 0x80, 0x3F800000), 0x01);

                s32 gx = ((s32)verts[i].x + draw_offset_x + 2048) << 4;
                s32 gy = ((s32)verts[i].y + draw_offset_y + 2048) << 4;
                Push_GIF_Data(GS_set_XYZ(gx, gy, 0), 0x05);
            }
            *gif_cursor = &gif_packet_buf[gif_packet_ptr];

            // Debug output
            if (gpu_debug_log)
            {
                fprintf(gpu_debug_log, "[GPU] Triangle A+D: PRIM=%llu\n", (unsigned long long)prim_reg);
                fflush(gpu_debug_log);
            }
        }

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Draw Poly: Cmd=%02X Shaded=%d Quad=%d Verts=%d Color=%06X Offset=(%d,%d)\n",
                    cmd, is_shaded, is_quad, num_psx_verts, color, draw_offset_x, draw_offset_y);
            for (int i = 0; i < num_psx_verts; i++)
            {
                fprintf(gpu_debug_log, "\tV%d: (%d, %d) Col=%06X UV=%04X\n", i, verts[i].x, verts[i].y, verts[i].color, verts[i].uv);
            }
            fflush(gpu_debug_log);
        }
    }
    else if ((cmd & 0xE0) == 0x60)
    { // Rectangle (Sprite) - use GS SPRITE primitive for reliable rendering
        int is_textured = (cmd & 0x04) != 0;
        int is_var_size = (cmd & 0x18) == 0x00;
        int size_mode = (cmd >> 3) & 3; // 0=Var, 1=1x1, 2=8x8, 3=16x16

        u32 color = cmd_word & 0xFFFFFF;
        int idx = 1;

        s16 x, y;
        u32 xy = psx_cmd[idx++];
        x = (s16)(xy & 0xFFFF);
        y = (s16)(xy >> 16);

        u32 uv_clut = 0;
        if (is_textured)
            uv_clut = psx_cmd[idx++];

        int w, h;
        if (is_var_size)
        {
            u32 wh = psx_cmd[idx++];
            w = wh & 0x3FF;
            h = (wh >> 16) & 0x1FF;
        }
        else
        {
            if (size_mode == 1)
            {
                w = 1;
                h = 1;
            }
            else if (size_mode == 2)
            {
                w = 8;
                h = 8;
            }
            else
            {
                w = 16;
                h = 16;
            }
        }

        // Use GS SPRITE primitive (type 6) - 2 vertices: top-left, bottom-right
        u64 prim_reg = 6; // SPRITE
        if (is_textured)
        {
            prim_reg |= (1 << 4); // TME
            prim_reg |= (1 << 8); // FST=1 (use UV register, not STQ)
        }
        if (cmd & 0x02)
            prim_reg |= (1 << 6); // ABE

        if (is_textured)
        {
            // Textured sprite using A+D mode (explicit register writes, most reliable)
            // Sync cursor to gif_packet_ptr
            gif_packet_ptr = *gif_cursor - gif_packet_buf;

            if (gpu_debug_log)
            {
                fprintf(gpu_debug_log, "[GPU] TexRect: Cmd=%02X (%d,%d) %dx%d UV_CLUT=%08X TexPage(%d,%d) fmt=%d flipXY=(%d,%d)\n",
                        cmd, x, y, w, h, uv_clut, tex_page_x, tex_page_y, tex_page_format, tex_flip_x, tex_flip_y);
                fflush(gpu_debug_log);
            }

            u32 u0_raw = Apply_Tex_Window_U(uv_clut & 0xFF);
            u32 v0_raw = Apply_Tex_Window_V((uv_clut >> 8) & 0xFF);
            u32 v1_raw = v0_raw + h;

            // V coordinates with Y-flip (Y-flip uses simple swap, works perfectly)
            u32 v0_gs = v0_raw + tex_page_y;
            u32 v1_gs = v1_raw + tex_page_y;
            if (tex_flip_y)
            {
                u32 tmp = v0_gs;
                v0_gs = v1_gs;
                v1_gs = tmp;
            }

            s32 gy0 = ((s32)y + draw_offset_y + 2048) << 4;
            s32 gy1 = ((s32)(y + h) + draw_offset_y + 2048) << 4;

            u64 rgbaq = GS_set_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

            // Check raw texture bit (bit 0): 1=Decal (raw texture), 0=Modulate (texture*color)
            int is_raw_texture = (cmd & 0x01) != 0;
            int is_semi_trans = (cmd & 0x02) != 0;

            // --- X-flip SPRITE splitting ---
            // PSX X-flip formula: u = (u_start + 1 - column) & 0xFF
            // The +1 comes from the hardware mirroring around u_start + 0.5.
            // This creates a discontinuity when u wraps from 0 to 255.
            // GS SPRITE uses linear UV interpolation which cannot reproduce this wrap.
            // Solution: split into 2 SPRITEs at the wrap point.
            // Part A: columns from u_eff down to 0 (monotonically decreasing)
            // Part B: columns from 255 down to (256-n_b) (monotonically decreasing)
            // Each part: N texels across N pixels = exact -16 step in 10.4 fixed point.
            int num_sprites;
            s32 sp_gx0[2], sp_gx1[2];
            u32 sp_u0[2], sp_u1[2];
            int use_stq_wrap = 0;    // Use STQ float mode for VRAM X-wrap
            float stq_s0, stq_s1, stq_t0, stq_t1;

            if (tex_flip_x && w > 1)
            {
                int u_start = (int)u0_raw;
                int u_eff = (u_start + 1) & 0xFF; // Effective first UV (PSX mirrors around u_start + 0.5)
                int k_at_zero = u_eff; // Column index where u reaches 0
                int n_a = (k_at_zero + 1 < (int)w) ? (k_at_zero + 1) : (int)w;
                int n_b = (int)w - n_a;

                num_sprites = (n_b > 0) ? 2 : 1;

                // Sprite A: first n_a columns, UV from u_eff down to 0
                sp_gx0[0] = ((s32)x + draw_offset_x + 2048) << 4;
                sp_gx1[0] = ((s32)(x + n_a) + draw_offset_x + 2048) << 4;
                sp_u0[0] = u_eff + tex_page_x;
                // V1.u = V0.u - n_a (gives exact -16/pixel step in 10.4 fixed point)
                // For 1-pixel sprite, V1.u doesn't affect output, use V0.u+1
                sp_u1[0] = (n_a == 1) ? (sp_u0[0] + 1) : (sp_u0[0] - n_a);

                if (n_b > 0)
                {
                    // Sprite B: remaining n_b columns, UV from 255 down to (256-n_b)
                    sp_gx0[1] = sp_gx1[0];
                    sp_gx1[1] = ((s32)(x + w) + draw_offset_x + 2048) << 4;
                    sp_u0[1] = 255 + tex_page_x;
                    sp_u1[1] = sp_u0[1] - n_b;
                }
            }
            else
            {
                // Normal (no X-flip or w<=1): single sprite, with VRAM wrap handling
                num_sprites = 1;
                sp_gx0[0] = ((s32)x + draw_offset_x + 2048) << 4;
                sp_gx1[0] = ((s32)(x + w) + draw_offset_x + 2048) << 4;
                sp_u0[0] = u0_raw + tex_page_x;
                sp_u1[0] = u0_raw + w + tex_page_x;

                // Check for VRAM X wrapping (texture page extends past 1024-pixel boundary)
                // PSX VRAM wraps at 1024; GS UV register is only 14-bit (max texel 1023)
                // so we can't represent UV=1024+. Use STQ float mode with REPEAT clamp instead.
                if (sp_u1[0] > 1024)
                {
                    use_stq_wrap = 1;
                    // Single sprite, full width - STQ mode handles the wrap
                    num_sprites = 1;
                    stq_s0 = (float)sp_u0[0] / 1024.0f;
                    stq_s1 = (float)sp_u1[0] / 1024.0f;
                    stq_t0 = (float)v0_gs / 512.0f;
                    stq_t1 = (float)v1_gs / 512.0f;
                }
            }

            int nregs = 1 + num_sprites * 6; // PRIM + N*(ST/UV+RGBAQ+XYZ2)*2
            nregs += 2;    // +DTHE=0 before, +DTHE=restore after (PSX rects never dither)
            if (is_raw_texture)
                nregs += 4; // +2 TEX0 before, +2 TEX0 after
            if (is_semi_trans)
                nregs += 1; // +1 ALPHA_1 before

            // For STQ wrap mode, use FST=0 (float texture coords with REPEAT)
            if (use_stq_wrap)
                prim_reg &= ~(1 << 8); // Clear FST bit → use STQ instead of UV

            Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE);

            // PSX rectangles never apply dithering - disable temporarily
            Push_GIF_Data(0, 0x45); // DTHE = 0

            // Set ALPHA_1 before the draw if semi-transparent
            if (is_semi_trans)
            {
                Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42); // ALPHA_1
            }

            // If raw texture, override TEX0 to TFX=1 (Decal)
            if (is_raw_texture)
            {
                u64 tex0_decal = 0;
                tex0_decal |= (u64)PSX_VRAM_FBW << 14;
                tex0_decal |= (u64)GS_PSM_16S << 20;
                tex0_decal |= (u64)10 << 26;
                tex0_decal |= (u64)9 << 30;
                tex0_decal |= (u64)1 << 34;      // TCC = 1
                tex0_decal |= (u64)1 << 35;      // TFX = 1 (Decal)
                Push_GIF_Data(tex0_decal, 0x06); // TEX0_1
                Push_GIF_Data(0, 0x3F);          // TEXFLUSH
            }

            Push_GIF_Data(prim_reg, 0x00); // PRIM register

            // Emit vertex pairs for each sprite
            for (int si = 0; si < num_sprites; si++)
            {
                if (use_stq_wrap)
                {
                    // STQ float mode: ST register (0x02) with float S,T coordinates
                    // GS REPEAT clamp (CLAMP_1=0) wraps S>=1.0 seamlessly at 1024 texels
                    u32 s0_bits, t0_bits, s1_bits, t1_bits;
                    memcpy(&s0_bits, &stq_s0, 4);
                    memcpy(&t0_bits, &stq_t0, 4);
                    memcpy(&s1_bits, &stq_s1, 4);
                    memcpy(&t1_bits, &stq_t1, 4);

                    Push_GIF_Data((u64)s0_bits | ((u64)t0_bits << 32), 0x02); // ST
                    Push_GIF_Data(rgbaq, 0x01);                               // RGBAQ (Q=1.0)
                    Push_GIF_Data(GS_set_XYZ(sp_gx0[si], gy0, 0), 0x05);     // XYZ2

                    Push_GIF_Data((u64)s1_bits | ((u64)t1_bits << 32), 0x02); // ST
                    Push_GIF_Data(rgbaq, 0x01);                               // RGBAQ (Q=1.0)
                    Push_GIF_Data(GS_set_XYZ(sp_gx1[si], gy1, 0), 0x05);     // XYZ2
                }
                else
                {
                    // UV fixed-point mode (14-bit, 10.4 format)
                    Push_GIF_Data(GS_set_XYZ(sp_u0[si] << 4, v0_gs << 4, 0), 0x03); // UV
                    Push_GIF_Data(rgbaq, 0x01);                                       // RGBAQ
                    Push_GIF_Data(GS_set_XYZ(sp_gx0[si], gy0, 0), 0x05);             // XYZ2

                    Push_GIF_Data(GS_set_XYZ(sp_u1[si] << 4, v1_gs << 4, 0), 0x03); // UV
                    Push_GIF_Data(rgbaq, 0x01);                                       // RGBAQ
                    Push_GIF_Data(GS_set_XYZ(sp_gx1[si], gy1, 0), 0x05);             // XYZ2
                }
            }

            // Restore TEX0 to TFX=0 (Modulate) after raw texture draw
            if (is_raw_texture)
            {
                u64 tex0_mod = 0;
                tex0_mod |= (u64)PSX_VRAM_FBW << 14;
                tex0_mod |= (u64)GS_PSM_16S << 20;
                tex0_mod |= (u64)10 << 26;
                tex0_mod |= (u64)9 << 30;
                tex0_mod |= (u64)1 << 34;      // TCC = 1
                tex0_mod |= (u64)0 << 35;      // TFX = 0 (Modulate)
                Push_GIF_Data(tex0_mod, 0x06); // TEX0_1
                Push_GIF_Data(0, 0x3F);        // TEXFLUSH
            }

            // Restore dither state
            Push_GIF_Data((u64)dither_enabled, 0x45); // DTHE = restore

            *gif_cursor = &gif_packet_buf[gif_packet_ptr];
        }
        else
        {
            // Flat sprite using A+D mode (explicit register writes)
            gif_packet_ptr = *gif_cursor - gif_packet_buf;

            s32 gx0 = ((s32)x + draw_offset_x + 2048) << 4;
            s32 gy0 = ((s32)y + draw_offset_y + 2048) << 4;
            s32 gx1 = ((s32)(x + w) + draw_offset_x + 2048) << 4;
            s32 gy1 = ((s32)(y + h) + draw_offset_y + 2048) << 4;

            u64 rgbaq = GS_set_RGBAQ(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, 0x80, 0x3F800000);

            int is_semi_trans = (cmd & 0x02) != 0;
            int nregs = 5; // PRIM + 2*(RGBAQ + XYZ2)
            nregs += 2;    // +DTHE=0 before, +DTHE=restore after (PSX rects never dither)
            if (is_semi_trans)
                nregs += 1; // +1 ALPHA_1

            Push_GIF_Tag(nregs, 1, 0, 0, 0, 1, 0xE);

            // PSX rectangles never apply dithering - disable temporarily
            Push_GIF_Data(0, 0x45); // DTHE = 0

            // Set ALPHA_1 before the draw if semi-transparent
            if (is_semi_trans)
            {
                Push_GIF_Data(Get_Alpha_Reg(semi_trans_mode), 0x42); // ALPHA_1
            }

            Push_GIF_Data(prim_reg, 0x00); // PRIM register

            // Vertex 0 (top-left)
            Push_GIF_Data(rgbaq, 0x01);                   // RGBAQ
            Push_GIF_Data(GS_set_XYZ(gx0, gy0, 0), 0x05); // XYZ2

            // Vertex 1 (bottom-right)
            Push_GIF_Data(rgbaq, 0x01);                   // RGBAQ
            Push_GIF_Data(GS_set_XYZ(gx1, gy1, 0), 0x05); // XYZ2

            // Restore dither state
            Push_GIF_Data((u64)dither_enabled, 0x45); // DTHE = restore

            *gif_cursor = &gif_packet_buf[gif_packet_ptr];
        }

        // printf("[GPU] Draw Sprite: Rect (%d,%d %dx%d) Color=%06X\n", x, y, w, h, color);
    }
    else if (cmd == 0x02)
    { // FillRect
        // GP0(02h) - Fill Rectangle in VRAM
        // NOT affected by Drawing Area or Mask settings
        // Coordinates are absolute VRAM positions

        u32 color = cmd_word & 0xFFFFFF;
        u32 xy = psx_cmd[1];
        u32 wh = psx_cmd[2];
        // Proper PSX coordinate masking for FillRect
        int x = (xy & 0xFFFF) & 0x3F0; // X: 10 bits, aligned to 16 pixels
        int y = (xy >> 16) & 0x1FF;    // Y: 9 bits
        int w = ((wh & 0xFFFF) & 0x3FF) + 0xF;
        w &= ~0xF; // W: round up to 16
        if (w == 0)
            w = 0x400;
        int h = (wh >> 16) & 0x1FF; // H: 9 bits
        if (h == 0)
            h = 0x200;

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Fill Rect: (%d,%d %dx%d) Color=%06X\n", x, y, w, h, color);
            fflush(gpu_debug_log);
        }

        // FillRect bypasses scissor - temporarily set scissor to full VRAM
        // GIF tag: NLOOP=3, EOP=1, AD mode (set scissor, draw sprite, restore scissor)
        Push_GIF_Tag(5, 1, 0, 0, 0, 1, 0xE);
        // Set SCISSOR to full VRAM
        u64 full_scissor = 0 | ((u64)(PSX_VRAM_WIDTH - 1) << 16) | ((u64)0 << 32) | ((u64)(PSX_VRAM_HEIGHT - 1) << 48);
        Push_GIF_Data(full_scissor, 0x40);
        // Set PRIM to SPRITE (6)
        Push_GIF_Data(6, 0x00);
        // Set RGBAQ
        u32 r = color & 0xFF;
        u32 g = (color >> 8) & 0xFF;
        u32 b = (color >> 16) & 0xFF;
        Push_GIF_Data(GS_set_RGBAQ(r, g, b, 0x80, 0x3F800000), 0x01);
        // XYZ2 top-left (absolute VRAM coordinates, not subject to draw offset)
        s32 x1 = (x + 2048) << 4;
        s32 y1 = (y + 2048) << 4;
        s32 x2 = (x + w + 2048) << 4;
        s32 y2 = (y + h + 2048) << 4;
        Push_GIF_Data(GS_set_XYZ(x1, y1, 0), 0x05);
        // XYZ2 bottom-right (triggers draw)
        Push_GIF_Data(GS_set_XYZ(x2, y2, 0), 0x05);
        // NOTE: Scissor will be restored next time E3/E4 are set, or we restore it now
        Flush_GIF();

        // Restore original scissor
        Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
        u64 orig_scissor = (u64)draw_clip_x1 | ((u64)draw_clip_x2 << 16) | ((u64)draw_clip_y1 << 32) | ((u64)draw_clip_y2 << 48);
        Push_GIF_Data(orig_scissor, 0x40);
        Flush_GIF();

        // Update shadow VRAM for filled area
        if (psx_vram_shadow)
        {
            u16 psx_color = ((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10);
            for (int row = y; row < y + h && row < 512; row++)
            {
                for (int col = x; col < x + w && col < 1024; col++)
                {
                    psx_vram_shadow[row * 1024 + col] = psx_color;
                }
            }
        }

        // FillRect writes directly to GIF, doesn't use cursor
        // Sync cursor back to current gif_packet_ptr position
        *gif_cursor = &gif_packet_buf[gif_packet_ptr];
    }
    else if ((cmd & 0xE0) == 0x40)
    { // Line (GP0 40h-5Fh) - using A+D mode for reliable rendering
        int is_shaded = (cmd & 0x10) != 0;
        int is_semi_trans = (cmd & 0x02) != 0;

        u32 color0 = cmd_word & 0xFFFFFF;
        int idx = 1;

        // Vertex 0
        u32 xy0 = psx_cmd[idx++];
        s16 x0 = (s16)(xy0 & 0xFFFF);
        s16 y0 = (s16)(xy0 >> 16);

        // Vertex 1
        u32 color1 = color0;
        if (is_shaded)
        {
            color1 = psx_cmd[idx++] & 0xFFFFFF;
        }
        u32 xy1 = psx_cmd[idx++];
        s16 x1 = (s16)(xy1 & 0xFFFF);
        s16 y1 = (s16)(xy1 >> 16);

        // Sync cursor to gif_packet_ptr
        gif_packet_ptr = *gif_cursor - gif_packet_buf;

        Emit_Line_Segment_AD(x0, y0, color0, x1, y1, color1, is_shaded, is_semi_trans);

        *gif_cursor = &gif_packet_buf[gif_packet_ptr];

        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] Draw Line: (%d,%d)-(%d,%d) Color=%06X->%06X\n",
                    x0, y0, x1, y1, color0, color1);
            fflush(gpu_debug_log);
        }
    }
}

// Get number of words for a command
static int GPU_GetCommandSize(u32 cmd)
{
    if ((cmd & 0xE0) == 0x20)
    { // Polygon
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        int num_verts = is_quad ? 4 : 3;

        int words = 1;      // Command/Color0
        words += num_verts; // Coords
        if (is_textured)
            words += num_verts; // UVs
        if (is_shaded)
            words += (num_verts - 1); // Colors 1..N
        return words;
    }
    if (cmd == 0x02)
        return 3; // FillRect: Cmd, XY, WH

    // Rectangles (0x60-0x7F)
    if ((cmd & 0xE0) == 0x60)
    {
        int is_textured = (cmd & 0x04) != 0;
        int size_mode = (cmd >> 3) & 3; // 0=Var, 1=1x1, 2=8x8, 3=16x16
        int words = 1;                  // Command/Color
        words += 1;                     // XY vertex
        if (is_textured)
            words += 1; // UV + CLUT
        if (size_mode == 0)
            words += 1; // Variable size: WH word
        return words;
    }

    // Lines (0x40-0x5F)
    if ((cmd & 0xE0) == 0x40)
    {
        int is_shaded = (cmd & 0x10) != 0;
        // Flat line = 3 words (cmd+color, v1, v2)
        // Shaded line = 4 words (cmd+c1, v1, c2, v2)
        // Polyline = variable (terminated by 0x5555_5555) - treat as 2 verts minimum
        if (is_shaded)
            return 4;
        return 3;
    }

    // VRAM-to-VRAM copy (0x80-0x9F)
    if ((cmd & 0xE0) == 0x80)
        return 4; // cmd, src XY, dst XY, WH

    return 1;
}

// Moved to file scope for use by Upload_Shadow_VRAM_Region
// (was previously at GPU_WriteGP0 scope)

// Convert 2 PSX pixels (32-bit word) to 2 GS pixels (2x 32-bit RGBA)
// Returns 64-bit (2 pixels)
// For CT16S framebuffer: PSX 16-bit pixels pass through directly
// No conversion needed - PSX RGBA5551 == GS CT16 format
static u64 Convert_Pixels_16(u32 data)
{
    // Just return the raw 32-bit word (2 x 16-bit pixels) as-is
    return (u64)data;
}

void GPU_WriteGP0(u32 data)
{
    // If transferring data (A0/C0)
    if (gpu_transfer_words > 0)
    {
        // Write to shadow VRAM (raw 16-bit data)
        if (psx_vram_shadow && vram_tx_w > 0)
        {
            u16 p0 = data & 0xFFFF;
            u16 p1 = data >> 16;

            int px = vram_tx_x + (vram_tx_pixel % vram_tx_w);
            int py = vram_tx_y + (vram_tx_pixel / vram_tx_w);
            if (px < 1024 && py < 512)
                psx_vram_shadow[py * 1024 + px] = p0;
            vram_tx_pixel++;

            px = vram_tx_x + (vram_tx_pixel % vram_tx_w);
            py = vram_tx_y + (vram_tx_pixel / vram_tx_w);
            if (px < 1024 && py < 512)
                psx_vram_shadow[py * 1024 + px] = p1;
            vram_tx_pixel++;
        }

        // For CT16S: accumulate raw 32-bit words (2 pixels each) into 128-bit qwords (8 pixels)
        static u32 pending_words[4];
        static int pending_count = 0;

        pending_words[pending_count++] = data;

        if (pending_count >= 4)
        {
            // Pack 4 x 32-bit words into one 128-bit qword
            u64 lo = (u64)pending_words[0] | ((u64)pending_words[1] << 32);
            u64 hi = (u64)pending_words[2] | ((u64)pending_words[3] << 32);
            u128 q = (u128)lo | ((u128)hi << 64);
            buf_image[buf_image_ptr++] = q;
            pending_count = 0;

            if (buf_image_ptr >= 1000)
            {
                // Flush to GIF using IMAGE mode (FLG=2)
                Push_GIF_Tag(buf_image_ptr, 0, 0, 0, 2, 0, 0);
                for (int i = 0; i < buf_image_ptr; i++)
                {
                    u64 *p = (u64 *)&buf_image[i];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
        }

        gpu_transfer_words--;
        if (gpu_transfer_words == 0)
        {
            // End of transfer. Flush remaining partial qword.
            if (pending_count > 0)
            {
                // Pad remaining words with 0
                while (pending_count < 4)
                    pending_words[pending_count++] = 0;
                u64 lo = (u64)pending_words[0] | ((u64)pending_words[1] << 32);
                u64 hi = (u64)pending_words[2] | ((u64)pending_words[3] << 32);
                u128 q = (u128)lo | ((u128)hi << 64);
                buf_image[buf_image_ptr++] = q;
                pending_count = 0;
            }
            if (buf_image_ptr > 0)
            {
                // FLG=2 (IMAGE mode), EOP=1 (end of transfer)
                Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 2, 0, 0);
                for (int i = 0; i < buf_image_ptr; i++)
                {
                    u64 *p = (u64 *)&buf_image[i];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
            Flush_GIF();

            // VRAM wrap fixup: if the transfer destination crossed the 1024-pixel X boundary,
            // the GS IMAGE transfer wrote pixels to wrong positions for the wrapped part.
            // Re-upload the wrapped region from shadow VRAM (which has correct wrapped data).
            if (vram_tx_x + vram_tx_w > 1024)
            {
                int wrap_w = (vram_tx_x + vram_tx_w) - 1024;
                Upload_Shadow_VRAM_Region(0, vram_tx_y, wrap_w, vram_tx_h);
            }
        }
        return;
    }

    // Polyline continuation - handle before normal command processing
    if (polyline_active)
    {
        // Check for polyline terminator
        if ((data & 0xF000F000) == 0x50005000)
        {
            polyline_active = 0;
            Flush_GIF();
            return;
        }

        if (polyline_shaded && polyline_expect_color)
        {
            // This is a color word for the next vertex
            polyline_next_color = data & 0xFFFFFF;
            polyline_expect_color = 0;
        }
        else
        {
            // This is a vertex word
            s16 x = (s16)(data & 0xFFFF);
            s16 y = (s16)(data >> 16);

            u32 new_color = polyline_shaded ? polyline_next_color : polyline_prev_color;

            Emit_Line_Segment_AD(polyline_prev_x, polyline_prev_y, polyline_prev_color,
                                 x, y, new_color,
                                 polyline_shaded, polyline_semi_trans);
            Flush_GIF();

            polyline_prev_x = x;
            polyline_prev_y = y;
            polyline_prev_color = new_color;

            if (polyline_shaded)
            {
                polyline_expect_color = 1; // Next word should be a color
            }
        }
        return;
    }

    if (gpu_cmd_remaining > 0)
    {
        gpu_cmd_buffer[gpu_cmd_ptr++] = data;
        gpu_cmd_remaining--;
        if (gpu_cmd_remaining == 0)
        {
            // Process accumulated command
            u32 cmd = gpu_cmd_buffer[0] >> 24;
            if (cmd == 0xA0)
            {
                // Load Image (CPU to VRAM)
                u32 wh = gpu_cmd_buffer[2];
                u32 w = wh & 0xFFFF;
                u32 h = wh >> 16;
                if (w == 0)
                    w = 1024;
                if (h == 0)
                    h = 512;
                gpu_transfer_words = (w * h + 1) / 2;
                gpu_transfer_total = gpu_transfer_words;
                printf("[GPU] GP0(A0) Start Transfer: %dx%d (%d words)\n", w, h, gpu_transfer_words);

                // Track transfer position for shadow VRAM
                u32 xy = gpu_cmd_buffer[1];
                vram_tx_x = xy & 0xFFFF;
                vram_tx_y = xy >> 16;
                vram_tx_w = w;
                vram_tx_h = h;
                vram_tx_pixel = 0;

                // Init GS Transfer
                Start_VRAM_Transfer(vram_tx_x, vram_tx_y, w, h);
            }
            else if (cmd == 0xC0)
            {
                // Copy VRAM to CPU
                u32 xy = gpu_cmd_buffer[1];
                u32 wh = gpu_cmd_buffer[2];
                vram_read_x = xy & 0xFFFF;
                vram_read_y = xy >> 16;
                vram_read_w = wh & 0xFFFF;
                vram_read_h = wh >> 16;
                if (vram_read_w == 0)
                    vram_read_w = 1024;
                if (vram_read_h == 0)
                    vram_read_h = 512;
                vram_read_remaining = (vram_read_w * vram_read_h + 1) / 2;
                vram_read_pixel = 0;

                // Set GPUSTAT bit 27 (ready to send VRAM to CPU)
                gpu_stat |= 0x08000000;

                printf("[GPU] GP0(C0) VRAM Read: %dx%d at (%d,%d), %d words\n",
                       vram_read_w, vram_read_h, vram_read_x, vram_read_y, vram_read_remaining);
            }
            else if ((cmd & 0xE0) == 0x80)
            {
                // GP0(80h) VRAM-to-VRAM Copy
                u32 src_xy = gpu_cmd_buffer[1];
                u32 dst_xy = gpu_cmd_buffer[2];
                u32 wh = gpu_cmd_buffer[3];
                int sx = src_xy & 0x3FF;
                int sy = (src_xy >> 16) & 0x1FF;
                int dx = dst_xy & 0x3FF;
                int dy = (dst_xy >> 16) & 0x1FF;
                int w = wh & 0x3FF;
                int h = (wh >> 16) & 0x1FF;
                if (w == 0)
                    w = 0x400;
                if (h == 0)
                    h = 0x200;

                printf("[GPU] GP0(80) VRAM Copy: (%d,%d)->(%d,%d) %dx%d\n", sx, sy, dx, dy, w, h);

                // Update shadow VRAM
                if (psx_vram_shadow)
                {
                    for (int row = 0; row < h; row++)
                    {
                        for (int col = 0; col < w; col++)
                        {
                            int src_px = ((sy + row) & 0x1FF) * 1024 + ((sx + col) & 0x3FF);
                            int dst_px = ((dy + row) & 0x1FF) * 1024 + ((dx + col) & 0x3FF);
                            psx_vram_shadow[dst_px] = psx_vram_shadow[src_px];
                        }
                    }
                }

                // Use GS local-to-local transfer
                Flush_GIF(); // Ensure pending draws complete before transfer

                Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);

                // BITBLTBUF: SBP=0, SBW=16, SPSM=CT16S, DBP=0, DBW=16, DPSM=CT16S
                u64 bitblt = ((u64)PSX_VRAM_FBW << 16) | ((u64)GS_PSM_16S << 24) |
                             ((u64)PSX_VRAM_FBW << 48) | ((u64)GS_PSM_16S << 56);
                Push_GIF_Data(bitblt, 0x50);

                // TRXPOS: SSAX=sx, SSAY=sy, DSAX=dx, DSAY=dy
                u64 trxpos = (u64)sx | ((u64)sy << 16) | ((u64)dx << 32) | ((u64)dy << 48);
                Push_GIF_Data(trxpos, 0x51);

                // TRXREG: RRW=w, RRH=h
                Push_GIF_Data((u64)w | ((u64)h << 32), 0x52);

                // TRXDIR: 2 = Local-to-Local
                Push_GIF_Data(2, 0x53);

                Flush_GIF();
            }
            else
            {
                // Execute Primitive via Translate_GP0_to_GS
                u128 *cursor = &gif_packet_buf[gif_packet_ptr];
                Translate_GP0_to_GS(gpu_cmd_buffer, &cursor);
                gif_packet_ptr = cursor - gif_packet_buf;
                Flush_GIF(); // Flush immediately so primitives are visible

                // Check if this was a polyline command - activate polyline mode
                if ((cmd & 0xE0) == 0x40 && (cmd & 0x08))
                {
                    polyline_active = 1;
                    polyline_shaded = (cmd & 0x10) != 0;
                    polyline_semi_trans = (cmd & 0x02) != 0;

                    // Save last vertex from the first segment
                    int v1_idx = polyline_shaded ? 3 : 2;
                    u32 xy1 = gpu_cmd_buffer[v1_idx];
                    polyline_prev_x = (s16)(xy1 & 0xFFFF);
                    polyline_prev_y = (s16)(xy1 >> 16);

                    if (polyline_shaded)
                    {
                        polyline_prev_color = gpu_cmd_buffer[2] & 0xFFFFFF; // color1
                        polyline_expect_color = 1;                          // Next word is a color
                    }
                    else
                    {
                        polyline_prev_color = gpu_cmd_buffer[0] & 0xFFFFFF; // Original flat color
                        polyline_expect_color = 0;                          // Next word is a vertex
                    }
                }
            }
        }
        return;
    }

    u32 cmd = (data >> 24) & 0xFF;

    // Commands that need parameter accumulation
    if (cmd == 0xA0 || cmd == 0xC0 || (cmd & 0xE0) == 0x80)
    {
        gpu_cmd_buffer[0] = data;
        gpu_cmd_ptr = 1;
        if (cmd == 0xA0)
            gpu_cmd_remaining = 2; // YX, WH
        else if (cmd == 0xC0)
            gpu_cmd_remaining = 2; // YX, WH
        else
            gpu_cmd_remaining = 3; // src XY, dst XY, WH
        return;
    }

    switch (cmd)
    {
    case 0xE1: // Draw Mode
    {
        // Parse Texture Page settings
        // 0-3: TP X (x64 halfwords)
        // 4: TP Y (x256 lines)
        // 7-8: TPF (0=4bit, 1=8bit, 2=15bit)

        u32 tp_x = data & 0xF;
        u32 tp_y = (data >> 4) & 1;
        u32 tpf = (data >> 7) & 3;

        // Store texture page position in PSX VRAM pixel coords
        // For all formats, the VRAM position is tp_x*64, tp_y*256 in halfword coords
        // Since we upload each halfword as one CT32 pixel at the same (x,y),
        // the GS VRAM position matches: (tp_x*64, tp_y*256)
        tex_page_x = tp_x * 64;
        tex_page_y = tp_y * 256;
        tex_page_format = tpf;

        // Semi-transparency mode: bits 5-6 of E1
        u32 trans_mode = (data >> 5) & 3;
        semi_trans_mode = trans_mode;

        // Texture flip bits: bits 12-13 of E1
        tex_flip_x = (data >> 12) & 1;
        tex_flip_y = (data >> 13) & 1;

        // Set TEX0: Use TBP0=0 (whole VRAM), UV offset applied per-vertex
        Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE);
        u64 tex0 = 0;                    // TBP0 = 0 (full VRAM base)
        tex0 |= (u64)PSX_VRAM_FBW << 14; // TBW = 16 (1024 pixels / 64)
        tex0 |= (u64)GS_PSM_16S << 20;   // PSM = CT16S (matches framebuffer)
        tex0 |= (u64)10 << 26;           // TW = 10 (2^10 = 1024)
        tex0 |= (u64)9 << 30;            // TH = 9 (2^9 = 512)
        tex0 |= (u64)1 << 34;            // TCC = 1 (RGBA)
        tex0 |= (u64)0 << 35;            // TFX = 0 (Modulate)

        Push_GIF_Data(tex0, 0x06); // TEX0_1
        Push_GIF_Data(0, 0x3F);    // TEXFLUSH

        // Dithering: bit 9 of GP0 E1
        u32 dither_enable = (data >> 9) & 1;
        dither_enabled = dither_enable;
        Push_GIF_Data(dither_enable, 0x45); // DTHE register

        // Semi-transparency blending (ALPHA_1 register 0x42)
        // PSX modes: 0=0.5B+0.5F, 1=B+F, 2=B-F, 3=B+0.25F
        // GS ALPHA: A=Cs, B=Cd, C=FIX/As, D=Cd/0
        // Formula: (A-B)*C + D
        {
            u64 alpha_reg;
            switch (trans_mode)
            {
            case 0: // 0.5*Cd + 0.5*Cs
                    // GS: ((Cs-Cd)*FIX >> 7)+Cd with FIX=0x40 (64/128=0.5)
                alpha_reg = (u64)0 | ((u64)1 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x40 << 32);
                break;
            case 1: // Cd + Cs (saturating)
                    // GS: ((Cs-0)*FIX >> 7)+Cd with FIX=0x80 (128/128=1.0)
                alpha_reg = (u64)0 | ((u64)2 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x80 << 32);
                break;
            case 2: // Cd - Cs
                    // GS: ((Cd-Cs)*FIX >> 7)+0 with FIX=0x80 (128/128=1.0)
                alpha_reg = (u64)1 | ((u64)0 << 2) | ((u64)2 << 4) | ((u64)2 << 6) | ((u64)0x80 << 32);
                break;
            case 3: // Cd + 0.25*Cs
                    // GS: ((Cs-0)*FIX >> 7)+Cd with FIX=0x20 (32/128=0.25)
                alpha_reg = (u64)0 | ((u64)2 << 2) | ((u64)2 << 4) | ((u64)1 << 6) | ((u64)0x20 << 32);
                break;
            }
            Push_GIF_Data(alpha_reg, 0x42); // ALPHA_1
        }

        // printf("[GPU] E1: TexPage(%d,%d) fmt=%d\n", tex_page_x, tex_page_y, tpf);
        if (gpu_debug_log)
        {
            fprintf(gpu_debug_log, "[GPU] E1: TexPage(%d,%d) fmt=%d trans=%d dither=%d flipX=%d flipY=%d\n",
                    tex_page_x, tex_page_y, tpf, trans_mode, dither_enable, tex_flip_x, tex_flip_y);
            fflush(gpu_debug_log);
        }
        Flush_GIF(); // Ensure ALPHA_1 and other settings reach GS immediately
    }
    break;
    case 0xE3: // Drawing Area Top-Left
        draw_clip_x1 = data & 0x3FF;
        draw_clip_y1 = (data >> 10) & 0x3FF;
        printf("[GPU] E3: Draw Area TL (%d,%d)\n", draw_clip_x1, draw_clip_y1);
        // Update SCISSOR (framebuffer space, no offset)
        {
            Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
            u64 scax0 = draw_clip_x1;
            u64 scax1 = draw_clip_x2;
            u64 scay0 = draw_clip_y1;
            u64 scay1 = draw_clip_y2;
            Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40); // SCISSOR_1
        }
        break;
    case 0xE4: // Drawing Area Bottom-Right
        draw_clip_x2 = data & 0x3FF;
        draw_clip_y2 = (data >> 10) & 0x3FF;
        printf("[GPU] E4: Draw Area BR (%d,%d)\n", draw_clip_x2, draw_clip_y2);
        // Update SCISSOR (framebuffer space, no offset)
        {
            Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
            u64 scax0 = draw_clip_x1;
            u64 scax1 = draw_clip_x2;
            u64 scay0 = draw_clip_y1;
            u64 scay1 = draw_clip_y2;
            Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40); // SCISSOR_1
        }
        break;
    case 0xE5: // Drawing Offset
        // printf("  [GPU] GP0: Draw Offset %08X\n", data);
        draw_offset_x = (s16)(data & 0x7FF);
        if (draw_offset_x & 0x400)
            draw_offset_x |= 0xF800;
        draw_offset_y = (s16)((data >> 11) & 0x7FF);
        if (draw_offset_y & 0x400)
            draw_offset_y |= 0xF800;

        // Don't update XYOFFSET (keep fixed at 2048,2048)
        // Draw offset is applied per-vertex in Translate_GP0_to_GS
        printf("[GPU] E5: Draw Offset = (%d, %d)\n", draw_offset_x, draw_offset_y);
        break;
    case 0xE2: // Texture Window Setting
        tex_win_mask_x = data & 0x1F;
        tex_win_mask_y = (data >> 5) & 0x1F;
        tex_win_off_x = (data >> 10) & 0x1F;
        tex_win_off_y = (data >> 15) & 0x1F;
        break;
    case 0xE6: // Mask Bit Setting
        // Bit 0: Set mask while drawing (1=set MSB)
        // Bit 1: Check mask before drawing (1=skip already-set pixels)
        break;
    case 0x00: // NOP
    case 0x01: // Clear Cache (NOP on real hardware too)
        break;
    default:
    {
        int size = GPU_GetCommandSize(cmd);
        if (size > 1)
        {
            gpu_cmd_buffer[0] = data;
            gpu_cmd_ptr = 1;
            gpu_cmd_remaining = size - 1;
        }
        else if (size == 1 && ((cmd & 0xE0) == 0x20 || cmd == 0x02))
        {
            // Single word primitive? Unlikely but safe.
            u32 buff[1] = {data};
            u128 *cursor = &gif_packet_buf[gif_packet_ptr];
            Translate_GP0_to_GS(buff, &cursor);
            gif_packet_ptr = cursor - gif_packet_buf;
            if (gif_packet_ptr > GIF_BUFFER_SIZE - 32)
                Flush_GIF();
        }
        break;
    }
    }
}

void GPU_WriteGP1(u32 data)
{
    u32 cmd = (data >> 24) & 0xFF;
    switch (cmd)
    {
    case 0x00: // Reset GPU
        gpu_stat = 0x14802000;
        draw_offset_x = 0;
        draw_offset_y = 0;
        draw_clip_x1 = 0;
        draw_clip_y1 = 0;
        draw_clip_x2 = 256;
        draw_clip_y2 = 240;
        gpu_cmd_remaining = 0;
        gpu_transfer_words = 0;
        break;
    case 0x01: // Reset Command Buffer
        gpu_cmd_remaining = 0;
        gpu_transfer_words = 0;
        break;
    case 0x02: // Ack IRQ
        gpu_stat &= ~0x01000000;
        break;
    case 0x03: // Display Enable
        if (data & 1)
            gpu_stat |= 0x00800000;
        else
            gpu_stat &= ~0x00800000;
        break;
    case 0x04: // DMA Direction
        gpu_stat = (gpu_stat & ~0x60000000) | ((data & 3) << 29);
        break;
    case 0x05: // Display Start
    {
        u32 x = data & 0x3FF;
        u32 y = (data >> 10) & 0x1FF;

        //                printf("[GPU] GP1(05) Display Start: %d, %d (Offset: %06X)\n", x, y, data & 0xFFFFFF);

        u64 dispfb = 0;
        dispfb |= (u64)0 << 0;            // FBP (Base 0)
        dispfb |= (u64)PSX_VRAM_FBW << 9; // FBW (1024 pixels)
        dispfb |= (u64)GS_PSM_16S << 15;  // PSM (CT16S - matches PSX 15-bit VRAM)
        dispfb |= (u64)x << 32;           // DBX
        dispfb |= (u64)y << 43;           // DBY

        *((volatile u64 *)0xB2000070) = dispfb; // DISPFB1
        *((volatile u64 *)0xB2000090) = dispfb; // DISPFB2
    }
    break;
    case 0x06: // Horizontal Display Range
               //            printf("[GPU] GP1(06) H Display Range: raw=0x%06X x1=%d x2=%d\n", data & 0xFFFFFF, data & 0xFFF, (data >> 12) & 0xFFF);
        break;
    case 0x07: // Vertical Display Range
               //            printf("[GPU] GP1(07) V Display Range: raw=0x%06X y1=%d y2=%d\n", data & 0xFFFFFF, data & 0x3FF, (data >> 10) & 0x3FF);
        break;
    case 0x08: // Display Mode
    {
        // Update gpu_stat bits
        gpu_stat = (gpu_stat & ~0x007F4000) |
                   ((data & 0x3F) << 17) | ((data & 0x40) << 10);

        // Log only on mode change
        static u32 last_display_mode = 0xFFFFFFFF;
        u32 mode_bits = data & 0x7F;
        if (mode_bits != last_display_mode)
        {
            last_display_mode = mode_bits;
            u32 hres = data & 3;
            u32 vres = (data >> 2) & 1;
            u32 pal = (data >> 3) & 1;
            u32 interlace = (data >> 5) & 1;
            int widths[] = {256, 320, 512, 640};
            printf("[GPU] GP1(08) Display Mode CHANGED: %dx%d %s %s\n",
                   widths[hres], vres ? 480 : 240,
                   pal ? "PAL" : "NTSC", interlace ? "Interlaced" : "Progressive");

            // Update GS display mode to match PSX
            int gs_mode = pal ? GRAPH_MODE_PAL : GRAPH_MODE_NTSC;
            int gs_interlace = interlace ? GRAPH_MODE_INTERLACED : GRAPH_MODE_NONINTERLACED;
            int gs_ffmd = 0; // Frame mode
            SetGsCrt(gs_interlace, gs_mode, gs_ffmd);
        }
        // PSX content renders to VRAM and is visible through the updated GS display window
    }
    break;
    case 0x10: // Get GPU Info
    {
        u32 info_type = data & 0x0F;
        switch (info_type)
        {
        case 2:
            gpu_read = 0;
            break; // Texture Window (not implemented)
        case 3:
            gpu_read = (draw_clip_y1 << 10) | draw_clip_x1;
            break;
        case 4:
            gpu_read = (draw_clip_y2 << 10) | draw_clip_x2;
            break;
        case 5:
            gpu_read = ((draw_offset_y & 0x7FF) << 11) | (draw_offset_x & 0x7FF);
            break;
        case 7:
            gpu_read = 2;
            break; // GPU Version
        default:
            gpu_read = 0;
            break;
        }
    }
    break;
    }
}

void GPU_Flush(void)
{
    //    printf("[GPU] GPU_Flush called\n");
    Flush_GIF();
}

void GPU_DMA2(u32 madr, u32 bcr, u32 chcr)
{
    u32 addr = madr & 0x1FFFFC;
    if ((chcr & 0x01000000) == 0)
        return;
    u32 sync_mode = (chcr >> 9) & 3;
    u32 direction = chcr & 1; // 0 = GPU->CPU, 1 = CPU->GPU

    // Flush any pending GIF data from direct GP0 writes before starting DMA
    Flush_GIF();

    // Sync Mode 0 (Continuous) and 1 (Block/Request): CPU -> GPU transfer
    if (sync_mode == 0 || sync_mode == 1)
    {
        if (direction == 1)
        { // CPU -> GPU
            u32 block_size = bcr & 0xFFFF;
            u32 block_count = (bcr >> 16) & 0xFFFF;
            u32 total_words;

            if (sync_mode == 0)
            {
                // Continuous mode: BCR = total words
                total_words = block_size;
                if (total_words == 0)
                    total_words = 0x10000;
            }
            else
            {
                // Block mode: BCR = block_size * block_count
                if (block_count == 0)
                    block_count = 0x10000;
                if (block_size == 0)
                    block_size = 0x10000;
                total_words = block_size * block_count;
            }

            //            printf("[GPU] DMA2 Block Transfer: %d words (mode=%d, bs=%d, bc=%d)\n",
            //                   total_words, sync_mode, block_size, block_count);

            for (u32 i = 0; i < total_words; i++)
            {
                u32 word = *(u32 *)(psx_ram + (addr & 0x1FFFFC));
                GPU_WriteGP0(word);
                addr += 4;
            }
        }
        else
        {
            // GPU -> CPU (VRAM Read)
            u32 block_size = bcr & 0xFFFF;
            u32 block_count = (bcr >> 16) & 0xFFFF;
            u32 total_words;

            if (sync_mode == 0)
            {
                total_words = block_size;
                if (total_words == 0)
                    total_words = 0x10000;
            }
            else
            {
                if (block_count == 0)
                    block_count = 0x10000;
                if (block_size == 0)
                    block_size = 0x10000;
                total_words = block_size * block_count;
            }

            printf("[GPU] DMA2 GPU->CPU Read: %d words\n", total_words);

            for (u32 i = 0; i < total_words; i++)
            {
                u32 word = GPU_Read();
                *(u32 *)(psx_ram + (addr & 0x1FFFFC)) = word;
                addr += 4;
            }
        }
        return;
    }

    if (sync_mode == 2)
    {
        int packets = 0;
        int max_packets = 20000;

        //        printf("[GPU] Start DMA2 Chain\n");
        fflush(stdout);

        while (packets < max_packets)
        {
            u32 packet_addr = addr;
            // Read List Header
            u32 header = GPU_GetWord(addr);
            u32 count = header >> 24;
            u32 next = header & 0xFFFFFF;

            if (count > 256)
            {
                printf("[GPU] ERROR: Packet count too large (%d). Aborting chain.\n", count);
                break;
            }

            // Process payload
            addr += 4;
            for (u32 i = 0; i < count;)
            {
                // If polyline is still active from previous command, continue feeding words
                if (polyline_active)
                {
                    u32 word = GPU_GetWord(addr);
                    GPU_WriteGP0(word);
                    i++;
                    addr += 4;
                    continue;
                }

                u32 cmd_word = GPU_GetWord(addr);
                u32 cmd_byte = cmd_word >> 24;

                // Line/polyline commands - route through GPU_WriteGP0 for polyline state machine
                if ((cmd_byte & 0xE0) == 0x40)
                {
                    Flush_GIF();
                    while (i < count)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                        if (gpu_cmd_remaining == 0 && gpu_transfer_words == 0 && !polyline_active)
                            break;
                    }
                }
                // Other render commands (polygons 0x20-0x3F, rectangles 0x60-0x7F)
                else if (cmd_byte >= 0x20 && cmd_byte <= 0x7F)
                {
                    // Get pointer to command in RAM
                    u32 *cmd_ptr = (u32 *)&psx_ram[addr];

                    // Translate directly to GIF buffer
                    // We need to pass address of current GIF pointer
                    u128 *cursor = &gif_packet_buf[gif_packet_ptr];
                    Translate_GP0_to_GS(cmd_ptr, &cursor);

                    // Update GIF pointer based on cursor movement
                    gif_packet_ptr = cursor - gif_packet_buf;

                    // Check buffer size and flush if needed
                    if (gif_packet_ptr > GIF_BUFFER_SIZE - 32)
                        Flush_GIF();

                    // Advance DMA
                    int size = GPU_GetCommandSize(cmd_word >> 24);
                    i += size;
                    addr += (size * 4);
                }
                else if ((cmd_word >> 24) == 0x02)
                {
                    // Fill Rect
                    u32 *cmd_ptr = (u32 *)&psx_ram[addr];

                    u128 *cursor = &gif_packet_buf[gif_packet_ptr];
                    Translate_GP0_to_GS(cmd_ptr, &cursor);
                    gif_packet_ptr = cursor - gif_packet_buf;
                    if (gif_packet_ptr > GIF_BUFFER_SIZE - 32)
                        Flush_GIF();

                    int size = 3;
                    i += size;
                    addr += (size * 4);
                }
                else if ((cmd_word >> 24) == 0xA0)
                {
                    // GP0(A0) - Copy CPU to VRAM
                    // Forward all words to GPU_WriteGP0 which handles A0 properly
                    // A0 needs: cmd word, XY word, WH word, then pixel data
                    // The pixel data may span multiple DMA nodes

                    // Flush pending GIF data before VRAM transfer
                    Flush_GIF();

                    // Send command word + params through GPU_WriteGP0
                    // First: A0 command
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;

                    // Then XY and WH words (GPU_WriteGP0 accumulates them)
                    while (i < count && gpu_cmd_remaining > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }

                    // Then pixel data (GPU_WriteGP0 handles transfer mode)
                    while (i < count && gpu_transfer_words > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }
                }
                else if (((cmd_word >> 24) & 0xE0) == 0x80)
                {
                    // GP0(80h) - VRAM-to-VRAM copy (4 words)
                    // Flush pending GIF data before transfer
                    Flush_GIF();

                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;
                    while (i < count && gpu_cmd_remaining > 0)
                    {
                        GPU_WriteGP0(GPU_GetWord(addr));
                        i++;
                        addr += 4;
                    }
                }
                else
                {
                    // Immediate command in list?
                    GPU_WriteGP0(cmd_word);
                    i++;
                    addr += 4;
                }
            }

            packets++;

            // Check for end of list
            if (next == 0xFFFFFF)
            {
                // printf("[GPU] End of Linked List (Terminator)\n");
                break;
            }

            // Check for self-reference (Infinite Loop Prevention)
            if (next == packet_addr)
            {
                printf("[GPU] Warning: Linked List Self-Reference %06X. Breaking chain to allow CPU operation.\n", next);
                break;
            }

            // Safety check for next address (alignment)
            if (next & 0x3)
            {
                printf("[GPU] ERROR: Unaligned next pointer %06X\n", next);
                break;
            }

            addr = next & 0x1FFFFC;
        }

        Flush_GIF();
        //        printf("[GPU] End DMA2 Chain (%d packets processed)\n", packets);
    }
}

void Init_Graphics()
{
    printf("Initializing Graphics (GS)...\n");

    // Uncomment for GPU debug logging:
    // gpu_debug_log = fopen("host:gpu_debug.log", "w");
    // if (gpu_debug_log)
    //     fprintf(gpu_debug_log, "GPU Debug Log\n");

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // Initialize graphics like libdraw does
    graph_initialize(fb_address, fb_width, fb_height, fb_psm, 0, 0);

    // Override DISPFB to use PSX VRAM width (1024) instead of display width (640)
    // This ensures the display reads from the same layout that FRAME_1 writes to
    {
        u64 dispfb = 0;
        dispfb |= (u64)0 << 0;                  // FBP (Base 0)
        dispfb |= (u64)PSX_VRAM_FBW << 9;       // FBW (1024 pixels)
        dispfb |= (u64)GS_PSM_16S << 15;        // PSM (CT16S - matches PSX 15-bit VRAM)
        dispfb |= (u64)0 << 32;                 // DBX
        dispfb |= (u64)0 << 43;                 // DBY
        *((volatile u64 *)0xB2000070) = dispfb; // DISPFB1
        *((volatile u64 *)0xB2000090) = dispfb; // DISPFB2
    }

    // Setup GS environment for rendering
    Setup_GS_Environment();

    printf("Graphics Initialized. GS rendering state set.\n");
}

#include <stdlib.h>
#include <malloc.h>

// Standard PS2 Hardware Registers
#define GS_CSR ((volatile u64 *)0x12001000)

// DMA Channel 1 (VIF1) registers - PCSX2 routes GS readback through VIF1
#define D1_CHCR ((volatile u32 *)0x10009000)
#define D1_MADR ((volatile u32 *)0x10009010)
#define D1_QWC ((volatile u32 *)0x10009020)

void DumpVRAM(const char *filename)
{
#ifdef ENABLE_VRAM_DUMP
    printf("[DumpVRAM] Dumping VRAM to %s...\n", filename);
#endif

    // 1. Finish any pending rendering
    Flush_GIF();

    // 2. Prepare transfer size (CT16S = 2 bytes per pixel)
    int width = 1024;
    int height = 512;
    int size_bytes = width * height * 2; // 16-bit color (CT16S)
    int qwc = size_bytes / 16;

    // Allocate buffer (aligned to 64 bytes)
    void *buf = memalign(64, size_bytes);
    if (!buf)
    {
#ifdef ENABLE_VRAM_DUMP
        printf("[DumpVRAM] Failed to allocate %d bytes\n", size_bytes);
#endif
        return;
    }

    // 3. Setup GS for StoreImage via GIF A+D packet
    u64 bitbltbuf = ((u64)0 << 0) | ((u64)16 << 16) | ((u64)GS_PSM_16S << 24);
    u64 trxpos = 0;
    u64 trxreg = ((u64)1024 << 0) | ((u64)512 << 32);
    u64 trxdir = 1; // Local -> Host

    u128 packet[8] __attribute__((aligned(16)));
    GifTag *tag = (GifTag *)packet;
    tag->NLOOP = 4;
    tag->EOP = 1;
    tag->pad1 = 0;
    tag->PRE = 0;
    tag->PRIM = 0;
    tag->FLG = 0; // PACKED (A+D)
    tag->NREG = 1;
    tag->REGS = 0xE; // A+D

    u64 *ptr = (u64 *)&packet[1];
    *ptr++ = bitbltbuf;
    *ptr++ = 0x50; // BITBLTBUF
    *ptr++ = trxpos;
    *ptr++ = 0x51; // TRXPOS
    *ptr++ = trxreg;
    *ptr++ = 0x52; // TRXREG
    *ptr++ = trxdir;
    *ptr++ = 0x53; // TRXDIR

    // 4. Send setup to GS then receive via VIF1/DMA Ch1
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet, 5, 0, 0);
    dma_wait_fast();

    // 5. Receive data via VIF1 DMA (PCSX2-specific readback path)
    u32 phys_addr = (u32)buf & 0x1FFFFFFF;
    u32 remaining_qwc = qwc;
    u32 current_addr = phys_addr;

    while (remaining_qwc > 0)
    {
        u32 transfer_qwc = (remaining_qwc > 0xFFFF) ? 0xFFFF : remaining_qwc;

        *D1_MADR = current_addr;
        *D1_QWC = transfer_qwc;
        *D1_CHCR = 0x100; // STR=1, DIR=0 (device→memory), MODE=0

        // Wait for completion
        while (*D1_CHCR & 0x100)
            ;

        current_addr += transfer_qwc * 16;
        remaining_qwc -= transfer_qwc;
    }

    // 6. Read via uncached access and save to file
    u8 *uncached_buf = (u8 *)(phys_addr | 0xA0000000);

#ifdef ENABLE_VRAM_DUMP
    u16 *p = (u16 *)uncached_buf;
    printf("[DumpVRAM] First pixel: %04X\n", p[0]);
    printf("[DumpVRAM] Center pixel: %04X\n", p[(512 * 1024 / 2) + 512]);
    fflush(stdout);
#endif

    FILE *f = fopen(filename, "wb");
    if (f)
    {
        fwrite(uncached_buf, 1, size_bytes, f);
        fclose(f);
#ifdef ENABLE_VRAM_DUMP
        printf("[DumpVRAM] Saved %d bytes to %s\n", size_bytes, filename);
        fflush(stdout);
#endif
    }
    else
    {
#ifdef ENABLE_VRAM_DUMP
        printf("[DumpVRAM] Error opening file %s\n", filename);
        fflush(stdout);
#endif
    }

    free(buf);
}