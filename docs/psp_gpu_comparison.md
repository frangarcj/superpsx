# PSP GPU Backend — Comparison with PS2 & Gap Analysis

> **Status:** Many gaps filled since initial audit. CLUT textures, state caching, vertex batching, dithering all implemented.

## Architecture Overview

| Aspect | PS2 Backend | PSP Backend |
|--------|-------------|-------------|
| **API** | Direct GS register writes via GIF DMA | sceGu (Sony PSP GU library) |
| **Format** | CT16S (16-bit + STP) | GU_PSM_5551 (15-bit + alpha) |
| **Display** | GS DISPLAY1/DISPLAY2 registers | sceGuDrawBuffer/sceGuDispBuffer |
| **Double-buffer** | DISPFB register swap | sceGuSwapBuffers() |
| **Coordinate** | GS XYOFFSET (2048 center) | PSX_to_PSP() scale helper |

## Feature Comparison

### 1. Display Setup
- **PS2**: Full resolution support (256/320/368/512/640 × 240/480), MAGH/MAGV magnification, interlace, PAL/NTSC.
- **PSP**: Fixed 480×272 output, scale-to-fit via PSX_to_PSP(). No interlace.

### 2. Primitive Rendering

| Primitive | PS2 | PSP |
|-----------|-----|-----|
| Flat triangle | ✅ REGLIST fast path | ✅ sceGuDrawArray |
| Gouraud triangle | ✅ IIP=1 | ✅ per-vertex colors |
| Textured triangle | ✅ HW CLUT + TEX0 | ⚠️ UV coords only, no CLUT decode |
| Flat quad | ✅ TRISTRIP | ✅ GU_TRIANGLE_STRIP |
| Gouraud quad | ✅ | ✅ |
| Textured quad | ✅ | ⚠️ no CLUT |
| Lines | ✅ GS LINE | ✅ GU_LINES |
| **Polylines** | ✅ state machine | ❌ **not implemented** |
| Rectangles/Sprites | ✅ GS SPRITE | ✅ GU_SPRITES |
| FillRect | ✅ GS sprite + VRAM | ✅ shadow VRAM + sceGu sprite |

### 3. Texture Handling — **CRITICAL GAP**

| Feature | PS2 | PSP |
|---------|-----|-----|
| 4BPP CLUT | HW CSM2 (zero CPU) | ✅ EDRAM tex cache (7 LRU slots) |
| 8BPP CLUT | HW CSM2 (zero CPU) | ✅ EDRAM tex cache (7 LRU slots) |
| 15BPP direct | Direct VRAM read | ✅ Direct from EDRAM |
| Texture page cache | 32 pages × 2 formats (96 entries) | N/A |
| Dirty tracking | 16-line granularity per page | N/A |
| Texture windowing | GS CLAMP1 REGION_REPEAT | ❌ not implemented |

**Status**: ✅ Implemented. EDRAM tex cache with 7 LRU slots, CLUT caching by clut_word + content hash, region-based invalidation.

### 4. VRAM Management

| Operation | PS2 | PSP |
|-----------|-----|-----|
| Shadow VRAM | ✅ 1MB aligned buffer | ✅ memalign(64, 1MB) |
| CPU→VRAM (A0) | GIF IMAGE upload + shadow | memcpy to EDRAM + shadow |
| VRAM→CPU (C0) | VIF1 DMA readback | ❌ **stub** |
| VRAM→VRAM (80) | Shadow copy + re-upload | Shadow copy only (no re-render) |
| Display area | GS DISPLAY registers | sceGuScissor (not set) |

### 5. Semi-Transparency / Blending

| Mode | PS2 (GS ALPHA) | PSP (sceGuBlendFunc) |
|------|-----------------|----------------------|
| 0: B/2 + F/2 | ALPHA(0,1,2,1,0x58) | GU_ADD, SRC_ALPHA, ONEm_SRC_ALPHA |
| 1: B + F | ALPHA(0,2,2,1,0x80) | GU_ADD, FIX(0xFF), FIX(0xFF) |
| 2: B − F | ALPHA(1,0,2,2,0x80) | GU_REVERSE_SUBTRACT, FIX, FIX |
| 3: B + F/4 | ALPHA(0,2,2,1,0x20) | GU_ADD, FIX(0x40), FIX(0x40) |

Both platforms implement all 4 modes. PSP approximation may differ slightly.

### 6. Masking Bit (GP0 E6h)
- **PS2**: FBA register (set bit) + alpha test (check bit). Fully correct.
- **PSP**: ❌ **Not implemented**. Parsed but ignored.

### 7. Dithering
- **PS2**: GS DTHE + DIMX matrix (PSX-compatible 4×4 pattern).
- **PSP**: ✅ Implemented via lazy state tracking.

### 8. State Caching
- **PS2**: Aggressive lazy state via `gs_state` struct. Per-primitive fast path: if cmd_key matches cache, emit only PRIM register (1 QW vs 5+ QW).
- **PSP**: ✅ Lazy state tracking implemented (f98e80f). Only emits sceGu calls when state changes.

### 9. DMA / Command Batching
- **PS2**: Double-buffered async GIF DMA. CPU fills buffer B while GPU processes buffer A. ~16KB batches.
- **PSP**: ✅ Vertex buffer batching implemented (75e70f1). Groups same-state primitives.

### 10. GP1 Commands
- **PS2**: All fully implemented (reset, display enable, DMA dir, display FB, H/V range, display mode, GPU info query).
- **PSP**: Mostly stubs — sets gpu_stat flags but minimal real action.

## Priority Gaps (ordered by visual impact)

1. ~~CLUT texture decoding~~ ✅ Done (EDRAM tex cache)
2. **VRAM readback (C0)** — Some games copy rendered VRAM back to CPU for effects. Currently stub.
3. **Polylines** — Used by some games for wireframe effects. Not implemented.
4. **Masking bit** — Used for sprite priorities and semi-transparency control. Could use PSP stencil buffer.
5. **Texture windowing** — Needed for repeated/tiled textures.
6. ~~State caching~~ ✅ Done (lazy state tracking)
7. ~~Dithering~~ ✅ Done

## Assessment

The PSP GPU backend is a **working prototype** that correctly renders:
- Untextured flat/gouraud polygons and quads
- Lines and rectangles
- FillRect
- Semi-transparency (4 modes)
- CPU→VRAM transfers
- GPUSTAT polling (bit 31 toggle)

The **single most impactful missing feature is CLUT texture decoding**. Implementing this would make most games visually recognizable on PSP. The PS2 uses hardware CLUT (GS CSM2 mode) which isn't available on PSP, so a CPU-based decode path with caching is needed.
