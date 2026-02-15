# SuperPSX Tools

This directory contains development and testing tools for the SuperPSX emulator project.

## VRAM Analysis Tools

### compare_vram.py
Numerical comparison tool for VRAM dumps against reference images.

**Usage:**
```bash
python3 compare_vram.py <vram_file.bin> [reference.png]
```

**Features:**
- Pixel-by-pixel comparison with reference images
- Percentage match calculation
- Color precision analysis (15-bit vs 32-bit)
- Detailed diff statistics and error reporting

### visualize_diff.py
Visual diff generation tool for debugging GPU rendering issues.

**Usage:**
```bash
python3 visualize_diff.py <vram_file.bin> <reference.png> [output.png]
```

**Features:**
- Generates visual representations of VRAM differences
- Color-coded diff images for easy identification of issues
- Supports various output formats for analysis

### run_analysis.sh
Convenience script for automated VRAM analysis.

**Usage:**
```bash
./run_analysis.sh <vram_file.bin> [reference.png]
```

**Features:**
- Auto-detection of reference files
- Comprehensive analysis workflow
- User-friendly output with next steps

## VRAM Dumping Control

VRAM dumping is controlled by the `ENABLE_VRAM_DUMP` define in the Makefile:

- **Enabled by default**: `-DENABLE_VRAM_DUMP` in `EE_CFLAGS`
- **To disable**: Comment out or remove `-DENABLE_VRAM_DUMP` from the Makefile
- **Performance impact**: Dumping VRAM every 1M iterations can slow down emulation
- **When to disable**: For performance testing or when VRAM dumps are not needed

## Integration

These tools are designed to work with:
- SuperPSX VRAM dumps (CT16S format, 1024x512 pixels)
- ps1-tests reference images (PNG format)
- PCSX2 emulator for testing

## Dependencies

- Python 3.x
- NumPy
- PIL/Pillow
- Matplotlib (for visualization)

## Workflow

1. Run SuperPSX with PCSX2 to generate VRAM dumps
2. Use `run_analysis.sh` for quick analysis
3. Use individual tools for detailed debugging
4. Iterate on GPU implementation based on results