#!/usr/bin/env python3
"""Analyze line rendering differences in the RenderLine test."""
import numpy as np
from PIL import Image

ref = np.array(Image.open('test_results/16bpp/RenderLine_ref.png').convert('RGB')) >> 3
got = np.array(Image.open('test_results/16bpp/RenderLine_got.png').convert('RGB')) >> 3

diff = (ref != got).any(axis=2)
miss = diff & ((got == 0).all(axis=2)) & ((ref != 0).any(axis=2))
extra = diff & ((ref == 0).all(axis=2)) & ((got != 0).any(axis=2))
color = diff & ((ref != 0).any(axis=2)) & ((got != 0).any(axis=2))

print(f"Total diffs: {diff.sum()}")
print(f"  MISS (ref drawn, got black): {miss.sum()}")
print(f"  EXTRA (ref black, got drawn): {extra.sum()}")
print(f"  COLOR (both non-zero, different): {color.sum()}")

# Group miss/extra by region to understand which line types are affected
print("\n=== MISS pixels by region ===")
my, mx = np.where(miss)
if len(my) > 0:
    # Y ranges for different line sections in the test
    sections = [
        (0, 30, "Single lines"),
        (30, 55, "Poly-lines"),
        (54, 115, "Triangle lines"),
        (115, 165, "Quad lines"),
        (163, 220, "Shaded lines"),
    ]
    for y0, y1, name in sections:
        mask_sec = (my >= y0) & (my < y1)
        count = mask_sec.sum()
        if count > 0:
            # Find some sample positions
            sec_x = mx[mask_sec]
            sec_y = my[mask_sec]
            print(f"  {name} (y={y0}..{y1}): {count} miss")
            # Sample first 10
            for i in range(min(10, count)):
                x, y = sec_x[i], sec_y[i]
                print(f"    ({x},{y}): ref={tuple(ref[y,x])} got={tuple(got[y,x])}")

print("\n=== EXTRA pixels by region ===")
ey, ex = np.where(extra)
if len(ey) > 0:
    sections = [
        (0, 30, "Single lines"),
        (30, 55, "Poly-lines"),
        (54, 115, "Triangle lines"),
        (115, 165, "Quad lines"),
        (163, 220, "Shaded lines"),
    ]
    for y0, y1, name in sections:
        mask_sec = (ey >= y0) & (ey < y1)
        count = mask_sec.sum()
        if count > 0:
            sec_x = ex[mask_sec]
            sec_y = ey[mask_sec]
            print(f"  {name} (y={y0}..{y1}): {count} extra")
            for i in range(min(10, count)):
                x, y = sec_x[i], sec_y[i]
                print(f"    ({x},{y}): ref={tuple(ref[y,x])} got={tuple(got[y,x])}")

print("\n=== COLOR diffs by region ===")
cy, cx = np.where(color)
if len(cy) > 0:
    sections = [
        (0, 30, "Single lines"),
        (30, 55, "Poly-lines"),
        (54, 115, "Triangle lines"),
        (115, 165, "Quad lines"),
        (163, 220, "Shaded lines"),
    ]
    for y0, y1, name in sections:
        mask_sec = (cy >= y0) & (cy < y1)
        count = mask_sec.sum()
        if count > 0:
            sec_x = cx[mask_sec]
            sec_y = cy[mask_sec]
            print(f"  {name} (y={y0}..{y1}): {count} color")
            for i in range(min(5, count)):
                x, y = sec_x[i], sec_y[i]
                print(f"    ({x},{y}): ref={tuple(ref[y,x])} got={tuple(got[y,x])}")

# Specific analysis: In the first section (simple lines), check for endpoint issues
print("\n=== First line: FillLine (33,8)-(54,29) red ===")
# This is a diagonal from (33,8) to (54,29): 22 steps in x, 22 steps in y → 45-degree
# PSX Bresenham should draw 22 pixels (from x=33 to x=54 inclusive, 22 pixels = 22 steps)
for x in range(33, 55):
    y_ref = np.argmax((ref[8:30, x] != 0).any(axis=1)) + 8
    r_ref = tuple(ref[y_ref, x])
    r_got = tuple(got[y_ref, x])
    if r_ref != r_got:
        print(f"  ({x},{y_ref}): ref={r_ref} got={r_got}")

# Check the LAST pixel specifically
print("\n  Endpoint check:")
print(f"  (33,8): ref={tuple(ref[8,33])} got={tuple(got[8,33])}")
print(f"  (54,29): ref={tuple(ref[29,54])} got={tuple(got[29,54])}")
print(f"  (55,30): ref={tuple(ref[30,55])} got={tuple(got[30,55])}")
