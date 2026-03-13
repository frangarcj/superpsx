# SuperPSX — Copilot Instructions

## Project Context

SuperPSX is a PSX (PlayStation 1) emulator running **natively on PlayStation 2** hardware (EE/R5900 CPU at ~294MHz). It uses a MIPS→MIPS JIT recompiler (dynarec) since both the PSX (R3000A) and PS2 (R5900) share the MIPS instruction set.

The emulator runs inside **PCSX2** for development/testing. The final target is real PS2 hardware.

A **PSP (Allegrex) port** also exists, running on PPSSPP for development. The PSP uses a similar MIPS ISA but lacks R5900 extensions (VU0, MMI, TLB, dual HI/LO).

## Communication Rules

- **ALWAYS use the `ask_questions` tool** to communicate with the user. The user speaks Spanish.
- **NEVER write plain text responses** for questions, confirmations, or status updates. Route everything through `ask_questions`.
- After completing a task, ask what to do next via `ask_questions`.
- If the user's intent is ambiguous, clarify via `ask_questions` before proceeding.

## Build & Test Commands

```bash
# Build (from workspace root)
cmake --build build 2>&1 | tail -5

# GTE test (expect: 1150 passed, 0 failed) — 20s is enough
rm -f build/superpsx.ini && printf "gte_vu0 = 0\n" > build/superpsx.ini && \
perl -e 'alarm 20; exec @ARGV' make -C build run \
  GAMEARGS=tests/gte/test-all/test-all.exe > ./build/gte_out.txt 2>&1; \
pkill -f pcsx2 2>/dev/null; \
grep -E "Passed|Failed" ./build/gte_out.txt | head -5; \
rm -f build/superpsx.ini && ln -sf $(pwd)/superpsx.ini build/superpsx.ini

# CPU test (expect: Result 00000101) — 20s is enough
perl -e 'alarm 20; exec @ARGV' make -C build run \
  GAMEARGS=tests/psxtest_cpu/psxtest_cpu.exe > ./build/cpu_out.txt 2>&1; \
pkill -f pcsx2 2>/dev/null; \
grep 'Result:' ./build/cpu_out.txt | head -3

# Timer test — 20s is enough
perl -e 'alarm 20; exec @ARGV' make -C build run \
  GAMEARGS=tests/timers/timers.exe > ./build/timer_out.txt 2>&1; \
pkill -f pcsx2 2>/dev/null; \
tail -5 ./build/timer_out.txt

# Crash Bandicoot (manual test — ask user)
make -C build run GAMEARGS=isos/CrashBandicoot/CrashBandicoot.cue
```

**IMPORTANT:**

- All tests run BIOS first, so if you broke jit maybe the Phase 1 (BIOS) test will fail instead of Phase 2 (CPU/GTE). Check the actual test output to confirm.
- macOS has no `timeout`/`gtimeout`. Use `perl -e 'alarm N; exec @ARGV'` for timeouts.
- **Always redirect to file** (`> ./build/out.txt 2>&1`), NEVER pipe (`|`). Pipes cause SIGPIPE to kill the emulator prematurely.
- After each `perl -e 'alarm ...'` test, add `pkill -f pcsx2 2>/dev/null` to clean up the PCSX2 process.
- For GPU/rendering changes, do NOT run automated tests — ask the user to launch Crash Bandicoot and MK2 manually and report results.

## PSP Build & Test Commands

```bash
# Configure PSP build (separate build directory)
cmake -S . -B build-psp 2>&1 | tail -5

# Build PSP main target
cmake --build build-psp 2>&1 | tail -5

# Build PSP playground
cmake --build build-psp --target jit_playground.elf 2>&1 | tail -5

# Run PSP playground (expect: 110/110 passed) — 25s is enough
/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL -i -v \
  build-psp/jit_playground.elf > ./build/psp_playground_out.txt 2>&1 &
PID=$!; sleep 25; kill $PID 2>/dev/null; wait $PID 2>/dev/null; \
grep "PRINTF" ./build/psp_playground_out.txt | sed 's/.*stdout: //' | \
  grep -E "Results|FAIL"
```

**PSP IMPORTANT:**

