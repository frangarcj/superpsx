#!/usr/bin/env python3
"""Disassemble the vram-to-vram-overlap test to find GP0 commands and block sizes."""
import struct
import sys

exe_path = 'tests/gpu/vram-to-vram-overlap/vram-to-vram-overlap.exe'

with open(exe_path, 'rb') as f:
    data = f.read()

text_start = 0x800
load_addr = 0x80010000


def vaddr(offset):
    return load_addr + offset - text_start if offset >= text_start else offset


# Parse all 32-bit words as MIPS instructions from text section
words = []
for i in range(text_start, len(data), 4):
    w = struct.unpack_from('<I', data, i)[0]
    words.append((i, vaddr(i), w))

# Find lui instructions that load GP0-related high values
print("=== LUI instructions with GPU-related immediates ===")
for off, va, w in words:
    op = (w >> 26) & 0x3F
    if op == 0x0F:  # lui
        rt = (w >> 16) & 0x1F
        imm = w & 0xFFFF
        hi_byte = (imm >> 8) & 0xFF
        if hi_byte in (0x02, 0x20, 0x28, 0x30, 0x38, 0x60, 0x62, 0x64, 0x65, 0x68, 0x6C, 0x74, 0x7C, 0x80, 0xA0, 0xC0, 0xE1, 0xE6):
            print(f"  0x{va:08X}: lui $r{rt}, 0x{imm:04X}")
            # Print next 8 instructions for context
            idx = (off - text_start) // 4
            for j in range(1, 9):
                if idx + j < len(words):
                    o2, va2, w2 = words[idx + j]
                    # Decode some common instructions
                    op2 = (w2 >> 26) & 0x3F
                    rs2 = (w2 >> 21) & 0x1F
                    rt2 = (w2 >> 16) & 0x1F
                    imm2 = w2 & 0xFFFF
                    if imm2 >= 0x8000:
                        imm2_s = imm2 - 0x10000
                    else:
                        imm2_s = imm2
                    if op2 == 0x0D:  # ori
                        print(
                            f"  0x{va2:08X}: ori $r{rt2}, $r{rs2}, 0x{imm2:04X}")
                    elif op2 == 0x09:  # addiu
                        print(
                            f"  0x{va2:08X}: addiu $r{rt2}, $r{rs2}, {imm2_s}")
                    elif op2 == 0x2B:  # sw
                        print(f"  0x{va2:08X}: sw $r{rt2}, {imm2_s}($r{rs2})")
                    elif op2 == 0x0F:  # lui
                        print(f"  0x{va2:08X}: lui $r{rt2}, 0x{imm2:04X}")
                    else:
                        print(f"  0x{va2:08X}: raw 0x{w2:08X} (op={op2})")
            print()

# Also look for data tables - sequences of GP0 command words
print("\n=== Searching for GP0 0x80 (VRAM-to-VRAM copy) command word in data ===")
for i in range(0, len(data), 4):
    w = struct.unpack_from('<I', data, i)[0]
    if (w >> 24) == 0x80 and w != 0x80000000:
        # Could be a stored GP0 command with parameters
        pass
    if w == 0x80000000:
        va = vaddr(i) if i >= text_start else i
        # Print surrounding words
        print(f"\n  0x80000000 at file offset 0x{i:X} (vaddr 0x{va:08X})")
        for j in range(-2, 6):
            off2 = i + j*4
            if 0 <= off2 < len(data) - 3:
                w2 = struct.unpack_from('<I', data, off2)[0]
                va2 = vaddr(off2) if off2 >= text_start else off2
                marker = " <--" if j == 0 else ""
                print(f"    0x{va2:08X}: 0x{w2:08X}{marker}")

# Find where block sizes 15/16 appear as addiu/ori immediates
print("\n=== Instructions with immediate 15 or 16 ===")
for off, va, w in words:
    op = (w >> 26) & 0x3F
    rt = (w >> 16) & 0x1F
    rs = (w >> 21) & 0x1F
    imm = w & 0xFFFF
    if imm >= 0x8000:
        imm_s = imm - 0x10000
    else:
        imm_s = imm
    if abs(imm_s) in (15, 16, 0x0F, 0x10) and op in (0x09, 0x0D, 0x0B, 0x0A):
        op_name = {0x09: 'addiu', 0x0D: 'ori', 0x0B: 'sltiu', 0x0A: 'slti'}[op]
        print(f"  0x{va:08X}: {op_name} $r{rt}, $r{rs}, {imm_s}")

# Find where value 42 (CELL size from analyze script) appears
print("\n=== Instructions with immediate 42 (CELL size) ===")
for off, va, w in words:
    op = (w >> 26) & 0x3F
    rt = (w >> 16) & 0x1F
    rs = (w >> 21) & 0x1F
    imm = w & 0xFFFF
    if imm >= 0x8000:
        imm_s = imm - 0x10000
    else:
        imm_s = imm
    if imm_s == 42 and op in (0x09, 0x0D):
        op_name = {0x09: 'addiu', 0x0D: 'ori'}[op]
        print(f"  0x{va:08X}: {op_name} $r{rt}, $r{rs}, {imm_s}")
