#!/usr/bin/env python3
"""Compare rendered output vs reference at specific rectangles."""
from PIL import Image
import sys, os, struct, glob, numpy as np

# Reference image
ref_path = "tests/PSX/GPU/16BPP/RenderTextureRectangle/15BPP/RenderTextureRectangle15BPP.png"
ref_img = np.array(Image.open(ref_path).convert("RGB"))
print(f"Reference: {ref_img.shape}")

# Get latest VRAM dump
vram_files = sorted(glob.glob("vram_*.bin"))
if not vram_files:
    print("No VRAM dump"); sys.exit(1)
vf = vram_files[-1]
print(f"VRAM: {vf}")

data = open(vf, "rb").read()
width, height = 1024, 512

# Decode VRAM to RGB (same as test runner)
got = np.zeros((height, width, 3), dtype=np.uint8)
for y in range(height):
    for x in range(width):
        off = (y * width + x) * 2
        val = struct.unpack('<H', data[off:off+2])[0]
        r = (val & 0x1F) << 3
        g = ((val >> 5) & 0x1F) << 3
        b = ((val >> 10) & 0x1F) << 3
        got[y, x] = [r, g, b]

# Crop to 320x224
got_crop = got[:224, :320]

# Compare 32x32 rect at (160,8)
print("\n=== 32x32 Raw texture rect at (160,8) ===")
for row in [0, 1, 4, 8, 12, 16, 20, 24, 28, 31]:
    ry = 8 + row
    ref_row = ref_img[ry, 160:192] >> 3  # 5-bit
    got_row = got_crop[ry, 160:192] >> 3
    match = np.sum(np.all(ref_row == got_row, axis=1))
    print(f"  Row {row:2d}: ref_R={list(ref_row[:, 0][:32])}")
    print(f"          got_R={list(got_row[:, 0][:32])}")
    print(f"          match={match}/32")

# Also check 8x8 at (32,8)
print("\n=== 8x8 Raw texture rect at (32,8) ===")
for row in range(8):
    ry = 8 + row
    ref_row = ref_img[ry, 32:40] >> 3
    got_row = got_crop[ry, 32:40] >> 3
    print(f"  Row {row}: ref_R={list(ref_row[:, 0])} got_R={list(got_row[:, 0])}")

# Check VRAM texture area more thoroughly
print("\n=== VRAM texture 32x32 at (536,0) all rows ===")
for row in [0, 4, 8, 12, 16, 20, 24, 28, 31]:
    vals = []
    for x in range(536, 568):
        off = (row * width + x) * 2
        val = struct.unpack('<H', data[off:off+2])[0]
        r = (val & 0x1F) << 3
        vals.append(r >> 3)
    print(f"  Row {row:2d}: {vals}")
