#!/usr/bin/env python3
from PIL import Image
import numpy as np

ref = np.array(Image.open('tests/gpu/rectangles/vram.png'))
cur = np.array(Image.open('vram_current.png'))

# Check flat semi-trans 0x62 in all groups
# 0x62 is at column 2 (x=40), variable size ~20x20
# Group 0: y=0, Group 1: y=64, Group 2: y=128, Group 3: y=192
for g in range(4):
    y = g * 64 + 5  # center of cell
    x = 45
    print(
        f'Group {g} 0x62 Flat,Semi ({x},{y}): ref={ref[y, x][:3]}  our={cur[y, x][:3]}')

print()

# Check flat non-semi 0x60 in all groups
for g in range(4):
    y = g * 64 + 5
    x = 5
    print(
        f'Group {g} 0x60 Flat ({x},{y}): ref={ref[y, x][:3]}  our={cur[y, x][:3]}')

print()

# Check textured semi 0x66 in all groups
for g in range(4):
    y = g * 64 + 5
    x = 125
    print(
        f'Group {g} 0x66 Tex,Semi ({x},{y}): ref={ref[y, x][:3]}  our={cur[y, x][:3]}')

print()

# Check flat semi 0x6A (1x1) in all groups
for g in range(4):
    y = g * 64 + 5
    x = 205  # col 10 * 20 = 200
    print(
        f'Group {g} 0x6A 1x1,Semi ({x},{y}): ref={ref[y, x][:3]}  our={cur[y, x][:3]}')

# Compare specific semi-trans cells between groups to see if ALPHA changes
print()
print("=== Does ALPHA change between groups? ===")
# Flat semi 0x72 at col 8 (160,20+goffs) = 8x8 semi
for g in range(4):
    y = g * 64 + 22
    x = 42
    print(
        f'Group {g} 0x72 8x8,Semi ({x},{y}): ref={ref[y, x][:3]}  our={cur[y, x][:3]}')