- PSP build uses `$PSPDEV/psp/share/pspdev.cmake` toolchain, auto-detected by CMake.
- PPSSPP at `/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL`.
- Use `-i` (interpreter) and `-v` (verbose) flags to capture `printf` output as `I[PRINTF]` lines.
- `printf` output is in PPSSPP's log at `I[PRINTF]: HLE/sceIo.cpp:... stdout: <text>`.
- PSP SDK libraries must be linked LAST (after `-lm -lc`). Never link both `-lpspuser` and `-lpspkernel` — use only `-lpspkernel` to avoid "stubs out of order" error.
- `psp-fixup-imports` must run on every ELF after linking (handled automatically by `create_pbp_file` for main target; added as POST_BUILD for playground).

## JIT Playground

A separate ELF (`jit_playground.elf`) for testing the dynarec in isolation with a mini-DSL. 110 micro-tests split across 8 category files, plus an expansion ratio report.

```bash
# Build playground (EXCLUDE_FROM_ALL — not built by default)
cmake --build build --target jit_playground.elf 2>&1 | tail -5

# Run playground (expect: 135/135 passed on PS2, 110/110 on PSP) — 20s is enough
perl -e 'alarm 20; exec @ARGV' make -C build run-playground \
  > ./build/playground_out.txt 2>&1; \
pkill -f pcsx2 2>/dev/null; \
grep -E "Results|FAIL" ./build/playground_out.txt
```

**Key files:**

- `tests/jit/playground.h` — DSL header (opcode encoding macros, test framework macros)
- `tests/jit/playground_main.c` — Entry point, `pg_run_jit()` dispatch loop
- `tests/jit/playground_tests.c` — Thin runner calling all category runners
- `tests/jit/test_alu.c` — ALU, shifts, mul/div, comparisons, HI/LO (21 tests)
- `tests/jit/test_memory.c` — Load/Store + ISC: LW/SW, LB/SB, LH/SH, LWL/LWR, SWL/SWR, MFC0, ISC checks (13 tests)
- `tests/jit/test_branch.c` — Branches: BEQ, BNE, BLTZ, BGEZ, BLEZ, BGTZ, delay slots (7 tests)
- `tests/jit/test_block.c` — Interactions, cross-block, loops, nested JAL, all-32-regs, dynamic alloc stress, prologue/pin (21 tests)
- `tests/jit/test_dirty.c` — Dirty writeback: single/multi slot, cross-block, branch paths, loop, SW-to-RAM, three-block chain (10 tests)
- `tests/jit/test_gte.c` — GTE/COP2: MTC2/MFC2 round-trip, slot preservation, CU2 dirty-mask regression (5 tests)
- `tests/jit/test_expansion.c` — Expansion ratio report (compile-only, no pass/fail; measures EE words per PSX instruction)
- `docs/jit_playground.md` — Design document

**Adding new tests:** Write a `static void test_xxx(void)` in the appropriate category file using `BEGIN_TEST/SET_REG/EMIT/RUN/EXPECT_REG/END_TEST` macros, then call it from the category runner (`pg_run_alu_tests`, `pg_run_memory_tests`, `pg_run_branch_tests`, `pg_run_block_tests`, `pg_run_dirty_tests`, or `pg_run_gte_tests`).

**Before committing JIT changes:** run the playground (`82/82 passed`) in addition to the standard GTE/CPU/Timer tests.

## Testing Protocol

Before committing ANY change to the dynarec or emulation core:

1. Build must succeed with zero warnings (except known ones in tlb_handler.c when TLB disabled)
2. **JIT Playground: 110/110 passed** (for dynarec changes)
3. GTE: 1150 passed, 0 failed
4. CPU: Result 00000101
5. Timer test: must complete without hangs
6. **For GPU/rendering changes:** ask the user to test Crash Bandicoot and MK2 manually

## Code Conventions

- **C99** (compiled with ee-gcc for PS2 EE target)
- All dynarec source files are in `src/dynarec_*.c` with shared header `src/dynarec.h`
- MIPS instruction encoding macros: `MK_R()`, `MK_I()`, `MK_J()` in `dynarec.h`
- Emit macros: `EMIT_LW()`, `EMIT_SW()`, `EMIT_MOVE()`, `EMIT_NOP()`, etc.
- CMake options pattern: `option(ENABLE_XXX "desc" ON/OFF)` + `target_compile_definitions(... PRIVATE ENABLE_XXX)`

## JIT Register Allocation

4 PSX registers are permanently pinned to EE callee-saved registers:

- Pinned: gp→S6, sp→S4, fp→S7, ra→S5
- Infrastructure: S0=cpu ptr, S1=RAM/TLB base, S2=cycles, S3=mask(0x1FFFFFFF), FP($s8)=&jit_ht[0]
- Dynamic slots: T0-T7 (8 slots, frequency-based per-block assignment, dirty writeback)
- Scratch: T8, T9 (with scratch cache for non-pinned regs), AT

