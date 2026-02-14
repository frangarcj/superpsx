#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <debug.h>
#include <unistd.h>
#include <iopcontrol.h>
#include <stdio.h>
#include <graph.h>
#include <libgs.h>
#include <gs_psm.h>
#include <draw.h>
#include <draw2d.h>
#include <dma.h>
#include <malloc.h>

static void dump_qwords(const char* label, qword_t* start, qword_t* end) {
    printf("\n=== %s ===\n", label);
    printf("Size: %d qwords (%d bytes)\n", (int)(end - start), (int)((end - start) * 16));
    
    qword_t* q = start;
    int index = 0;
    while (q < end) {
        u64 lo = ((u64*)q)[0];
        u64 hi = ((u64*)q)[1];
        printf("QW[%02d]: %016llX %016llX\n", index++, hi, lo);
        q++;
    }
}

int main(int argc, char *argv[]) {
    SifInitRpc(0);

    while (!SifIopReset("", 0));
    while (!SifIopSync());

    SifInitRpc(0);

    printf("=================================================\n");
    printf("Comparing libdraw vs manual GIF packet creation\n");
    printf("=================================================\n");

    // Setup framebuffer
    framebuffer_t frame;
    zbuffer_t z;
    
    frame.width = 640;
    frame.height = 448;
    frame.mask = 0;
    frame.psm = GS_PSM_32;
    frame.address = graph_vram_allocate(frame.width, frame.height, frame.psm, GRAPH_ALIGN_PAGE);
    
    z.enable = DRAW_DISABLE;
    z.address = graph_vram_allocate(frame.width, frame.height, GS_ZBUF_32, GRAPH_ALIGN_PAGE);
    z.mask = 1;
    z.method = ZTEST_METHOD_ALLPASS;
    z.zsm = GS_ZBUF_32;
    
    graph_initialize(frame.address, frame.width, frame.height, frame.psm, 0, 0);
    
    printf("Framebuffer: addr=0x%08X size=%dx%d\n", frame.address, frame.width, frame.height);

    // ============================================
    // TEST 1: libdraw draw_rect_filled
    // ============================================
    qword_t *packet1 = (qword_t *)memalign(64, 10000);
    qword_t *q1 = packet1;
    
    // Setup environment (esto es necesario para que funcione)
    q1 = draw_setup_environment(q1, 0, &frame, &z);
    qword_t* after_setup = q1;
    
    // Draw red rectangle
    rect_t rect;
    rect.v0.x = 100.0f;
    rect.v0.y = 100.0f;
    rect.v0.z = 0;
    rect.v1.x = 300.0f;
    rect.v1.y = 300.0f;
    rect.color.r = 255;
    rect.color.g = 0;
    rect.color.b = 0;
    rect.color.a = 128;
    rect.color.q = 1.0f;
    
    qword_t* before_rect = q1;
    q1 = draw_rect_filled(q1, 0, &rect);
    qword_t* after_rect = q1;
    
    q1 = draw_finish(q1);
    qword_t* after_finish = q1;
    
    dump_qwords("LIBDRAW SETUP_ENVIRONMENT", after_setup - 100 > packet1 ? after_setup - 100 : packet1, after_setup);
    dump_qwords("LIBDRAW DRAW_RECT_FILLED", before_rect, after_rect);
    dump_qwords("LIBDRAW DRAW_FINISH", after_rect, after_finish);
    
    // ============================================
    // TEST 2: Manual GIF packet (like graphics.c)
    // ============================================
    qword_t *packet2 = (qword_t *)memalign(64, 10000);
    qword_t *q2 = packet2;
    
    // Manual sprite construction
    typedef struct {
        u64 NLOOP : 15;
        u64 EOP : 1;
        u64 pad0 : 16;
        u64 id : 14;
        u64 PRE : 1;
        u64 PRIM : 11;
        u64 FLG : 2;
        u64 NREG : 4;
        u64 REGS : 64;
    } GifTag;
    
    GifTag* tag = (GifTag*)q2;
    tag->NLOOP = 1;
    tag->EOP = 1;
    tag->PRE = 1;
    tag->PRIM = 6; // PRIM_SPRITE
    tag->FLG = 0;  // REGLIST
    tag->NREG = 4;
    tag->REGS = 0x5120E; // PRIM, RGBAQ, XYZ2, XYZ2
    tag->id = 0;
    tag->pad0 = 0;
    
    q2++;
    u64* data = (u64*)q2;
    
    // RGBAQ
    *data++ = (u64)255 | ((u64)0 << 8) | ((u64)0 << 16) | ((u64)128 << 24) | ((u64)0 << 32);
    
    // XYZ2 top-left
    float start_off = 2047.5625f;
    float end_off = 2048.5625f;
    s32 x1 = (s32)((float)100 + start_off) << 4;
    s32 y1 = (s32)((float)100 + start_off) << 4;
    s32 x2 = (s32)((float)300 + end_off) << 4;
    s32 y2 = (s32)((float)300 + end_off) << 4;
    
    *data++ = (u64)(x1 & 0xFFFF) | ((u64)(y1 & 0xFFFF) << 16) | ((u64)0 << 32);
    
    // XYZ2 bottom-right
    *data++ = (u64)(x2 & 0xFFFF) | ((u64)(y2 & 0xFFFF) << 16) | ((u64)0 << 32);
    
    q2 = (qword_t*)data;
    
    dump_qwords("MANUAL SPRITE", packet2, q2);
    
    printf("\n=================================================\n");
    printf("Comparison complete. Check differences above.\n");
    printf("=================================================\n");
    
    // Wait a bit before exiting
    sleep(5);
    
    return 0;
}
