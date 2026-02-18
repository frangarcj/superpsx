#!/usr/bin/env python3
"""Categorize remaining diffs for the 15BPP texture rect test."""
from PIL import Image
import struct, glob, numpy as np

ref = np.array(Image.open('tests/PSX/GPU/16BPP/RenderTextureRectangle/15BPP/RenderTextureRectangle15BPP.png').convert('RGB'))
vf = sorted(glob.glob('vram_*.bin'))[-1]
data = open(vf, 'rb').read()

got = np.zeros((512, 1024, 3), dtype=np.uint8)
for y in range(512):
    for x in range(1024):
        off = (y * 1024 + x) * 2
        val = struct.unpack('<H', data[off:off+2])[0]
        got[y, x] = [((val) & 0x1F) << 3, ((val >> 5) & 0x1F) << 3, ((val >> 10) & 0x1F) << 3]

gc = got[:224, :320]
r5 = ref >> 3
g5 = gc >> 3

diff = ~np.all(r5 == g5, axis=2)
ys, xs = np.where(diff)

print(f"Total diffs: {len(ys)}")

# Show first 30 diffs
for i in range(min(30, len(ys))):
    y, x = ys[i], xs[i]
    print(f"  ({x:3d},{y:3d}) ref=({r5[y,x,0]:2d},{r5[y,x,1]:2d},{r5[y,x,2]:2d}) got=({g5[y,x,0]:2d},{g5[y,x,1]:2d},{g5[y,x,2]:2d})")

# Categorize
got_zero = np.all(g5[ys, xs] == 0, axis=-1)
ref_nonzero = np.any(r5[ys, xs] != 0, axis=-1)
ref_zero = np.all(r5[ys, xs] == 0, axis=-1)
got_nonzero = np.any(g5[ys, xs] != 0, axis=-1)

missing = (got_zero & ref_nonzero).sum()
extra = (ref_zero & got_nonzero).sum()
wrong_val = (~got_zero & ~ref_zero & ref_nonzero).sum()

print(f"\nMissing (ref!=0, got=0): {missing}")
print(f"Extra (ref=0, got!=0): {extra}")
print(f"Wrong value (both nonzero): {wrong_val}")

# Check got values for "wrong_val" cases
wv_mask = ~got_zero & ~ref_zero & ref_nonzero
if wv_mask.sum() > 0:
    wv_got = g5[ys[wv_mask], xs[wv_mask]]
    wv_ref = r5[ys[wv_mask], xs[wv_mask]]
    print(f"\nWrong-value got R values: {np.unique(wv_got[:, 0])}")
    print(f"Wrong-value ref R values: {np.unique(wv_ref[:, 0])}")
    # Check common patterns
    got9 = (wv_got[:, 0] == 9).sum()
    got21 = (wv_got[:, 0] == 21).sum()
    print(f"Got=9: {got9}, Got=21: {got21}")
