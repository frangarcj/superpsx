#!/usr/bin/env python3
"""Check VRAM dump to understand wrap region status."""
import struct
import sys

dump = sys.argv[1] if len(sys.argv) > 1 else "vram_5000000.bin"
data = open(dump, 'rb').read()

def read_px(x, y):
    off = (y * 1024 + x) * 2
    px = struct.unpack_from('<H', data, off)[0]
    r = (px & 0x1F) << 3
    g = ((px >> 5) & 0x1F) << 3
    b = ((px >> 10) & 0x1F) << 3
    return r, g, b

def show_region(title, x0, x1, y0, y1):
    print(f"\n=== {title} (x={x0}..{x1-1}, y={y0}..{y1-1}) ===")
    for y in range(y0, min(y1, y0+4)):
        row = []
        for x in range(x0, min(x1, x0+8)):
            r, g, b = read_px(x, y)
            row.append(f"({r:3d},{g:3d},{b:3d})")
        print(f"  y={y}: {' '.join(row)}")

# Where the upload fixup should have written: x=0..127, y=256..383
show_region("Wrapped region (should have texture data)", 0, 128, 256, 384)

# Original transfer region: x=896..1023, y=256..383
show_region("Non-wrapped region (original transfer)", 896, 1024, 256, 384)

# Display area where we see solid red: roughly (17,16)-(272,144)
show_region("Display area (top-left, should show texture)", 17, 50, 16, 24)

# Check if wrapped region is all zeros or all same value
print("\n=== Wrapped region statistics ===")
unique = set()
red_count = 0
zero_count = 0
total = 0
for y in range(256, 384):
    for x in range(0, 128):
        r, g, b = read_px(x, y)
        unique.add((r, g, b))
        if r == 248 and g == 0 and b == 0:
            red_count += 1
        if r == 0 and g == 0 and b == 0:
            zero_count += 1
        total += 1
print(f"  Total pixels: {total}")
print(f"  Unique colors: {len(unique)}")
print(f"  Pure red (248,0,0): {red_count}")
print(f"  Black (0,0,0): {zero_count}")
if len(unique) <= 10:
    print(f"  Colors: {unique}")

# Check reference for comparison
try:
    from PIL import Image
    ref = Image.open(sys.argv[2] if len(sys.argv) > 2 else "tests/gpu/texture-overflow/vram.png")
    ref = ref.convert("RGB")
    print("\n=== Reference wrapped region (x=0..7, y=256..259) ===")
    for y in range(256, 260):
        row = []
        for x in range(0, 8):
            r, g, b = ref.getpixel((x, y))
            row.append(f"({r:3d},{g:3d},{b:3d})")
        print(f"  y={y}: {' '.join(row)}")
    
    # Check what reference has at display area
    print("\n=== Reference display area (x=17..24, y=16..19) ===")
    for y in range(16, 20):
        row = []
        for x in range(17, 25):
            r, g, b = ref.getpixel((x, y))
            row.append(f"({r:3d},{g:3d},{b:3d})")
        print(f"  y={y}: {' '.join(row)}")
except:
    print("\n(Could not load reference image)")
