#!/usr/bin/env python3
"""Extract .vutext section from a dvp-as ELF object file.

Usage: extract_vutext.py input.o output.bin

Reads the ELF section headers to find .vutext and extracts
only the program bytes (no ELF overhead or padding)."""

import struct
import sys

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.o output.bin", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    # Parse ELF32 header
    if data[:4] != b'\x7fELF':
        print("Error: not an ELF file", file=sys.stderr)
        sys.exit(1)

    e_shoff = struct.unpack_from('<I', data, 32)[0]
    e_shentsize = struct.unpack_from('<H', data, 46)[0]
    e_shnum = struct.unpack_from('<H', data, 48)[0]
    e_shstrndx = struct.unpack_from('<H', data, 50)[0]

    # Find string table section
    str_sh_offset = struct.unpack_from('<I', data, e_shoff + e_shstrndx * e_shentsize + 16)[0]

    # Find .vutext section
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        sh_name_idx = struct.unpack_from('<I', data, sh_off)[0]
        name = data[str_sh_offset + sh_name_idx:].split(b'\x00', 1)[0].decode()
        if name == '.vutext':
            sh_offset = struct.unpack_from('<I', data, sh_off + 16)[0]
            sh_size = struct.unpack_from('<I', data, sh_off + 20)[0]
            with open(sys.argv[2], 'wb') as out:
                out.write(data[sh_offset:sh_offset + sh_size])
            print(f"Extracted .vutext: {sh_size} bytes ({sh_size // 8} instructions)")
            return

    print("Error: .vutext section not found", file=sys.stderr)
    sys.exit(1)

if __name__ == '__main__':
    main()
