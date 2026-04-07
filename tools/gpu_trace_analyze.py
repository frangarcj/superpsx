#!/usr/bin/env python3
"""
gpu_trace_analyze.py — Offline analysis of SuperPSX GPU trace dumps

Reads binary dumps produced by the GPU trace ring buffer (ENABLE_GPU_TRACE)
and produces command histograms, state change analysis, and batching metrics.

Binary format:
  Header:  magic("GPTD") | version(u32) | frame_count(u32) | max_words(u32)
  Per frame: frame_id(u32) | word_count(u32) | gp0_data[word_count]

Usage:
  python3 tools/gpu_trace_analyze.py gpu_trace.bin
  python3 tools/gpu_trace_analyze.py gpu_trace.bin --frames 10   # last N frames
  python3 tools/gpu_trace_analyze.py gpu_trace.bin --detail       # per-frame detail
"""
import struct
import sys
import argparse
from collections import Counter, defaultdict

# GP0 command names (top byte → description)
GP0_NAMES = {
    0x00: "NOP",
    0x01: "ClearCache",
    0x02: "FillRect",
    0x1F: "IRQ",
    0x20: "Tri-Flat",        0x22: "Tri-Flat-Semi",
    0x24: "Tri-Tex",         0x25: "Tri-Tex-Raw",
    0x26: "Tri-Tex-Semi",    0x27: "Tri-Tex-Raw-Semi",
    0x28: "Quad-Flat",       0x2A: "Quad-Flat-Semi",
    0x2C: "Quad-Tex",        0x2D: "Quad-Tex-Raw",
    0x2E: "Quad-Tex-Semi",   0x2F: "Quad-Tex-Raw-Semi",
    0x30: "Tri-Shade",       0x32: "Tri-Shade-Semi",
    0x34: "Tri-Shade-Tex",   0x36: "Tri-Shade-Tex-Semi",
    0x38: "Quad-Shade",      0x3A: "Quad-Shade-Semi",
    0x3C: "Quad-Shade-Tex",  0x3E: "Quad-Shade-Tex-Semi",
    0x40: "Line-Flat",       0x42: "Line-Flat-Semi",
    0x48: "Polyline-Flat",   0x4A: "Polyline-Flat-Semi",
    0x50: "Line-Shade",      0x52: "Line-Shade-Semi",
    0x58: "Polyline-Shade",  0x5A: "Polyline-Shade-Semi",
    0x60: "Rect-Var-Flat",   0x62: "Rect-Var-Flat-Semi",
    0x64: "Rect-Var-Tex",    0x65: "Rect-Var-Tex-Raw",
    0x66: "Rect-Var-Tex-Semi", 0x67: "Rect-Var-Tex-Raw-Semi",
    0x68: "Rect-1x1-Flat",   0x6A: "Rect-1x1-Flat-Semi",
    0x6C: "Rect-1x1-Tex",    0x6D: "Rect-1x1-Tex-Raw",
    0x70: "Rect-8x8-Flat",   0x72: "Rect-8x8-Flat-Semi",
    0x74: "Rect-8x8-Tex",    0x75: "Rect-8x8-Tex-Raw",
    0x78: "Rect-16x16-Flat",  0x7A: "Rect-16x16-Flat-Semi",
    0x7C: "Rect-16x16-Tex",   0x7D: "Rect-16x16-Tex-Raw",
    0x80: "VRAM-Copy",
    0xA0: "LoadImage",
    0xC0: "StoreImage",
    0xE1: "DrawMode",
    0xE2: "TexWindow",
    0xE3: "DrawAreaTL",
    0xE4: "DrawAreaBR",
    0xE5: "DrawOffset",
    0xE6: "MaskBit",
}

# GP0 command sizes (same as gpu_cmd_size[] in C)
GP0_SIZE = {}
for i in range(256):
    GP0_SIZE[i] = 1  # default
GP0_SIZE[0x02] = 3
for b in range(0x20, 0x28):
    GP0_SIZE[b] = 4
for b in range(0x28, 0x30):
    GP0_SIZE[b] = 5 if (b & 4) == 0 else 7  # quad = +2 over tri
for b in range(0x24, 0x28):
    GP0_SIZE[b] = 7
for b in range(0x2C, 0x30):
    GP0_SIZE[b] = 9
