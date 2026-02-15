#!/usr/bin/env python3
"""Analyze texture-overflow diff pattern."""
import sys
import numpy as np
from PIL import Image

dump = sys.argv[1] if len(sys.argv) > 1 else "vram_5000000.bin"
ref_f = sys.argv[2] if len(sys.argv) > 2 else "tests/gpu/texture-overflow/vram.png"

with open(dump, 'rb') as f:
    data = np.frombuffer(f.read(), dtype=np.uint16).reshape(512, 1024)
ref = np.array(Image.open(ref_f))

print("=== Display rows with diffs ===")
for y in range(0, 240, 5):
    first_diff = -1
    last_diff = -1
    ndiff = 0
    for x in range(320):
        val = int(data[y, x])
        r5 = val & 0x1F
        g5 = (val >> 5) & 0x1F
        b5 = (val >> 10) & 0x1F
        rr = int(ref[y, x, 0]) // 8
        rg = int(ref[y, x, 1]) // 8
        rb = int(ref[y, x, 2]) // 8
        if r5 != rr or g5 != rg or b5 != rb:
            ndiff += 1
            if first_diff < 0:
                first_diff = x
            last_diff = x
    if ndiff > 0:
        val = int(data[y, first_diff])
        r5 = val & 0x1F
        g5 = (val >> 5) & 0x1F
        b5 = (val >> 10) & 0x1F
        rr = int(ref[y, first_diff, 0]) // 8
        rg = int(ref[y, first_diff, 1]) // 8
        rb = int(ref[y, first_diff, 2]) // 8
        print(f"  y={y:3d}: {ndiff:3d} diff, x=[{first_diff}..{last_diff}] first:OUR=({r5},{g5},{b5}) REF=({rr},{rg},{rb})")
    else:
        print(f"  y={y:3d}: OK")

# Show where our red pixels are
print("\n=== Our red (31,0,0) pixels region ===")
red_count = 0
for y in range(240):
    for x in range(320):
        val = int(data[y, x])
        if (val & 0x1F) == 31 and ((val >> 5) & 0x1F) == 0 and ((val >> 10) & 0x1F) == 0:
            red_count += 1
print(f"Total red pixels: {red_count}")

# Find bounding box of red
first_red_y = -1
for y in range(240):
    for x in range(320):
        val = int(data[y, x])
        if (val & 0x1F) == 31 and ((val >> 5) & 0x1F) == 0:
            if first_red_y < 0:
                first_red_y = y
            last_red_y = y
            break
if first_red_y >= 0:
    print(f"Red Y range: {first_red_y} to {last_red_y}")
