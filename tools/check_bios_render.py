#!/usr/bin/env python3
"""Check BIOS rendering in VRAM dump."""
import sys
from PIL import Image

img = Image.open(sys.argv[1])

print("=== Display area (0,0)-(640,480) scan ===")
for y in [100, 200, 240, 300, 350, 360, 370, 380, 390, 400, 410, 420, 430, 440, 450]:
    non_black = 0
    colors = set()
    for x in range(0, 640):
        r, g, b, a = img.getpixel((x, y))
        if r > 0 or g > 0 or b > 0:
            non_black += 1
            colors.add((r, g, b))
    if non_black > 0:
        sample = list(colors)[:5]
        print(
            f"  y={y}: {non_black} non-black px, {len(colors)} colors, sample: {sample}")
    else:
        print(f"  y={y}: all black")

print("\n=== Second framebuffer (y=256..511) scan ===")
for y in [356, 366, 376, 386, 396, 406, 416, 426]:
    non_black = 0
    for x in range(0, 640):
        r, g, b, a = img.getpixel((x, y))
        if r > 0 or g > 0 or b > 0:
            non_black += 1
    if non_black > 0:
        print(f"  y={y}: {non_black} non-black pixels")
    else:
        print(f"  y={y}: all black")

print("\n=== Texture area (x=960..1023) ===")
for y in range(0, 50, 10):
    non_black = 0
    for x in range(960, 1024):
        r, g, b, a = img.getpixel((x, y))
        if r > 0 or g > 0 or b > 0:
            non_black += 1
    if non_black > 0:
        print(f"  y={y} x=[960..1023]: {non_black} non-black px")

print("\n=== CLUT area (x=320, y=480..511) ===")
for y in [480, 490, 500]:
    non_black = 0
    for x in range(320, 400):
        r, g, b, a = img.getpixel((x, y))
        if r > 0 or g > 0 or b > 0:
            non_black += 1
    if non_black > 0:
        print(f"  y={y} x=[320..399]: {non_black} non-black px")

# ASCII art render of display area
print("\n=== ASCII preview of display (0,0)-(320,240) scaled ===")
for y in range(0, 240, 10):
    row = ""
    for x in range(0, 320, 4):
        r, g, b, a = img.getpixel((x, y))
        brightness = (r + g + b) / 3
        if brightness > 200:
            row += "#"
        elif brightness > 150:
            row += "*"
        elif brightness > 100:
            row += "+"
        elif brightness > 50:
            row += "."
        elif brightness > 10:
            row += ","
        else:
            row += " "
    print(row)

# Check decoded CLUT area at y=512 (our decoded texture storage)
# Wait - y=512 is outside the 1024x512 image
# Let's check if there's anything in the texture page upload area
print("\n=== Full VRAM non-black pixel count by region ===")
regions = {
    "FB0 (0,0)-(640,256)": (0, 0, 640, 256),
    "FB1 (0,256)-(640,512)": (0, 256, 640, 512),
    "Tex (640,0)-(1024,256)": (640, 0, 1024, 256),
    "Tex (640,256)-(1024,512)": (640, 256, 1024, 512),
}
for name, (x1, y1, x2, y2) in regions.items():
    count = 0
    for yy in range(y1, y2):
        for xx in range(x1, x2):
            r, g, b, a = img.getpixel((xx, yy))
            if r > 0 or g > 0 or b > 0:
                count += 1
    print(f"  {name}: {count} non-black pixels")
