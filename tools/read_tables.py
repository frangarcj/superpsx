#!/usr/bin/env python3
"""Read data tables from vram-to-vram-overlap binary."""
import struct

with open('tests/gpu/vram-to-vram-overlap/vram-to-vram-overlap.exe', 'rb') as f:
    data = f.read()

text_start = 0x800
load_addr = 0x80010000


def file_off(vaddr_offset):
    """Convert offset from load_addr to file offset."""
    return vaddr_offset + text_start


# Table at addiu v0, 0x3D78 (15736)
print("=== Data table at offset 15736 (0x3D78 from load_addr) ===")
base = file_off(0x3D78)
for i in range(8):
    if base + i*4 + 4 <= len(data):
        val = struct.unpack_from('<I', data, base + i*4)[0]
        print("  [%d] = %d (0x%08X)" % (i, val, val))

# Table at 15740 (0x3D7C)
print("\n=== Data table at offset 15740 (0x3D7C) ===")
base = file_off(0x3D7C)
for i in range(8):
    if base + i*4 + 4 <= len(data):
        val = struct.unpack_from('<I', data, base + i*4)[0]
        print("  [%d] = %d (0x%08X)" % (i, val, val))

# String at 15708 (0x3D5C)
print("\n=== String at offset 15708 (0x3D5C) ===")
base = file_off(0x3D5C)
s = data[base:base+60]
# Find null terminator
idx = s.find(b'\x00')
if idx > 0:
    print("  '%s'" % s[:idx].decode('ascii', errors='replace'))
print("  raw:", s[:40].hex())

# Data at 15688 (0x3D48)
print("\n=== Data at offset 15688 (0x3D48) ===")
base = file_off(0x3D48)
for i in range(12):
    if base + i*4 + 4 <= len(data):
        val = struct.unpack_from('<I', data, base + i*4)[0]
        print("  [%d] offset=0x%X val=%d (0x%08X)" %
              (i, 0x3D48 + i*4, val, val))

# Data at 15692 (0x3D4C)
print("\n=== Data at offset 15692 (0x3D4C) ===")
base = file_off(0x3D4C)
for i in range(8):
    if base + i*4 + 4 <= len(data):
        val = struct.unpack_from('<I', data, base + i*4)[0]
        print("  [%d] = %d (0x%08X)" % (i, val, val))

# Let's dump the entire data section to find block size arrays
# Looking for sequences like {2, 8, 15, 16, ...}
print("\n=== Searching for block size sequences ===")
for off in range(text_start, len(data) - 28, 4):
    vals = [struct.unpack_from('<I', data, off + i*4)[0] for i in range(7)]
    if vals[0] == 2 and vals[1] == 8 and vals[2] == 15:
        print("  Found at file offset 0x%X: %s" % (off, vals))
    # Also check for them as 16-bit
for off in range(text_start, len(data) - 14, 2):
    vals = [struct.unpack_from('<H', data, off + i*2)[0] for i in range(7)]
    if vals[0] == 2 and vals[1] == 8 and vals[2] == 15:
        print("  Found (16-bit) at file offset 0x%X: %s" % (off, vals))
