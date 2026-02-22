/**
 * gpu_vram.c — VRAM transfer operations
 *
 * HOST→LOCAL / LOCAL→HOST transfers, shadow VRAM upload helpers,
 * readback for CLUT decode, and the full-VRAM dump used by tests.
 */
#include "gpu_state.h"

/* ── Start a HOST→LOCAL VRAM transfer ────────────────────────────── */

void Start_VRAM_Transfer(int x, int y, int w, int h)
{
    // Using simple GIF tags to set registers
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), 0xE); // NLOOP=4, EOP=1, A+D

    // BITBLTBUF (0x50): DBP=0 (Base 0), DBW=16 (1024px), DPSM=CT16S
    Push_GIF_Data(((uint64_t)GS_PSM_16S << 56) | ((uint64_t)PSX_VRAM_FBW << 48), 0x50);

    // TRXPOS (0x51): SSAX=0, SSAY=0, DSAX=x, DSAY=y, DIR=0
    Push_GIF_Data(((uint64_t)y << 48) | ((uint64_t)x << 32), 0x51);

    // TRXREG (0x52): RRW=w, RRH=h
    Push_GIF_Data(((uint64_t)h << 32) | (uint64_t)w, 0x52);

    // TRXDIR (0x53): XDIR=0 (Host -> Local)
    Push_GIF_Data(0, 0x53);
}

/* ── Upload a region from shadow VRAM to GS VRAM ────────────────── */

