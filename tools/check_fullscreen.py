#!/usr/bin/env python3
"""Full-screen ASCII render of VRAM dump."""
import sys
from PIL import Image

img = Image.open(sys.argv[1])

print(f"=== Full display area (0,0)-(640,480) ===")
print(f"     {''.join([str((x//100) % 10) for x in range(0, 640, 8)])}")
print(f"     {''.join([str((x//10) % 10) for x in range(0, 640, 8)])}")
print(f"     {''.join([str(x % 10) for x in range(0, 640, 8)])}")

for y in range(0, 256, 4):
    row = f"{y:3d}: "
    for x in range(0, 640, 8):
        r, g, b, a = img.getpixel((x, y))
        brightness = (r + g + b) / 3
        if brightness < 5:
            row += " "
        elif brightness < 30:
            row += ","
        elif brightness < 60:
            row += ";"
        elif brightness < 90:
            row += "+"
        elif brightness < 120:
            row += "*"
        elif brightness < 160:
            row += "#"
        elif brightness < 190:
            row += "@"
        else:
            row += "X"
    print(row)

# Also look at specific color analysis of the top area
print("\n=== Top 50 rows detailed color analysis ===")
for y in range(0, 50, 2):
    colors = {}
    for x in range(0, 640, 2):
        r, g, b, a = img.getpixel((x, y))
        key = (r, g, b)
        colors[key] = colors.get(key, 0) + 1
    # Sort by count, show top 3
    top = sorted(colors.items(), key=lambda x: -x[1])[:3]
    desc = ", ".join([f"({c[0][0]},{c[0][1]},{c[0][2]})x{c[1]}" for c in top])
    print(f"  y={y:3d}: {desc}")

# Check the second framebuffer at (0,256)
print("\n=== Second framebuffer Y=256..511 top area ===")
for y in range(256, 310, 2):
    colors = {}
    for x in range(0, 640, 2):
        r, g, b, a = img.getpixel((x, y))
        key = (r, g, b)
        colors[key] = colors.get(key, 0) + 1
    top = sorted(colors.items(), key=lambda x: -x[1])[:3]
    desc = ", ".join([f"({c[0][0]},{c[0][1]},{c[0][2]})x{c[1]}" for c in top])
    print(f"  y={y:3d}: {desc}")
