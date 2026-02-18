#!/usr/bin/env python3
"""Investigate semi-transparency reference values."""
from PIL import Image

ref = Image.open('tests/PSX/GPU/16BPP/RenderRectangle/RenderRectangle16BPP.png')
px = ref.getpixel((80, 50))
print(f'Green-on-black raw RGBA: {px}')
r, g, b, a = px

# It's RGBA with alpha=255. So alpha doesn't affect compositing. 
# The raw value IS the intended value.

# Check more pixels to look for patterns
print("\nChecking various semi-transparent overlaps:")
# Row 1: var-size rects
# Red opaque at (32,8) 42x36, Green alpha at (54,26) 42x36
# Blue opaque at (224,8) 42x36, Blue alpha at (246,26) 42x36
positions = {
    'Red opaque':          (40, 20),
    'Green opaque':        (140, 20),
    'Blue opaque':         (240, 20),
    'Red+Green overlap':   (60, 30),
    'Green+Blue overlap':  (156, 30),
    'Blue+Blue overlap':   (256, 30),
    'Green on black':      (80, 50),
    'Blue on black':       (176, 50),
    'Red on black (alpha)': (272, 50),
}

for name, pos in positions.items():
    px = ref.getpixel(pos)
    print(f'  {name:25s} {pos}: RGBA={px[:4]}, 5bit=({px[0]>>3},{px[1]>>3},{px[2]>>3})')

# Now check the 8x8 rects
print("\n8x8 rects:")
positions_8x8 = {
    'Red 8x8 opaque':      (35, 95),
    'Green 8x8 alpha':     (38, 98),
    'Red+Green 8x8 overlap': (38, 98),
}
for name, pos in positions_8x8.items():
    px = ref.getpixel(pos)
    print(f'  {name:25s} {pos}: RGBA={px[:4]}, 5bit=({px[0]>>3},{px[1]>>3},{px[2]>>3})')

# Check: what channels are non-zero for the overlaps?
print("\nAnalyzing blend ratios:")
# For green-on-black: G_result/G_input
g_result = ref.getpixel((80, 50))[1]  # green channel of green-on-black
print(f'  Green on black: result={g_result}/255 = {g_result/255:.4f}')
print(f'  As fraction of 248 (31<<3): {g_result/248:.4f}')

# Check if 175 matches any expansion of 5-bit 21
v5 = 21
print(f'\n5-bit {v5}:')
print(f'  <<3 = {v5*8}')
print(f'  (c<<3)|(c>>2) = {(v5<<3)|(v5>>2)}')
print(f'  c*255/31 = {v5*255/31:.1f} -> {round(v5*255/31)}')

# AH! (21<<3)|(21>>2) = 168|5 = 173
# and 21*255/31 = 172.4
# Neither is 175.
# So 175 is NOT any standard expansion of 5-bit 21!
# 175 = ? in 5-bit: 175 >> 3 = 21 (remainder 7)
# So it's 21 in 5-bit with a "remainder" of 7, which is 
# (21 << 3) + 7 = 175. The extra "+7" suggests (c<<3)|111₂ = c*8+7

# That looks like it might be: c * 8 + (c >> 2) + something?
# 21*8 = 168, 21>>2 = 5, 168+5 = 173. Not 175.
# 21*8 + 7 = 175. Where does 7 come from?

# What about: c * 8.333? 21 * 8.333 = 175. 
# 8.333 = 255/30.6 ≈ 255/31 * 31/30.6... no.

# Actually: 175 / 21 = 8.333... = 25/3
# So maybe the expansion is c * 25/3? For c=31: 31*25/3 = 258.3. Capped at 255.

# What if expansion is simply: the PNG was saved with a specific color profile?
# The PNG has no explicit gamma, so values should be sRGB (gamma ≈ 2.2)
# Our VRAM dump is linear. Maybe the conversion needs sRGB encode?

import math
def linear_to_srgb(c):
    c = c / 255.0
    if c <= 0.0031308:
        return min(255, int(12.92 * c * 255))
    else:
        return min(255, int((1.055 * math.pow(c, 1.0/2.4) - 0.055) * 255))

def srgb_to_linear(c):
    c = c / 255.0
    if c <= 0.04045:
        return int(c / 12.92 * 255)
    else:
        return int(math.pow((c + 0.055) / 1.055, 2.4) * 255)

print("\nsRGB conversions:")
for v in [120, 124, 127, 128, 248, 255]:
    print(f'  linear {v} -> sRGB {linear_to_srgb(v)}')

print("\nLinearize ref values:")
for v in [175, 255]:
    print(f'  sRGB {v} -> linear {srgb_to_linear(v)}')