void Upload_Shadow_VRAM_Region(int x, int y, int w, int h)
{
    if (!psx_vram_shadow || w <= 0 || h <= 0)
        return;

    // Set up GS IMAGE transfer for the region
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), 0xE);
    Push_GIF_Data(((uint64_t)GS_PSM_16S << 56) | ((uint64_t)PSX_VRAM_FBW << 48), 0x50);
    Push_GIF_Data(((uint64_t)y << 48) | ((uint64_t)x << 32), 0x51);
    Push_GIF_Data(((uint64_t)h << 32) | (uint64_t)w, 0x52);
    Push_GIF_Data(0, 0x53); // Host -> Local

    // Send pixel data from shadow VRAM
    for (int row = 0; row < h; row++)
    {
        int sy = y + row;
        if (sy >= 512)
            sy -= 512;
        uint32_t pending[4];
        int pc = 0;

        for (uint32_t col = 0; col < (uint32_t)w; col += 2)
        {
            int sx = x + col;
            if (sx >= 1024)
                sx -= 1024;
            uint16_t p0 = psx_vram_shadow[sy * 1024 + sx];
            uint16_t p1 = (col + 1 < (uint32_t)w) ? psx_vram_shadow[sy * 1024 + ((sx + 1) & 0x3FF)] : 0;
            // Set STP bit for non-zero pixels → GS alpha=0x80 for opaque, 0x00 for transparent
            // Only 0x0000 is transparent; 0x8000 (black + STP=1) is opaque
            if (p0 != 0)
                p0 |= 0x8000;
            if (p1 != 0)
                p1 |= 0x8000;
            pending[pc++] = (uint32_t)p0 | ((uint32_t)p1 << 16);

            if (pc >= 4)
            {
                uint64_t lo = (uint64_t)pending[0] | ((uint64_t)pending[1] << 32);
                uint64_t hi = (uint64_t)pending[2] | ((uint64_t)pending[3] << 32);
                unsigned __int128 q = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
                buf_image[buf_image_ptr++] = q;
                pc = 0;

                if (buf_image_ptr >= 1000)
                {
                    Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
                    for (int i = 0; i < buf_image_ptr; i++)
                    {
                        uint64_t *pp = (uint64_t *)&buf_image[i];
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
            uint64_t lo = (uint64_t)pending[0] | ((uint64_t)pending[1] << 32);
            uint64_t hi = (uint64_t)pending[2] | ((uint64_t)pending[3] << 32);
            unsigned __int128 q = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
            buf_image[buf_image_ptr++] = q;
            pc = 0;
        }
    }

    // Final flush
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
        for (int i = 0; i < buf_image_ptr; i++)
        {
            uint64_t *pp = (uint64_t *)&buf_image[i];
            Push_GIF_Data(pp[0], pp[1]);
        }
        buf_image_ptr = 0;
    }
    Flush_GIF();
}

/* ── Read back a rectangular region from GS VRAM ─────────────────── */

uint16_t *GS_ReadbackRegion(int x, int y, int w_aligned, int h, void *buf, int buf_qwc)
{
    Flush_GIF();

    unsigned __int128 rb_packet[8] __attribute__((aligned(16)));
    uint64_t *rp = (uint64_t *)rb_packet;
    // GIF tag: NLOOP=4, EOP=1, FLG=0(PACKED), NREG=1
    rp[0] = 4 | ((uint64_t)1 << 15) | ((uint64_t)0 << 58) | ((uint64_t)1 << 60);
    rp[1] = 0xE; // REGS = A+D
    rp[2] = ((uint64_t)PSX_VRAM_FBW << 16) | ((uint64_t)GS_PSM_16S << 24);
    rp[3] = 0x50; // BITBLTBUF (source only)
    rp[4] = (uint64_t)x | ((uint64_t)y << 16);
    rp[5] = 0x51; // TRXPOS
    rp[6] = (uint64_t)w_aligned | ((uint64_t)h << 32);
    rp[7] = 0x52; // TRXREG
    rp[8] = 1;    // TRXDIR = Local→Host
    rp[9] = 0x53;

    dma_channel_send_normal(DMA_CHANNEL_GIF, rb_packet, 5, 0, 0);
    dma_wait_fast();

    // Receive via VIF1 DMA
    uint32_t phys = (uint32_t)buf & 0x1FFFFFFF;
    uint32_t rem = buf_qwc;
    uint32_t addr = phys;
    while (rem > 0)
    {
        uint32_t xfer = (rem > 0xFFFF) ? 0xFFFF : rem;
        *D1_MADR = addr;
        *D1_QWC = xfer;
        *D1_CHCR = 0x100;
        while (*D1_CHCR & 0x100)
            ;
        addr += xfer * 16;
        rem -= xfer;
    }

    return (uint16_t *)((uint32_t)buf | 0xA0000000);
}

/* ── Upload decoded 16-bit pixels to GS VRAM as an IMAGE transfer ── */

void GS_UploadRegion(int x, int y, int w, int h, const uint16_t *pixels)
{
    // Set up BITBLTBUF, TRXPOS, TRXREG, TRXDIR for Host→Local
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), 0xE);
    Push_GIF_Data(((uint64_t)GS_PSM_16S << 56) | ((uint64_t)PSX_VRAM_FBW << 48), 0x50);
    Push_GIF_Data(((uint64_t)y << 48) | ((uint64_t)x << 32), 0x51);
    Push_GIF_Data(((uint64_t)h << 32) | (uint64_t)w, 0x52);
    Push_GIF_Data(0, 0x53); // Host → Local

    // Pack pixels into IMAGE transfer qwords
    buf_image_ptr = 0;
    uint32_t pend[4];
    int pc = 0;
    int total = w * h;
    for (int i = 0; i < total; i += 2)
    {
        uint16_t p0 = pixels[i];
        uint16_t p1 = (i + 1 < total) ? pixels[i + 1] : 0;
        // Set STP bit for non-zero pixels → GS alpha=0x80 for opaque, 0x00 for transparent
        // Only 0x0000 is transparent; 0x8000 (black + STP=1) is opaque
        if (p0 != 0)
            p0 |= 0x8000;
        if (p1 != 0)
            p1 |= 0x8000;
        pend[pc++] = (uint32_t)p0 | ((uint32_t)p1 << 16);
        if (pc >= 4)
        {
            uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
            uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
            buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
            pc = 0;
            if (buf_image_ptr >= 1000)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 0, 0, 0, 2, 0), 0);
                for (int j = 0; j < buf_image_ptr; j++)
                {
                    uint64_t *pp = (uint64_t *)&buf_image[j];
                    Push_GIF_Data(pp[0], pp[1]);
                }
                buf_image_ptr = 0;
            }
        }
    }
    // Flush remaining
    if (pc > 0)
    {
        while (pc < 4)
            pend[pc++] = 0;
        uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
        uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
        buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
    }
    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0);
        for (int j = 0; j < buf_image_ptr; j++)
        {
            uint64_t *pp = (uint64_t *)&buf_image[j];
            Push_GIF_Data(pp[0], pp[1]);
        }
    }
}