# Shaded polygons
GP0_SIZE[0x30] = 6; GP0_SIZE[0x31] = 6; GP0_SIZE[0x32] = 6; GP0_SIZE[0x33] = 6
GP0_SIZE[0x34] = 9; GP0_SIZE[0x35] = 9; GP0_SIZE[0x36] = 9; GP0_SIZE[0x37] = 9
GP0_SIZE[0x38] = 8; GP0_SIZE[0x39] = 8; GP0_SIZE[0x3A] = 8; GP0_SIZE[0x3B] = 8
GP0_SIZE[0x3C] = 12; GP0_SIZE[0x3D] = 12; GP0_SIZE[0x3E] = 12; GP0_SIZE[0x3F] = 12
# Lines
for b in range(0x40, 0x48): GP0_SIZE[b] = 3
for b in range(0x48, 0x50): GP0_SIZE[b] = 0  # polyline = variable
for b in range(0x50, 0x58): GP0_SIZE[b] = 4
for b in range(0x58, 0x60): GP0_SIZE[b] = 0  # polyline = variable
# Rectangles
for b in range(0x60, 0x64): GP0_SIZE[b] = 3
for b in range(0x64, 0x68): GP0_SIZE[b] = 4
for b in range(0x68, 0x6C): GP0_SIZE[b] = 2
for b in range(0x6C, 0x70): GP0_SIZE[b] = 3
for b in range(0x70, 0x74): GP0_SIZE[b] = 2
for b in range(0x74, 0x78): GP0_SIZE[b] = 3
for b in range(0x78, 0x7C): GP0_SIZE[b] = 2
for b in range(0x7C, 0x80): GP0_SIZE[b] = 3
# VRAM ops
for b in range(0x80, 0xA0): GP0_SIZE[b] = 4
for b in range(0xA0, 0xB0): GP0_SIZE[b] = 0  # LoadImage = variable
for b in range(0xC0, 0xD0): GP0_SIZE[b] = 0  # StoreImage = variable
# Quads: correct sizes
for b in [0x28, 0x29, 0x2A, 0x2B]: GP0_SIZE[b] = 5
for b in [0x2C, 0x2D, 0x2E, 0x2F]: GP0_SIZE[b] = 9


def cmd_name(byte):
    """Return human-readable command name."""
    if byte in GP0_NAMES:
        return GP0_NAMES[byte]
    # Expand ranges
    if 0x80 <= byte <= 0x9F: return "VRAM-Copy"
    if 0xA0 <= byte <= 0xAF: return "LoadImage"
    if 0xC0 <= byte <= 0xCF: return "StoreImage"
    return f"Unknown-{byte:02X}"


def is_draw_cmd(byte):
    """Check if this is a rendering command (0x20-0x7F)."""
    return 0x20 <= byte <= 0x7F


def is_env_cmd(byte):
    """Check if this is an environment/state command (0xE1-0xE6)."""
    return 0xE1 <= byte <= 0xE6


def parse_trace(path):
    """Parse a GPU trace binary file into structured frame data."""
    with open(path, "rb") as f:
        data = f.read()

    # Parse header
    magic, version, frame_count, max_words = struct.unpack_from("<IIII", data, 0)
    if magic != 0x44545047:
        print(f"ERROR: bad magic 0x{magic:08X} (expected GPTD/0x44545047)")
        sys.exit(1)
    if version != 1:
        print(f"WARNING: unknown version {version}")

    frames = []
    offset = 16
    for i in range(frame_count):
        frame_id, word_count = struct.unpack_from("<II", data, offset)
        offset += 8
        if word_count > 0:
            words = struct.unpack_from(f"<{word_count}I", data, offset)
            offset += word_count * 4
        else:
            words = ()
        frames.append({"id": frame_id, "words": words})

    return frames, frame_count, max_words


