#!/bin/bash
# SuperPSX VRAM Diff Analysis - Example Usage Script

echo "SuperPSX VRAM Diff Analysis Tool"
echo "================================="
echo

# Check if VRAM file exists
if [ $# -eq 0 ]; then
    echo "Usage: $0 <vram_file.bin> [reference.png]"
    echo
    echo "Examples:"
    echo "  $0 vram_5000000.bin"
    echo "  $0 vram_5000000.bin /path/to/reference.png"
    echo
    echo "This will compare the VRAM dump with the reference triangle test."
    echo "If no reference is provided, it will look for tests/gpu/triangle/vram.png"
    exit 1
fi

VRAM_FILE="$1"
REFERENCE_FILE="${2:-}"

# Auto-detect reference file if not provided
if [ -z "$REFERENCE_FILE" ]; then
    # Try common locations
    if [ -f "../../../tests/gpu/triangle/vram.png" ]; then
        REFERENCE_FILE="../../../tests/gpu/triangle/vram.png"
    elif [ -f "../../tests/gpu/triangle/vram.png" ]; then
        REFERENCE_FILE="../../tests/gpu/triangle/vram.png"
    elif [ -f "../tests/gpu/triangle/vram.png" ]; then
        REFERENCE_FILE="../tests/gpu/triangle/vram.png"
    fi
fi

if [ ! -f "$VRAM_FILE" ]; then
    echo "Error: VRAM file '$VRAM_FILE' not found!"
    echo "Make sure SuperPSX has generated a VRAM dump first."
    exit 1
fi

echo "Analyzing VRAM file: $VRAM_FILE"
echo "File size: $(stat -f%z "$VRAM_FILE") bytes"

if [ -n "$REFERENCE_FILE" ] && [ -f "$REFERENCE_FILE" ]; then
    echo "Reference file: $REFERENCE_FILE"
else
    echo "Reference file: Not found (using built-in defaults)"
fi
echo

# Run comparison
echo "Running numerical comparison..."
if [ -n "$REFERENCE_FILE" ] && [ -f "$REFERENCE_FILE" ]; then
    python3 compare_vram.py "$VRAM_FILE" "$REFERENCE_FILE"
else
    python3 compare_vram.py "$VRAM_FILE"
fi

echo
echo "Analysis complete!"
echo
echo "For visual diff, run:"
if [ -n "$REFERENCE_FILE" ] && [ -f "$REFERENCE_FILE" ]; then
    echo "python3 visualize_diff.py $VRAM_FILE $REFERENCE_FILE diff_output.png"
else
    echo "python3 visualize_diff.py $VRAM_FILE /path/to/reference.png diff_output.png"
fi