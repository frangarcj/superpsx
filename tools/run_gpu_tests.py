#!/usr/bin/env python3
"""Run GPU tests that have vram.png for 20s and compare VRAM with reference.
Outputs a short RESULT line per test with MATCH percentage.
"""
import subprocess
import time
import os
import glob
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
os.chdir(ROOT)

refs = sorted(glob.glob('tests/gpu/**/vram.png', recursive=True))
if not refs:
    print('No vram.png references found under tests/gpu')
    raise SystemExit(1)

results = []
for ref in refs:
    dirpath = os.path.dirname(ref)
    exefile = None
    for name in os.listdir(dirpath):
        if name.endswith('.exe'):
            exefile = os.path.join(dirpath, name)
            break
    testname = dirpath.replace('tests/gpu/', '')
    print('===TEST:', testname, '===')
    if not exefile:
        print('NO EXE FOUND in', dirpath, 'skipping')
        print()
        results.append((testname, None))
        continue

    # Ensure no running emulator and clean old dumps
    subprocess.run(['pkill', '-f', 'PCSX2'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for f in glob.glob('vram_*.bin'):
        try:
            os.remove(f)
        except OSError:
            pass

    # Start emulator via make run (source env inside bash). Use the full relative path
    # to the exe so `make run` finds the test (was using basename which broke some tests).
    cmd = f"source ../ps2dev_env.sh >/dev/null 2>&1 || true; make run GAMEARGS='{exefile}'"
    p = subprocess.Popen(['bash', '-lc', cmd],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if (exefile.endswith('texture-flip.exe')):
        time.sleep(23)  # texture-flip needs extra time for the X-flip test
    else:
        time.sleep(15)
    # Stop emulator
    subprocess.run(['pkill', '-f', 'PCSX2'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    if os.path.exists('vram_5000000.bin'):
        # Try all available dumps and pick the one with lowest diff
        # (timing variations can cause a mid-frame capture at any specific iteration)
        dumps = sorted(glob.glob('vram_*.bin'))
        best_match = None
        best_diffl = 100.0
        best_dump = None
        best_stdout = ''
        for dump in dumps:
            proc = subprocess.run(['python3', 'tools/compare_vram.py',
                                  dump, ref], capture_output=True, text=True)
            m = re.search(r"Differing pixels:.*\(([0-9.]+)%\)", proc.stdout)
            if m:
                diffl = float(m.group(1))
                if diffl < best_diffl:
                    best_diffl = diffl
                    best_match = round(100.0 - diffl, 1)
                    best_dump = dump
                    best_stdout = proc.stdout
        if best_dump:
            print(best_stdout)
            print(f'RESULT: {testname} MATCH={best_match}% (diff={best_diffl}%) [from {best_dump}]')
            results.append((testname, best_match))
        else:
            print('COMPARE FAILED for', testname)
            results.append((testname, None))
    else:
        print('No vram_5000000.bin produced')
        results.append((testname, None))
    print()

# Summary
print('=== SUMMARY ===')
for name, match in results:
    if match is None:
        print(f'{name}: no result')
    else:
        print(f'{name}: {match}%')
