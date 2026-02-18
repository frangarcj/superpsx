#!/usr/bin/env python3
"""Check specific pixels in RenderRectangle reference to verify semi-trans formula."""
from PIL import Image
import numpy as np

ref = np.array(Image.open("test_results/16bpp/RenderRectangle_ref.png").convert('RGB'))
got = np.array(Image.open("test_results/16bpp/RenderRectangle_got.png").convert('RGB'))

# The test draws:
# 1. Red rect (31,0,0) at (32,8)-(73,43)  
# 2. Green rect (0,31,0) at (128,8)-(169,43)
# 3. Blue rect (0,0,31) at (224,8)-(265,43)
# 4. Green alpha at (54,26)-(95,61)
# 5. Blue alpha at (150,26)-(191,61)
# 6. Red alpha at (246,26)-(287,61)

regions = [
    ("Red only", 40, 12),         # Inside red rect, before alpha overlap
    ("Green only", 140, 12),      # Inside green rect, before alpha overlap
    ("Blue only", 234, 12),       # Inside blue rect, before alpha overlap
    ("Red+GreenA", 60, 30),       # Red background + Green alpha overlay
    ("Green+BlueA", 155, 30),     # Green background + Blue alpha overlay
    ("Blue+RedA", 250, 30),       # Blue background + Red alpha overlay
    ("GreenA only", 80, 50),      # Green alpha on black background
    ("BlueA only", 175, 50),      # Blue alpha on black background
    ("RedA only", 270, 50),       # Red alpha on black background
    ("Black bg", 10, 10),         # Pure background (should be black)
]

print("Pixel analysis (8-bit RGB from PNG, 5-bit = >>3):")
print(f"{'Region':<20} {'ref_8bit':<20} {'got_8bit':<20} {'ref_5bit':<15} {'got_5bit':<15}")
for name, x, y in regions:
    rr, rg, rb = ref[y, x]
    gr, gg, gb = got[y, x]
    print(f"{name:<20} ({rr:3d},{rg:3d},{rb:3d})     ({gr:3d},{gg:3d},{gb:3d})     ({rr>>3:2d},{rg>>3:2d},{rb>>3:2d})       ({gr>>3:2d},{gg>>3:2d},{gb>>3:2d})")

# Also check multiple overlap pixels for consistency
print("\nDetailed overlap region Red+GreenA:")
for dy in range(3):
    for dx in range(3):
        x, y = 60+dx, 30+dy
        rr, rg, rb = ref[y, x]
        gr, gg, gb = got[y, x]
        eq = "OK" if (rr>>3 == gr>>3 and rg>>3 == gg>>3 and rb>>3 == gb>>3) else "DIFF"
        print(f"  ({x:3d},{y:3d}): ref=({rr>>3:2d},{rg>>3:2d},{rb>>3:2d}) got=({gr>>3:2d},{gg>>3:2d},{gb>>3:2d}) {eq}")
