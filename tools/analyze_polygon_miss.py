#!/usr/bin/env python3
"""Analyze missing pixels in TexturePolygon tests — are they at triangle edges?"""
from PIL import Image
import numpy as np

tests = [
    ('RenderTexturePolygon_15BPP', '15BPP'),
    ('RenderTexturePolygon_CLUT4BPP', 'CLUT4'),
    ('RenderTexturePolygon_MASK15BPP', 'MASK'),
]

for name, label in tests:
    try:
        ref = np.array(Image.open(f'test_results/16bpp/{name}_ref.png'))
        got = np.array(Image.open(f'test_results/16bpp/{name}_got.png'))
    except FileNotFoundError:
        print(f"{name}: NOT FOUND")
        continue

    ref5 = ref >> 3
    got5 = got >> 3
    diff_mask = np.any(ref5 != got5, axis=2)
    
    # Categorize
    total = diff_mask.sum()
    miss = 0; extra = 0; color = 0
    
    ref_black = np.all(ref5 == 0, axis=2)
    got_black = np.all(got5 == 0, axis=2)
    
    miss_mask = diff_mask & got_black & ~ref_black
    extra_mask = diff_mask & ref_black & ~got_black
    color_mask = diff_mask & ~miss_mask & ~extra_mask
    
    miss = miss_mask.sum()
    extra = extra_mask.sum()
    color = color_mask.sum()
    
    print(f"\n{'='*60}")
    print(f"{label}: {total} diffs (miss={miss} extra={extra} color={color})")
    
    # Look at spatial distribution of MISSING pixels
    miss_pos = np.argwhere(miss_mask)
    if len(miss_pos) > 0:
        y_min, x_min = miss_pos.min(axis=0)
        y_max, x_max = miss_pos.max(axis=0)
        print(f"  Missing range: x=[{x_min},{x_max}] y=[{y_min},{y_max}]")
        
        # Group by Y to see if they're at edges
        y_counts = {}
        for y, x in miss_pos:
            if y not in y_counts:
                y_counts[y] = []
            y_counts[y].append(x)
        
        print(f"  Missing spans {len(y_counts)} rows")
        
        # Show first 20 rows with missing pixels  
        for i, y in enumerate(sorted(y_counts.keys())[:20]):
            xs = sorted(y_counts[y])
            if len(xs) <= 5:
                x_str = ','.join(str(x) for x in xs)
            else:
                x_str = f"{xs[0]}-{xs[-1]} ({len(xs)} px)"
            # Check if these are edge pixels (adjacent to non-drawn)
            print(f"    row {y:3d}: {x_str}")
    
    # Look at spatial distribution of EXTRA pixels
    extra_pos = np.argwhere(extra_mask)
    if len(extra_pos) > 0:
        y_min, x_min = extra_pos.min(axis=0)
        y_max, x_max = extra_pos.max(axis=0)
        print(f"  Extra range: x=[{x_min},{x_max}] y=[{y_min},{y_max}]")
        
        y_counts_e = {}
        for y, x in extra_pos:
            if y not in y_counts_e:
                y_counts_e[y] = []
            y_counts_e[y].append(x)
        
        for i, y in enumerate(sorted(y_counts_e.keys())[:10]):
            xs = sorted(y_counts_e[y])
            if len(xs) <= 5:
                x_str = ','.join(str(x) for x in xs)
            else:
                x_str = f"{xs[0]}-{xs[-1]} ({len(xs)} px)"
            print(f"    row {y:3d}: {x_str} [EXTRA]")
    
    # COLOR diffs: check delta pattern
    color_pos = np.argwhere(color_mask)
    if len(color_pos) > 0:
        deltas = got5[color_mask].astype(int) - ref5[color_mask].astype(int)
        abs_deltas = np.abs(deltas)
        print(f"  Color delta abs mean: R={abs_deltas[:,0].mean():.2f} G={abs_deltas[:,1].mean():.2f} B={abs_deltas[:,2].mean():.2f}")
        print(f"  Color delta abs max: R={abs_deltas[:,0].max()} G={abs_deltas[:,1].max()} B={abs_deltas[:,2].max()}")
        
        # How many are from semi-transparency (delta=-12 on all channels)?
        is_semitrans = np.all(deltas == -12, axis=1)
        n_semi = is_semitrans.sum()
        print(f"  Semi-trans pattern (delta=-12,-12,-12): {n_semi}/{color}")
