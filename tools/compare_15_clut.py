#!/usr/bin/env python3
"""Compare 15BPP vs CLUT4 missing pixels in TexturePolygon to isolate CLUT-specific issues."""
from PIL import Image
import numpy as np

ref_15 = np.array(Image.open('test_results/16bpp/RenderTexturePolygon_15BPP_ref.png'))
got_15 = np.array(Image.open('test_results/16bpp/RenderTexturePolygon_15BPP_got.png'))
ref_c4 = np.array(Image.open('test_results/16bpp/RenderTexturePolygon_CLUT4BPP_ref.png'))
got_c4 = np.array(Image.open('test_results/16bpp/RenderTexturePolygon_CLUT4BPP_got.png'))

ref5_15 = ref_15 >> 3
got5_15 = got_15 >> 3
ref5_c4 = ref_c4 >> 3
got5_c4 = got_c4 >> 3

# Check if reference images are identical
refs_same = np.array_equal(ref5_15, ref5_c4)
print(f"Reference images identical: {refs_same}")
if not refs_same:
    ref_diff = np.any(ref5_15 != ref5_c4, axis=2)
    print(f"  Reference differs at {ref_diff.sum()} pixels")

# Missing pixels in each
miss_15 = np.all(got5_15 == 0, axis=2) & np.any(ref5_15 != 0, axis=2)
miss_c4 = np.all(got5_c4 == 0, axis=2) & np.any(ref5_c4 != 0, axis=2)

print(f"\n15BPP missing: {miss_15.sum()}")
print(f"CLUT4 missing: {miss_c4.sum()}")

# Pixels missing in CLUT4 but NOT in 15BPP
clut_only_miss = miss_c4 & ~miss_15
common_miss = miss_c4 & miss_15
only_15_miss = miss_15 & ~miss_c4

print(f"\nCommon missing (both): {common_miss.sum()}")
print(f"CLUT4-only missing: {clut_only_miss.sum()}")
print(f"15BPP-only missing: {only_15_miss.sum()}")

# Examine CLUT4-only missing pixels
if clut_only_miss.sum() > 0:
    pos = np.argwhere(clut_only_miss)
    print(f"\nCLUT4-only missing positions (first 30):")
    for y, x in pos[:30]:
        r15 = ref5_15[y, x]
        g15 = got5_15[y, x]
        rc4 = ref5_c4[y, x]
        gc4 = got5_c4[y, x]
        print(f"  ({x:3d},{y:3d}): 15BPP ref=({r15[0]:2d},{r15[1]:2d},{r15[2]:2d}) got=({g15[0]:2d},{g15[1]:2d},{g15[2]:2d}) | CLUT4 ref=({rc4[0]:2d},{rc4[1]:2d},{rc4[2]:2d}) got=({gc4[0]:2d},{gc4[1]:2d},{gc4[2]:2d})")

# Examine 15BPP-only missing pixels
if only_15_miss.sum() > 0:
    pos = np.argwhere(only_15_miss)
    print(f"\n15BPP-only missing positions (first 30):")
    for y, x in pos[:30]:
        r15 = ref5_15[y, x]
        g15 = got5_15[y, x]
        rc4 = ref5_c4[y, x]
        gc4 = got5_c4[y, x]
        print(f"  ({x:3d},{y:3d}): 15BPP ref=({r15[0]:2d},{r15[1]:2d},{r15[2]:2d}) got=({g15[0]:2d},{g15[1]:2d},{g15[2]:2d}) | CLUT4 ref=({rc4[0]:2d},{rc4[1]:2d},{rc4[2]:2d}) got=({gc4[0]:2d},{gc4[1]:2d},{gc4[2]:2d})")

# Check what 15BPP GOT at the CLUT4-only-missing positions
if clut_only_miss.sum() > 0:
    pos = np.argwhere(clut_only_miss)
    # At these positions, 15BPP got something but CLUT4 got black
    # What values did 15BPP get?
    vals = got5_15[clut_only_miss]
    unique_vals = set(tuple(v) for v in vals)
    print(f"\n15BPP got values at CLUT4-only-missing positions ({len(unique_vals)} unique):")
    from collections import Counter
    val_counts = Counter(tuple(v) for v in vals)
    for v, c in val_counts.most_common(20):
        print(f"  ({v[0]:2d},{v[1]:2d},{v[2]:2d}): {c}")
