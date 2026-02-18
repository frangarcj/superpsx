# SuperPSX

A PlayStation 1 (PSX) emulator that runs on the PlayStation 2, leveraging the Emotion Engine (EE) for native MIPS-to-MIPS dynamic recompilation.

## Overview

SuperPSX takes a unique approach to PSX emulation: instead of running on a PC, it targets the PS2 hardware directly. Since the PS2's Emotion Engine (R5900) is instruction-set compatible with the PSX's CPU (R3000A), the dynarec can perform near 1:1 instruction mapping, while the Graphics Synthesizer (GS) handles rendering translated from PSX GP0 commands.

### Key Features

- **Native MIPS Dynarec** — R3000A instructions compiled directly to R5900 code on the EE
- **GPU Translation** — PSX GP0 commands (polygons, lines, sprites) converted to PS2 GS GIF packets
- **GTE Emulation** — Geometry Transformation Engine via software emulation
- **VRAM Shadowing** — Shadow VRAM for accurate CPU-to-VRAM and FillRect operations
- **CD-ROM / ISO Support** — Load games from CUE/BIN disc images
- **Scheduler-based Timing** — Event-driven scheduler for interrupts, DMA, and CD-ROM timing
- **PCSX2 Development Workflow** — Build and test using PCSX2 with auto-discovered run targets

### Current Status

- BIOS boots successfully (SCPH1001.BIN)
- CPU dynarec passes 86%+ of hardware accuracy tests (533 remaining edge cases)
- GPU renders polygons, lines, sprites, textured quads, and semi-transparency
- GTE handles basic geometry transforms (RTPS, RTPT, NCLIP)
- CD-ROM reads sectors and supports ISO 9660 filesystem navigation
- Controller input working via ps2_drivers joystick interface

## AI-Assisted Development

This project has been developed with significant assistance from AI (GitHub Copilot). AI has contributed to:

- Architecture design and implementation of core emulation subsystems
- Dynamic recompiler code generation and optimization
- GPU command translation pipeline (GP0 to GS)
- Debugging and fixing hardware accuracy issues against reference test logs
- Writing analysis tools and test infrastructure
- Documentation and CI/CD setup

## Building

### Prerequisites

