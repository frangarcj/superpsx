#!/usr/bin/env python3
"""Compare reference and got images at specific texture rectangle positions."""
from PIL import Image
import sys

ref = Image.open('tests/PSX/GPU/16BPP/RenderTextureRectangle/15BPP/RenderTextureRectangle15BPP.png')
got = Image.open('test_results/16bpp/RenderTextureRectangle_15BPP_got.png')

positions = [
    (32, 8, 8, 8, '8x8 raw'),
    (96, 8, 16, 16, '16x16 raw'),
    (160, 8, 32, 32, '32x32 raw'),
    (224, 8, 64, 64, '64x64 raw'),
    (34, 9, 8, 8, '8x8 flip alpha'),
    (100, 10, 16, 16, '16x16 flip alpha'),
    (168, 12, 32, 32, '32x32 flip alpha'),
    (240, 16, 64, 64, '64x64 flip alpha'),
]

for px, py, w, h, name in positions:
    match = 0
    total = 0
    missing = 0
    extra = 0
    color_wrong = 0
    for dy in range(h):
        for dx in range(w):
            x, y = px + dx, py + dy
            if x >= 320 or y >= 224:
                continue
            rp = ref.getpixel((x, y))
            gp = got.getpixel((x, y))
            rr, rg, rb = rp[0] >> 3, rp[1] >> 3, rp[2] >> 3
            gr, gg, gb = gp[0] >> 3, gp[1] >> 3, gp[2] >> 3
            total += 1
            if (rr, rg, rb) == (gr, gg, gb):
                match += 1
            elif (rr, rg, rb) == (0, 0, 0) and (gr, gg, gb) != (0, 0, 0):
                extra += 1
            elif (rr, rg, rb) != (0, 0, 0) and (gr, gg, gb) == (0, 0, 0):
                missing += 1
            else:
                color_wrong += 1
    pct = match * 100.0 / total if total > 0 else 0
    print(f'{name:25s} ({px},{py}) {w}x{h}: {pct:5.1f}% match ({match}/{total}) missing={missing} extra={extra} color_wrong={color_wrong}')
    
# Also check the row 24 cross-section
print('\nRow 24 (mid of 32x32 at 160,8), x=160..192 R values:')
ref_r = [ref.getpixel((x, 24))[0] >> 3 for x in range(160, 192)]
got_r = [got.getpixel((x, 24))[0] >> 3 for x in range(160, 192)]
print(f'ref: {ref_r}')
print(f'got: {got_r}')
