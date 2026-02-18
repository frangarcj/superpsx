#!/usr/bin/env python3
"""Quick diff category analysis for test results."""
from PIL import Image
import numpy as np

tests = [
    "RenderPolygon",
    "RenderRectangle",
    "RenderLine",
    "RenderTextureRectangle_CLUT4BPP",
    "RenderTexturePolygon_CLUT4BPP",
    "RenderTextureRectangle_15BPP",
    "RenderTexturePolygon_MASK15BPP",
    "RenderTextureWindowRectangle_15BPP",
]

for test in tests:
    ref_path = f"test_results/16bpp/{test}_ref.png"
    got_path = f"test_results/16bpp/{test}_got.png"
    try:
        ref = np.array(Image.open(ref_path).convert('RGB'))
        got = np.array(Image.open(got_path).convert('RGB'))
    except:
        print(f"{test}: file not found")
        continue

    ref5 = ref >> 3
    got5 = got >> 3
    diff = np.any(ref5 != got5, axis=-1)
    total_diff = int(np.sum(diff))

    ref_nonblack = np.any(ref5 != 0, axis=-1)
    got_nonblack = np.any(got5 != 0, axis=-1)
    missing = int(np.sum(diff & ref_nonblack & (~got_nonblack)))
    extra = int(np.sum(diff & (~ref_nonblack) & got_nonblack))
    color_diff = int(np.sum(diff & ref_nonblack & got_nonblack))

    pct = 100.0 * (1 - total_diff / (320 * 224))
    print(f"{test:45s} {pct:6.2f}% | {total_diff:5d} diffs: miss={missing:5d} extra={extra:5d} color={color_diff:5d}")
