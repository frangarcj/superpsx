VRAM Viewer
===============

Small helper to convert PSX VRAM dumps (raw 16-bit little-endian) to PNG.

Usage example:

```bash
python3 tools/vram_viewer.py vram_5000000.bin vram.png
```

Options:
- `--width` / `--height` : override dimensions (default 1024x512)
- `--format` : `bgr` (default) or `rgb` bit ordering in 15-bit word
- `--alpha` : treat high bit (bit15) as alpha
- `--endian` : `le` or `be` (default `le`)

This is a convenience tool used during GPU test debugging.