The other 27 non-pinned PSX GPRs compete for 8 dynamic slots (T0-T7). Unslotted regs go through `LW/SW` to `cpu.regs[]` (offset from S0).

### Alignment Tracking

`align_known_mask` (uint32_t bitmask) tracks PSX registers known to be word-aligned at compile time. Pinned regs ($gp/$sp/$fp/$ra) are always marked via `ALIGN_PINNED_MASK`. Propagation rules: LUI → always aligned, ADDIU/ADDI from aligned + `(imm & 3)==0` → aligned, MOVE from aligned → aligned. When alignment is known AND offset is naturally aligned, memory emitters skip the ANDI+BNE alignment check (saves 2 native words per access).

### Cold Slow Path (P7/P10)

Memory access slow paths (IO, misaligned, out-of-range) are deferred to block end via `cold_queue[]`. Each cold entry patches forward branches from the inline fast path. For writes with `has_abort=1`, a shared per-block abort check subroutine is emitted once and called via JAL (2 words per entry vs 5-13 inline). The shared stub flushes all assigned dynamic slots and jumps to the abort trampoline.

Externally pushed cold entries (e.g., SWC2 inline from dynarec_insn.c) use `cold_slow_push()` API.

### Dirty Writeback Protocol

Dynamic slots use compile-time dirty tracking via `dyn_dirty_mask` (uint8_t):

- `emit_store_psx_reg` / `emit_sync_reg` / `flush_dirty_consts`: update slot EE register + set dirty bit
- `dyn_flush_dirty_slots()`: emit SW only for dirty slots, clear mask
- `dyn_flush_all_slots()`: emit SW for ALL assigned slots, clear mask

7 flush sites labeled A-G — **ALL use dirty-only** flush:

- **A** (emit_call_c): `dirty-only` — mid-block full C call
- **B** (emit_call_c_lite): `dirty-only` — mid-block lite C call
- **C** (emit_abort_check / P10 shared stub): `dirty-only` (inline) or `all-assigned` (shared stub) — conditional abort path
- **D** (deferred taken): `dirty-only` — branch-taken cold code
- **E** (block epilogue): `dirty-only` — block exit return to C
- **F** (branch epilogue): `dirty-only` — direct block link exit
- **G** (JR/JALR dispatch): `dirty-only` — register jump exit

**Critical invariant:** Any `emit_call_c` inside a conditional (BNE-skipped) code path
must save/restore `dyn_dirty_mask`. The CU exception checks (COP1/COP2/COP3/LWC*/SWC*)
all do this with `{ uint8_t saved = dyn_dirty_mask; emit_call_c(...); dyn_dirty_mask = saved; }`.
Without this, the compile-time dirty mask is cleared but the runtime SWs only execute
on the exception path, silently losing dirty slot values on the normal path.

## Code Buffer Layout

Trampolines at fixed offsets in `code_buffer[]`:

- [0]: slow-path, [2]: abort, [32]: full C-call, [68]: lite C-call, [96]: jump dispatch, [128]: mem slow-path
- JIT blocks start at [144+]
- `DYNAREC_PROLOGUE_WORDS` = 22 (skip in direct block links)

## Current Roadmap

See `docs/jit_optimization_roadmap.md` for the master roadmap.

Completed optimizations (P1-P15):