void GS_UploadRegionFast(uint32_t coords, uint32_t dims, uint32_t *data_ptr, uint32_t word_count)
{
    int x = coords & 0x3FF;
    int y = (coords >> 16) & 0x1FF;
    int w = dims & 0xFFFF;
    int h = (dims >> 16) & 0xFFFF;

    if (w <= 0 || h <= 0)
        return;

    /* Track dirty region for texture cache invalidation */
    vram_gen_counter++;
    Tex_Cache_DirtyRegion(x, y, w, h);

    // 1. Update shadow VRAM (optional, but good for CLUT textures since they might be used immediately)
    if (psx_vram_shadow)
    {
        for (uint32_t i = 0; i < word_count; i++)
        {
            uint32_t data = data_ptr[i];
            uint32_t n = i * 2;

            // Pixel 0 of the word
            int px0 = x + (n % w);
            int py0 = y + (n / w);
            if (px0 < 1024 && py0 < 512)
                psx_vram_shadow[py0 * 1024 + px0] = data & 0xFFFF;

            // Pixel 1 of the word
            int px1 = x + ((n + 1) % w);
            int py1 = y + ((n + 1) / w);
            if (px1 < 1024 && py1 < 512)
                psx_vram_shadow[py1 * 1024 + px1] = data >> 16;
        }
    }

    // 2. Upload to GS via GIF IMAGE transfer
    Push_GIF_Tag(GIF_TAG_LO(4, 1, 0, 0, 0, 1), 0xE);
    Push_GIF_Data(((uint64_t)GS_PSM_16S << 56) | ((uint64_t)PSX_VRAM_FBW << 48), 0x50);
    Push_GIF_Data(((uint64_t)y << 48) | ((uint64_t)x << 32), 0x51);
    Push_GIF_Data(((uint64_t)h << 32) | (uint64_t)w, 0x52);
    Push_GIF_Data(0, 0x53); // Host -> Local

    buf_image_ptr = 0;
    uint32_t pend[4];
    int pc = 0;

    for (uint32_t i = 0; i < word_count; i++)
    {
        uint32_t word = data_ptr[i];
        uint16_t p0 = word & 0xFFFF;
        uint16_t p1 = word >> 16;

        if (p0 != 0)
            p0 |= 0x8000;
        if (p1 != 0)
            p1 |= 0x8000;

        pend[pc++] = (uint32_t)p0 | ((uint32_t)p1 << 16);

        if (pc >= 4)
        {
            uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
            uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
            buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
            pc = 0;

            if (buf_image_ptr >= 1000)
            {
                Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0); // IMAGE mode
                for (int j = 0; j < buf_image_ptr; j++)
                {
                    uint64_t *p = (uint64_t *)&buf_image[j];
                    Push_GIF_Data(p[0], p[1]);
                }
                buf_image_ptr = 0;
            }
        }
    }

    if (pc > 0)
    {
        while (pc < 4)
            pend[pc++] = 0;
        uint64_t lo = (uint64_t)pend[0] | ((uint64_t)pend[1] << 32);
        uint64_t hi = (uint64_t)pend[2] | ((uint64_t)pend[3] << 32);
        buf_image[buf_image_ptr++] = (unsigned __int128)lo | ((unsigned __int128)hi << 64);
    }

    if (buf_image_ptr > 0)
    {
        Push_GIF_Tag(GIF_TAG_LO(buf_image_ptr, 1, 0, 0, 2, 0), 0); // IMAGE mode
        for (int j = 0; j < buf_image_ptr; j++)
        {
            uint64_t *p = (uint64_t *)&buf_image[j];
            Push_GIF_Data(p[0], p[1]);
        }
        buf_image_ptr = 0;
    }

    Flush_GIF();
}

