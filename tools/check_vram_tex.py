#!/usr/bin/env python3
"""Check VRAM dump at texture coordinates to verify texture upload."""
import struct, sys, glob, os

# Find latest VRAM dump
pattern = "vram_*.bin"
files = sorted(glob.glob(pattern))
if not files:
    print("No VRAM dump found")
    sys.exit(1)

vram_file = files[-1]
print(f"Using: {vram_file}")

data = open(vram_file, "rb").read()
# VRAM is 1024x512 in CT16S format (16-bit per pixel)
# But the dump format might be different. Let's check size
print(f"Size: {len(data)} bytes")

# Expected: 1024*512*2 = 1048576 bytes for 16-bit
# Or 1024*512*4 = 2097152 for 32-bit

width = 1024
height = 512

if len(data) == width * height * 4:
    bpp = 4
    print("32-bit VRAM dump")
elif len(data) == width * height * 2:
    bpp = 2
    print("16-bit VRAM dump")
else:
    print(f"Unexpected size: {len(data)}, expected {width*height*2} or {width*height*4}")
    sys.exit(1)

def get_pixel(x, y):
    if bpp == 4:
        off = (y * width + x) * 4
        r, g, b, a = data[off], data[off+1], data[off+2], data[off+3]
        return (r, g, b, a)
    else:
        off = (y * width + x) * 2
        val = struct.unpack('<H', data[off:off+2])[0]
        r = (val & 0x1F) << 3
        g = ((val >> 5) & 0x1F) << 3
        b = ((val >> 10) & 0x1F) << 3
        a = (val >> 15) & 1
        return (r, g, b, a)

# Check texture regions:
# 8x8 at (512,0), 16x16 at (520,0), 32x32 at (536,0), 64x64 at (568,0)
regions = [
    ("8x8", 512, 0, 8, 8),
    ("16x16", 520, 0, 16, 16),
    ("32x32", 536, 0, 32, 32),
    ("64x64", 568, 0, 64, 64),
]

for name, rx, ry, rw, rh in regions:
    print(f"\n--- Texture {name} at VRAM({rx},{ry}) ---")
    # Check first row
    row_pixels = []
    for px in range(rx, rx + rw):
        r, g, b, a = get_pixel(px, ry)
        row_pixels.append(f"{r:3d}" if r > 0 or g > 0 or b > 0 else "  0")
    print(f"  Row 0 R: {' '.join(row_pixels[:min(40, rw)])}")
    
    # Count non-zero pixels
    nonzero = 0
    for py in range(ry, ry + rh):
        for px in range(rx, rx + rw):
            r, g, b, a = get_pixel(px, py)
            if r > 0 or g > 0 or b > 0:
                nonzero += 1
    print(f"  Non-zero pixels: {nonzero}/{rw*rh}")

# Also check the visible area where rects are drawn
# 32x32 rect drawn at screen (160,8)
print("\n--- Visible 32x32 rect at screen (160,8) ---")
for row in [0, 8, 16, 24, 31]:
    vy = 8 + row
    row_vals = []
    for px in range(160, 160 + 32):
        r, g, b, a = get_pixel(px, vy)
        row_vals.append(r >> 3)  # 5-bit
    print(f"  Row {row:2d}: {row_vals}")