- P1: CU2 hoist to prologue (COP2 24x → ~10x)
- P3/P3ext: Inline MTC2/MFC2/LWC2/SWC2 data transfers (24x → ~5x)
- P4: Branchless DIV/DIVU (15x → ~11x)
- P5: FP($s8) for jit_ht fast dispatch
- P6: Alignment tracking — skip alignment checks for known-aligned regs (LW 22x → 7.4x)
- P7: SWC2 cold slow path via cold_slow_push() API
- P9: Fill delay slots in trampolines and cold stubs
- P10: Shared cold abort check stub (SW 24x → 22x)
- P11: 11 simple GTE ops fully inline (NCLIP, AVSZ3/4, SQR, OP, GPF, GPL, DPCS, INTPL, DCPL, DPCT)
- P12: NCS family inline — 8 ops (NCS/NCT/NCCS/NCCT/CC/CDP/NCDS/NCDT) with 10 shared helpers
- P13: MVMVA standalone inline (decoded mx/v/cv, bugged paths fall back to C)
- P14: RTPS/RTPT inline with C-call division (emit_call_c_lite for UNR)
- P15: RTPS/RTPT fully inline — branchless CLZ16 + Newton-Raphson + 64-bit multiply. Zero C-calls.
- P16: FPU DIV.S for RTPS/RTPT perspective division — replaces 52-word UNR (CLZ16+Newton+table) with 18-word FPU path.
- P17: VU0 matrix multiply in JIT — emit_inline_mvmva uses VMULAX/VMADDAY/VMADDZ/VADD via VU0JITCache. ~30% faster multiply, same code size. C call for matrix refresh.
- P18: Shared matrix loads in ×3 variants — vu0_preloaded[] array lets ×3 callers (RTPT/NCT/NCDT/NCCT) preload matrices into VF1-4/VF7-10 once, skipping redundant C calls. NCDT: 600x→568x, 4 fewer lite calls per ×3 op.
- P19: MMI PMAXW/PMINW saturation — replace SLT+MOVN clamp patterns with R5900 PMAXW/PMINW (signed per-word min/max). Saves ~6 words per 3-channel clamp across all GTE inline ops. RTPS: 125x→113x, NCDT: 568x→488x.
- P20: RTPT vertex overlap — hide VU0 micro latency behind EE projection. While EE projects vertex N, VU0 computes vertex N+1 matrix multiply. RTPT: 297.8x→269.3x, micro speed 97.5%→102.7%.
- P21: NCS family ×3 vertex overlap — NCT, NCDT, NCCT. Same pattern: overlap Light×V(N+1) with post-lighting of V(N). NCT: 322.7x→283.6x, NCDT: 488.0x→448.9x, NCCT: 408.0x→368.9x.
- P22: Inline ISC skip for pure RAM stores — when SMRV+aligned+block_isc_cached, replace cold-path branch with inline BNE skip. SW: 395→164 (21.9x→9.1x, -58%), SB: 355→164 (-54%), SWC2: 422→192 (-54%). Buffer -14.5%.

Current expansion baselines (135/135 playground tests):

- ALU: ADDU 40 (2.2x), ADDIU 39 (2.2x), SLL 39 (2.2x), LUI 38 (2.1x)
- MulDiv: MULT 149 (8.3x), DIV 197 (10.9x), DIVU 165 (9.2x)
- Memory: LW 133 (7.4x), SW 164 (9.1x), LB 133 (7.4x), SB 164 (9.1x)
- GTE: MTC2 82 (4.6x), MFC2 83 (4.6x), LWC2 144 (8.0x), SWC2 192 (10.7x)
- GTE commands: RTPS 1808 (100.4x), NCLIP 288 (16.0x), MVMVA 912 (50.7x)
- GTE lighting: NCS 1712 (95.1x), NCDS 2704 (150.2x), NCDT 8080 (448.9x)
- GTE ×3: RTPT 4848 (269.3x), NCT 5104 (283.6x), NCCT 6640 (368.9x)
- Mixed: GTE xform 435 (24.2x), SW burst 167 (9.3x)

Next optimization targets (P23+):

## File Management

- **NEVER commit analysis/documentation markdown files** unless the user explicitly asks for it.
- Keep `docs/` for planning documents (not committed to git unless requested).
- The `jit_optimization_analysis.md` was removed from git — do not re-add.

## Git Workflow

- Always `git add -A && git diff --cached --stat` before committing to review changes.
- Use descriptive commit messages with test results (GTE/CPU/Timer).
- Force push only when user requests (with `--force-with-lease`).

## JIT Debugging Methodology (Chain-of-Thought)

When a JIT change causes a regression:

1. **Always dump generated code** — don't guess what the JIT emits. Add hex/disasm dumps of:
   - Trampoline regions (code_buffer[32..143]) at init time
   - Compiled blocks (first N blocks: PC, native word count, hex dump)
   - Cold-slow / TLB stubs (generated per-block at end of compilation)
2. **Compare dumps** between working and broken versions before forming hypotheses.
3. **Analyze the actual instruction stream** — trace register usage, verify delay slots, check branch offsets.
4. **Bisection + Code Analysis**: bisect to narrow down the failing code region, then dump the exact generated code to identify the root cause. Don't rely solely on test pass/fail — read the actual machine instructions.
5. This is a CoT (Chain-of-Thought) approach: hypothesize → verify with data → refine → fix.

THIS METHODOLOGY IS CRITICAL. Avoid guessing or making assumptions about what the JIT is doing — always verify with actual dumps of the generated code.
