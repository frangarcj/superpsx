#!/usr/bin/env python3
"""Check specific pixel positions to isolate rendering issues."""
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

# Check 32x32 raw rect at (160,8) - non-overlapping with flip rect (x<168)
print("=== Raw rect (160,8) 32x32 - NON-overlap region (x=160-167) ===")
for row in range(32):
    ry = 8 + row
    vals = list(g5[ry, 160:168, 0])
    rvals = list(r5[ry, 160:168, 0])
    match = "OK" if vals == rvals else f"DIFF"
    print(f"  Row {row:2d}: got={vals} ref={rvals} {match}")

# Check overlap region at specific rows
print("\n=== Overlap region x=168-191 ===")
for row in [0, 4, 8, 12, 16, 20, 24, 28, 31]:
    ry = 8 + row
    got_row = list(g5[ry, 168:192, 0])
    ref_row = list(r5[ry, 168:192, 0])
    match_count = sum(1 for g, r in zip(got_row, ref_row) if g == r)
    print(f"  Row {row:2d} (y={ry}): got={got_row}")
    print(f"           ref={ref_row}  match={match_count}/24")

# Check VRAM texture rows
print("\n=== VRAM texture 32x32 at (536,0) ===")
for row in [0, 4, 8, 12, 16, 20, 24, 28, 31]:
    vals = []
    for x in range(536, 568):
        off = (row * 1024 + x) * 2
        val = struct.unpack('<H', data[off:off+2])[0]
        r = (val & 0x1F)
        vals.append(r)
    nonzero = sum(1 for v in vals if v > 0)
    print(f"  Row {row:2d}: {vals}  ({nonzero} non-zero)")
