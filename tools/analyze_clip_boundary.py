#!/usr/bin/env python3
"""Analyze clip boundary pixels in RenderRectangleClip to determine inclusive/exclusive behavior."""
import numpy as np
from PIL import Image

ref = np.array(Image.open('test_results/16bpp/RenderRectangleClip_ref.png').convert('RGB')) >> 3
got = np.array(Image.open('test_results/16bpp/RenderRectangleClip_got.png').convert('RGB')) >> 3

print("=== Reference image analysis at clip boundaries ===")
print(f"Image shape: {ref.shape}")

# First clip area: DrawingArea 43,17, 277,53
print("\n--- Clip area 1: (43,17)-(277,53) ---")
# Check x=277 column (right boundary)
print("x=277 (right boundary):")
for y in range(15, 55):
    r, g, b = ref[y, 277]
    gr, gg, gb = got[y, 277]
    status = "MATCH" if (r==gr and g==gg and b==gb) else f"DIFF ref=({r},{g},{b}) got=({gr},{gg},{gb})"
    if r != 0 or g != 0 or b != 0 or gr != 0 or gg != 0 or gb != 0:
        print(f"  y={y}: ref=({r},{g},{b}) got=({gr},{gg},{gb}) {status}")

# Check x=276 column (one inside right boundary)
print("\nx=276 (one inside right):")
for y in range(15, 55):
    r, g, b = ref[y, 276]
    gr, gg, gb = got[y, 276]
    status = "MATCH" if (r==gr and g==gg and b==gb) else f"DIFF"
    if r != 0 or g != 0 or b != 0:
        print(f"  y={y}: ref=({r},{g},{b}) got=({gr},{gg},{gb}) {status}")

# Check y=53 row (bottom boundary)
print("\ny=53 (bottom boundary):")
for x in range(41, 280):
    r, g, b = ref[53, x]
    gr, gg, gb = got[53, x]
    if r != 0 or g != 0 or b != 0 or gr != 0 or gg != 0 or gb != 0:
        status = "MATCH" if (r==gr and g==gg and b==gb) else "DIFF"
        print(f"  x={x}: ref=({r},{g},{b}) got=({gr},{gg},{gb}) {status}")

# Check y=52 row (one inside bottom)
print("\ny=52 (one inside bottom):")
for x in [54, 95, 150, 191, 246, 277]:
    r, g, b = ref[52, x]
    gr, gg, gb = got[52, x]
    print(f"  x={x}: ref=({r},{g},{b}) got=({gr},{gg},{gb})")

# Second clip area: DrawingArea 85,52, 85,72
print("\n--- Clip area 2: (85,52)-(85,72) 1x1 tests ---")
print(f"  (85,52): ref={tuple(ref[52,85])} got={tuple(got[52,85])}")
print(f"  (85,72): ref={tuple(ref[72,85])} got={tuple(got[72,85])}")

# Third clip area: DrawingArea 181,52, 182,73
print("\n--- Clip area 3: (181,52)-(182,73) ---")
print(f"  (181,52): ref={tuple(ref[52,181])} got={tuple(got[52,181])}")
print(f"  (182,52): ref={tuple(ref[52,182])} got={tuple(got[52,182])}")
print(f"  (181,72): ref={tuple(ref[72,181])} got={tuple(got[72,181])}")
print(f"  (181,73): ref={tuple(ref[73,181])} got={tuple(got[73,181])}")
print(f"  (182,73): ref={tuple(ref[73,182])} got={tuple(got[73,182])}")

# Fourth clip area: DrawingArea 276,51, 277,72
print("\n--- Clip area 4: (276,51)-(277,72) ---")
print(f"  (276,51): ref={tuple(ref[51,276])} got={tuple(got[51,276])}")
print(f"  (276,52): ref={tuple(ref[52,276])} got={tuple(got[52,276])}")
print(f"  (277,52): ref={tuple(ref[52,277])} got={tuple(got[52,277])}")
print(f"  (277,72): ref={tuple(ref[72,277])} got={tuple(got[72,277])}")
print(f"  (276,72): ref={tuple(ref[72,276])} got={tuple(got[72,276])}")

# Count all extras
diff = (ref != got).any(axis=2)
extras = diff & (ref == 0).all(axis=2) & ((got != 0).any(axis=2))
print(f"\nTotal EXTRA pixels: {extras.sum()}")

# Check ref pixels at key column/row
print("\n=== Summary: Is drawing area inclusive or exclusive? ===")
# Check if ANY non-zero ref pixel at x=277 between y=17 and y=53
r277 = ref[17:54, 277]
print(f"Non-zero ref pixels at x=277 (y=17..53): {(r277 != 0).any(axis=1).sum()}")

# Check if ANY non-zero ref pixel at y=53 between x=43 and x=278
r53 = ref[53, 43:278]
print(f"Non-zero ref pixels at y=53 (x=43..277): {(r53 != 0).any(axis=1).sum()}")
