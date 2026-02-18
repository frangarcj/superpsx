#!/usr/bin/env python3
"""Analyze TextureRectangle color diffs — is this all semi-transparency?"""
import numpy as np
from PIL import Image

for name in ['RenderTextureRectangle/15BPP', 'RenderTextureWindowRectangle/15BPP', 'RenderRectangle']:
    sname = name.replace('/', '_')
    try:
        ref = np.array(Image.open(f'test_results/16bpp/{sname}_ref.png').convert('RGB')) >> 3
        got = np.array(Image.open(f'test_results/16bpp/{sname}_got.png').convert('RGB')) >> 3
    except:
        print(f"Skipping {name}")
        continue
    
    diff = (ref != got).any(axis=2)
    color = diff & ((ref != 0).any(axis=2)) & ((got != 0).any(axis=2))
    
    print(f"\n=== {name} ({color.sum()} color diffs) ===")
    
    cy, cx = np.where(color)
    ref_vals = ref[cy, cx]
    got_vals = got[cy, cx]
    delta = got_vals.astype(int) - ref_vals.astype(int)
    
    # Check for common semi-trans patterns
    # Semi-trans mode 0 with FIX=0x58: result is ~0.6875 * (Cs+Cd)
    # vs PSX mode 0: (Cs+Cd)/2 
    
    # Group by unique delta patterns
    from collections import Counter
    delta_tuples = [tuple(d) for d in delta]
    counter = Counter(delta_tuples)
    
    print(f"Top 20 delta patterns (got-ref):")
    for pattern, count in counter.most_common(20):
        print(f"  {pattern}: {count}")
    
    # Also check: for each diff pixel, what's the ref and got color?
    print(f"\nSample ref/got values:")
    indices = np.random.RandomState(42).choice(len(cy), min(20, len(cy)), replace=False)
    for i in sorted(indices):
        y, x = cy[i], cx[i]
        print(f"  ({x},{y}): ref={tuple(ref[y,x])} got={tuple(got[y,x])} delta={tuple(delta[i])}")
