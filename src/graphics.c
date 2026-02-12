#include "superpsx.h"
#include <graph.h>
#include <draw.h>
#include <dma.h>
#include <stdio.h>

// Global context for libgraph
// Assuming simple single buffered for now to just clear screen.

void Init_Graphics() {
    printf("Initializing Graphics (GS)...\n");
    
    // Reset Graph
    // graph_vram_clear puts GS in a known state and clears VRAM.
    // Coordinates usually 0,0 to width,height.
    // 4MB VRAM on PS2.
    // We'll just clear everything.
    
    // Initialize DMA for GS
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    // Set Video Mode
    // GRAPH_MODE_NTSC, GRAPH_MODE_INTERLACED, GRAPH_MODE_FIELD
    graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
    
    // Clear VRAM
    // 0,0,0 is black.
    graph_vram_clear();
    
    // Wait for VSync
    graph_wait_vsync();
    
    printf("Graphics Initialized. Screen should be black.\n");
}
