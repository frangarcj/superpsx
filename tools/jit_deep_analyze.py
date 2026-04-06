#!/usr/bin/env python3
"""
jit_deep_analyze.py — Deep offline analysis of JIT dump for optimization discovery.

Detects patterns, estimates savings, identifies superblock candidates,
and provides concrete optimization recommendations.

Usage:
    python3 tools/jit_deep_analyze.py build/jitdump.bin
"""

import struct
import sys
from collections import defaultdict

# --- Re-use dump parser from jit_analyze.py ---

def parse_dump(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        assert magic == b"JITD", f"Bad magic: {magic!r}"
        (block_count,) = struct.unpack("<I", f.read(4))
        blocks = []
        for _ in range(block_count):
            hdr = f.read(20)
            if len(hdr) < 20:
                break
            psx_pc, instr_count, native_count, cycle_count, exec_count = struct.unpack("<5I", hdr)
            psx_code = list(struct.unpack(f"<{instr_count}I", f.read(instr_count * 4)))
            native_code = list(struct.unpack(f"<{native_count}I", f.read(native_count * 4)))
            blocks.append({
                "pc": psx_pc, "instr_count": instr_count,
                "native_count": native_count, "cycle_count": cycle_count,
                "exec_count": exec_count, "psx_code": psx_code,
                "native_code": native_code,
            })
    return blocks


REG_NAMES = [
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
]

def OP(w):   return (w >> 26) & 0x3F
def RS(w):   return (w >> 21) & 0x1F
def RT(w):   return (w >> 16) & 0x1F
def RD(w):   return (w >> 11) & 0x1F
def FUNC(w): return w & 0x3F
def IMM(w):  return w & 0xFFFF
def SIMM(w):
    v = w & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v
def JTARGET(w, pc): return (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


PROLOGUE_WORDS = 22  # Known prologue size (compile_block preamble)


# ===================================================================
#  ANALYSIS 1: True Execution Overhead (corrected for hash dispatch)
# ===================================================================

def analyze_prologue_overhead(blocks):
    """Corrected analysis: hash dispatch skips the prologue, so prologue
    only runs on C→JIT transitions (once per chain).  Show the real
    post-prologue expansion ratios and per-instruction-type costs."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 1: TRUE EXECUTION MODEL (POST-PROLOGUE)")
    print("=" * 70)

    total_full_impact = 0
    total_post_prologue_impact = 0
    total_exec = 0
    total_psx = 0

    per_type_words = defaultdict(lambda: {"psx": 0, "ee_post": 0, "exec_weighted_ee": 0, "exec_weighted_psx": 0})

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        full_impact = b["exec_count"] * b["native_count"]
        post_prologue = max(b["native_count"] - PROLOGUE_WORDS, 0)
        pp_impact = b["exec_count"] * post_prologue
        total_full_impact += full_impact
        total_post_prologue_impact += pp_impact
        total_exec += b["exec_count"]
        total_psx += b["instr_count"] * b["exec_count"]

        # Classify PSX instructions
        for w in b["psx_code"]:
            op = OP(w)
            if op in (0x23, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26):
                cat = "Load"
            elif op in (0x2B, 0x28, 0x29, 0x2A, 0x2E):
                cat = "Store"
            elif op == 0x32:
                cat = "LWC2"
            elif op == 0x3A:
                cat = "SWC2"
            elif op in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07) or OP(w) == 1:
                cat = "Branch/Jump"
            elif op == 0 and FUNC(w) in (0x08, 0x09):
                cat = "JR/JALR"
            elif op == 0x12:
                rs_f = (w >> 21) & 0x1F
                if rs_f >= 0x10:
                    cat = "GTE_cmd"
                elif rs_f in (0, 2, 4, 6):
                    cat = "COP2data"
                else:
                    cat = "COP2other"
            elif op == 0x10:
                cat = "COP0"
            elif op in (0x18, 0x19):
                cat = "MulDiv"
            elif op == 0 and FUNC(w) in (0x18, 0x19, 0x1A, 0x1B):
                cat = "MulDiv"
            else:
                cat = "ALU"
            per_type_words[cat]["psx"] += 1
            per_type_words[cat]["exec_weighted_psx"] += b["exec_count"]

    # Estimate per-type EE words using known expansion ratios (post-prologue)
    # These are derived from playground measurements minus prologue
    KNOWN_COSTS = {
        "ALU": 2, "Load": 7, "Store": 10, "LWC2": 8, "SWC2": 11,
        "Branch/Jump": 8, "JR/JALR": 10, "GTE_cmd": 50, "COP2data": 5,
        "COP0": 4, "MulDiv": 10, "COP2other": 4,
    }

    print(f"\n NOTE: Hash dispatch (jit_ht) stores native+{PROLOGUE_WORDS} — prologue is skipped")
    print(f" on most block entries. Prologue only runs on C→JIT (~once per chain).")
    print(f"\n Total block executions:       {total_exec:>12,}")
    print(f" Total with-prologue impact:   {total_full_impact:>12,} EE-word-execs")
    print(f" Total post-prologue impact:   {total_post_prologue_impact:>12,} EE-word-execs")
    print(f" Prologue-free overhead:       {(total_full_impact-total_post_prologue_impact)/max(total_full_impact,1)*100:.1f}% removed")

    print(f"\n Per-instruction-type cost model (weighted by execution frequency):")
    print(f" {'Type':>14} {'StaticPSX':>10} {'EstEE/insn':>10} {'WeightedPSXi':>13} {'WeightedEE':>12} {'Share%':>7}")
    total_weighted_ee = sum(per_type_words[t]["exec_weighted_psx"] * KNOWN_COSTS.get(t, 3) for t in per_type_words)
    for t in sorted(per_type_words.keys(), key=lambda x: -per_type_words[x]["exec_weighted_psx"] * KNOWN_COSTS.get(x, 3)):
        p = per_type_words[t]
        cost = KNOWN_COSTS.get(t, 3)
        weighted_ee = p["exec_weighted_psx"] * cost
        share = weighted_ee / max(total_weighted_ee, 1) * 100
        print(f" {t:>14} {p['psx']:>10,} {cost:>10} {p['exec_weighted_psx']:>13,} {weighted_ee:>12,} {share:>6.1f}%")

    print(f"\n {'TOTAL':>14} {'':>10} {'':>10} {total_psx:>13,} {total_weighted_ee:>12,} {'100.0':>6}%")


# ===================================================================
#  ANALYSIS 2: Pattern Detection
# ===================================================================

def detect_block_patterns(blocks):
    """Classify blocks into known patterns with optimization potential."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 2: BLOCK PATTERN DETECTION")
    print("=" * 70)

    patterns = {
        "simple_increment": [],     # Load, increment, store, return
        "polling_loop": [],         # Load status, test, branch to self
        "exception_dispatch": [],   # LUI+ADDIU+JR (exception vectors)
        "leaf_function": [],        # No JAL, ends with JR $ra
        "call_heavy": [],           # Multiple JAL instructions
        "memory_copy": [],          # Load/store pairs (memcpy-like)
        "gte_compute": [],          # Heavy GTE command usage
        "tiny_block": [],           # ≤5 PSX instructions
    }

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        code = b["psx_code"]
        n = len(code)
        pc = b["pc"]

        # Tiny block (≤5 insns)
        if n <= 5:
            patterns["tiny_block"].append(b)

        # Exception dispatch: LUI+ADDIU+JR sequence
        if n <= 5 and any(OP(w) == 0x0F for w in code):  # Has LUI
            jr_found = any(OP(w) == 0 and FUNC(w) == 0x08 for w in code)
            if jr_found:
                patterns["exception_dispatch"].append(b)
                continue

        # Simple increment: LUI+ADDIU to form addr, LW, ADDIU +1, JR, SW
        has_lw = any(OP(w) == 0x23 for w in code)
        has_sw = any(OP(w) == 0x2B for w in code)
        has_jr_ra = any(OP(w) == 0 and FUNC(w) == 0x08 and RS(w) == 31 for w in code)
        has_jal = any(OP(w) == 0x03 for w in code)
        if n <= 10 and has_lw and has_sw and has_jr_ra and not has_jal:
            # Check for increment pattern
            has_addiu_1 = any(OP(w) == 0x09 and SIMM(w) == 1 for w in code)
            if has_addiu_1:
                patterns["simple_increment"].append(b)
                continue

        # Polling loop: LHU/LBU from I/O, ANDI, BEQ/BNE back
        if n <= 8:
            has_io_load = False
            has_branch_back = False
            for i, w in enumerate(code):
                if OP(w) in (0x25, 0x24, 0x21, 0x23):  # LHU/LBU/LH/LW
                    base = RS(w)
                    # I/O if base is $s1 or known I/O pattern
                    has_io_load = True
                if OP(w) in (0x04, 0x05):  # BEQ/BNE
                    offset = SIMM(w)
                    target = pc + (i + 1) * 4 + offset * 4
                    if target >= pc and target < pc + n * 4:
                        has_branch_back = True
            if has_io_load and has_branch_back:
                patterns["polling_loop"].append(b)
                continue

        # Leaf function (no JAL, ends with JR $ra)
        if has_jr_ra and not has_jal:
            patterns["leaf_function"].append(b)

        # Call-heavy (2+ JALs)
        jal_count = sum(1 for w in code if OP(w) == 0x03)
        if jal_count >= 2:
            patterns["call_heavy"].append(b)

        # GTE compute (has GTE commands)
        gte_cmds = sum(1 for w in code if OP(w) == 0x12 and (w >> 21) & 0x1F >= 0x10)
        if gte_cmds >= 1:
            patterns["gte_compute"].append(b)

        # Memory copy pattern (interleaved LW/SW)
        lw_count = sum(1 for w in code if OP(w) == 0x23)
        sw_count = sum(1 for w in code if OP(w) == 0x2B)
        if lw_count >= 3 and sw_count >= 3 and abs(lw_count - sw_count) <= 2:
            patterns["memory_copy"].append(b)

    print()
    for pat_name, pat_blocks in sorted(patterns.items(), key=lambda x: -sum(b["exec_count"] * b["native_count"] for b in x[1])):
        total_exec = sum(b["exec_count"] for b in pat_blocks)
        total_impact = sum(b["exec_count"] * b["native_count"] for b in pat_blocks)
        avg_ratio = sum(b["native_count"] for b in pat_blocks) / max(sum(b["instr_count"] for b in pat_blocks), 1)
        print(f" {pat_name:<22} {len(pat_blocks):>5} blocks, {total_exec:>10,} exec, {total_impact:>12,} impact, avg {avg_ratio:.1f}x")
        # Show top 3 blocks in pattern
        for bb in sorted(pat_blocks, key=lambda x: -x["exec_count"] * x["native_count"])[:3]:
            ratio = bb["native_count"] / max(bb["instr_count"], 1)
            print(f"   0x{bb['pc']:08X}  {bb['instr_count']:>3}i→{bb['native_count']:>4}w ({ratio:.1f}x)  exec:{bb['exec_count']:>8,}")


# ===================================================================
#  ANALYSIS 3: Superblock Candidates (frequently chained blocks)
# ===================================================================

def analyze_superblock_candidates(blocks):
    """Find block sequences that chain frequently and could be merged."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 3: SUPERBLOCK / TRACE CANDIDATES")
    print("=" * 70)

    block_map = {b["pc"]: b for b in blocks}
    end_pc_map = {b["pc"]: b["pc"] + b["instr_count"] * 4 for b in blocks}

    # Find J (jump) and fall-through edges between compiled blocks
    edges = defaultdict(int)  # (src_pc, dst_pc) → weight

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        code = b["psx_code"]
        pc = b["pc"]

        # Block can chain to its successor (fall-through)
        fall_through_pc = pc + len(code) * 4
        if fall_through_pc in block_map:
            # Not all blocks fall through — need to check if last insn is unconditional jump
            last = code[-1] if code else 0
            second_last = code[-2] if len(code) >= 2 else 0
            # If second-to-last is J/JR (delay slot is last), no fall-through
            is_unconditional_exit = (OP(second_last) == 0x02 or  # J
                                     (OP(second_last) == 0 and FUNC(second_last) == 0x08))  # JR
            if not is_unconditional_exit:
                edges[(pc, fall_through_pc)] += b["exec_count"]

        # J targets
        for i, w in enumerate(code):
            ipc = pc + i * 4
            if OP(w) == 0x02:  # J
                target = JTARGET(w, ipc)
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]
            elif OP(w) in (0x04, 0x05, 0x06, 0x07):  # BEQ/BNE/BLEZ/BGTZ
                target = (ipc + 4 + SIMM(w) * 4) & 0xFFFFFFFF
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]
            elif OP(w) == 0x03:  # JAL
                target = JTARGET(w, ipc)
                if target in block_map:
                    edges[(pc, target)] += b["exec_count"]

    # Find hot chains: sequences of blocks that execute together frequently
    print(f"\n Top 20 hot edges (block→block, weighted by exec):")
    print(f" {'From':>10} → {'To':>10}  {'Weight':>10}  {'SrcI':>5} {'DstI':>5} {'Combined EEw':>12}  {'SavedOverhead':>14}")

    OVERHEAD_PER_BOUNDARY = 28  # prologue(22) + epilogue(6) saved per merge

    ranked_edges = sorted(edges.items(), key=lambda x: -x[1])[:20]
    for (src, dst), weight in ranked_edges:
        src_b = block_map.get(src)
        dst_b = block_map.get(dst)
        if not src_b or not dst_b:
            continue
        combined_ee = src_b["native_count"] + dst_b["native_count"]
        saved = weight * OVERHEAD_PER_BOUNDARY
        print(f" 0x{src:08X} → 0x{dst:08X}  {weight:>10,}  {src_b['instr_count']:>5} {dst_b['instr_count']:>5} {combined_ee:>12}  {saved:>14,}")

    # Multi-block traces: find chains of length 3+
    print(f"\n Hot traces (3+ blocks, sorted by total weight):")

    # BFS to find chains
    traces = []
    visited_starts = set()
    for (src, dst), weight in sorted(edges.items(), key=lambda x: -x[1]):
        if weight < 1000 or src in visited_starts:
            continue
        # Build trace greedily
        trace = [src]
        current = dst
        while current in block_map and len(trace) < 10:
            trace.append(current)
            # Find heaviest outgoing edge
            best_next = None
            best_w = 0
            for (s, d), w in edges.items():
                if s == current and w > best_w and d not in trace:
                    best_next = d
                    best_w = w
            if best_next is None or best_w < weight * 0.3:
                break
            current = best_next
        if len(trace) >= 3:
            traces.append((trace, weight))
            visited_starts.update(trace)

    for trace, weight in sorted(traces, key=lambda x: -x[1])[:10]:
        total_psx = sum(block_map[pc]["instr_count"] for pc in trace if pc in block_map)
        total_ee = sum(block_map[pc]["native_count"] for pc in trace if pc in block_map)
        saved_overhead = weight * OVERHEAD_PER_BOUNDARY * (len(trace) - 1)
        pcs = " → ".join(f"0x{pc:08X}" for pc in trace)
        print(f"  [{len(trace)} blocks] weight={weight:,}  {total_psx}i→{total_ee}w  overhead saved: {saved_overhead:,}")
        print(f"    {pcs}")


# ===================================================================
#  ANALYSIS 4: Register Pressure & Slot Utilization
# ===================================================================

def analyze_register_pressure(blocks):
    """Estimate how many unique PSX registers each hot block uses."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 4: REGISTER PRESSURE IN HOT BLOCKS")
    print("=" * 70)

    PINNED = {28, 29, 30, 31}  # $gp, $sp, $fp, $ra

    hot = sorted(blocks, key=lambda b: -b["exec_count"] * b["native_count"])[:30]

    print(f"\n {'PC':>10} {'PSXi':>5} {'EEw':>5} {'Ratio':>6} {'UniqueRegs':>10} {'Pinned':>7} {'Dynamic':>8} {'Spill?':>6}")
    for b in hot:
        regs_read = set()
        regs_written = set()
        for w in b["psx_code"]:
            op = OP(w)
            if op == 0:  # SPECIAL
                regs_read.add(RS(w))
                regs_read.add(RT(w))
                regs_written.add(RD(w))
            elif op in (0x04, 0x05):  # BEQ/BNE
                regs_read.add(RS(w))
                regs_read.add(RT(w))
            elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):  # ALU imm
                regs_read.add(RS(w))
                regs_written.add(RT(w))
            elif op == 0x0F:  # LUI
                regs_written.add(RT(w))
            elif op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):  # Loads
                regs_read.add(RS(w))
                regs_written.add(RT(w))
            elif op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):  # Stores
                regs_read.add(RS(w))
                regs_read.add(RT(w))
            elif op == 0x12:  # COP2
                rs_f = (w >> 21) & 0x1F
                if rs_f in (0, 2):  # MFC2/CFC2
                    regs_written.add(RT(w))
                elif rs_f in (4, 6):  # MTC2/CTC2
                    regs_read.add(RT(w))

        all_regs = (regs_read | regs_written) - {0}  # exclude $zero
        pinned_used = all_regs & PINNED
        dynamic_needed = all_regs - PINNED
        spills = len(dynamic_needed) > 8  # DYN_SLOT_COUNT = 8

        ratio = b["native_count"] / max(b["instr_count"], 1)
        print(f" 0x{b['pc']:08X} {b['instr_count']:>5} {b['native_count']:>5} {ratio:>5.1f}x {len(all_regs):>10} {len(pinned_used):>7} {len(dynamic_needed):>8} {'YES' if spills else 'no':>6}")


