#!/usr/bin/env python3
"""Check dither test pixel values."""
from PIL import Image

got = Image.open("test_results/16bpp/RenderPolygonDither_got.png").convert("RGB")
ref = Image.open("test_results/16bpp/RenderPolygonDither_ref.png").convert("RGB")

print("Non-dithered gradient (row 20):")
for x in range(8, 312, 30):
    rg = got.getpixel((x, 20))
    rr = ref.getpixel((x, 20))
    print(f"  x={x:3d}: got_R={rg[0]:3d}({rg[0]>>3:2d}) ref_R={rr[0]:3d}({rr[0]>>3:2d})")

print("\nDithered gradient (row 50):")
for x in range(8, 312, 30):
    rg = got.getpixel((x, 50))
    rr = ref.getpixel((x, 50))
    print(f"  x={x:3d}: got_R={rg[0]:3d}({rg[0]>>3:2d}) ref_R={rr[0]:3d}({rr[0]>>3:2d})")

print("\nAdjacent dithered pixels (row 50, x=100-107):")
for x in range(100, 108):
    rg = got.getpixel((x, 50))
    rr = ref.getpixel((x, 50))
    print(f"  x={x}: got_R={rg[0]:3d}({rg[0]>>3:2d}) ref_R={rr[0]:3d}({rr[0]>>3:2d})")

print("\nAdjacent dithered pixels (row 51, x=100-107):")
for x in range(100, 108):
    rg = got.getpixel((x, 51))
    rr = ref.getpixel((x, 51))
    print(f"  x={x}: got_R={rg[0]:3d}({rg[0]>>3:2d}) ref_R={rr[0]:3d}({rr[0]>>3:2d})")
