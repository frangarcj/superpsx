#!/usr/bin/env python3
"""
batch_vram_viewer.py - find all vram_*.bin files and convert them to PNGs

Usage:
  python3 tools/batch_vram_viewer.py [dir] [--viewer-args "..."]

Examples:
  python3 tools/batch_vram_viewer.py .
  python3 tools/batch_vram_viewer.py ./superpsx --viewer-args "--alpha --format rgb"

This script calls `tools/vram_viewer.py` for each matching file.
"""

import sys
import os
import glob
import shlex
import subprocess


def find_vrams(path):
    pattern = os.path.join(path, 'vram_*.bin')
    return sorted(glob.glob(pattern))


def make_output_name(inp):
    base = os.path.splitext(os.path.basename(inp))[0]
    return base + '.png'


def main():
    if len(sys.argv) >= 2 and not sys.argv[1].startswith('--'):
        path = sys.argv[1]
        rest = sys.argv[2:]
    else:
        path = '.'
        rest = sys.argv[1:]

    # parse optional --viewer-args "..."
    viewer_args = ''
    if '--viewer-args' in rest:
        i = rest.index('--viewer-args')
        if i+1 < len(rest):
            viewer_args = rest[i+1]
            # remove the pair
            rest.pop(i)
            rest.pop(i)

    vrams = find_vrams(path)
    if not vrams:
        print('No vram_*.bin files found in', os.path.abspath(path))
        return 0

    python = sys.executable or 'python3'
    viewer = os.path.join(os.path.dirname(__file__), 'vram_viewer.py')

    results = []
    for v in vrams:
        out = os.path.join(os.path.dirname(v), make_output_name(v))
        cmd = [python, viewer, v, out]
        if viewer_args:
            cmd += shlex.split(viewer_args)

        print('->', ' '.join(shlex.quote(x) for x in cmd))
        try:
            r = subprocess.run(
                cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            print(r.stdout.strip())
            results.append((v, out, True, r.stdout.strip()))
        except subprocess.CalledProcessError as e:
            print('ERROR running viewer for', v)
            print(e.stderr.strip())
            results.append((v, out, False, e.stderr.strip()))

    # summary
    success = sum(1 for r in results if r[2])
    print(
        f'Processed {len(results)} files, {success} succeeded, {len(results)-success} failed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
