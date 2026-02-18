#!/usr/bin/env python3
"""Analyze semi-transparency color diffs in RenderRectangle test."""
from PIL import Image
import numpy as np

ref = np.array(Image.open("test_results/16bpp/RenderRectangle_ref.png").convert('RGB'))
got = np.array(Image.open("test_results/16bpp/RenderRectangle_got.png").convert('RGB'))

ref5 = ref >> 3
got5 = got >> 3
diff = np.any(ref5 != got5, axis=-1)

rows, cols = np.where(diff)
print(f"Total diffs: {len(rows)}")

# Group by (ref_pixel, got_pixel) to find patterns
from collections import Counter
patterns = Counter()
for i in range(len(rows)):
    y, x = rows[i], cols[i]
    ref_r, ref_g, ref_b = ref5[y, x]
    got_r, got_g, got_b = got5[y, x]
    # Show 5-bit values
    patterns[(int(ref_r), int(ref_g), int(ref_b), int(got_r), int(got_g), int(got_b))] += 1

print(f"\nTop 40 error patterns (ref_r,g,b -> got_r,g,b) count:")
for (rr, rg, rb, gr, gg, gb), cnt in patterns.most_common(40):
    # Calculate per-channel difference
    dr = gr - rr
    dg = gg - rg
    db = gb - rb
    print(f"  ref=({rr:2d},{rg:2d},{rb:2d}) got=({gr:2d},{gg:2d},{gb:2d}) d=({dr:+3d},{dg:+3d},{db:+3d}) x{cnt}")

# Also check: what background (dest) values were likely before drawing
# Look at surrounding pixels that match
print("\nSample diffs with neighboring context:")
unique_ys = sorted(set(rows))[:10]
for target_y in unique_ys[:5]:
    mask = rows == target_y
    diff_xs = cols[mask]
    if len(diff_xs) == 0:
        continue
    x = diff_xs[0]
    y = target_y
    # Show a horizontal slice at this y
    print(f"\n  Row y={y}, diff_x={x}:")
    for xx in range(max(0, x-2), min(320, x+3)):
        r5, g5, b5 = ref5[y, xx]
        gr5, gg5, gb5 = got5[y, xx]
        is_diff = "***" if diff[y, xx] else "   "
        print(f"    x={xx:3d}: ref=({r5:2d},{g5:2d},{b5:2d}) got=({gr5:2d},{gg5:2d},{gb5:2d}) {is_diff}")