# ===================================================================
#  ANALYSIS 5: Memory Access Locality
# ===================================================================

def analyze_memory_locality(blocks):
    """Find blocks with multiple loads/stores to same base register — base caching opportunities."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 5: MEMORY ACCESS BASE REUSE")
    print("=" * 70)

    total_reuse_impact = 0
    reuse_blocks = []

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        # Count loads/stores per base register
        base_accesses = defaultdict(int)
        for w in b["psx_code"]:
            op = OP(w)
            if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,  # Loads
                      0x28, 0x29, 0x2A, 0x2B, 0x2E,  # Stores
                      0x32, 0x3A):  # LWC2/SWC2
                base = RS(w)
                base_accesses[base] += 1

        # Find bases with 2+ accesses
        reusable = {base: cnt for base, cnt in base_accesses.items() if cnt >= 2}
        if reusable:
            max_reuse = max(reusable.values())
            total_reuse_accesses = sum(v - 1 for v in reusable.values())  # Savings = re-accesses
            # Each re-access saves ~2 words (AND mask + ADDU base) if base already computed
            savings_per_exec = total_reuse_accesses * 2
            total_savings = savings_per_exec * b["exec_count"]
            total_reuse_impact += total_savings
            reuse_blocks.append({
                "block": b,
                "reusable": reusable,
                "savings_per_exec": savings_per_exec,
                "total_savings": total_savings,
            })

    reuse_blocks.sort(key=lambda x: -x["total_savings"])

    total_ee_impact = sum(b["exec_count"] * b["native_count"] for b in blocks if b["exec_count"] > 0)
    print(f"\n Total EE impact:           {total_ee_impact:>14,}")
    print(f" Total base-reuse savings:  {total_reuse_impact:>14,} ({total_reuse_impact/max(total_ee_impact,1)*100:.1f}%)")
    print(f"\n Top 15 blocks with base reuse opportunities:")
    print(f" {'PC':>10} {'Exec':>8} {'PSXi':>5} {'Bases':>40}  {'Save/exec':>10} {'TotalSave':>12}")

    for item in reuse_blocks[:15]:
        b = item["block"]
        bases_str = ", ".join(f"{REG_NAMES[base]}×{cnt}" for base, cnt in sorted(item["reusable"].items(), key=lambda x: -x[1]))
        print(f" 0x{b['pc']:08X} {b['exec_count']:>8,} {b['instr_count']:>5} {bases_str:>40}  {item['savings_per_exec']:>10} {item['total_savings']:>12,}")


# ===================================================================
#  ANALYSIS 6: Branch Overhead
# ===================================================================

def analyze_branch_overhead(blocks):
    """Estimate EE words spent on branch handling vs actual computation."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 6: BRANCH OVERHEAD ESTIMATION")
    print("=" * 70)

    # Each PSX branch typically generates:
    #  - ~8-12 EE words for taken path (flush slots, set PC, BEQ/BNE, epilogue)
    #  - ~4-6 EE words for fall-through (just patch target)
    # vs ALU: ~2-3 EE words, Load: ~6-8 EE words, Store: ~7-9 EE words

    BRANCH_EE_COST = 12  # Estimated average EE words per PSX branch
    ALU_EE_COST = 2
    LOAD_EE_COST = 6
    STORE_EE_COST = 8

    total_branch_cost = 0
    total_block_exec_impact = 0

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        branches = sum(1 for w in b["psx_code"] if OP(w) in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07) or
                       (OP(w) == 0 and FUNC(w) in (0x08, 0x09)) or
                       OP(w) == 1)
        total_branch_cost += branches * BRANCH_EE_COST * b["exec_count"]
        total_block_exec_impact += b["exec_count"] * b["native_count"]

    print(f"\n Estimated branch handling:  {total_branch_cost:>14,} EE-word-execs")
    print(f" Total EE impact:           {total_block_exec_impact:>14,}")
    print(f" Branch overhead share:     {total_branch_cost/max(total_block_exec_impact,1)*100:.1f}%")


