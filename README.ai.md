# AI / Copilot Context — SuperPSX

PSX emulator running **natively on PS2** (EE/R5900 ~294MHz) via MIPS→MIPS JIT recompiler.

## Build & Test

```bash
# Build
cmake -S . -B build && cmake --build build

# GTE test (1150/0 expected, gte_vu0=0 for accuracy mode)
rm -f build/superpsx.ini && printf "gte_vu0 = 0\n" > build/superpsx.ini && \
perl -e 'alarm 60; exec @ARGV' make -C build run GAMEARGS=tests/gte/test-all/test-all.exe 2>&1 | \
grep -E "Passed|Failed" | head -5; \
rm -f build/superpsx.ini && ln -s $(pwd)/superpsx.ini build/superpsx.ini

# CPU test (227 "errors" expected = known deviations)
perl -e 'alarm 120; exec @ARGV' make -C build run GAMEARGS=tests/psxtest_cpu/psxtest_cpu.exe 2>&1 | grep -c "error"

# Timer test
perl -e 'alarm 60; exec @ARGV' make -C build run GAMEARGS=tests/timers/timers.exe 2>&1 | tail -20

# Crash Bandicoot (main test game)
make -C build run GAMEARGS=isos/CrashBandicoot/CrashBandicoot.cue
```

**NOTE:** macOS has no `timeout`/`gtimeout`; use `perl -e 'alarm N; exec @ARGV'` wrapper.

## CMake Options

| Flag | Default | Description |
|---|---|---|
| `ENABLE_PSX_TLB` | OFF | TLB fast-path for RAM (experimental, known bugs) |
| `ENABLE_VRAM_DUMP` | OFF | VRAM dumping (reduces performance) |
| `ENABLE_HOST_LOG` | ON | Host logging |
| `ENABLE_DEBUG_LOG` | ON | Debug logging |
| `ENABLE_STUCK_DETECTION` | ON | Stuck detection |
| `ENABLE_PROFILING` | OFF | gprof instrumentation |
| `ENABLE_LTO` | OFF | Link-Time Optimization |
| `ENABLE_DYNAREC_STATS` | OFF | Dynarec execution statistics |
| `ENABLE_SUBSYSTEM_PROFILER` | ON | Per-subsystem wall-clock profiler |
| `HEADLESS` | OFF | Build without GPU (no-op stubs) |

## Architecture

### JIT Register Map (13 pinned PSX → EE registers)

| PSX Reg | EE Reg | Role |
|---|---|---|
| $v0 (2) | S6 (22) | Return value |
| $v1 (3) | V1 (3) | Return value 2 |
| $a0-$a3 (4-7) | T3-T6 (11-14) | Arguments |
| $t0-$t2 (8-10) | T7-T9 (15,24,25) | Temporaries |
| $gp (28) | FP (30) | Global pointer |
| $sp (29) | S4 (20) | Stack pointer |
| $s8/fp (30) | S7 (23) | Frame pointer |
| $ra (31) | S5 (21) | Return address |

**Infrastructure registers:** S0=cpu ptr, S1=RAM base/TLB, S2=cycles_left, S3=mask(0x1FFFFFFF)
**Scratch:** T0, T1, T2, AT (with T0/T1 scratch cache for recently accessed non-pinned regs)

### Dynarec Module Layout

| File | Purpose |
|---|---|
| `dynarec_compile.c` | Block compiler, prologue/epilogue, DCE pre-scan, branch handling |
| `dynarec_emit.c` | Low-level emitters, pinned reg sync, C call trampolines |
| `dynarec_insn.c` | Per-instruction emission (ALU, loads, stores, COP0/COP2) |
| `dynarec_memory.c` | Memory access emission (range checks, fast paths, cold paths) |
| `dynarec_run.c` | Block dispatch loop, trampoline setup, hash table |
| `dynarec_cache.c` | Block cache, SMC page tracking, direct block linking |

### Code Buffer Trampoline Layout

| Offset | Size | Trampoline |
|---|---|---|
| [0] | 2 words | Slow-path (JR RA) |
| [2] | 28 words | Abort/exit (flush pinned + restore callee-saved) |
| [32] | 36 words | Full C-call (flush/reload all 13 pinned regs) |
| [68] | 24 words | Lite C-call (flush/reload 8 caller-saved pinned) |
| [96] | ~30 words | Jump dispatch (JR/JALR inline hash lookup) |
| [128] | 10 words | Memory slow-path (save RA, store PC, call lite) |
| [144+] | ... | JIT compiled blocks |

### Key Subsystems

- **GTE:** Inline dispatch for all 22 commands + VU0 macro mode for RTPS/RTPT/MVMVA/light pipeline
- **GPU:** GIF-based rendering to GS, texture cache with VRAM dirty tracking
- **Memory:** Range-check based routing (RAM→fast, non-RAM→C helpers). TLB experimental.
- **Scheduler:** Event-based timing with cycle-accurate timer/VBlank/CD-ROM scheduling
- **SPU:** Batch ADSR optimization, non-blocking audio

## Optimization History

| Commit | Optimization | Impact |
|---|---|---|
| c5adc2d | GTE inline all COP2 ops + VRAM memcpy + SMC fix | +15-25% |
| cdf3a2d | VU0 RTPS/RTPT matrix multiply | +0.7-1.0% |
| fb61e84 | VU0 MVMVA (all 3 matrices) | geometry-heavy |
| bdb8725 | VU0 light pipeline dispatch | geometry-heavy |
| f2894ca | Inline SMC page check | small |
| 4257e8e | SPU batch ADSR | -93.5% SPU time |
| 25091f6 | Scratch register cache (T0/T1) | reduced LW/SW |
| 397a7a5 | Dead Code Elimination (backward liveness) | ~5-10% |
| b85b0fe | Per-instruction cycle adjustment | correctness |
| 48f4974 | TLB 1MB alignment + always emit range checks | correctness |
| 1334e30 | ENABLE_PSX_TLB compile flag (OFF by default) | safety |
| 59e3a94 | Pin PSX $gp to EE $fp (13 pinned regs total) | reduced LW/SW |

## Next Steps (Roadmap)

See `docs/jit_optimization_roadmap.md` for detailed analysis.

**Immediate plan:** Refactor to 2-pass compilation (Scan + Emit), then apply:
1. Dirty bitmask for flush_pinned
2. FlushCache batching
3. SMRV (speculative memory region)
4. LQ/SQ bulk flush (PS2-specific)
5. Multi-entry blocks
6. Dynamic register allocation (long-term)

## Communication

- **User speaks Spanish.** Use `ask_questions` tool for all communication.
- Always run GTE 1150/0 + CPU 227 tests before committing.
- Never commit analysis/documentation files unless explicitly asked.
