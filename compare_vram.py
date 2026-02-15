#!/usr/bin/env python3
"""Compare VRAM dump with reference image."""
from PIL import Image
import numpy as np
import sys
import os

def load_vram_bin(path):
    """Load raw VRAM dump (1024x512 CT16S) and convert to PIL RGB image."""
    with open(path, 'rb') as f:
        data = f.read()
    print(f'File size: {len(data)} bytes')
    expected_16 = 1024 * 512 * 2  # 1048576
    expected_32 = 1024 * 512 * 4  # 2097152
    if len(data) == expected_32:
        print('Detected 32-bit RGBA dump (legacy)')
        img = Image.frombytes('RGBA', (1024, 512), data)
        return img.convert('RGB')
    elif len(data) == expected_16:
        print('Detected 16-bit CT16S dump')
        pixels = np.frombuffer(data, dtype=np.uint16).reshape((512, 1024))
        r = ((pixels & 0x001F) << 3).astype(np.uint8)
        g = (((pixels >> 5) & 0x001F) << 3).astype(np.uint8)
        b = (((pixels >> 10) & 0x001F) << 3).astype(np.uint8)
        rgb = np.stack([r, g, b], axis=2)
        return Image.fromarray(rgb, 'RGB')
    else:
        print(f'WARNING: unexpected file size (expected {expected_16} or {expected_32})')
        return None

def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else 'vram_5000000.bin'
    ref_path = 'tests/gpu/triangle/vram.png'
    
    if not os.path.exists(dump_path):
        print(f'ERROR: {dump_path} not found')
        return
    
    img = load_vram_bin(dump_path)
    if not img:
        return
    
    img_rgb = img
    
    # Sample key pixels
    samples = [
        ('BG(5,5)', 5, 5),
        ('Center(160,120)', 160, 120),
        ('V0-Red(45,220)', 45, 220),
        ('V1-Green(275,220)', 275, 220),
        ('V2-Blue(160,20)', 160, 20),
        ('BigTri(768,256)', 768, 256),
        ('BottomTri(160,360)', 160, 360),
    ]
    
    print('\n=== VRAM Dump Pixel Samples ===')
    for name, x, y in samples:
        px = img_rgb.getpixel((x, y))
        print(f'  {name}: RGB={px}')
    
    # Save as PNG
    img_rgb.save('vram_current.png')
    print('\nSaved vram_current.png')
    
    # Load reference
    if not os.path.exists(ref_path):
        print(f'WARNING: {ref_path} not found, skipping comparison')
        return
    
    ref = Image.open(ref_path).convert('RGB')
    print(f'Reference size: {ref.size}')
    
    print('\n=== Reference Pixel Samples ===')
    for name, x, y in samples:
        px = ref.getpixel((x, y))
        print(f'  {name}: RGB={px}')
    
    # Compute difference
    arr_cur = np.array(img_rgb)
    arr_ref = np.array(ref)
    diff = np.abs(arr_cur.astype(int) - arr_ref.astype(int))
    max_diff = diff.max()
    mean_diff = diff.mean()
    nonzero = np.count_nonzero(diff.sum(axis=2))
    total_pixels = 1024 * 512
    
    print(f'\n=== Comparison Results ===')
    print(f'  Max pixel channel diff: {max_diff}')
    print(f'  Mean pixel channel diff: {mean_diff:.2f}')
    print(f'  Differing pixels: {nonzero}/{total_pixels} ({100*nonzero/total_pixels:.1f}%)')
    
    # Break down by region
    # Top-left quadrant (main display 0-320, 0-240)
    tl_diff = diff[:240, :320]
    tl_nonzero = np.count_nonzero(tl_diff.sum(axis=2))
    tl_total = 240 * 320
    print(f'  Main display (0-320, 0-240): {tl_nonzero}/{tl_total} diff ({100*tl_nonzero/tl_total:.1f}%)')
    
    # Right side (big triangle region 518-1023, 0-512)
    rt_diff = diff[:512, 518:]
    rt_nonzero = np.count_nonzero(rt_diff.sum(axis=2))
    rt_total = 512 * (1024-518)
    print(f'  Right region (518-1023): {rt_nonzero}/{rt_total} diff ({100*rt_nonzero/rt_total:.1f}%)')
    
    # Bottom region (dithered triangle 0-320, 240-480)
    bt_diff = diff[240:480, :320]
    bt_nonzero = np.count_nonzero(bt_diff.sum(axis=2))
    bt_total = 240 * 320
    print(f'  Bottom display (0-320, 240-480): {bt_nonzero}/{bt_total} diff ({100*bt_nonzero/bt_total:.1f}%)')
    
    # Save diff image (amplified)
    diff_img = Image.fromarray(np.clip(diff * 4, 0, 255).astype(np.uint8))
    diff_img.save('vram_diff.png')
    print('\nSaved vram_diff.png')

if __name__ == '__main__':
    main()