- [ps2dev toolchain](https://github.com/ps2dev/ps2dev) installed
- `PS2DEV` and `PS2SDK` environment variables set
- CMake 3.13+

### Compile

```bash
export PS2DEV=/path/to/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH="$PS2DEV/ee/bin:$PATH"

cmake -B build
cmake --build build
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_VRAM_DUMP` | `OFF` | Enable VRAM dumping (reduces performance) |
| `ENABLE_HOST_LOG` | `ON` | Enable host logging via `printf` |
| `ENABLE_DEBUG_LOG` | `ON` | Enable debug logging per subsystem |
| `ENABLE_STUCK_DETECTION` | `ON` | Detect infinite loops in emulated code |
| `ENABLE_PROFILING` | `OFF` | Build with gprof instrumentation (libprofglue) |
| `ENABLE_LTO` | `OFF` | Enable Link-Time Optimization |
| `ENABLE_UNLIMITED_SPEED` | `ON` | Run PCSX2 without frame limiting |

Example with options:

```bash
cmake -B build \
  -DENABLE_VRAM_DUMP=OFF \
  -DENABLE_DEBUG_LOG=ON \
  -DENABLE_HOST_LOG=OFF

cmake --build build
```

## Running

### BIOS Setup

Place `SCPH1001.BIN` in the `bios/` directory before running.

### Launch with PCSX2

Pass `GAMEARGS` at run time with the relative path to a test or disc image:

```bash
# Build once
cmake -B build
cmake --build build

# Run a test
make -C build run GAMEARGS=tests/gpu/triangle/triangle.exe

# Run an ISO
make -C build run GAMEARGS=isos/240pTestSuitePS1/240pTestSuitePS1.cue

# Boot BIOS shell (no game)
make -C build run
```

Or from the build directory:

```bash
cd build
make run GAMEARGS=tests/gpu/triangle/triangle.exe
```

## Regression Tests

The `tests/` directory contains 48 hardware-accuracy test executables from [ps1-tests](https://github.com/JaCzekanski/ps1-tests), covering:

| Category | Tests | What's Covered |
|----------|-------|----------------|
| `cpu` | Access time, code-in-io, COP, I/O bitwidth | CPU instruction accuracy, memory access |
| `gpu` | Triangle, quad, lines, rectangles, clipping, transparency, textures, VRAM overlap | GPU rendering correctness |
| `gte` | test-all, gte-fuzz | Geometry transform accuracy |
| `dma` | Chain looping, chopping, DPCR, OTC | DMA controller behavior |
| `cdrom` | Disc swap, getloc, terminal, timing | CD-ROM subsystem |
| `spu` | Memory transfer, playback, stereo, toolbox | Sound processing |
| `mdec` | 4-bit, 8-bit, frame, movie | Motion decoder |
| `timers` | Timer dump, timers | Hardware timer accuracy |
| `input` | Pad | Controller input |

Each test directory contains:
- `<test>.exe` — PS-EXE binary to run
- `psx.log` — Expected output from real PSX hardware (ground truth)
- `vram.png` — (GPU tests) Expected VRAM screenshot

### Running Tests

```bash
# Run a single test
cmake --build build --target run-test-gpu-triangle

# Compare output against expected
diff tests/gpu/triangle/psx.log output.log
```

## Visual Regression Testing

GPU correctness is enforced through **screenshot comparison** — every GPU test includes a reference `vram.png` captured from real PSX hardware. When `ENABLE_VRAM_DUMP=ON`, the emulator periodically dumps the shadow VRAM so it can be compared pixel-by-pixel against these reference screenshots to detect visual regressions.

### Workflow

```bash
# 1. Build with VRAM dumping enabled
cmake -B build -DENABLE_VRAM_DUMP=ON
cmake --build build

# 2. Run a GPU test
cmake --build build --target run-test-gpu-triangle

# 3. Compare the VRAM dump against the hardware-captured reference screenshot
python3 tools/compare_vram.py vram_dump.bin tests/gpu/triangle/vram.png

# 4. Generate a visual diff image highlighting mismatched pixels
python3 tools/visualize_diff.py vram_dump.bin tests/gpu/triangle/vram.png diff.png

# 5. Run all GPU tests in batch and report pass/fail per screenshot
python3 tools/run_gpu_tests.py
```

### What Gets Compared

Each GPU test directory contains a `vram.png` — a 1024×512 VRAM screenshot from real hardware. The comparison tools decode the emulator's raw VRAM dump (CT16S, 15-bit color) and diff it against the reference image. Any pixel mismatch is flagged, with detailed reports of region, color channel, and percentage match.

This ensures that changes to the GPU translation pipeline, texture handling, or semi-transparency logic don't introduce visual regressions — every commit can be validated against ground-truth screenshots.

## Tools

The `tools/` directory contains 50+ Python scripts and utilities for visual regression, debugging, and analysis. They are symlinked into the build directory for convenience.

### Screenshot Comparison & Visual Diff

| Tool | Purpose |
|------|---------|
| `compare_vram.py` | Pixel-by-pixel VRAM comparison against reference screenshots |
| `compare_detailed.py` | Detailed comparison with per-region breakdown |
| `compare_15_clut.py` | 15-bit CLUT-aware comparison |
| `compare_tex_rect.py` | Textured rectangle comparison |
| `visualize_diff.py` | Visual diff image generation (color-coded mismatches) |
| `quick_diff.py` | Quick visual diff for fast iteration |
| `categorize_diffs.py` | Categorize rendering differences by type |

### Batch Runners

| Tool | Purpose |
|------|---------|
| `run_gpu_tests.py` | Batch runner for the full GPU test suite |
| `run_16bpp_tests.py` | 16-bit color depth test runner |
| `run_analysis.sh` | Automated VRAM analysis workflow |

### VRAM Viewers

| Tool | Purpose |
|------|---------|
| `vram_viewer.py` | Interactive VRAM viewer |
| `batch_vram_viewer.py` | Batch VRAM viewer for multiple dumps |

### Rendering Checks

| Tool | Purpose |
|------|---------|
| `check_bg.py` | Background rendering validation |
| `check_gradient.py` | Gradient / Gouraud shading check |
| `check_texture.py` | Texture mapping check |
| `check_blend.py` | Alpha blending check |
| `check_dither.py` | Dithering accuracy check |
| `check_pixels.py` | Per-pixel accuracy check |
| `check_semitrans_raw.py` | Semi-transparency raw output check |
| `check_fullscreen.py` | Fullscreen rendering check |
| `check_wrap.py` | Texture wrapping check |
| `check_vram_regions.py` | VRAM region validation |
| `check_vram_tex.py` | VRAM texture page check |
| `check_bios_progression.py` | BIOS boot progression validation |
| `check_bios_render.py` | BIOS rendering output check |

### GPU Analysis

| Tool | Purpose |
|------|---------|
| `analyze_diff.py`, `analyze_diff8.py` | General / 8-bit diff analysis |
| `analyze_polygon.py`, `analyze_polygon_miss.py` | Polygon rendering & miss analysis |
| `analyze_rects.py`, `analyze_texrect.py`, `analyze_texrect_color.py` | Rectangle & textured rect analysis |
| `analyze_line.py` | Line rendering analysis |
| `analyze_clip_boundary.py`, `analyze_clip_extra.py` | Clipping boundary analysis |
| `analyze_overflow.py` | Coordinate overflow analysis |
| `analyze_overlap.py` | VRAM-to-VRAM overlap analysis |
| `analyze_mask.py` | Mask bit analysis |
| `analyze_semitrans.py` | Semi-transparency analysis |
| `full_breakdown.py` | Full rendering breakdown report |
| `isolate_issue.py` | Issue isolation utility |

### Disassembly & Inspection

| Tool | Purpose |
|------|---------|
| `disasm.py` | PSX instruction disassembly |
| `disasm_overlap.py` | Overlap-focused disassembly |
| `find_texture.py` | Texture search in VRAM |
| `read_tables.py` | Table data reader |
| `trace_overlap.py` | VRAM-to-VRAM overlap tracing |

See [tools/README.md](tools/README.md) for detailed usage.

## Project Structure

```
superpsx/
├── CMakeLists.txt          # Build system
├── bios/                   # SCPH1001.BIN (not included)
├── include/                # Header files
│   ├── superpsx.h          # Main header (CPU struct, constants)
│   ├── gpu_state.h         # GPU state definitions
│   ├── scheduler.h         # Event scheduler
│   └── ...
├── src/                    # Source files
│   ├── main.c              # Entry point
│   ├── cpu.c               # CPU state & exceptions
│   ├── dynarec.c           # MIPS-to-MIPS dynamic recompiler
│   ├── memory.c            # Memory map & access
│   ├── hardware.c          # I/O register dispatch
│   ├── gpu_core.c          # GPU command processing
│   ├── gpu_gif.c           # GP0-to-GS translation
│   ├── gpu_vram.c          # VRAM shadow & transfers
│   ├── gpu_primitives.c    # Polygon/line/sprite rendering
│   ├── gpu_texture.c       # Texture handling
│   ├── gpu_commands.c      # GP0/GP1 command parsing
│   ├── gpu_dma.c           # GPU DMA transfers
│   ├── gte.c               # GTE (Coprocessor 2) emulation
│   ├── cdrom.c             # CD-ROM controller
│   ├── scheduler.c         # Event-driven scheduler
│   ├── iso_image.c         # CUE/BIN image reader
│   ├── iso_fs.c            # ISO 9660 filesystem
│   ├── joystick.c          # Controller input
│   └── loader.c            # BIOS & PS-EXE loader
├── tests/                  # Hardware accuracy tests (ps1-tests)
├── isos/                   # Disc images (not included)
├── tools/                  # Analysis & debugging scripts
└── .github/workflows/      # CI pipeline
```

## CI

The project uses GitHub Actions with a build matrix covering multiple configurations (Default, Debug, Full logging, VRAM dump). See [.github/workflows/CI.yml](.github/workflows/CI.yml).

## Credits

- **[psx-spx](https://github.com/psx-spx/psx-spx.github.io)** — Comprehensive PlayStation 1 technical documentation (originally by Martin "nocash" Korth)
- **[ps1-tests](https://github.com/JaCzekanski/ps1-tests)** — Collection of PlayStation 1 hardware tests used for accuracy validation

## License

[MIT](LICENSE) — Copyright (c) 2026 Francisco José García García
