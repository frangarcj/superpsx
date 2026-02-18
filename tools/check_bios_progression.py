#!/usr/bin/env python3
"""Compare multiple VRAM dump PNGs to see progression."""
import sys
from PIL import Image

files = [
    "vram_1000000.png",
    "vram_10000000.png",
    "vram_30000000.png",
    "vram_50000000.png",
    "vram_70000000.png",
    "vram_79000000.png",
]

for fname in files:
    try:
        img = Image.open(fname)
    except Exception:
        continue

    # Count non-black pixels in key regions
    text_area_px = 0  # y=380-450, x=100-540 (BIOS text area)
    gradient_px = 0   # y=200-350 (gradient band)
    logo_px = 0       # y=0-50, x=0-640 (top logo area)

    # Scan text area for non-background pixels
    text_diff = 0
    for y in range(380, 450):
        for x in range(100, 540):
            r, g, b, a = img.getpixel((x, y))
            if r > 0 or g > 0 or b > 0:
                text_area_px += 1
            # Check if it differs from background gray (181,181,181)
            if abs(r - 181) > 10 or abs(g - 181) > 10 or abs(b - 181) > 10:
                text_diff += 1

    # Scan gradient
    for y in range(200, 350):
        for x in range(0, 640):
            r, g, b, a = img.getpixel((x, y))
            if r > 0 or g > 0 or b > 0:
                gradient_px += 1

    # Scan top for logo
    for y in range(0, 50):
        for x in range(0, 640):
            r, g, b, a = img.getpixel((x, y))
            if r > 0 or g > 0 or b > 0:
                logo_px += 1

    print(f"{fname}:")
    print(f"  logo(0-50):    {logo_px:6d} px")
    print(f"  gradient(200-350): {gradient_px:6d} px")
    print(f"  text(380-450): {text_area_px:6d} px, non-bg: {text_diff:6d}")

    # ASCII art of text region
    row = ""
    for x in range(100, 540, 5):
        count = 0
        for y in range(380, 450):
            r, g, b, a = img.getpixel((x, y))
            if abs(r - 181) > 10 or abs(g - 181) > 10 or abs(b - 181) > 10:
                count += 1
        if count > 20:
            row += "#"
        elif count > 10:
            row += "*"
        elif count > 5:
            row += "+"
        elif count > 0:
            row += "."
        else:
            row += " "
    print(f"  text preview: [{row}]")
    print()
