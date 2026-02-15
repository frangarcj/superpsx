#!/usr/bin/env python3
"""Analyze rectangle test VRAM diffs in detail."""
from PIL import Image
import numpy as np
import sys


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else 'vram_5000000.bin'
    ref_path = 'tests/gpu/rectangles/vram.png'

    # Load dump
    with open(dump, 'rb') as f:
        data = f.read()
    pixels = np.frombuffer(data, dtype=np.uint16).reshape((512, 1024))
    r = ((pixels & 0x001F) << 3).astype(np.uint8)
    g = (((pixels >> 5) & 0x001F) << 3).astype(np.uint8)
    b = (((pixels >> 10) & 0x001F) << 3).astype(np.uint8)
    cur = np.stack([r, g, b], axis=2)

    ref = np.array(Image.open(ref_path).convert('RGB'))
    diff = np.abs(cur.astype(int) - ref.astype(int))
    maxdiff = diff.max(axis=2)

    rows, cols = np.where(maxdiff > 0)
    if len(rows) > 0:
        print(
            f'Diff bounding box: x=[{cols.min()},{cols.max()}] y=[{rows.min()},{rows.max()}]')

    total_diff = np.count_nonzero(maxdiff)
    total_pixels = 1024 * 512
    match_pct = 100.0 * (1 - total_diff / total_pixels)
    print(
        f'Total diff: {total_diff}/{total_pixels} ({100*total_diff/total_pixels:.1f}%)')
    print(f'Match: {match_pct:.1f}%')

    # The test draws 4 groups of rectangles at y offsets 0, 64, 128, 192
    # Each group has 2 rows (y=0,20 within group) of 16 columns (x=0..15*20)
    # Rectangle type = 0x60 + row_in_group*16 + col

    print('\nDetailed grid analysis:')
    for group in range(4):
        base_y = group * 64
        print(f'\n--- Group {group} (y_offset={base_y}) ---')
        for row in range(2):
            for col in range(16):
                rect_type = 0x60 + row * 16 + col
                gx = col * 20
                gy = base_y + row * 20
                cell = maxdiff[gy:gy+20, gx:gx+20]
                nc = np.count_nonzero(cell)
                if nc > 0:
                    mc = cell.max()
                    # Find the max diff pixel
                    cy, cx = np.unravel_index(cell.argmax(), cell.shape)
                    curpx = tuple(cur[gy+cy, gx+cx])
                    refpx = tuple(ref[gy+cy, gx+cx])

                    is_textured = (rect_type & 0x04) != 0
                    is_semi = (rect_type & 0x02) != 0
                    is_raw = (rect_type & 0x01) != 0
                    size_mode = (rect_type >> 3) & 3
                    sizes = ['Var', '1x1', '8x8', '16x16']

                    flags = []
                    if is_textured:
                        flags.append('Tex')
                    if is_semi:
                        flags.append('Semi')
                    if is_raw:
                        flags.append('Raw')
                    if not flags:
                        flags.append('Flat')

                    print(
                        f'  0x{rect_type:02X} [{sizes[size_mode]:5s}] {",".join(flags):12s} @ ({gx},{gy}): {nc:3d}px diff, max={mc:3d}, cur={curpx} ref={refpx}')

    # Summary by type
    print('\n=== Summary by rectangle feature ===')
    categories = {
        'Flat only': lambda t: not (t & 0x04) and not (t & 0x02) and not (t & 0x01),
        'Textured': lambda t: bool(t & 0x04),
        'Semi-trans': lambda t: bool(t & 0x02),
        'Raw texture': lambda t: bool(t & 0x01) and bool(t & 0x04),
        'Bit0 only (no tex)': lambda t: bool(t & 0x01) and not (t & 0x04),
    }

    for cat_name, pred in categories.items():
        cat_diff = 0
        cat_total = 0
        for group in range(4):
            base_y = group * 64
            for row in range(2):
                for col in range(16):
                    rect_type = 0x60 + row * 16 + col
                    if pred(rect_type):
                        gx = col * 20
                        gy = base_y + row * 20
                        cell = maxdiff[gy:gy+20, gx:gx+20]
                        cat_diff += np.count_nonzero(cell)
                        cat_total += 20 * 20
        if cat_total > 0:
            print(
                f'  {cat_name:25s}: {cat_diff:5d}/{cat_total} diff ({100*cat_diff/cat_total:.1f}%)')


if __name__ == '__main__':
    main()
