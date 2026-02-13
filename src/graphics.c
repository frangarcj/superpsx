#include "superpsx.h"
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <stdio.h>

// Global context for libgraph
// Assuming simple single buffered for now to just clear screen.


// -- GPU State --
static u32 gpu_stat = 0x14802000;
static u32 gpu_read = 0;

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

// -- GIF Tag --
typedef struct {
    u64 NLOOP : 15;
    u64 EOP   : 1;
    u64 pad1  : 30;
    u64 PRE   : 1;
    u64 PRIM  : 11;
    u64 FLG   : 2;
    u64 NREG  : 4;
    u64 REGS;
} GifTag __attribute__((aligned(16)));

// -- DMA Packet --
typedef struct {
    GifTag tag;
    u64 data[2]; // Variable length normally
} GSPacket;

// -- Helper Functions --

static void Flush_GIF(void) {
    if (gif_packet_ptr > 0) {
        // Send to GIF (Channel 2)
        dma_channel_send_normal(DMA_CHANNEL_GIF, gif_packet_buf, gif_packet_ptr, 0, 0);
        dma_wait_fast(); // Wait for completion for now to be safe
        gif_packet_ptr = 0;
    }
}

static void Push_GIF_Tag(u64 nloop, u64 eop, u64 pre, u64 prim, u64 flg, u64 nreg, u64 regs) {
    if (gif_packet_ptr + 1 >= GIF_BUFFER_SIZE) Flush_GIF();
    
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

static void Push_GIF_Data(u64 d0, u64 d1) {
    if (gif_packet_ptr + 1 >= GIF_BUFFER_SIZE) Flush_GIF();
    
    u64 *p = (u64 *)&gif_packet_buf[gif_packet_ptr];
    p[0] = d0;
    p[1] = d1;
    
    gif_packet_ptr++;
}


static u32 GPU_GetWord(u32 addr) {
    addr &= 0x1FFFFC; 
    return *(u32 *)&psx_ram[addr];
}

// Translate PSX coordinates to GS coordinates (12.4 fixed point)
// GS XYZ2 register:
// 0-15: X
// 16-31: Y
// 32-63: Z
// Standard GS center offset is 2048.0 (32768 in 12.4)
static u64 Wrap_Coord(s16 x, s16 y) {
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
    
    s32 gx = (s32)(x + draw_offset_x + 2048) << 4;
    s32 gy = (s32)(y + draw_offset_y + 2048) << 4;
    
    // Clamp to valid GS range (though GS usually handles this via scissor)
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    
    u32 z = 0;
    
    return (u64)z << 32 | (u64)(gy & 0xFFFF) << 16 | (u64)(gx & 0xFFFF);
}

// Helper to bundle color (RGBA8)
static u64 Wrap_Color(u32 bgr) {
    u64 r = bgr & 0xFF;
    u64 g = (bgr >> 8) & 0xFF;
    u64 b = (bgr >> 16) & 0xFF;
    u64 a = 0x80; // Default alpha (not transparent)
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void Setup_GS_Rendering(void) {
    // Set up FRAME, XYOFFSET, SCISSOR
    
    // NLOOP=6, EOP=1, PRE=0, PRIM=0, FLG=PACKED, NREG=1, REGS=AD(0xE)
    Push_GIF_Tag(6, 1, 0, 0, 0, 1, 0xE);
    
    // FRAME_1 (Reg 0x4C)
    // FBP=0 (Base 0), FBW=10 (640px), PSM=0 (CT32)
    Push_GIF_Data(0x000000000000000A, 0x4C); 
    
    // XYOFFSET_1 (Reg 0x18)
    // Set to center (2048, 2048) in 12.4 format
    u64 center = 2048 << 4;
    Push_GIF_Data(center | (center << 32), 0x18);
    
    // SCISSOR_1 (Reg 0x40)
    // Full screen 640x480
    u64 scax0 = 0, scax1 = 639, scay0 = 0, scay1 = 479;
    Push_GIF_Data(scax0 | (scax1 << 16) | (scay0 << 32) | (scay1 << 48), 0x40);
    
    // PRMODECONT (Reg 0x1A)
    Push_GIF_Data(1, 0x1A);
    
    // COLCLAMP (Reg 0x46)
    Push_GIF_Data(1, 0x46);
    
    // DTHE (Reg 0x45) - Dithering on?
    Push_GIF_Data(0, 0x45);

    Flush_GIF();
}

// -- Implementation --

u32 GPU_Read(void) {
    return gpu_read;
}

u32 GPU_ReadStatus(void) {
    // Force Bit 27 (Ready to Send DMA) to 1 to unblock BIOS
    return gpu_stat | 0x1C802000;
}

void GPU_VBlank(void) {
    gpu_stat ^= 0x80000000;
} 

// -- Immediate Mode State --
static int gpu_cmd_remaining = 0;
static u32 gpu_cmd_buffer[16];
static int gpu_cmd_ptr = 0;
static int gpu_transfer_words = 0;
static int gpu_transfer_total = 0;

static void Start_VRAM_Transfer(int x, int y, int w, int h) {
    // 1. Set BITBLTBUF (Buffer Address)
    // DBP=0 (Base 0), DBW=10 (640px), PSM=0 (CT32)
    // 2. Set TRXPOS (Dst X,Y)
    // 3. Set TRXREG (W, H)
    // 4. Set TRXDIR (0 = Host -> Local)
    
    // Using simple GIF tags to set registers
    Push_GIF_Tag(4, 1, 0, 0, 0, 1, 0xE); // NLOOP=4, EOP=1, A+D
    
    // BITBLTBUF (0x50): Base=0, Width=10(640), PSM=0
    Push_GIF_Data(0x0000000000000A00, 0x50);
    
    // TRXPOS (0x51): SSAX=0, SSAY=0, DSAX=x, DSAY=y, DIR=0
    Push_GIF_Data(((u64)y << 32) | ((u64)x << 16), 0x51);
    
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

static u128 buf_image[1024];
static int buf_image_ptr = 0;

static void Flush_Image_Buffer(void) {
    if (buf_image_ptr > 0) {
        // Send IMAGE data
        // NLOOP = buf_image_ptr, EOP=1?? No, only EOP on last?
        // Actually, logic is: send packet with EOP if it's the last one?
        // Or can we send multiple packets?
        // GIF expects 'w*h' pixels total.
        // As long as we don't set EOP until the very end?
        // But dma_channel_send_normal expects EOP to terminate transfer?
        // Wait. dma_channel_wait waits for completion.
        
        // We will treat each batch as a separate GIF packet.
        // But for IMAGE mode, does registers state persist?
        // Yes, TRX* registers persist.
        // We just need to issue a new GIF tag with IMAGE primitive?
        // "HWREG" (0x54) ?? No.
        // To send image data, we use standard GIF packed mode?
        // No, usually "IMAGE" mode (2) or "PACKED" (0) with HWREG?
        // The standard way is sending data to HWREG (0x54) port.
        
        // Let's use PACKED mode (0) addressing register 0x54 (HWREG).
        
        Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 0, 1, 0x54); // NLOOP=ptr, EOP=1, Reg=HWREG
        
        // Copy buffer to global GIF buffer?
        // Push_GIF_Tag uses 'gif_packet_buf'.
        // We can just append our data there.
        
        for (int i=0; i<buf_image_ptr; i++) {
             Push_GIF_Data((u64)buf_image[i], 0); // Only lower 64 bits?
             // HWREG is 64-bit input? Or 32?
             // CT32 pixels: 32 bits.
             // GIF sends 128-bit Qwords.
             // Each Qword contains 4 pixels (32-bit each)?
             // Or 2 pixels (64-bit)?
             // PSX sends 32-bit words (2 pixels 1555).
             // CT32 expects 32-bit RGBA.
             // We need to convert 1555 -> 8888.
        }
        
        buf_image_ptr = 0;
    }
}

// Convert 2 PSX pixels (32-bit word) to 2 GS pixels (2x 32-bit RGBA)
// Returns 64-bit (2 pixels)
static u64 Convert_Pixels(u32 data) {
    u16 p0 = data & 0xFFFF;
    u16 p1 = data >> 16;
    
    u32 rgba0 = (p0 & 0x1F) << 3 | ((p0 >> 5) & 0x1F) << 11 | ((p0 >> 10) & 0x1F) << 19 | 0x80000000;
    u32 rgba1 = (p1 & 0x1F) << 3 | ((p1 >> 5) & 0x1F) << 11 | ((p1 >> 10) & 0x1F) << 19 | 0x80000000;
    
    return (u64)rgba0 | ((u64)rgba1 << 32);
}

void GPU_WriteGP0(u32 data) {
    // If transferring data (A0/C0)
    if (gpu_transfer_words > 0) {
        
        // Convert to GS format and buffer
        // 'data' has 2 pixels.
        // GS expects 32-bit pixels (CT32).
        // Each input word = 2 GS pixels = 64 bits.
        // GIF packet data is 128-bit (2 Qwords? No 1 Qword).
        // 1 Qword = 128 bits = 4 GS pixels = 2 input words.
        
        // We buffer individual CONVERTED 64-bit chunks?
        // Let's store 128-bit Qwords in buf_image.
        
        static u64 pending_qword = 0;
        static int pending_has_data = 0;
        
        u64 pixels_64 = Convert_Pixels(data);
        
        if (!pending_has_data) {
            pending_qword = pixels_64;
            pending_has_data = 1;
        } else {
            // Combine with pending
            u128 q = (u128)pending_qword | ((u128)pixels_64 << 64);
            buf_image[buf_image_ptr++] = q;
            pending_has_data = 0;
            
            if (buf_image_ptr >= 1000) {
                // Flush to GIF
                // Note: Manually handle tag because Flush_Image_Buffer logic above was pseudo.
                Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 0, 1, 0x54); // HWREG
                for (int i=0; i<buf_image_ptr; i++) {
                     // We need a version of Push_GIF_Data that takes u128
                     // Hack: Cast to u64*
                     u64* p = (u64*)&buf_image[i];
                     Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
        }

        gpu_transfer_words--;
        if (gpu_transfer_words == 0) {
             // End of transfer. Flush remaining.
             if (pending_has_data) {
                 u128 q = (u128)pending_qword; // Pad with 0?
                 buf_image[buf_image_ptr++] = q;
             }
             if (buf_image_ptr > 0) {
                Push_GIF_Tag(buf_image_ptr, 1, 0, 0, 0, 1, 0x54);
                for (int i=0; i<buf_image_ptr; i++) {
                     u64* p = (u64*)&buf_image[i];
                     Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
             }
             Flush_GIF();
        }
        return;
    }

    if (gpu_cmd_remaining > 0) {
        gpu_cmd_buffer[gpu_cmd_ptr++] = data;
        gpu_cmd_remaining--;
        if (gpu_cmd_remaining == 0) {
            // Process accumulated command
            u32 cmd = gpu_cmd_buffer[0] >> 24;
            if (cmd == 0xA0) {
                // Load Image
                u32 wh = gpu_cmd_buffer[2];
                u32 w = wh & 0xFFFF;
                u32 h = wh >> 16;
                if (w == 0) w = 1024;
                if (h == 0) h = 512;
                gpu_transfer_words = (w * h + 1) / 2;
                gpu_transfer_total = gpu_transfer_words;
                printf("[GPU] GP0(A0) Start Transfer: %dx%d (%d words)\n", w, h, gpu_transfer_words);
                
                // Init GS Transfer
                u32 xy = gpu_cmd_buffer[1];
                Start_VRAM_Transfer(xy & 0xFFFF, xy >> 16, w, h);
            }
        }
        return;
    }

    u32 cmd = (data >> 24) & 0xFF;
    
    // Command A0 needs 2 extra parameters, then data
    if (cmd == 0xA0) {
        gpu_cmd_buffer[0] = data;
        gpu_cmd_ptr = 1;
        gpu_cmd_remaining = 2; // YX, WH
        return;
    }

    switch(cmd) {
        case 0xE1: // Draw Mode
            printf("  [GPU] GP0: Draw Mode %08X\n", data);
            break;
        case 0xE3: // Drawing Area Top-Left
            printf("  [GPU] GP0: Draw Area TL %08X\n", data);
            draw_clip_x1 = data & 0x3FF;
            draw_clip_y1 = (data >> 10) & 0x3FF;
            // Update SCISSOR
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
            printf("  [GPU] GP0: Draw Area BR %08X\n", data);
            draw_clip_x2 = data & 0x3FF;
            draw_clip_y2 = (data >> 10) & 0x3FF;
            // Update SCISSOR
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
            printf("  [GPU] GP0: Draw Offset %08X\n", data);
            draw_offset_x = (s16)(data & 0x7FF); 
            if (draw_offset_x & 0x400) draw_offset_x |= 0xF800;
            draw_offset_y = (s16)((data >> 11) & 0x7FF);
            if (draw_offset_y & 0x400) draw_offset_y |= 0xF800;
            
            // Update XYOFFSET
            {
                 Push_GIF_Tag(1, 1, 0, 0, 0, 1, 0xE);
                 u64 ofx = (draw_offset_x + 2048) << 4;
                 u64 ofy = (draw_offset_y + 2048) << 4;
                 Push_GIF_Data(ofx | (ofy << 32), 0x18); // XYOFFSET_1
            }
            break;
    }
}

void GPU_WriteGP1(u32 data) {
    u32 cmd = (data >> 24) & 0xFF;
    switch (cmd) {
        case 0x00: gpu_stat = 0x14802000; break;
        case 0x01: break; 
        case 0x03:
            if (data & 1) gpu_stat |= 0x00800000;
            else gpu_stat &= ~0x00800000;
            break;
        case 0x04: gpu_stat = (gpu_stat & ~0x60000000) | ((data & 3) << 29); break;
        case 0x10: gpu_read = 2; break;
    }
}

// Processing a single primitve from the list
static int GPU_ProcessPrimitive(u32 addr, u32 cmd_word) {
    u32 cmd = (cmd_word >> 24) & 0xFF;
    
    // Polygon?
    if ((cmd & 0xE0) == 0x20) {
        int is_quad = (cmd & 0x08) != 0;
        int is_shaded = (cmd & 0x10) != 0;
        int is_textured = (cmd & 0x04) != 0;
        
        int num_verts = is_quad ? 4 : 3;
        int prim_type = is_quad ? 4 : 3; 
        
        Push_GIF_Tag(num_verts, 1, 0, prim_type, 0, 2, 0x51);

        u32 current_addr = addr;
        u32 color = cmd_word & 0xFFFFFF; 
        
        // V0
        s16 x0 = ReadHalf(current_addr + 4);
        s16 y0 = ReadHalf(current_addr + 6);
        
        printf("    [GPU] Poly Color=%06X V0=(%d, %d)\n", color, x0, y0);
        fflush(stdout);

        current_addr += 8;
        if (is_textured) current_addr += 4; // Skip UV0/CLUT
        
        Push_GIF_Data(Wrap_Color(color), 0);
        Push_GIF_Data(Wrap_Coord(x0, y0), 0);
        
        // V1
        if (is_shaded) color = GPU_GetWord(current_addr);
        if (is_shaded) current_addr += 4;
        
        s16 x1 = ReadHalf(current_addr);
        s16 y1 = ReadHalf(current_addr + 2);
        current_addr += 4;
        if (is_textured) current_addr += 4; // Skip UV1/Page
        
        Push_GIF_Data(Wrap_Color(color), 0);
        Push_GIF_Data(Wrap_Coord(x1, y1), 0); 
        
        // V2
        if (is_shaded) color = GPU_GetWord(current_addr);
        if (is_shaded) current_addr += 4;
        
        s16 x2 = ReadHalf(current_addr);
        s16 y2 = ReadHalf(current_addr + 2);
        current_addr += 4;
        if (is_textured) current_addr += 4; // Skip UV2

        Push_GIF_Data(Wrap_Color(color), 0);
        Push_GIF_Data(Wrap_Coord(x2, y2), 0);

        if (is_quad) {
             // V3
            if (is_shaded) color = GPU_GetWord(current_addr);
            if (is_shaded) current_addr += 4;
            
            s16 x3 = ReadHalf(current_addr);
            s16 y3 = ReadHalf(current_addr + 2);
            current_addr += 4;
            if (is_textured) current_addr += 4; // Skip UV3
            
            Push_GIF_Data(Wrap_Color(color), 0);
            Push_GIF_Data(Wrap_Coord(x3, y3), 0);
        }
        
        return (current_addr - addr) / 4;
    } else if (cmd == 0x02) {
        // Fill Rectangle (VRAM Clear)
        u32 color = cmd_word & 0xFFFFFF;
        u32 x = ReadHalf(addr + 4);
        u32 y = ReadHalf(addr + 6);
        u32 w = ReadHalf(addr + 8);
        u32 h = ReadHalf(addr + 10);
        printf("    [GPU] Fill Rect Color=%06X XY=(%d,%d) WH=(%d,%d)\n", color, x, y, w, h);
        return 3; // 3 words
    }
    
    printf("    [GPU] Unknown/Unhandled Primitive Cmd=%02X\n", cmd);
    return 1;
}

void GPU_DMA2(u32 madr, u32 bcr, u32 chcr) {
    u32 addr = madr & 0x1FFFFC;
    if ((chcr & 0x01000000) == 0) return; 
    u32 sync_mode = (chcr >> 9) & 3;
    
    gif_packet_ptr = 0;

    if (sync_mode == 2) {
        int packets = 0;
        int max_packets = 20000; 
        
        printf("[GPU] Start DMA2 Chain\n");

        while (packets < max_packets) {
            // Read List Header
            u32 header = GPU_GetWord(addr);
            u32 count = header >> 24;
            u32 next = header & 0xFFFFFF;
            
            printf("[GPU] DMA2 Packet #%d addr=%06X header=%08X (cnt=%d next=%06X)\n", 
                   packets, addr, header, count, next);
            fflush(stdout);

            if (count > 256) {
                printf("[GPU] ERROR: Packet count too large (%d). Aborting chain.\n", count);
                break;
            }

            // Process payload
            addr += 4; 
            for (u32 i = 0; i < count; ) {
                u32 cmd_word = GPU_GetWord(addr);
                
                // If it's a render command, translate it
                if (((cmd_word >> 24) >= 0x20 && (cmd_word >> 24) <= 0x7F) || (cmd_word >> 24) == 0x02) {
                     int consumed = GPU_ProcessPrimitive(addr, cmd_word);
                     // Safety check
                     if (consumed <= 0) {
                         printf("[GPU] Error: ProcessPrimitive returned %d. Stalled.\n", consumed);
                         i++; addr += 4;
                     } else {
                         i += consumed;
                         addr += (consumed * 4);
                     }
                } else if ((cmd_word >> 24) == 0xA0) {
                     // GP0(A0) - Copy CPU to VRAM
                     // Word 0: A0xxxxxx
                     // Word 1: Y(16) | X(16) (Dest Coord)
                     // Word 2: H(16) | W(16) (Size)
                     // Data follows...
                     if (i + 3 <= count) {
                         u32 xy = GPU_GetWord(addr + 4);
                         u32 wh = GPU_GetWord(addr + 8);
                         u32 w = wh & 0xFFFF;
                         u32 h = wh >> 16;
                         if (w == 0) w = 1024; // Does 0 mean max?
                         if (h == 0) h = 512;
                         
                         int words = (w * h + 1) / 2; // 2 pixels per word? No, 16-bit pixels.
                         // PSX VRAM is 16-bit. CPU sends 32-bit words.
                         // Each 32-bit word contains 2 pixels.
                         
                         printf("[GPU] CPU->VRAM Copy (A0). Rect=(%d,%d %dx%d). Size=%d words\n", 
                                xy & 0xFFFF, xy >> 16, w, h, words);
                         
                         // Here we should actually upload data to GS texture or VRAM.
                         // For now, we just CONSUME the data to keep stream sync.
                         
                         int packet_size = 3 + words;
                         i += packet_size;
                         addr += (packet_size * 4);
                     } else {
                         printf("[GPU] Error: A0 command truncated in DMA buffer.\n");
                         i++; addr += 4; // Skip to avoid hang
                     }
                } else {
                     // Immediate command in list?
                     GPU_WriteGP0(cmd_word);
                     i++;
                     addr += 4;
                }
            }
            
            packets++;
            
            // Check for end of list
            if (next == 0xFFFFFF) {
                printf("[GPU] End of Linked List (Terminator)\n");
                break;
            }
            
            // Check for self-reference (Infinite Loop Prevention)
            if (next == addr) {
                printf("[GPU] Warning: Linked List Self-Reference %06X. Breaking chain to allow CPU operation.\n", next);
                break;
            }
            
            // Safety check for next address (alignment)
            if (next & 0x3) {
                 printf("[GPU] ERROR: Unaligned next pointer %06X\n", next);
                 break;
            }
            
            addr = next & 0x1FFFFC;
        }
        
        Flush_GIF();
        printf("[GPU] End DMA2 Chain (%d packets)\n", packets);
    }
}

void Init_Graphics() {
    printf("Initializing Graphics (GS)...\n");
    
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
    graph_vram_clear();
    
    Setup_GS_Rendering();
    
    // DEBUG: Clear to Red to verify output
    {
        // PRIM=6 (Sprite), PRE=0, FLG=0, NREG=4 (RGBAQ, XYZ2)
        // NLOOP=2 (2 vertices for sprite), EOP=1
        Push_GIF_Tag(2, 1, 0, 6, 0, 2, 0x51);
        Push_GIF_Data(0x000000FF, 0); // Red
        Push_GIF_Data(0, 0); // (0,0)
        Push_GIF_Data(0x000000FF, 0); // Red
        Push_GIF_Data(Wrap_Coord(640, 480), 0); // (640,480)
        Flush_GIF();
        graph_wait_vsync();
    }
    
    graph_wait_vsync();
    
    printf("Graphics Initialized. GS rendering state set.\n");
}
