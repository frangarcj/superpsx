#!/usr/bin/env python3
"""Analyze vram-to-vram-overlap test differences by grid cell."""
import numpy as np
from PIL import Image
import sys

dump = sys.argv[1]
ref_path = sys.argv[2]

with open(dump, 'rb') as f:
    raw = f.read()
# Dump is already linear (GS readback deswizzles)
vram_raw = np.frombuffer(raw, dtype=np.uint16).reshape((512, 1024))

# Convert to RGB
cur = np.zeros((512, 1024, 3), dtype=np.uint8)
cur[:, :, 0] = ((vram_raw & 0x1F) << 3).astype(np.uint8)
cur[:, :, 1] = (((vram_raw >> 5) & 0x1F) << 3).astype(np.uint8)
cur[:, :, 2] = (((vram_raw >> 10) & 0x1F) << 3).astype(np.uint8)

ref_img = Image.open(ref_path).convert('RGB')
ref = np.array(ref_img)

CELL = 42
MARGIN = 4
sizes = [2, 8, 15, 16, 16, 16, 16]
labels = ["BLK2 ", "BLK8 ", "BLK15", "BLK16", "BK16X", "BK16Y", "BKXY "]

print("=== Per-cell diff (y_off, x_off) ===")
header = "                "
for oy in [-1, 0, 1]:
    for ox in range(-3, 4):
        header += f"{ox:+d},{oy:+d} "
    header += "| "
print(header)
print("-" * len(header))

for row_idx in range(7):
    size = sizes[row_idx]
    line = f"{labels[row_idx]} (sz={size:2d}) | "
    col = 0
    for oy in [-1, 0, 1]:
        for ox in range(-3, 4):
            cell_x = (col + 1) * CELL + MARGIN
            cell_y = row_idx * CELL + MARGIN
            x0 = max(0, min(cell_x + ox, cell_x))
            y0 = max(0, min(cell_y + oy, cell_y))
            x1 = min(1024, max(cell_x + ox + size, cell_x + size))
            y1 = min(512, max(cell_y + oy + size, cell_y + size))
            region_cur = cur[y0:y1, x0:x1]
            region_ref = ref[y0:y1, x0:x1]
            diff = np.abs(region_cur.astype(int) - region_ref.astype(int))
            ndiff = np.sum(np.max(diff, axis=2) > 0)
            if ndiff > 0:
                line += f"{ndiff:4d} "
            else:
                line += "   . "
            col += 1
        line += "| "
    print(line)

# Sample some specific diff pixels for the first broken cell
print("\n=== Sample pixel diffs from first broken cells ===")
for row_idx in [0, 3]:  # BLK2 and BLK16
    size = sizes[row_idx]
    for oy_idx, oy in enumerate([-1, 0, 1]):
        for ox_idx, ox in enumerate(range(-3, 4)):
            col = oy_idx * 7 + ox_idx
            cell_x = (col + 1) * CELL + MARGIN
            cell_y = row_idx * CELL + MARGIN
            x0 = cell_x
            y0 = cell_y
            diff_pixels = []
            for py in range(max(0, min(oy, 0)), size + max(0, oy)):
                for px in range(max(0, min(ox, 0)), size + max(0, ox)):
                    gy, gx = y0 + py, x0 + px
                    if 0 <= gy < 512 and 0 <= gx < 1024:
                        d = abs(int(cur[gy, gx, 0]) - int(ref[gy, gx, 0])) + \
                            abs(int(cur[gy, gx, 1]) - int(ref[gy, gx, 1])) + \
                            abs(int(cur[gy, gx, 2]) - int(ref[gy, gx, 2]))
                        if d > 0:
                            diff_pixels.append(
                                (gx, gy, tuple(cur[gy, gx]), tuple(ref[gy, gx])))
            if diff_pixels:
                print(
                    f"  {labels[row_idx]} off=({ox:+d},{oy:+d}): {len(diff_pixels)} diffs, first 3:")
                for gx, gy, c, r in diff_pixels[:3]:
                    print(f"    ({gx},{gy}): cur={c} ref={r}")
