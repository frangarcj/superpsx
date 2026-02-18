#!/usr/bin/env python3
"""Analyze diff patterns between reference and VRAM dump for a given test."""
import struct, os, sys, glob
from PIL import Image
import numpy as np

test_name = sys.argv[1] if len(sys.argv) > 1 else "RenderPolygon"

# Find test dir
base = "tests/PSX/GPU/16BPP"
test_dirs = sorted(glob.glob(os.path.join(base, "**"), recursive=True))
candidates = [d for d in test_dirs if os.path.isdir(d) and test_name in d]
if not candidates:
    print(f"No test dir matching '{test_name}'")
    sys.exit(1)
test_dir = candidates[0]
print(f"Test dir: {test_dir}")

# Find reference PNG
pngs = glob.glob(os.path.join(test_dir, "*.png"))
if not pngs:
    print("No reference PNG found")
    sys.exit(1)
ref_path = pngs[0]
ref = np.array(Image.open(ref_path).convert('RGB'))
print(f"Ref: {ref_path} shape={ref.shape}")

# Find VRAM dump
dumps = sorted(glob.glob("/tmp/vram_dump/vram_*.bin"))
if not dumps:
    print("No VRAM dumps found")
    sys.exit(1)
dump = dumps[-1]
print(f"VRAM dump: {dump}")

with open(dump, "rb") as f:
    raw = f.read()
vram = np.frombuffer(raw, dtype=np.uint16).reshape(512, 1024)

# Crop 320x224 and convert to RGB
crop = vram[0:224, 0:320]
r = ((crop & 0x1F) << 3).astype(np.uint8)
g = (((crop >> 5) & 0x1F) << 3).astype(np.uint8)
b = (((crop >> 10) & 0x1F) << 3).astype(np.uint8)
got = np.stack([r, g, b], axis=-1)

# Compare at 5-bit
ref5 = (ref >> 3).astype(np.uint8)
got5 = (got >> 3).astype(np.uint8)
diff = np.any(ref5 != got5, axis=-1)

rows, cols = np.where(diff)
total_diff = len(rows)
total_pixels = 320 * 224
match_pct = 100.0 * (1 - total_diff / total_pixels)
print(f"\nTotal diff pixels: {total_diff} / {total_pixels} ({match_pct:.2f}% match)")

if total_diff == 0:
    print("Perfect match!")
    sys.exit(0)

print(f"Y range: {rows.min()}-{rows.max()}, X range: {cols.min()}-{cols.max()}")

# Categorize
ref_nonblack = np.any(ref5 != 0, axis=-1)
got_nonblack = np.any(got5 != 0, axis=-1)
missing = diff & ref_nonblack & (~got_nonblack)
extra = diff & (~ref_nonblack) & got_nonblack
color_diff = diff & ref_nonblack & got_nonblack

print(f"\nDiff categories:")
print(f"  Missing (ref colored, got black): {np.sum(missing)}")
print(f"  Extra   (ref black, got colored): {np.sum(extra)}")
print(f"  Color   (both colored, differ):   {np.sum(color_diff)}")

# Edge analysis
edge_count = 0
interior_count = 0
for s in range(min(500, total_diff)):
    y, x = rows[s], cols[s]
    is_edge = False
    for dy, dx in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
        ny, nx = y + dy, x + dx
        if 0 <= ny < 224 and 0 <= nx < 320:
            if not diff[ny, nx]:
                is_edge = True
                break
    if is_edge:
        edge_count += 1
    else:
        interior_count += 1

print(f"\nOf first {min(500, total_diff)} diffs: {edge_count} at edges, {interior_count} interior")

# Sample diffs
print("\nSample diffs (x, y): ref_rgb -> got_rgb")
np.random.seed(42)
samples = np.random.choice(total_diff, min(30, total_diff), replace=False)
for s in sorted(samples):
    y, x = rows[s], cols[s]
    r_ref, g_ref, b_ref = ref[y, x]
    r_got, g_got, b_got = got[y, x]
    cat = "MISS" if missing[y, x] else ("EXTRA" if extra[y, x] else "COLOR")
    print(f"  ({x:3d},{y:3d}) {cat}: ref=({r_ref:3d},{g_ref:3d},{b_ref:3d}) got=({r_got:3d},{g_got:3d},{b_got:3d})")

# Save diff image
diff_img = np.zeros((224, 320, 3), dtype=np.uint8)
diff_img[missing] = [255, 0, 0]
diff_img[extra] = [0, 0, 255]
diff_img[color_diff] = [255, 255, 0]
diff_img_pil = Image.fromarray(diff_img)
diff_path = f"/tmp/diff_{test_name.replace('/', '_')}.png"
diff_img_pil.save(diff_path)
print(f"\nDiff image saved to {diff_path}")
