#!/usr/bin/env python3
"""Analyze diff=8 pixels in texture-flip VRAM comparison."""
from PIL import Image
import numpy as np
import sys

dump_path = sys.argv[1] if len(sys.argv) > 1 else 'vram_5000000.bin'
ref_path = sys.argv[2] if len(
    sys.argv) > 2 else 'tests/gpu/texture-flip/vram.png'

cur = np.frombuffer(open(dump_path, 'rb').read(),
                    dtype=np.uint16).reshape(512, 1024)
ref_img = Image.open(ref_path).convert('RGB')
ref = np.array(ref_img)

# Convert cur to RGB
r = ((cur & 0x1F) << 3).astype(np.uint8)
g = (((cur >> 5) & 0x1F) << 3).astype(np.uint8)
b = (((cur >> 10) & 0x1F) << 3).astype(np.uint8)
c = np.stack([r, g, b], axis=2)
diff = np.abs(c.astype(int) - ref.astype(int)).max(axis=2)

# Non-flipped rect area (0-255, 0-255)
d8 = np.where(diff[:256, :256] == 8)
print(f'Diff=8 in non-flipped rect (0-255,0-255): {len(d8[0])} pixels')
for i in range(min(10, len(d8[0]))):
    y, x = d8[0][i], d8[1][i]
    cd = c[y, x].astype(int) - ref[y, x].astype(int)
    print(
        f'  ({x},{y}): cur={tuple(c[y, x])} ref={tuple(ref[y, x])} diff={tuple(cd)}')

# Check row pattern
for row in [0, 5, 100, 128, 200]:
    d8_x = np.where(diff[row, :256] == 8)[0]
    print(f'Row {row}: {len(d8_x)}/256 diff=8 pixels, x={d8_x[:15].tolist()}')

# Check X-flipped rect (260-515, 0-255)
d8f = np.where(diff[:256, 260:516] == 8)
print(f'\nDiff=8 in X-flipped rect (260-515, 0-255): {len(d8f[0])} pixels')

# Check if diff=8 correlates with even/odd pixels
d8_even = np.sum(diff[:256, :256:2] == 8)
d8_odd = np.sum(diff[:256, 1:256:2] == 8)
print(f'\nDiff=8 even x: {d8_even}, odd x: {d8_odd}')

# Check the large-diff pixels at the flip boundary
print('\nLarge diff (>=128) pixels around x=260:')
for x in range(258, 265):
    for y in range(0, 3):
        d = diff[y, x]
        if d > 0:
            print(
                f'  ({x},{y}): cur={tuple(c[y, x])} ref={tuple(ref[y, x])} diff={d}')

# Count pixels in each rect zone
zones = [
    ("Normal(0-255,0-255)", 0, 256, 0, 256),
    ("X-flip(260-515,0-255)", 0, 256, 260, 516),
    ("Y-flip(0-255,260-515)", 260, 516, 0, 256),
    ("XY-flip(260-515,260-515)", 260, 516, 260, 516),
]
print('\nPer-zone summary:')
for name, y0, y1, x0, x1 in zones:
    z = diff[y0:y1, x0:x1]
    total = z.size
    d0 = np.sum(z == 0)
    d8v = np.sum(z == 8)
    dlarge = np.sum(z >= 128)
    print(f'  {name}: {d0}/{total} exact ({100*d0/total:.1f}%), {d8v} diff=8, {dlarge} diff>=128')
