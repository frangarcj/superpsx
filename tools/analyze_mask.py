#!/usr/bin/env python3
"""Analyze remaining diffs in MASK tests."""
from PIL import Image
import numpy as np
import sys

names = [
    'RenderTextureRectangle_MASK15BPP',
    'RenderTextureWindowRectangle_MASK15BPP',
    'RenderTexturePolygon_MASK15BPP',
    'RenderTexturePolygonClip_MASK15BPP',
]

for name in names:
    try:
        ref = np.array(Image.open(f'test_results/16bpp/{name}_ref.png'))
        got = np.array(Image.open(f'test_results/16bpp/{name}_got.png'))
    except FileNotFoundError:
        print(f"\n{name}: FILE NOT FOUND")
        continue
    
    ref5 = ref >> 3
    got5 = got >> 3
    
    diff_mask = np.any(ref5 != got5, axis=2)
    diff_positions = np.argwhere(diff_mask)
    
    print(f"\n{'='*60}")
    print(f"{name}: {len(diff_positions)} diffs")
    print(f"{'='*60}")
    
    if len(diff_positions) == 0:
        continue
    
    # Categorize all diffs
    miss = 0; extra = 0; color = 0
    deltas_r = []; deltas_g = []; deltas_b = []
    
    for y, x in diff_positions:
        r_px = ref5[y, x]
        g_px = got5[y, x]
        r_sum = int(r_px[0]) + int(r_px[1]) + int(r_px[2])
        g_sum = int(g_px[0]) + int(g_px[1]) + int(g_px[2])
        if g_sum == 0 and r_sum > 0:
            miss += 1
        elif r_sum == 0 and g_sum > 0:
            extra += 1
        else:
            color += 1
            deltas_r.append(int(g_px[0]) - int(r_px[0]))
            deltas_g.append(int(g_px[1]) - int(r_px[1]))
            deltas_b.append(int(g_px[2]) - int(r_px[2]))
    
    print(f"  miss={miss} extra={extra} color={color}")
    
    if color > 0:
        deltas_r = np.array(deltas_r)
        deltas_g = np.array(deltas_g)
        deltas_b = np.array(deltas_b)
        print(f"  R delta: min={deltas_r.min()} max={deltas_r.max()} mean={deltas_r.mean():.2f}")
        print(f"  G delta: min={deltas_g.min()} max={deltas_g.max()} mean={deltas_g.mean():.2f}")
        print(f"  B delta: min={deltas_b.min()} max={deltas_b.max()} mean={deltas_b.mean():.2f}")
        
        # Show unique delta patterns
        all_deltas = list(zip(deltas_r.tolist(), deltas_g.tolist(), deltas_b.tolist()))
        from collections import Counter
        patterns = Counter(all_deltas)
        print(f"  Top delta patterns:")
        for pat, cnt in patterns.most_common(15):
            print(f"    delta=({pat[0]:+d},{pat[1]:+d},{pat[2]:+d}): {cnt}")
    
    # Show first 15 diffs with coordinates
    print(f"\n  First 15 diffs:")
    for i, (y, x) in enumerate(diff_positions[:15]):
        r_px = ref5[y, x]
        g_px = got5[y, x]
        r_sum = int(r_px[0]) + int(r_px[1]) + int(r_px[2])
        g_sum = int(g_px[0]) + int(g_px[1]) + int(g_px[2])
        cat = "MISS" if (g_sum == 0 and r_sum > 0) else ("EXTRA" if (r_sum == 0 and g_sum > 0) else "COLOR")
        print(f"    ({x:3d},{y:3d}): ref=({r_px[0]:2d},{r_px[1]:2d},{r_px[2]:2d}) got=({g_px[0]:2d},{g_px[1]:2d},{g_px[2]:2d}) [{cat}]")
