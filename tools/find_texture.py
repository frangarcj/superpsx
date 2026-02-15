#!/usr/bin/env python3
"""Find texture location in VRAM."""
from PIL import Image
import numpy as np
import sys


def main():
    ref = np.array(Image.open('tests/gpu/rectangles/vram.png').convert('RGB'))

    # Also load current dump
    dump_path = sys.argv[1] if len(sys.argv) > 1 else 'vram_5000000.bin'
    with open(dump_path, 'rb') as f:
        data = f.read()
    pixels = np.frombuffer(data, dtype=np.uint16).reshape((512, 1024))
    r = ((pixels & 0x001F) << 3).astype(np.uint8)
    g = (((pixels >> 5) & 0x001F) << 3).astype(np.uint8)
    b = (((pixels >> 10) & 0x001F) << 3).astype(np.uint8)
    cur = np.stack([r, g, b], axis=2)

    print('=== Texture hunt in reference VRAM ===')
    regions = [
        ('Display 320x240', 0, 0, 320, 240),
        ('Right 320-640 top', 320, 0, 640, 128),
        ('Right 640-768 top', 640, 0, 768, 128),
        ('Right 768-896 top', 768, 0, 896, 128),
        ('Right 896-1024 top', 896, 0, 1024, 128),
        ('Right 320-640 mid', 320, 128, 640, 256),
        ('Right 640-1024 mid', 640, 128, 1024, 256),
        ('Bottom 0-512', 0, 256, 512, 512),
        ('Bottom 512-1024', 512, 256, 1024, 512),
    ]

    for name, x0, y0, x1, y1 in regions:
        region = ref[y0:y1, x0:x1]
        white = np.all(region == [248, 248, 248], axis=2)
        black = np.all(region == [0, 0, 0], axis=2)
        n_white = np.sum(white)
        n_black = np.sum(black)
        total = (y1-y0) * (x1-x0)
        n_other = total - n_white - n_black
        if n_other > 0:
            avg = np.mean(region[~white & ~black],
                          axis=0) if n_other > 0 else [0, 0, 0]
            print(
                f'  {name}: {n_other}/{total} colored, {n_white} white, {n_black} black, avg=({avg[0]:.0f},{avg[1]:.0f},{avg[2]:.0f})')
        else:
            print(f'  {name}: {n_white} white, {n_black} black')

    print('\n=== Same regions in current VRAM ===')
    for name, x0, y0, x1, y1 in regions:
        region = cur[y0:y1, x0:x1]
        white = np.all(region == [248, 248, 248], axis=2)
        black = np.all(region == [0, 0, 0], axis=2)
        n_white = np.sum(white)
        n_black = np.sum(black)
        total = (y1-y0) * (x1-x0)
        n_other = total - n_white - n_black
        if n_other > 0:
            avg = np.mean(region[~white & ~black],
                          axis=0) if n_other > 0 else [0, 0, 0]
            print(
                f'  {name}: {n_other}/{total} colored, {n_white} white, {n_black} black, avg=({avg[0]:.0f},{avg[1]:.0f},{avg[2]:.0f})')
        else:
            print(f'  {name}: {n_white} white, {n_black} black')

    # Fine-grained search for Lena: look for 64x64 blocks with high variance
    print('\n=== High-variance 64x64 blocks (likely texture data) ===')
    for y0 in range(0, 512, 64):
        for x0 in range(0, 1024, 64):
            block_ref = ref[y0:y0+64, x0:x0+64].astype(float)
            var_ref = np.var(block_ref)
            block_cur = cur[y0:y0+64, x0:x0+64].astype(float)
            var_cur = np.var(block_cur)
            if var_ref > 1000:
                # Check if current also has variance there
                print(
                    f'  Ref block ({x0},{y0}): var={var_ref:.0f}, avg={np.mean(block_ref):.0f} | Cur: var={var_cur:.0f}, avg={np.mean(block_cur):.0f}')


if __name__ == '__main__':
    main()