def parse_commands(words):
    """Parse a stream of GP0 words into individual commands."""
    commands = []
    i = 0
    n = len(words)
    while i < n:
        w = words[i]
        cmd_byte = (w >> 24) & 0xFF
        size = GP0_SIZE.get(cmd_byte, 1)

        if size == 0:
            # Variable-length: LoadImage or polyline
            if 0xA0 <= cmd_byte <= 0xAF and i + 3 <= n:
                dims = words[i + 2]
                w_px = dims & 0xFFFF
                h_px = dims >> 16
                if w_px == 0: w_px = 1024
                if h_px == 0: h_px = 512
                img_words = (w_px * h_px + 1) // 2
                total = 3 + img_words
                commands.append({"cmd": cmd_byte, "words": words[i:i+min(total, n-i)], "size": total})
                i += min(total, n - i)
                continue
            elif 0xC0 <= cmd_byte <= 0xCF:
                commands.append({"cmd": cmd_byte, "words": words[i:i+min(3, n-i)], "size": 3})
                i += min(3, n - i)
                continue
            else:
                # Polyline — scan for terminator
                commands.append({"cmd": cmd_byte, "words": (w,), "size": 1})
                i += 1
                continue

        end = min(i + size, n)
        commands.append({"cmd": cmd_byte, "words": words[i:end], "size": size})
        i += size

    return commands


def extract_texpage(e1_word):
    """Extract texture page info from E1 word."""
    tp_x = (e1_word & 0xF) * 64
    tp_y = ((e1_word >> 4) & 1) * 256
    fmt = (e1_word >> 7) & 3
    fmt_name = ["4bpp", "8bpp", "15bpp", "15bpp"][fmt]
    semi = (e1_word >> 5) & 3
    return tp_x, tp_y, fmt_name, semi