# ===================================================================
#  ANALYSIS 7: Native Code Overhead Breakdown
# ===================================================================

def analyze_native_overhead(blocks):
    """Scan actual EE native instructions to categorize where code size goes.
    Detects: prologue, ISC check, alignment check, range check, SMC/page-gen
    check, abort check, hash dispatch, direct link, slot loads/flushes."""
    print("\n" + "=" * 70)
    print(" ANALYSIS 7: NATIVE CODE OVERHEAD BREAKDOWN")
    print("=" * 70)

    totals = defaultdict(int)  # category → total words across all blocks
    weighted = defaultdict(int)  # category → exec-weighted EE-word-execs
    block_details = []
    total_ee = 0
    total_pp = 0

    for b in blocks:
        if b["exec_count"] == 0:
            continue
        nc = b["native_code"]
        n = len(nc)
        cats = defaultdict(int)
        used = [False] * n

        # --- Prologue: first 22 words (only executes on C→JIT entry) ---
        pro_end = min(PROLOGUE_WORDS, n)
        for i in range(pro_end):
            cats["prologue"] += 1
            used[i] = True

        # --- ISC check: LW AT,0xBC(S0); SRL AT,AT,16; ANDI AT,AT,1; SW AT,0x50(SP) ---
        for i in range(pro_end, n - 3):
            if used[i]:
                continue
            w0, w1, w2, w3 = nc[i], nc[i+1] if i+1<n else 0, nc[i+2] if i+2<n else 0, nc[i+3] if i+3<n else 0
            # LW $at, 0xBC($s0) = 0x8E0100BC
            if w0 == 0x8E0100BC and w1 == 0x00010C02 and w2 == 0x30210001:
                # Check SW to stack (AFA1xxxx where xx=offset on stack)
                if (w3 >> 16) == 0xAFA1:
                    for j in range(4):
                        cats["isc_check"] += 1
                        used[i+j] = True

        # --- SMC/page-gen check: LUI+ORI addr, LW, BEQ zero skip, ... JAL ---
        # Pattern: LUI Rx, hi; ORI Rx, lo; LW Rx, 0(Rx); BEQ Rx, ZERO, skip; NOP; ...
        for i in range(pro_end, n - 5):
            if used[i]:
                continue
            w0 = nc[i]
            if OP(w0) != 0x0F:  # LUI
                continue
            rt0 = RT(w0)
            if i+1 >= n:
                continue
            w1 = nc[i+1]
            if OP(w1) != 0x0D or RS(w1) != rt0 or RT(w1) != rt0:  # ORI same reg
                continue
            w2 = nc[i+2] if i+2 < n else 0
            if OP(w2) != 0x23 or RS(w2) != rt0:  # LW from that addr
                continue
            w3 = nc[i+3] if i+3 < n else 0
            if OP(w3) == 0x04 and RS(w3) == RT(w2) and RT(w3) == 0:  # BEQ reg, zero
                # This is a page-gen check. Count from LUI through the BEQ skip target
                skip_off = SIMM(nc[i+3])
                end = i + 4 + skip_off
                if end > n:
                    end = min(i + 16, n)  # cap
                count = 0
                for j in range(i, min(end, n)):
                    if not used[j]:
                        cats["smc_pagegen"] += 1
                        used[j] = True
                        count += 1

        # --- Abort check: BLEZ S2,+3; LW AT,irq_fast; BEQ AT,0,+3; NOP; J abort; NOP ---
        for i in range(pro_end, n - 5):
            if used[i]:
                continue
            w = nc[i]
            # BLEZ $s2, +3 = 0x1A40 0003
            if w == 0x1A400003:
                count = 0
                for j in range(i, min(i + 6, n)):
                    if not used[j]:
                        cats["abort_check"] += 1
                        used[j] = True
                        count += 1

        # --- Hash dispatch: LW T8,cpu.pc(S0); ADDIU S2; J trampoline; NOP ---
        for i in range(pro_end, n - 3):
            if used[i]:
                continue
            w = nc[i]
            # LW $t8, 0x80($s0) = 0x8E180080
            if w == 0x8E180080:
                # Check if followed by ADDIU S2 and J
                w1 = nc[i+1] if i+1 < n else 0
                w2 = nc[i+2] if i+2 < n else 0
                if (w1 >> 16) == 0x2652 and OP(w2) == 0x02:  # ADDIU S2,S2,imm + J
                    for j in range(i, min(i + 4, n)):
                        if not used[j]:
                            cats["hash_dispatch"] += 1
                            used[j] = True

        # --- Guarded JR: load imm32 into AT, BNE T8,AT ---
        for i in range(pro_end, n - 2):
            if used[i]:
                continue
            w = nc[i]
            # emit_load_imm32 into AT: ORI $at,$zero,imm or LUI $at,hi
            if OP(w) == 0x0D and RS(w) == 0 and RT(w) == 1:  # ORI $at, $zero, imm
                w1 = nc[i+1] if i+1 < n else 0
                if OP(w1) == 0x05 and RS(w1) == 24 and RT(w1) == 1:  # BNE $t8, $at
                    cats["guarded_jr"] += 1
                    used[i] = True
                    cats["guarded_jr"] += 1
                    used[i+1] = True

        # --- Direct link (J + NOP at end): J to block or slow-path ---
        for i in range(n - 2, pro_end - 1, -1):
            if used[i]:
                continue
            w = nc[i]
            if OP(w) == 0x02:  # J
                w1 = nc[i+1] if i+1 < n else 0
                if w1 == 0:  # NOP
                    cats["direct_link"] += 1
                    used[i] = True
                    cats["direct_link"] += 1
                    used[i+1] = True

        # --- Dynamic slot loads: LW Tx, offset(S0) near start of post-prologue ---
        for i in range(pro_end, min(pro_end + 16, n)):
            if used[i]:
                continue
            w = nc[i]
            if OP(w) == 0x23 and RS(w) == 16:  # LW rt, offset($s0)
                rt = RT(w)
                if 8 <= rt <= 15:  # T0-T7
                    cats["slot_load"] += 1
                    used[i] = True

        # --- Dynamic slot flushes: SW Tx, offset(S0) near end ---
        for i in range(max(n - 20, pro_end), n):
            if used[i]:
                continue
            w = nc[i]
            if OP(w) == 0x2B and RS(w) == 16:  # SW rt, offset($s0)
                rt = RT(w)
                if 8 <= rt <= 15:  # T0-T7
                    cats["slot_flush"] += 1
                    used[i] = True

        # Everything else is "computation" (actual work)
        for i in range(n):
            if not used[i]:
                cats["computation"] += 1

        for cat, count in cats.items():
            totals[cat] += count
            weighted[cat] += count * b["exec_count"]

        total_ee += n * b["exec_count"]
        total_pp += max(n - PROLOGUE_WORDS, 0) * b["exec_count"]

        block_details.append({"block": b, "cats": dict(cats)})

    # Print summary
    print(f"\n Total EE-word-execs (with prologue): {total_ee:>14,}")
    print(f" Total EE-word-execs (post-prologue): {total_pp:>14,}")
    print(f"\n {'Category':>16} {'TotalWords':>11} {'WeightedExec':>14} {'%ofTotal':>9} {'%ofPostPro':>11}")
    for cat in sorted(weighted.keys(), key=lambda x: -weighted[x]):
        pct_total = weighted[cat] / max(total_ee, 1) * 100
        pct_pp = weighted[cat] / max(total_pp, 1) * 100 if cat != "prologue" else 0.0
        print(f" {cat:>16} {totals[cat]:>11,} {weighted[cat]:>14,} {pct_total:>8.1f}% {pct_pp:>10.1f}%")

    # Show top blocks by SMC overhead
    smc_blocks = [(d["block"], d["cats"].get("smc_pagegen", 0)) for d in block_details if d["cats"].get("smc_pagegen", 0) > 0]
    smc_blocks.sort(key=lambda x: -x[1] * x[0]["exec_count"])
    if smc_blocks:
        print(f"\n Top 10 blocks by SMC page-gen overhead:")
        print(f" {'PC':>10} {'Exec':>8} {'SMCwords':>9} {'TotalEE':>8} {'SMC%':>6} {'SMCimpact':>12}")
        for b, smc in smc_blocks[:10]:
            pp = max(b["native_count"] - PROLOGUE_WORDS, 0)
            pct = smc / max(pp, 1) * 100
            impact = smc * b["exec_count"]
            print(f" 0x{b['pc']:08X} {b['exec_count']:>8,} {smc:>9} {pp:>8} {pct:>5.1f}% {impact:>12,}")

    # Show top blocks by ISC overhead
    isc_blocks = [(d["block"], d["cats"].get("isc_check", 0)) for d in block_details if d["cats"].get("isc_check", 0) > 0]
    isc_blocks.sort(key=lambda x: -x[1] * x[0]["exec_count"])
    if isc_blocks:
        total_isc_impact = sum(c * b["exec_count"] for b, c in isc_blocks)
        print(f"\n ISC check overhead: {total_isc_impact:,} EE-word-execs ({total_isc_impact/max(total_pp,1)*100:.1f}% of post-prologue)")
        print(f" Blocks with ISC check: {len(isc_blocks)}")

    return weighted, total_pp


