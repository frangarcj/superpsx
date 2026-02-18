#!/usr/bin/env python3
"""Full error breakdown for all 16BPP tests."""
from PIL import Image
import numpy as np
import os

test_dir = 'test_results/16bpp'
results = []

for f in sorted(os.listdir(test_dir)):
    if not f.endswith('_ref.png'):
        continue
    name = f.replace('_ref.png', '')
    ref_path = os.path.join(test_dir, f'{name}_ref.png')
    got_path = os.path.join(test_dir, f'{name}_got.png')
    if not os.path.exists(got_path):
        continue
    
    ref = np.array(Image.open(ref_path).convert('RGB')) >> 3
    got = np.array(Image.open(got_path).convert('RGB')) >> 3
    diff = np.any(ref != got, axis=2)
    total = diff.sum()
    
    if total == 0:
        results.append((name, 100.0, 0, 0, 0, 0, 0))
        continue
    
    ref_black = np.all(ref == 0, axis=2)
    got_black = np.all(got == 0, axis=2)
    
    miss = (diff & got_black & ~ref_black).sum()
    extra = (diff & ref_black & ~got_black).sum()
    color_mask = diff & ~(diff & got_black & ~ref_black) & ~(diff & ref_black & ~got_black)
    color = color_mask.sum()
    
    # Count semi-trans pattern (-12 across all channels)
    deltas = got[color_mask].astype(int) - ref[color_mask].astype(int)
    deltas5 = (got >> 3)[color_mask].astype(int) - (ref >> 3)[color_mask].astype(int)
    # Actually use the 5-bit values directly
    got5 = got[color_mask]
    ref5 = ref[color_mask]
    delta5 = got5.astype(int) - ref5.astype(int)
    n_semi = np.sum(np.all(delta5 == -12, axis=1))
    
    pct = 100.0 * (1.0 - total / 71680.0)
    results.append((name, pct, total, miss, extra, color, n_semi))

# Sort by percentage (ascending = worst first)
results.sort(key=lambda x: x[1])

print(f"{'Test':<50} {'Score':>7} {'Total':>6} {'Miss':>5} {'Extra':>5} {'Color':>6} {'SemiT':>5}")
print('-' * 90)
for name, pct, total, miss, extra, color, n_semi in results:
    score = f"{pct:.2f}%" if pct < 100 else "100% ✅"
    print(f"{name:<50} {score:>7} {total:>6} {miss:>5} {extra:>5} {color:>6} {n_semi:>5}")