def analyze(frames, show_detail=False, last_n=None):
    """Analyze parsed frames and print report."""
    # Filter to non-empty frames
    active = [f for f in frames if len(f["words"]) > 0]
    if last_n and last_n < len(active):
        active = active[-last_n:]

    if not active:
        print("No frames with data found.")
        return

    print(f"{'='*60}")
    print(f"GPU Trace Analysis")
    print(f"{'='*60}")
    print(f"Frames with data: {len(active)}")
    print(f"Frame range: #{active[0]['id']} — #{active[-1]['id']}")
    print()

    # Aggregate stats
    total_cmd_count = Counter()
    total_words = 0
    total_draws = 0
    total_env_changes = 0
    texpage_usage = Counter()
    e1_changes = 0
    batch_runs = []  # consecutive same-type draw runs
    per_frame_stats = []

    for frame in active:
        words = frame["words"]
        total_words += len(words)
        cmds = parse_commands(words)

        frame_cmd_count = Counter()
        frame_draws = 0
        frame_env = 0
        prev_draw_type = None
        cur_run = 0

        for c in cmds:
            cb = c["cmd"]
            frame_cmd_count[cb] += 1
            total_cmd_count[cb] += 1

            if is_draw_cmd(cb):
                frame_draws += 1
                total_draws += 1
                if cb == prev_draw_type:
                    cur_run += 1
                else:
                    if cur_run > 0:
                        batch_runs.append(cur_run)
                    cur_run = 1
                    prev_draw_type = cb
            elif is_env_cmd(cb):
                frame_env += 1
                total_env_changes += 1
                if cb == 0xE1:
                    e1_changes += 1
                    tp_x, tp_y, fmt, semi = extract_texpage(c["words"][0])
                    texpage_usage[(tp_x, tp_y, fmt)] += 1

        if cur_run > 0:
            batch_runs.append(cur_run)

        per_frame_stats.append({
            "id": frame["id"],
            "words": len(words),
            "cmds": sum(frame_cmd_count.values()),
            "draws": frame_draws,
            "env": frame_env,
        })

    # ── Summary ──
    print(f"Total GP0 words: {total_words:,} ({total_words*4/1024:.1f} KB)")
    avg_words = total_words / len(active)
    print(f"Average per frame: {avg_words:.0f} words ({avg_words*4/1024:.1f} KB)")
    print(f"Total draw commands: {total_draws:,}")
    print(f"Total env changes: {total_env_changes:,}")
    print()

    # ── Command histogram ──
    print(f"{'─'*60}")
    print(f"Command Histogram (top 25)")
    print(f"{'─'*60}")
    print(f"  {'Cmd':>5}  {'Count':>7}  {'%':>6}  Name")
    total_cmds = sum(total_cmd_count.values())
    for cb, count in total_cmd_count.most_common(25):
        pct = count * 100.0 / total_cmds if total_cmds > 0 else 0
        print(f"  {cb:02X}h    {count:>7}  {pct:5.1f}%  {cmd_name(cb)}")
    print()

    # ── Draw type breakdown ──
    draw_counts = {cb: c for cb, c in total_cmd_count.items() if is_draw_cmd(cb)}
    if draw_counts:
        print(f"{'─'*60}")
        print(f"Draw Command Breakdown")
        print(f"{'─'*60}")
        textured = sum(c for cb, c in draw_counts.items() if cb & 0x04)
        flat = sum(c for cb, c in draw_counts.items() if not (cb & 0x04) and cb < 0x60)
        rects = sum(c for cb, c in draw_counts.items() if 0x60 <= cb <= 0x7F)
        lines = sum(c for cb, c in draw_counts.items() if 0x40 <= cb <= 0x5F)
        print(f"  Textured polys:  {textured}")
        print(f"  Flat polys:      {flat}")
        print(f"  Rectangles:      {rects}")
        print(f"  Lines:           {lines}")
        print()

    # ── State change analysis ──
    env_counts = {cb: c for cb, c in total_cmd_count.items() if is_env_cmd(cb)}
    if env_counts:
        print(f"{'─'*60}")
        print(f"State Change Analysis")
        print(f"{'─'*60}")
        for cb in sorted(env_counts.keys()):
            per_frame = env_counts[cb] / len(active)
            print(f"  {cmd_name(cb):12s} (E{cb-0xE0}): {env_counts[cb]:>5}  ({per_frame:.1f}/frame)")
        print()

    # ── Texture page usage ──
    if texpage_usage:
        print(f"{'─'*60}")
        print(f"Texture Page Usage (from E1 commands)")
        print(f"{'─'*60}")
        for (tp_x, tp_y, fmt), count in texpage_usage.most_common(20):
            print(f"  TP({tp_x:3d},{tp_y:3d}) {fmt:5s}: {count:>5} E1 switches")
        print()

    # ── Batching analysis ──
    if batch_runs:
        print(f"{'─'*60}")
        print(f"Draw Batching Opportunities")
        print(f"{'─'*60}")
        avg_run = sum(batch_runs) / len(batch_runs)
        max_run = max(batch_runs)
        runs_1 = sum(1 for r in batch_runs if r == 1)
        runs_gt4 = sum(1 for r in batch_runs if r > 4)
        print(f"  Total draw runs: {len(batch_runs)}")
        print(f"  Average run length: {avg_run:.1f}")
        print(f"  Max run length: {max_run}")
        print(f"  Single-draw runs (=1): {runs_1} ({runs_1*100/len(batch_runs):.0f}%)")
        print(f"  Long runs (>4): {runs_gt4}")
        print(f"  → State changes interrupt {runs_1} potential batches")
        print()

    # ── Cross-type batching analysis ──
    # Reparse all frames to compute cross-type runs (ignoring semi-trans bit)
    cross_runs = []
    transition_counts = Counter()  # ALL draw→draw transitions where cmd changes
    for frame in active:
        cmds = parse_commands(frame["words"])
        prev_draw_fmt = None
        prev_draw_cmd = None
        cur_run = 0
        for c in cmds:
            cb = c["cmd"]
            if is_draw_cmd(cb):
                fmt = cb & 0xFD  # strip semi-trans bit (bit 1)
                # Track ALL transitions where draw type changes
                if prev_draw_cmd is not None and cb != prev_draw_cmd:
                    transition_counts[(prev_draw_cmd, cb)] += 1
                if fmt == prev_draw_fmt:
                    cur_run += 1
                else:
                    if cur_run > 0:
                        cross_runs.append(cur_run)
                    cur_run = 1
                    prev_draw_fmt = fmt
                prev_draw_cmd = cb
            elif is_env_cmd(cb):
                pass  # env cmds don't break draws for this analysis
        if cur_run > 0:
            cross_runs.append(cur_run)

    if cross_runs and batch_runs:
        print(f"{'─'*60}")
        print(f"Cross-Type Batching (ignore semi-trans bit)")
        print(f"{'─'*60}")
        cavg = sum(cross_runs) / len(cross_runs)
        cmax = max(cross_runs)
        cruns_1 = sum(1 for r in cross_runs if r == 1)
        cruns_gt4 = sum(1 for r in cross_runs if r > 4)
        saved = len(batch_runs) - len(cross_runs)
        print(f"  Same-type runs:   {len(batch_runs):>5}  (avg {avg_run:.1f}, max {max_run})")
        print(f"  Cross-type runs:  {len(cross_runs):>5}  (avg {cavg:.1f}, max {cmax})")
        print(f"  Runs eliminated:  {saved:>5}  ({saved*100/len(batch_runs):.1f}%)")
        print(f"  Single-draw runs: {cruns_1:>5}  (was {runs_1})")
        print(f"  Long runs (>4):   {cruns_gt4:>5}  (was {runs_gt4})")

        # GIF overhead estimate
        # Each batch = 3 QW overhead (PRIM AD tag 2QW + REGLIST tag 1QW)
        # Each sub-batch in cross-type = same 3 QW
        # Cross-type: fewer batch start/ends = fewer state validations
        same_overhead = len(batch_runs) * 3 * len(active)  # total QW across frames
        # In cross-type, eliminated runs become sub-batches (still need PRIM+REGLIST)
        # But validation overhead is saved on CPU
        print(f"  CPU validation saves: ~{saved * 80:,} cycles/frame ({saved * 80 / 294000:.2f}ms)")
        print()

    # ── Transition matrix ──
    if transition_counts:
        # Only show transitions that break cross-type batching (different format)
        hard_breaks = {k: v for k, v in transition_counts.items()
                       if (k[0] & 0xFD) != (k[1] & 0xFD)}
        soft_breaks = {k: v for k, v in transition_counts.items()
                       if (k[0] & 0xFD) == (k[1] & 0xFD) and k[0] != k[1]}
        if soft_breaks or hard_breaks:
            print(f"{'─'*60}")
            print(f"Draw Type Transitions (per {len(active)} frames)")
            print(f"{'─'*60}")
            if soft_breaks:
                print(f"  Soft breaks (semi-trans only, merged by cross-type):")
                for (f, t), cnt in sorted(soft_breaks.items(), key=lambda x: -x[1])[:10]:
                    print(f"    {f:02X}h→{t:02X}h ({cmd_name(f)[:15]:>15} → {cmd_name(t)[:15]:<15}): {cnt:>5}  ({cnt/len(active):.1f}/frame)")
            if hard_breaks:
                print(f"  Hard breaks (format change, require batch flush):")
                for (f, t), cnt in sorted(hard_breaks.items(), key=lambda x: -x[1])[:10]:
                    print(f"    {f:02X}h→{t:02X}h ({cmd_name(f)[:15]:>15} → {cmd_name(t)[:15]:<15}): {cnt:>5}  ({cnt/len(active):.1f}/frame)")
            print()

    # ── VRAM transfers ──
    loads = total_cmd_count.get(0xA0, 0)
    stores = total_cmd_count.get(0xC0, 0)
    copies = sum(total_cmd_count.get(b, 0) for b in range(0x80, 0xA0))
    if loads + stores + copies > 0:
        print(f"{'─'*60}")
        print(f"VRAM Transfers")
        print(f"{'─'*60}")
        print(f"  LoadImage (CPU→VRAM):  {loads}")
        print(f"  StoreImage (VRAM→CPU): {stores}")
        print(f"  VRAM Copy:             {copies}")
        print()

    # ── Per-frame detail ──
    if show_detail:
        print(f"{'─'*60}")
        print(f"Per-Frame Detail")
        print(f"{'─'*60}")
        print(f"  {'Frame':>7}  {'Words':>6}  {'Cmds':>5}  {'Draws':>5}  {'Env':>4}  {'KB':>6}")
        for fs in per_frame_stats:
            kb = fs["words"] * 4 / 1024
            print(f"  #{fs['id']:>6}  {fs['words']:>6}  {fs['cmds']:>5}  {fs['draws']:>5}  {fs['env']:>4}  {kb:5.1f}")
        print()


def main():
    parser = argparse.ArgumentParser(description="Analyze SuperPSX GPU trace dumps")
    parser.add_argument("trace_file", help="Path to gpu_trace.bin")
    parser.add_argument("--frames", type=int, help="Analyze only last N frames")
    parser.add_argument("--detail", action="store_true", help="Show per-frame detail")
    args = parser.parse_args()

    frames, frame_count, max_words = parse_trace(args.trace_file)
    print(f"Loaded {frame_count} frame slots (max {max_words} words/frame)")
    analyze(frames, show_detail=args.detail, last_n=args.frames)


if __name__ == "__main__":
    main()