# ===================================================================
#  ANALYSIS 8: Optimization Recommendations (corrected)
# ===================================================================

def generate_recommendations(blocks):
    """Generate concrete, prioritized optimization recommendations using
    native-code-level analysis."""
    print("\n" + "=" * 70)
    print(" OPTIMIZATION RECOMMENDATIONS (sorted by estimated impact)")
    print("=" * 70)

    total_ee_impact = sum(b["exec_count"] * max(b["native_count"] - PROLOGUE_WORDS, 0)
                          for b in blocks if b["exec_count"] > 0)

    recommendations = []

    # R1: SMC page-gen check optimization
    smc_savings = 0
    smc_store_count = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        stores = sum(1 for w in b["psx_code"] if OP(w) in (0x2B, 0x28, 0x29, 0x2A, 0x2E, 0x3A))
        if stores > 0:
            # Each store emits ~12-14 words of SMC check
            # If we could skip SMC for stores to non-code pages: save ~12 words/store
            smc_savings += stores * 12 * b["exec_count"]
            smc_store_count += stores

    if smc_savings > 0:
        recommendations.append({
            "name": "Smart SMC check: skip page-gen for data-only stores",
            "impact": smc_savings,
            "pct": smc_savings / max(total_ee_impact, 1) * 100,
            "detail": f"{smc_store_count} stores across all blocks. Each emits ~12 words of page-gen check. "
                      f"Track which pages contain compiled blocks; skip SMC check for stores to non-code pages.",
        })

    # R2: ISC check elimination at block level
    isc_savings = 0
    isc_blocks = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        has_mem = any(OP(w) in (0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x28,0x29,0x2A,0x2B,0x2E,0x32,0x3A)
                      for w in b["psx_code"])
        if has_mem:
            # ISC check = 4 words at block start
            isc_savings += 4 * b["exec_count"]
            isc_blocks += 1

    if isc_savings > 0:
        recommendations.append({
            "name": "Eliminate ISC check (cache isolation unused post-BIOS)",
            "impact": isc_savings,
            "pct": isc_savings / max(total_ee_impact, 1) * 100,
            "detail": f"{isc_blocks} blocks with memory accesses emit 4-word ISC check at start. "
                      f"After BIOS boot, ISC is always 0. Add a runtime flag to skip ISC checks post-boot.",
        })

    # R3: Dead dynamic slot loads (written before read)
    dead_slot_savings = 0
    dead_blocks = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        # Detect regs that are written (LUI/ADDIU/LW-dest) before being read
        written_before_read = set()
        read_regs = set()
        for w in b["psx_code"]:
            op = OP(w)
            # Reads
            if op == 0:  # R-type
                if FUNC(w) not in (0x00, 0x02, 0x03):  # Not shift-by-imm
                    read_regs.add(RS(w))
                read_regs.add(RT(w))
            elif op in (0x04,0x05,0x28,0x29,0x2A,0x2B,0x2E,0x3A):  # Branch/store: reads RS and RT
                read_regs.add(RS(w))
                read_regs.add(RT(w))
            elif op in (0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x32):
                read_regs.add(RS(w))
            # Writes
            dest = None
            if op == 0:
                dest = RD(w)
            elif op == 0x0F:  # LUI
                dest = RT(w)
            elif op in (0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x32):
                dest = RT(w)
            if dest and dest != 0 and dest not in read_regs:
                written_before_read.add(dest)
            if dest:
                read_regs.discard(dest)  # Reset for tracking purposes

        # Exclude pinned regs (28-31) and $zero (0)
        dead_count = len(written_before_read - {0, 28, 29, 30, 31})
        if dead_count > 0:
            dead_slot_savings += dead_count * b["exec_count"]
            dead_blocks += 1

    if dead_slot_savings > 0:
        recommendations.append({
            "name": "Dead dynamic slot load elimination",
            "impact": dead_slot_savings,
            "pct": dead_slot_savings / max(total_ee_impact, 1) * 100,
            "detail": f"{dead_blocks} blocks load dynamic slots for regs that are written before being read. "
                      f"Skip the initial LW from cpu.regs[] for these registers (save 1 word per dead slot).",
        })

    # R4: Memory base address caching
    base_cache_savings = 0
    for b in blocks:
        if b["exec_count"] == 0:
            continue
        base_accesses = defaultdict(int)
        for w in b["psx_code"]:
            op = OP(w)
            if op in (0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x28,0x29,0x2A,0x2B,0x2E,0x32,0x3A):
                base_accesses[RS(w)] += 1
        for base, cnt in base_accesses.items():
            if cnt >= 2:
                base_cache_savings += (cnt - 1) * 2 * b["exec_count"]

    if base_cache_savings > 0:
        recommendations.append({
            "name": "Memory base address caching (AND+ADDU reuse)",
            "impact": base_cache_savings,
            "pct": base_cache_savings / max(total_ee_impact, 1) * 100,
            "detail": "When multiple L/S use same base register, compute host address once and reuse. "
                      "Saves AND(mask)+ADDU(base) = 2 words per reuse. NOTE: T9 scratch clobber limits this.",
        })

    # Sort by impact and print
    recommendations.sort(key=lambda r: -r["impact"])
    print()
    for i, r in enumerate(recommendations):
        print(f" {i+1}. {r['name']}")
        print(f"    Estimated saving: {r['impact']:,} EE-word-execs ({r['pct']:.1f}% of total)")
        print(f"    {r['detail']}")
        print()


# ===================================================================
#  Main
# ===================================================================

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <jitdump.bin>")
        sys.exit(1)

    blocks = parse_dump(sys.argv[1])
    print(f"Loaded {len(blocks)} blocks")

    for b in blocks:
        b["ee_impact"] = b["exec_count"] * b["native_count"]
        b["ratio"] = b["native_count"] / max(b["instr_count"], 1)

    analyze_prologue_overhead(blocks)
    detect_block_patterns(blocks)
    analyze_superblock_candidates(blocks)
    analyze_register_pressure(blocks)
    analyze_memory_locality(blocks)
    analyze_native_overhead(blocks)
    generate_recommendations(blocks)


if __name__ == "__main__":
    main()
