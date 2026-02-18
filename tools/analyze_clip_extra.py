#!/usr/bin/env python3
"""Analyze EXTRA pixels in RenderRectangleClip."""
from PIL import Image
import numpy as np

ref = np.array(Image.open('test_results/16bpp/RenderRectangleClip_ref.png').convert('RGB')) >> 3
got = np.array(Image.open('test_results/16bpp/RenderRectangleClip_got.png').convert('RGB')) >> 3

diff = np.any(ref != got, axis=2)
ref_black = np.all(ref == 0, axis=2)
got_black = np.all(got == 0, axis=2)

extra_mask = diff & ref_black & ~got_black
extra_pos = np.argwhere(extra_mask)
print(f"Extra pixels: {len(extra_pos)}")

# Show spatial distribution
if len(extra_pos) > 0:
    y_min, x_min = extra_pos.min(axis=0)
    y_max, x_max = extra_pos.max(axis=0)
    print(f"Range: x=[{x_min},{x_max}] y=[{y_min},{y_max}]")
    
    # Group by Y
    from collections import defaultdict
    by_y = defaultdict(list)
    for y, x in extra_pos:
        by_y[y].append(x)
    
    for y in sorted(by_y.keys())[:30]:
        xs = sorted(by_y[y])
        if len(xs) <= 8:
            x_str = ','.join(str(x) for x in xs)
        else:
            x_str = f"{xs[0]}-{xs[-1]} ({len(xs)} px)"
        # Show what color was drawn
        colors = set()
        for x in xs[:5]:
            g = got[y, x]
            colors.add(f"({g[0]},{g[1]},{g[2]})")
        print(f"  y={y:3d}: x=[{x_str}] got={','.join(colors)}")

# Check: are extra pixels at clip boundaries?
print(f"\nClip area analysis:")
# The krom RenderRectangleClip test uses a specific clip area
# Let's check if extra pixels are at specific X or Y boundaries
x_coords = extra_pos[:, 1]
y_coords = extra_pos[:, 0]
print(f"Unique X values: {sorted(set(x_coords.tolist()))[:20]}")
print(f"Unique Y values: {sorted(set(y_coords.tolist()))[:20]}")
