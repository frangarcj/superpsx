#!/usr/bin/env python3
"""
vram_viewer.py - simple VRAM dump -> PNG converter for PSX VRAM dumps

Usage:
  python3 vram_viewer.py input_vram.bin output.png

Options:
  --width W       VRAM width (default 1024)
  --height H      VRAM height (default 512)
  --format F      pixel ordering: 'bgr' (default) or 'rgb'
  --alpha         treat bit 15 as alpha (1=opaque)
  --endian LE|BE  byte order in file (default LE)

The script expects 16-bit packed pixels (15-bit color, 5:5:5), common for
PSX/VRAM dumps. It expands 5-bit channels to 8-bit and writes an RGBA PNG.
"""

import sys
import argparse
from PIL import Image


def parse_args():
    p = argparse.ArgumentParser(description='Convert PSX VRAM dump to PNG')
    p.add_argument('input', help='input VRAM binary file')
    p.add_argument('output', help='output PNG file')
    p.add_argument('--width', type=int, default=1024,
                   help='image width (default 1024)')
    p.add_argument('--height', type=int, default=512,
                   help='image height (default 512)')
    p.add_argument('--format', choices=['bgr', 'rgb'], default='bgr',
                   help="color order in the 15-bit word (default 'bgr')")
    p.add_argument('--alpha', action='store_true',
                   help='treat bit15 as alpha (1=opaque)')
    p.add_argument('--endian', choices=['le', 'be'], default='le',
                   help='byte order in file (default little-endian)')
    return p.parse_args()


def expand5(v):
    # Expand 5-bit value to 8-bit (simple replicate high bits)
    return (v << 3) | (v >> 2)


def decode_word(w, fmt='bgr', alpha=False):
    if alpha:
        a = 255 if (w & 0x8000) else 0
        pix = w & 0x7FFF
    else:
        a = 255
        pix = w & 0x7FFF

    if fmt == 'bgr':
        b = (pix >> 10) & 0x1F
        g = (pix >> 5) & 0x1F
        r = pix & 0x1F
    else:
        r = (pix >> 10) & 0x1F
        g = (pix >> 5) & 0x1F
        b = pix & 0x1F

    return (expand5(r), expand5(g), expand5(b), a)


def main():
    args = parse_args()
    try:
        with open(args.input, 'rb') as f:
            data = f.read()
    except Exception as e:
        print('ERROR reading input:', e, file=sys.stderr)
        sys.exit(2)

    expected = args.width * args.height * 2
    if len(data) < expected:
        print(
            f'WARNING: input size {len(data)} smaller than expected {expected}', file=sys.stderr)
    if len(data) > expected:
        print(
            f'NOTE: input size {len(data)} larger than expected {expected} — extra bytes will be ignored', file=sys.stderr)

    pixels = []
    order = 'little' if args.endian == 'le' else 'big'

    # iterate by 2 bytes
    for i in range(0, min(len(data), expected), 2):
        w = int.from_bytes(data[i:i+2], order)
        pixels.append(decode_word(w, fmt=args.format, alpha=args.alpha))

    # If file shorter than expected, pad with transparent black
    total = args.width * args.height
    if len(pixels) < total:
        pad = total - len(pixels)
        pixels.extend([(0, 0, 0, 0)] * pad)

    img = Image.new('RGBA', (args.width, args.height))
    img.putdata(pixels[:total])
    try:
        img.save(args.output)
    except Exception as e:
        print('ERROR saving PNG:', e, file=sys.stderr)
        sys.exit(3)

    print(f'Wrote {args.output} ({args.width}x{args.height})')


if __name__ == '__main__':
    main()
