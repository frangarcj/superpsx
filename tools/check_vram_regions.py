#!/usr/bin/env python3
"""Check all GPU test reference images for white vs black in unused VRAM regions."""

from PIL import Image
import glob

refs = sorted(glob.glob("tests/gpu/*/vram.png"))

print(f"{'Test':<30} {'Size':<12} {'Right(x>=320,y<240)':<25} {'Below(y>=240,x<320)':<25} {'Right':<10} {'Below':<10}")
print("-" * 120)

for path in refs:
    name = path.split("/")[-2]
    im = Image.open(path).convert("RGB")
    w, h = im.size

    # Right of display (x>=320, y<240)
    right_pixels = []
    for y in range(min(240, h)):
        for x in range(320, w):
            right_pixels.append(im.getpixel((x, y)))

    # Below display (y>=240, x<320)
    below_pixels = []
    for y in range(240, h):
        for x in range(min(320, w)):
            below_pixels.append(im.getpixel((x, y)))

    def mean_rgb(pixels):
        if not pixels:
            return (0, 0, 0)
        r = sum(p[0] for p in pixels) / len(pixels)
        g = sum(p[1] for p in pixels) / len(pixels)
        b = sum(p[2] for p in pixels) / len(pixels)
        return (r, g, b)

    def classify(mean):
        avg = (mean[0] + mean[1] + mean[2]) / 3
        if avg > 200:
            return "WHITE"
        elif avg < 50:
            return "BLACK"
        else:
            return f"MIXED({avg:.0f})"

    rm = mean_rgb(right_pixels)
    bm = mean_rgb(below_pixels)

    print(
        f"{name:<30} {w}x{h:<8} ({rm[0]:6.1f},{rm[1]:6.1f},{rm[2]:6.1f})  ({bm[0]:6.1f},{bm[1]:6.1f},{bm[2]:6.1f})  {classify(rm):<10} {classify(bm):<10}")

print(f"\nTotal reference images: {len(refs)}")
