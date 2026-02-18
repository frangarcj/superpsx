#!/usr/bin/env python3
"""Check exact 8-bit semi-trans values in reference PNG to understand PSX formula."""
import numpy as np
from PIL import Image

# RenderRectangle test
ref_8bit = np.array(Image.open('test_results/16bpp/RenderRectangle_ref.png').convert('RGB'))

# Known pixel from analysis: (54,30) is semi-trans green on opaque red
# Red = (255, 0, 0) in 8-bit (31 in 5-bit)
# Semi-trans green foreground = (0, 255, 0)

print("=== RenderRectangle ref raw 8-bit values ===")
# Background red at (32,8) — should be pure opaque red
print(f"Pure opaque red pixel (33,10): {ref_8bit[10, 33]}")  # R should be 248
print(f"Pure opaque green pixel (129,10): {ref_8bit[10, 129]}")

# Semi-trans overlap: green on red
print(f"\nSemi-trans GREEN on RED at (54,30): {ref_8bit[30, 54]}")
print(f"  5-bit: {ref_8bit[30, 54] >> 3}")

# Semi-trans on black: edges where semi-trans rect doesn't overlap opaque
# Semi-trans green at (85,30) — green on black
print(f"Semi-trans GREEN on BLACK at (85,30): {ref_8bit[30, 85]}")
print(f"  5-bit: {ref_8bit[30, 85] >> 3}")

# Semi-trans red at (246,30) — should overlap with blue opaque
# ...

# Let me check a few more positions
print(f"\n--- Checking various semi-trans positions ---")
positions = [(54,26), (54,30), (54,40), (85,30), (85,40), (95,30)]
for x, y in positions:
    v8 = ref_8bit[y, x]
    v5 = v8 >> 3
    r, g, b = v8
    print(f"  ({x},{y}): 8bit=({r},{g},{b}) 5bit={tuple(v5)}")

# Also check the raw pixel for the standard case
# If PSX mode 0 is B/2 + F/2 with 5-bit values:
# F=31 (green), B=0 (black) → (0+31)>>1 = 15
# F=31 (green), B=31 (red) → R: (31+0)>>1=15, G: (0+31)>>1=15
print(f"\n--- Expected vs Actual ---")
print(f"PSX mode 0, green(31) on black(0): expected 15, ref says {ref_8bit[30, 85] >> 3}")
# The opaque pixel values
print(f"Opaque red in ref: {ref_8bit[10, 40]}")  # inside red rect
print(f"Opaque green in ref: {ref_8bit[10, 140]}")  # inside green rect

# What if the ref PNG was NOT generated from a 5-bit framebuffer?
# What if it was captured at 8-bit precision?
# Then mode 0 at 8-bit: (0 + 248) / 2 = 124 at 8-bit
# 124 >> 3 = 15 in "5-bit equivalent"
# Still 15, not 21.

# What about mode 0 with 8-bit values NOT multiplied by 8?
# If PSX stores raw 5-bit and operates in 5-bit, then the PNG would have:
# value * 8 = 120 for result=15
# But 120 >> 3 = 15. So the 5-bit precision check is consistent.

# The key mystery: ref=21 means the actual framebuffer value is 21 in 5-bit.
# This does NOT match B/2+F/2=15 for ANY interpretation.

# Theory: maybe the ref was NOT generated from mode 0 but from a different setup
# Or maybe the test uses a different GP0 command that doesn't set mode 0

# Let me check the ref PNG for the krom test RenderTextureRectangle
# since that test has clear semi-trans with known textures
try:
    texref = np.array(Image.open('test_results/16bpp/RenderTextureRectangle_15BPP_ref.png').convert('RGB'))
    print(f"\n=== TextureRectangle/15BPP ref ===")
    # Check a known pixel
    print(f"Top-left area (10,10): {texref[10, 10]}")
except:
    pass