/* ── Full VRAM dump to file (for testing / debugging) ─────────────── */

void DumpVRAM(const char *filename)
{
#ifdef ENABLE_VRAM_DUMP
    DLOG("DumpVRAM: Dumping VRAM to %s...\n", filename);
#endif

    // 1. Finish any pending rendering
    Flush_GIF();

    // 2. Prepare transfer size (CT16S = 2 bytes per pixel)
    int width = 1024;
    int height = 512;
    int size_bytes = width * height * 2;
    int qwc = size_bytes / 16;

    // Allocate buffer (aligned to 64 bytes)
    void *buf = memalign(64, size_bytes);
    if (!buf)
    {
#ifdef ENABLE_VRAM_DUMP
        DLOG("Failed to allocate %d bytes\n", size_bytes);
#endif
        return;
    }

    // 3. Setup GS for StoreImage via GIF A+D packet
    uint64_t bitbltbuf = ((uint64_t)0 << 0) | ((uint64_t)16 << 16) | ((uint64_t)GS_PSM_16S << 24);
    uint64_t trxpos = 0;
    uint64_t trxreg = ((uint64_t)1024 << 0) | ((uint64_t)512 << 32);
    uint64_t trxdir = 1; // Local -> Host

    unsigned __int128 packet[8] __attribute__((aligned(16)));
    GifTag *tag = (GifTag *)packet;
    tag->NLOOP = 4;
    tag->EOP = 1;
    tag->pad1 = 0;
    tag->PRE = 0;
    tag->PRIM = 0;
    tag->FLG = 0; // PACKED (A+D)
    tag->NREG = 1;
    tag->REGS = 0xE; // A+D

    uint64_t *ptr = (uint64_t *)&packet[1];
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
    uint32_t phys_addr = (uint32_t)buf & 0x1FFFFFFF;
    uint32_t remaining_qwc = qwc;
    uint32_t current_addr = phys_addr;

    while (remaining_qwc > 0)
    {
        uint32_t transfer_qwc = (remaining_qwc > 0xFFFF) ? 0xFFFF : remaining_qwc;

        *D1_MADR = current_addr;
        *D1_QWC = transfer_qwc;
        *D1_CHCR = 0x100;

        while (*D1_CHCR & 0x100)
            ;

        current_addr += transfer_qwc * 16;
        remaining_qwc -= transfer_qwc;
    }

    // 6. Read via uncached access and save to file
    uint8_t *uncached_buf = (uint8_t *)(phys_addr | 0xA0000000);

#ifdef ENABLE_VRAM_DUMP
    uint16_t *p = (uint16_t *)uncached_buf;
    DLOG("DumpVRAM: First pixel: %04X\n", p[0]);
    DLOG("DumpVRAM: Center pixel: %04X\n", p[(512 * 1024 / 2) + 512]);
    fflush(stdout);
#endif

    FILE *f = fopen(filename, "wb");
    if (f)
    {
        fwrite(uncached_buf, 1, size_bytes, f);
        fclose(f);
#ifdef ENABLE_VRAM_DUMP
        DLOG("DumpVRAM: Saved %d bytes to %s\n", size_bytes, filename);
        fflush(stdout);
#endif
    }
    else
    {
#ifdef ENABLE_VRAM_DUMP
        DLOG("Error opening file %s\n", filename);
        fflush(stdout);
#endif
    }

    free(buf);
}
