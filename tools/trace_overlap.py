#!/usr/bin/env python3
"""Simulate the vram-to-vram-overlap test's copy loop to extract coordinates."""

# From the binary data table at offset 0x3D78:
sizes = [2, 8, 15, 16, 16, 16, 16]

# The main copy loop (second loop starting at file offset 0x960):
# Reconstructed from MIPS disassembly

# Outer loop: s2 = 0..6 (row index)
# s6 = block size (loaded from sizes[])
# s3 = pointer into sizes array, starts at sizes[1]

# Key observations from disassembly:
# - cell_y_offset starts at 0, increments by 42 each row
# - s1 = cell_y_offset + 4 (source y)
# - s7 = cell_y_offset + 3 (dest y), incremented by 1 in middle loop
# - s0 accumulates offset, starts at 0
# - Inner loop: 8 iterations per column group
# - Middle loop: 3 iterations (s5: 1->8->15->22)
# - sx starts at s0-298, increments by 42
# - dx starts at s0-301, increments by 43

# Additional logic for rows 4,5,6:
# s0_flag = (s2 == 4) -- x overlap flag
# v0_flag = (s2 == 5) -- y overlap flag
# Row 6: both flags set

print("=== VRAM-to-VRAM Copy Commands ===")
print(f"{'Row':>3} {'Label':>6} {'Size':>4}  {'#':>2}  {'sx':>4} {'sy':>4} {'dx':>4} {'dy':>4} {'w':>3} {'h':>3}  offset")

labels = ["BLK2", "BLK8", "BLK15", "BLK16", "BK16X", "BK16Y", "BKXY"]

cell_y_offset = 0  # sp[24], incremented by 42 per row
s0_global = 0
size_idx = 0  # index into sizes for loading after first row

for row in range(7):
    size = sizes[row]

    # Determine overlap flags based on row
    x_overlap = 0  # extra x offset flag
    y_overlap = 0  # extra y offset flag

    if row == 4:
        x_overlap = 1
    elif row == 5:
        y_overlap = 1
    elif row == 6:
        x_overlap = 1
        y_overlap = 1

    # The middle loop: 3 iterations (s5 goes 1, 8, 15 -> terminates at 22)
    s0 = 0
    sy_base = cell_y_offset + 4
    dy_base = cell_y_offset + 3

    copy_num = 0

    for mid in range(3):
        s0 += 344
        s4 = s0 - 298  # sx start
        s8 = s0 - 301  # dx start (3 less than sx)

        # The inner loop: iterate until s8 catches up to s0
        # s8 starts at s0-301 and increments by 43 each iteration
        # 301/43 ~ 7, so about 7-8 iterations
        # Actually s0/43 = 344/43 = 8, so 8 iterations per inner loop

        col = 0
        while s8 != s0:
            sx = s4
            dx = s8
            sy = sy_base
            dy = dy_base + mid  # dy increments by 1 per middle iteration

            # The offsets within the cell:
            # sx - dx = s4 - s8 = (s0-298) - (s0-301) = 3 initially
            # But s4 increments by 42, s8 by 43, so difference shrinks

            offset_x = sx - dx  # how much sx > dx
            offset_y = sy - dy  # always 1 initially, decreases with mid

            print(
                f"{row:3d} {labels[row]:>6} {size:4d}  {copy_num:2d}  {sx:4d} {sy:4d} {dx:4d} {dy:4d} {size:3d} {size:3d}  ox={offset_x:+d} oy={offset_y:+d}")

            s4 += 42
            s8 += 43
            col += 1
            copy_num += 1

        s0 += 294  # from delay slot at 0x9e0

    cell_y_offset += 42

# Let's also figure out what the offsets mean
print("\n=== Summary of offset patterns per row ===")
cell_y_offset = 0
for row in range(7):
    size = sizes[row]
    print(f"\nRow {row} ({labels[row]}, size={size}):")

    s0 = 0
    for mid in range(3):
        s0 += 344
        s4 = s0 - 298
        s8 = s0 - 301

        offsets = []
        while s8 != s0:
            ox = s4 - s8
            offsets.append(ox)
            s4 += 42
            s8 += 43

        # sy_base - (dy_base + mid) = (cell_y+4) - (cell_y+3+mid) = 1-mid
        oy = 1 - mid
        print(f"  mid={mid}: oy={oy:+d}, ox offsets = {offsets}")
        s0 += 294

print("\n=== BLK15 (row 2) specific copy commands ===")
cell_y = 2 * 42  # = 84
size = 15
print(f"Block size: {size}x{size}")
print(f"Source Y base: {cell_y + 4} = {cell_y}+4")
print(f"Dest Y base: {cell_y + 3} = {cell_y}+3")

s0 = 0
for mid in range(3):
    s0 += 344
    s4 = s0 - 298
    s8 = s0 - 301
    dy = cell_y + 3 + mid
    sy = cell_y + 4

    print(f"\n  Middle iteration {mid}: sy={sy}, dy={dy} (oy={sy-dy:+d})")
    col = 0
    while s8 != s0:
        ox = s4 - s8
        print(
            f"    Col {col}: sx={s4}, dx={s8}, ox={ox:+d} -> copy {size}x{size} from ({s4},{sy}) to ({s8},{dy})")

        # Check for actual overlap
        src_x0, src_x1 = s4, s4 + size
        dst_x0, dst_x1 = s8, s8 + size
        src_y0, src_y1 = sy, sy + size
        dst_y0, dst_y1 = dy, dy + size

        x_overlap = max(0, min(src_x1, dst_x1) - max(src_x0, dst_x0))
        y_overlap = max(0, min(src_y1, dst_y1) - max(src_y0, dst_y0))

        if x_overlap > 0 and y_overlap > 0:
            print(f"      ** OVERLAP: {x_overlap}x{y_overlap} pixels")

        s4 += 42
        s8 += 43
        col += 1
    s0 += 294

print("\n=== BLK16 (row 3) specific copy commands for comparison ===")
cell_y = 3 * 42  # = 126
size = 16
print(f"Block size: {size}x{size}")
print(f"Source Y base: {cell_y + 4} = {cell_y}+4")
print(f"Dest Y base: {cell_y + 3} = {cell_y}+3")

s0 = 0
for mid in range(3):
    s0 += 344
    s4 = s0 - 298
    s8 = s0 - 301
    dy = cell_y + 3 + mid
    sy = cell_y + 4

    print(f"\n  Middle iteration {mid}: sy={sy}, dy={dy} (oy={sy-dy:+d})")
    col = 0
    while s8 != s0:
        ox = s4 - s8
        print(
            f"    Col {col}: sx={s4}, dx={s8}, ox={ox:+d} -> copy {size}x{size} from ({s4},{sy}) to ({s8},{dy})")

        src_x0, src_x1 = s4, s4 + size
        dst_x0, dst_x1 = s8, s8 + size
        src_y0, src_y1 = sy, sy + size
        dst_y0, dst_y1 = dy, dy + size

        x_overlap = max(0, min(src_x1, dst_x1) - max(src_x0, dst_x0))
        y_overlap = max(0, min(src_y1, dst_y1) - max(src_y0, dst_y0))

        if x_overlap > 0 and y_overlap > 0:
            print(f"      ** OVERLAP: {x_overlap}x{y_overlap} pixels")

        s4 += 42
        s8 += 43
        col += 1
    s0 += 294
