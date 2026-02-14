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
#include <string.h>

// Helper to dump qwords
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

// Compare two packet buffers
static int compare_packets(qword_t* a, qword_t* b, int n) {
    for (int i = 0; i < n; ++i) {
        u64 alo = ((u64*)a)[i*2+0];
        u64 ahi = ((u64*)a)[i*2+1];
        u64 blo = ((u64*)b)[i*2+0];
        u64 bhi = ((u64*)b)[i*2+1];
        if (alo != blo || ahi != bhi) {
            printf("Mismatch at QW[%d]:\n  A: %016llX %016llX\n  B: %016llX %016llX\n", i, ahi, alo, bhi, blo);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    SifInitRpc(0);
    while (!SifIopReset("", 0));
    while (!SifIopSync());
    SifInitRpc(0);
    printf("[TEST] Packet equivalence: libdraw vs manual\n");

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

    // --- LIBDRAW ---
    qword_t *packet1 = (qword_t *)memalign(64, 10000);
    qword_t *q1 = packet1;
    q1 = draw_setup_environment(q1, 0, &frame, &z);
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
    int libdraw_n = after_rect - before_rect;

    // --- MANUAL ---
    qword_t *packet2 = (qword_t *)memalign(64, 10000);
    qword_t *q2 = packet2;
    // Build GIFTAG manually to match libdraw exactly
    // GIFTAG: NLOOP=1, EOP=0, PRE=0, PRIM=0, FLG=0, NREG=4, REGS=0x5510, id=0x44
    u64 giftag_lo = 0x4400000000000001ULL; // id=0x44, NLOOP=1, EOP=0
    u64 giftag_hi = 0x0000000000005510ULL; // REGS=0x5510
    ((u64*)q2)[0] = giftag_lo;
    ((u64*)q2)[1] = giftag_hi;
    q2++;
    u64* data = (u64*)q2;
    *data++ = 6; // PRIM_SPRITE
    // RGBAQ: R=255, G=0, B=0, A=128, Q=1.0 (0x3F800000)
    *data++ = (u64)255 | ((u64)0 << 8) | ((u64)0 << 16) | ((u64)128 << 24) | ((u64)0x3F800000 << 32);
    float start_off = 2047.5625f;
    float end_off = 2048.5625f;
    // Use float multiplication to match libdraw precision
    s32 x1 = (s32)((100.0f + start_off) * 16.0f);
    s32 y1 = (s32)((100.0f + start_off) * 16.0f);
    s32 x2 = (s32)((300.0f + end_off) * 16.0f);
    s32 y2 = (s32)((300.0f + end_off) * 16.0f);
    *data++ = (u64)(x1 & 0xFFFF) | ((u64)(y1 & 0xFFFF) << 16) | ((u64)0 << 32);
    *data++ = (u64)(x2 & 0xFFFF) | ((u64)(y2 & 0xFFFF) << 16) | ((u64)0 << 32);
    q2 = (qword_t*)data;
    int manual_n = q2 - packet2;

    // --- TEST ---
    printf("\nComparando %d qwords (libdraw) vs %d qwords (manual)\n", libdraw_n, manual_n);
    int ok = (libdraw_n == manual_n) && compare_packets(before_rect, packet2, libdraw_n);
    if (ok) {
        printf("\n[TEST] OK: Los paquetes son equivalentes.\n");
    } else {
        printf("\n[TEST] ERROR: Los paquetes NO son equivalentes.\n");
        dump_qwords("LIBDRAW", before_rect, after_rect);
        dump_qwords("MANUAL", packet2, q2);
    }
    sleep(3);
    return ok ? 0 : 1;
}
