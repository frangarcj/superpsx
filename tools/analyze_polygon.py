#!/usr/bin/env python3
"""Analyze polygon rendering differences - focus on RenderPolygon miss/extra patterns."""
import numpy as np
from PIL import Image

# Load both polygon tests
tests = {
    'RenderPolygon': 'test_results/16bpp/RenderPolygon',
    'RenderPolygonClip': 'test_results/16bpp/RenderPolygonClip',
}

for name, path in tests.items():
    try:
        ref = np.array(Image.open(f'{path}_ref.png').convert('RGB')) >> 3
        got = np.array(Image.open(f'{path}_got.png').convert('RGB')) >> 3
    except:
        print(f"Skipping {name} — files not found")
        continue
    
    diff = (ref != got).any(axis=2)
    miss = diff & ((got == 0).all(axis=2)) & ((ref != 0).any(axis=2))
    extra = diff & ((ref == 0).all(axis=2)) & ((got != 0).any(axis=2))
    color = diff & ((ref != 0).any(axis=2)) & ((got != 0).any(axis=2))
    
    print(f"\n=== {name} ===")
    print(f"Total diffs: {diff.sum()} miss={miss.sum()} extra={extra.sum()} color={color.sum()}")
    
    # Analyze miss pixels — are they along polygon edges?
    my, mx = np.where(miss)
    if len(my) > 0:
        print(f"\nMISS pixel sample (first 20):")
        for i in range(min(20, len(my))):
            x, y = mx[i], my[i]
            neighbors = 0
            for dy in [-1, 0, 1]:
                for dx in [-1, 0, 1]:
                    if dy == 0 and dx == 0:
                        continue
                    ny, nx = y+dy, x+dx
                    if 0 <= ny < ref.shape[0] and 0 <= nx < ref.shape[1]:
                        if (ref[ny, nx] != 0).any():
                            neighbors += 1
            print(f"  ({x},{y}): ref={tuple(ref[y,x])} neighbors_with_color={neighbors}")
    
    # Analyze extra pixels
    ey, ex = np.where(extra)
    if len(ey) > 0:
        print(f"\nEXTRA pixel sample (first 20):")
        for i in range(min(20, len(ey))):
            x, y = ex[i], ey[i]
            neighbors = 0
            for dy in [-1, 0, 1]:
                for dx in [-1, 0, 1]:
                    if dy == 0 and dx == 0:
                        continue
                    ny, nx = y+dy, x+dx
                    if 0 <= ny < ref.shape[0] and 0 <= nx < ref.shape[1]:
                        if (ref[ny, nx] != 0).any():
                            neighbors += 1
            print(f"  ({x},{y}): got={tuple(got[y,x])} neighbors_with_ref_color={neighbors}")
    
    # Analyze color diff magnitude
    if color.sum() > 0:
        cy, cx = np.where(color)
        rdiff = np.abs(ref[cy, cx].astype(int) - got[cy, cx].astype(int))
        max_diff = rdiff.max(axis=1)
        print(f"\nCOLOR diff magnitude: mean={max_diff.mean():.1f} median={np.median(max_diff):.0f} max={max_diff.max()}")
        
        # Distribution of max per-channel diff
        bins = [0, 1, 2, 3, 4, 5, 10, 15, 20, 31]
        hist = np.histogram(max_diff, bins=bins)
        print("  Diff distribution (max per-channel per pixel):")
        for i in range(len(bins)-1):
            if hist[0][i] > 0:
                print(f"    {bins[i]}-{bins[i+1]-1}: {hist[0][i]}")
