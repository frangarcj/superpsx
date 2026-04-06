# SuperPSX Optimization Audit

Two expert audits conducted: embedded systems + DRC specialist.
Combined: **25+ actionable optimizations**, estimated **15-40% cumulative speedup**.

---

## Tier 1: Quick Wins (~10-12% improvement, 1-3 days)

### QW1: Small Constant Optimization (O6b)
**1 LOC change in `emit_load_imm32()`**. Currently always emits LUI+ORI (2 insns) even for
small values. Use ADDIU/ORI from $zero for values fitting 16 bits → saves 1 insn per small
constant (~5-10% of all constants).

### QW2: Lower Dynamic Slot Threshold (O2a)
Change `best_count = 1` → `best_count = 0` in `dyn_assign_slots()`. Allows single-access
registers to get slots. Saves 1 LW per block for registers used once. ~3% speedup.

### QW3: Extend Alignment Tracking to Half-Word (O3a)
Add `aligned_2` bitmask alongside existing `align_known_mask` (word). When a register is
known half-word-aligned and LH/SH offset is even, skip alignment check (1 BNE + delay saved).
~3-5% speedup on mixed load/store workloads.

### QW4: Lazy Overflow Exceptions (O9b)
Don't check ADD/SUB overflow inline. Defer check to next COP0 status read. Saves 5-8 insns
per overflow-checked instruction. ~1-2% speedup.

---

## Tier 2: Medium Effort (~8-15% additional, 1-2 weeks)

### M1: CPU State Cache-Line Reorder (Embedded P1)
Move hot fields (cycles_left, i_stat, i_mask, pc, irq_pending, block_aborted) to first 32
bytes of R3000CPU. Add `__attribute__((aligned(64)))`. Current layout puts these across 5+
cache lines → 2-3 L1 misses per dispatch. Reorder → 1 miss. **+2-5 FPS on real PS2.**

### M2: Hash Table Redesign (DRC O1a + Embedded P2)
Replace 2-way 4096-entry table with direct-mapped 8192-entry + overflow chain.
- Simpler hash: `(pc >> 2) & 0x1FFF` (no XOR needed)
- Single entry per lookup (vs dual slot check)
- Pad entries to 32 bytes to avoid cache-line splits
- Saves 2-5 instructions per dispatch, better cache behavior

### M3: Speculative Inline Range Check (O3b)
Replace SRL+BNE range check with unsigned comparison against 0x200000. Consolidates
alignment + range into fewer branches. ~5% speedup on memory-heavy code.

### M4: Spatial Locality for Register Allocation (O2b)
Analyze register liveness windows (first_use to last_use) instead of just frequency count.
Prioritize registers with short, dense access windows. ~5-8% improvement in slot utilization.

### M5: Branch Delay Slot Filler Pass (O5a)
Post-compilation pass: find NOP delay slots, fill with independent preceding instruction.
Needs liveness analysis. ~2% speedup.

### M6: Constant Deduplication (O6a)
Per-block cache of loaded 32-bit constants. If same value already in a register, emit MOVE
(1 insn) instead of LUI+ORI (2 insns). ~2-3% on blocks with repeated constants.

### M7: Hot Block Micro-Cache (Embedded P3)
4-entry cache of recently executed blocks (PC → BlockEntry*). Check this before page table
lookup. 16 bytes, fits in one cache line. ~60-80% hit rate on loops. ~3-5 FPS on real PS2.

---

## Tier 3: High Effort (~10-20% additional, 2-4 weeks)

### H1: Compressed Memory Fast Path (O3e)
When SMRV + alignment proves address is in aligned RAM:
```
ADDU t9, rs, s1
LW   v0, 0(t9)
```
Just 2-3 instructions instead of 8-11. Reduces LW to ~4-5x expansion. **Risky** — unsound
analysis leads to crashes. Needs thorough SMRV verification.

### H2: Two-Pass Register Allocation (O2d)
Build live interval graph in pass 1, solve weighted interval scheduling in pass 2. Could push
effective utilization from 8 to 9-10 equivalent slots. Major refactor of `dyn_assign_slots()`.

### H3: Partial Buffer Eviction (O8a)
Replace all-or-nothing code buffer flush with LRU-based partial eviction. Track block reference
counts. Evict cold blocks when buffer hits 80%. Keeps hot loops resident during long sessions.

### H4: Shared Prologue/Epilogue (O4a)
For blocks <100 instructions, use trampolines instead of inline prologue/epilogue (saves 22+13
= 35 insns per block at cost of 2-insn call overhead). Significant for short blocks.

---

## Architecture-Level Insights

1. **8-slot dynamic allocation is near-optimal** for R5900, but spill patterns aren't measured.
   Need profiling instrumentation to validate improvements.

2. **LW/SW expansion (7.4-9.1x) is competitive** but could reach 5-6x with compressed fast path.

3. **Dispatch overhead (20 insns) is architectural** — direct-mapped hash + speculative loads
   could cut to 12-14 insns.

4. **GTE optimizations (P1-P22) are mature** — further gains require VFPU pipelining or
   micro-mode approaches (see vu0_micro_analysis.md).

5. **Scratchpad (1KB) and VU0 LS1 (4KB) unused** — hot data (cpu state, hash table subset,
   timer state) could live there for guaranteed 1-cycle access.

---

## Priority Implementation Order

| Phase | Items | Est. Speedup | Effort |
|-------|-------|-------------|--------|
| **Phase A** | QW1 + QW2 + QW3 + QW4 | ~10-12% | 1-3 days |
| **Phase B** | M1 + M2 + M3 | ~8-12% | 1 week |
| **Phase C** | M4 + M5 + M6 + M7 | ~5-8% | 1 week |
| **Phase D** | H1 + H2 + H3 + H4 | ~10-20% | 2-4 weeks |
| **Total** | All | **25-40%** | **4-6 weeks** |

All estimates assume mixed Crash Bandicoot workload. Actual gains depend on game-specific
instruction mix and cache behavior, which needs profiling on real PS2 or PCSX2 perf counters.
