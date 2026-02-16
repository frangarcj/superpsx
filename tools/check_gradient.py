#!/usr/bin/env python3
"""Check gradient values at self-copy cell in vram-to-vram-overlap test."""
import numpy as np
from PIL import Image

with open('vram_5000000.bin', 'rb') as f:
    raw = f.read()
# Dump is already linear (GS readback deswizzles) - read as 16-bit pixels directly
vram = np.frombuffer(raw, dtype=np.uint16).reshape((512, 1024))

ref = Image.open('tests/gpu/vram-to-vram-overlap/vram.png').convert('RGB')
ref = np.array(ref)

# BLK16 self-copy cell at (466, 130) - off=(0,0) means self-copy = no-op
bx, by = 466, 130
print('=== BLK16 self-copy cell at (%d,%d) ===' % (bx, by))
print('Expected: gradient pixel(x,y) = y*64 + x*2')
print()

wrong = 0
for y in range(16):
    for x in range(16):
        p = int(vram[by+y, bx+x])
        expected = y * 64 + x * 2
        if p != expected:
            wrong += 1
print('Wrong pixels in our dump: %d / 256' % wrong)

# Show first 4 rows of actual vs expected
print('\nFirst 4 rows of our dump (hex pixel values):')
for y in range(4):
    vals = ['%04X' % int(vram[by+y, bx+x]) for x in range(16)]
    print('  y=%d: %s' % (y, ' '.join(vals)))

print('\nFirst 4 rows of expected gradient:')
for y in range(4):
    vals = ['%04X' % (y*64+x*2) for x in range(16)]
    print('  y=%d: %s' % (y, ' '.join(vals)))

# Also check reference: should match gradient since self-copy is no-op
print('\nFirst 4 rows of reference (raw from PNG):')
for y in range(4):
    parts = []
    for x in range(16):
        r, g, b = ref[by+y, bx+x]
        # Convert RGB back to 15-bit pixel
        p = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        parts.append('%04X' % p)
    print('  y=%d: %s' % (y, ' '.join(parts)))

# Check: does our dump match reference? (diff already reported as 256)
print('\n=== Reference vs gradient match ===')
ref_wrong = 0
for y in range(16):
    for x in range(16):
        r, g, b = ref[by+y, bx+x]
        p = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        expected = y * 64 + x * 2
        if p != expected:
            ref_wrong += 1
print('Reference pixels not matching gradient: %d / 256' % ref_wrong)

# Try another cell: BLK2, off=(0,0), i=11, at (466, 4)
bx2, by2 = 466, 4
print('\n=== BLK2 self-copy at (%d,%d), size 2x2 ===' % (bx2, by2))
for y in range(2):
    for x in range(2):
        p_cur = int(vram[by2+y, bx2+x])
        expected = y * 64 + x * 2
        r_ref, g_ref, b_ref = ref[by2+y, bx2+x]
        p_ref = (r_ref >> 3) | ((g_ref >> 3) << 5) | ((b_ref >> 3) << 10)
        status = ''
        if p_cur == expected:
            status = 'OK'
        elif p_ref == expected:
            status = 'REF_OK, CUR_WRONG'
        else:
            status = 'BOTH_WRONG'
        print('  (%d,%d): cur=0x%04X exp=0x%04X ref=0x%04X %s' %
              (x, y, p_cur, expected, p_ref, status))

# Check overall: how much of the gradient was written correctly?
print('\n=== BLK16 row-by-row pixel match analysis ===')
for y in range(16):
    match = 0
    for x in range(16):
        p = int(vram[by+y, bx+x])
        expected = y * 64 + x * 2
        if p == expected:
            match += 1
    print('  Row %2d: %2d/16 match' % (y, match))
