#!/usr/bin/env python3
"""Run the krom PSX GPU 16BPP tests.

Each test under tests/PSX/GPU/16BPP/ has a .exe (PSX executable) and a .png
(reference screenshot of the 320x224 visible display area).

Workflow per test:
  1. Build superpsx, launch it under PCSX2 with the test .exe as GAMEARGS
  2. Wait for the test to render (it loops forever after drawing)
  3. Kill PCSX2, pick up the VRAM dump (vram_*.bin)
  4. Crop the top-left 320x224 from the full 1024x512 CT16S dump
  5. Compare with the reference PNG in 5-bit (PSX 15-bit) precision
  6. Report pixel-perfect match percentage
"""
import subprocess
import time
import os
import sys
import glob
import re
import shutil
import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
os.chdir(ROOT)

# Display region the reference PNGs cover
REF_W, REF_H = 320, 224

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def load_vram_dump(path):
    """Load a 1024x512 CT16S (16-bit) raw VRAM dump and return an (512,1024,3) uint8 array."""
    data = open(path, 'rb').read()
    if len(data) != 1024 * 512 * 2:
        return None
    pixels = np.frombuffer(data, dtype=np.uint16).reshape((512, 1024))
    r = ((pixels & 0x001F) << 3).astype(np.uint8)
    g = (((pixels >> 5) & 0x001F) << 3).astype(np.uint8)
    b = (((pixels >> 10) & 0x001F) << 3).astype(np.uint8)
    return np.stack([r, g, b], axis=2)


def to_5bit(arr):
    """Quantize an 8-bit RGB array to 5-bit precision (0-31)."""
    return (arr.astype(np.uint16) >> 3).astype(np.uint8)


def compare(dump_crop, ref_arr):
    """Compare two RGB arrays in 5-bit space.  Returns (match%, diff_count, total, diff_img)."""
    d5 = to_5bit(dump_crop)
    r5 = to_5bit(ref_arr)
    diff = (d5 != r5).any(axis=2)   # boolean per-pixel
    diff_count = int(np.count_nonzero(diff))
    total = diff.shape[0] * diff.shape[1]
    match_pct = round(100.0 * (1.0 - diff_count / total), 2)
    # Build visual diff image (amplified)
    abs_diff = np.abs(dump_crop.astype(int) - ref_arr.astype(int))
    diff_img = np.clip(abs_diff * 8, 0, 255).astype(np.uint8)
    return match_pct, diff_count, total, diff_img


def find_tests():
    """Return list of (testname, exe_path, png_path)."""
    tests = []
    base = 'tests/PSX/GPU/16BPP'
    for png in sorted(glob.glob(os.path.join(base, '**', '*.png'), recursive=True)):
        d = os.path.dirname(png)
        exes = glob.glob(os.path.join(d, '*.exe'))
        if not exes:
            continue
        name = os.path.relpath(d, base)
        tests.append((name, exes[0], png))
    return tests


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    single = sys.argv[1] if len(sys.argv) > 1 else None
    tests = find_tests()
    if not tests:
        print('No tests found!')
        return

    if single:
        tests = [(n, e, p) for n, e, p in tests if single.lower() in n.lower()]
        if not tests:
            print(f'No test matching "{single}"')
            return

    os.makedirs('test_results/16bpp', exist_ok=True)
    results = []

    for name, exe, ref_png in tests:
        print(f'\n{"="*60}')
        print(f'TEST: {name}')
        print(f'{"="*60}')

        # Clean old dumps
        for f in glob.glob('vram_*.bin'):
            try:
                os.remove(f)
            except OSError:
                pass

        # Kill any running PCSX2
        subprocess.run(['pkill', '-f', 'PCSX2'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

        # Launch emulator
        cmd = f"source ../ps2dev_env.sh >/dev/null 2>&1 || true; make run GAMEARGS='{exe}'"
        p = subprocess.Popen(['bash', '-lc', cmd],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(5)

        # Stop emulator
        subprocess.run(['pkill', '-f', 'PCSX2'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)

        # Find best VRAM dump
        dumps = sorted(glob.glob('vram_*.bin'))
        if not dumps:
            print('  NO VRAM DUMP produced!')
            results.append((name, None))
            continue

        # Load reference
        ref_img = Image.open(ref_png).convert('RGB')
        ref_arr = np.array(ref_img)
        assert ref_arr.shape == (
            REF_H, REF_W, 3), f'Unexpected ref shape {ref_arr.shape}'

        best_match = -1
        best_info = ''
        best_dump_crop = None
        best_diff_img = None

        for dump in dumps:
            vram = load_vram_dump(dump)
            if vram is None:
                continue
            crop = vram[:REF_H, :REF_W]
            match_pct, diff_count, total, diff_img = compare(crop, ref_arr)
            if match_pct > best_match:
                best_match = match_pct
                best_info = f'{os.path.basename(dump)}: {match_pct}% ({diff_count}/{total} diff)'
                best_dump_crop = crop
                best_diff_img = diff_img

        if best_dump_crop is not None:
            safe_name = name.replace('/', '_')
            Image.fromarray(best_dump_crop).save(
                f'test_results/16bpp/{safe_name}_got.png')
            Image.fromarray(best_diff_img).save(
                f'test_results/16bpp/{safe_name}_diff.png')
            shutil.copy(ref_png, f'test_results/16bpp/{safe_name}_ref.png')
            print(f'  {best_info}')
        else:
            print('  COMPARE FAILED')
            best_match = None

        results.append((name, best_match))

    # Summary
    print(f'\n{"="*60}')
    print('SUMMARY')
    print(f'{"="*60}')
    passed = 0
    for name, match in results:
        if match is None:
            tag = 'NO RESULT'
        elif match == 100.0:
            tag = 'PASS 100%'
            passed += 1
        else:
            tag = f'{match}%'
        print(f'  {name:45s} {tag}')
    print(f'\n  {passed}/{len(results)} tests at 100%')


if __name__ == '__main__':
    main()
