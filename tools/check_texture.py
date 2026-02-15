#!/usr/bin/env python3
"""Check texture data in VRAM vs reference at specific positions."""
import sys
import numpy as np
from PIL import Image

dump_file = sys.argv[1] if len(sys.argv) > 1 else "vram_5000000.bin"
ref_file = sys.argv[2] if len(sys.argv) > 2 else "tests/gpu/texture-flip/vram.png"

with open(dump_file, 'rb') as f:
    data = np.frombuffer(f.read(), dtype=np.uint16).reshape(512, 1024)

ref = np.array(Image.open(ref_file))

print("=== Texture data in OUR VRAM (CT16S raw) at row 0 ===")
for u in [0, 1, 2, 3, 4, 5, 253, 254, 255]:
    val = int(data[0, 640 + u])
    r5 = val & 0x1F
    g5 = (val >> 5) & 0x1F
    b5 = (val >> 10) & 0x1F
    print(f"  Offset ({u},0): raw=0x{val:04X} val={val} R5={r5} G5={g5} B5={b5}")

print()
print("=== Texture data in REFERENCE at row 0 ===")
for u in [0, 1, 2, 3, 4, 5, 253, 254, 255]:
    r, g, b = int(ref[0, 640 + u, 0]), int(ref[0, 640 + u, 1]), int(ref[0, 640 + u, 2])
    r5, g5, b5 = r // 8, g // 8, b // 8
    val = (b5 << 10) | (g5 << 5) | r5
    print(f"  Offset ({u},0): RGB8=({r},{g},{b}) val={val} R5={r5} G5={g5} B5={b5}")

print()
print("=== Texture data differences at row 0 ===")
diffs = 0
for u in range(256):
    our_val = int(data[0, 640 + u])
    r, g, b = int(ref[0, 640 + u, 0]), int(ref[0, 640 + u, 1]), int(ref[0, 640 + u, 2])
    ref_val = ((b // 8) << 10) | ((g // 8) << 5) | (r // 8)
    if our_val != ref_val:
        diffs += 1
        if diffs <= 5:
            print(f"  Offset ({u},0): OUR val={our_val} REF val={ref_val} diff={ref_val - our_val}")
print(f"  Total different: {diffs}/256")

print()
print("=== Rendered X-flip pixels at y=0 ===")
for x in [260, 261, 262, 263, 264, 265, 513, 514, 515]:
    our_val = int(data[0, x])
    our_r5 = our_val & 0x1F
    our_g5 = (our_val >> 5) & 0x1F
    our_b5 = (our_val >> 10) & 0x1F
    ref_r, ref_g, ref_b = int(ref[0, x, 0]) // 8, int(ref[0, x, 1]) // 8, int(ref[0, x, 2]) // 8
    match = our_r5 == ref_r and our_g5 == ref_g and our_b5 == ref_b
    print(f"  x={x}: OUR=({our_r5},{our_g5},{our_b5}) REF=({ref_r},{ref_g},{ref_b}) {'OK' if match else f'DIFF r={ref_r-our_r5} g={ref_g-our_g5} b={ref_b-our_b5}'}")

print()
print("=== Rendered non-flipped pixels at y=0 ===")
for x in [0, 1, 2, 3, 4, 5, 253, 254, 255]:
    our_val = int(data[0, x])
    our_r5 = our_val & 0x1F
    our_g5 = (our_val >> 5) & 0x1F
    our_b5 = (our_val >> 10) & 0x1F
    ref_r, ref_g, ref_b = int(ref[0, x, 0]) // 8, int(ref[0, x, 1]) // 8, int(ref[0, x, 2]) // 8
    match = our_r5 == ref_r and our_g5 == ref_g and our_b5 == ref_b
    print(f"  x={x}: OUR=({our_r5},{our_g5},{our_b5}) REF=({ref_r},{ref_g},{ref_b}) {'OK' if match else f'DIFF r={ref_r-our_r5} g={ref_g-our_g5} b={ref_b-our_b5}'}")
