#!/usr/bin/env python3
"""Analyze TextureRectangle 15BPP color diffs."""
from PIL import Image
import numpy as np
from collections import Counter

ref = np.array(Image.open("test_results/16bpp/RenderTextureRectangle_15BPP_ref.png").convert('RGB'))
got = np.array(Image.open("test_results/16bpp/RenderTextureRectangle_15BPP_got.png").convert('RGB'))

ref5 = ref >> 3
got5 = got >> 3
diff = np.any(ref5 != got5, axis=-1)

rows, cols = np.where(diff)
print(f"Total diffs: {len(rows)}")

# Per-channel difference analysis
delta_r = got5[diff, 0].astype(int) - ref5[diff, 0].astype(int)
delta_g = got5[diff, 1].astype(int) - ref5[diff, 1].astype(int)
delta_b = got5[diff, 2].astype(int) - ref5[diff, 2].astype(int)

print(f"\nPer-channel delta stats:")
for ch, d in [("R", delta_r), ("G", delta_g), ("B", delta_b)]:
    nonzero = d[d != 0]
    if len(nonzero) > 0:
        print(f"  {ch}: {len(nonzero)} diffs, range=[{nonzero.min()}, {nonzero.max()}], "
              f"mean={nonzero.mean():.2f}, median={int(np.median(nonzero))}")
        vals, counts = np.unique(nonzero, return_counts=True)
        for v, c in sorted(zip(vals, counts), key=lambda x: -x[1])[:10]:
            print(f"    delta={v:+3d}: {c}")

# Check spatial distribution: which ROWS have most diffs
print(f"\nDiff density by Y region:")
for y0 in range(0, 224, 32):
    y1 = min(y0 + 32, 224)
    region_diff = np.sum(diff[y0:y1, :])
    print(f"  Y={y0:3d}-{y1:3d}: {region_diff} diffs")

# Show some sample diffs  
print(f"\nSample diffs:")
np.random.seed(42)
samples = np.random.choice(len(rows), min(30, len(rows)), replace=False)
for s in sorted(samples):
    y, x = rows[s], cols[s]
    print(f"  ({x:3d},{y:3d}): ref=({ref5[y,x,0]:2d},{ref5[y,x,1]:2d},{ref5[y,x,2]:2d}) "
          f"got=({got5[y,x,0]:2d},{got5[y,x,1]:2d},{got5[y,x,2]:2d})")
