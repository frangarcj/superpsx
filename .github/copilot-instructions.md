# SuperPSX — Copilot Instructions

## Project Context

SuperPSX is a PSX (PlayStation 1) emulator running **natively on PlayStation 2** hardware (EE/R5900 CPU at ~294MHz). It uses a MIPS→MIPS JIT recompiler (dynarec) since both the PSX (R3000A) and PS2 (R5900) share the MIPS instruction set.

The emulator runs inside **PCSX2** for development/testing. The final target is real PS2 hardware.

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
  GAMEARGS=tests/gte/test-all/test-all.exe > /tmp/gte_out.txt 2>&1; \
grep -E "Passed|Failed" /tmp/gte_out.txt | head -5; \
rm -f build/superpsx.ini && ln -sf $(pwd)/superpsx.ini build/superpsx.ini

# CPU test (expect: Result 00000101) — 20s is enough
# NOTE: CPU test output goes to PCSX2 emulog, NOT stdout/stderr.
# Must clear emulog before running, then check it after.
cat /dev/null > ~/Library/Application\ Support/PCSX2/logs/emulog.txt && \
perl -e 'alarm 20; exec @ARGV' make -C build run \
  GAMEARGS=tests/psxtest_cpu/psxtest_cpu.exe > /tmp/cpu_out.txt 2>&1; \
grep 'Result:' ~/Library/Application\ Support/PCSX2/logs/emulog.txt | head -3

# Timer test — 20s is enough
perl -e 'alarm 20; exec @ARGV' make -C build run \
  GAMEARGS=tests/timers/timers.exe > /tmp/timer_out.txt 2>&1; \
tail -5 /tmp/timer_out.txt

# Crash Bandicoot (manual test — ask user)
make -C build run GAMEARGS=isos/CrashBandicoot/CrashBandicoot.cue
```

**IMPORTANT:**
- macOS has no `timeout`/`gtimeout`. Use `perl -e 'alarm N; exec @ARGV'` for timeouts.
- **Always redirect to file** (`> /tmp/out.txt 2>&1`), NEVER pipe (`|`). Pipes cause SIGPIPE to kill the emulator prematurely.
- **PCSX2 emulog.txt is CUMULATIVE** across runs. Always clear it before each test run: `cat /dev/null > ~/Library/Application\ Support/PCSX2/logs/emulog.txt`
- For GPU/rendering changes, do NOT run automated tests — ask the user to launch Crash Bandicoot and MK2 manually and report results.

## JIT Playground

A separate ELF (`jit_playground.elf`) for testing the dynarec in isolation with a mini-DSL. 62 micro-tests split across 4 category files.

```bash
# Build playground (EXCLUDE_FROM_ALL — not built by default)
cmake --build build --target jit_playground.elf 2>&1 | tail -5

# Run playground (expect: 62/62 passed) — 15s is enough
perl -e 'alarm 15; exec @ARGV' make -C build run-playground \
  > /tmp/playground_out.txt 2>&1; \
grep -E "Results|FAIL" /tmp/playground_out.txt
```

**Key files:**
- `tests/jit/playground.h` — DSL header (opcode encoding macros, test framework macros)
- `tests/jit/playground_main.c` — Entry point, stubs, `pg_run_jit()` dispatch loop
- `tests/jit/playground_tests.c` — Thin runner calling the 4 category runners
- `tests/jit/test_alu.c` — ALU, shifts, mul/div, comparisons, HI/LO (21 tests)
- `tests/jit/test_memory.c` — Load/Store + ISC: LW/SW, LB/SB, LH/SH, LWL/LWR, SWL/SWR, MFC0, ISC checks (13 tests)
- `tests/jit/test_branch.c` — Branches: BEQ, BNE, BLTZ, BGEZ, BLEZ, BGTZ, delay slots (7 tests)
- `tests/jit/test_block.c` — Interactions, cross-block, loops, nested JAL, all-32-regs, dynamic alloc stress, prologue/pin (21 tests)
- `docs/jit_playground.md` — Design document

**Adding new tests:** Write a `static void test_xxx(void)` in the appropriate category file using `BEGIN_TEST/SET_REG/EMIT/RUN/EXPECT_REG/END_TEST` macros, then call it from the category runner (`pg_run_alu_tests`, `pg_run_memory_tests`, `pg_run_branch_tests`, or `pg_run_block_tests`).

**Before committing JIT changes:** run the playground (`62/62 passed`) in addition to the standard GTE/CPU/Timer tests.

## Testing Protocol

Before committing ANY change to the dynarec or emulation core:

1. Build must succeed with zero warnings (except known ones in tlb_handler.c when TLB disabled)
2. **JIT Playground: 62/62 passed** (for dynarec changes)
3. GTE: 1150 passed, 0 failed
4. CPU: Result 00000101 (check PCSX2 emulog.txt, NOT stdout)
5. Timer test: must complete without hangs
6. **For GPU/rendering changes:** ask the user to test Crash Bandicoot and MK2 manually

## Code Conventions

- **C99** (compiled with ee-gcc for PS2 EE target)
- All dynarec source files are in `src/dynarec_*.c` with shared header `src/dynarec.h`
- MIPS instruction encoding macros: `MK_R()`, `MK_I()`, `MK_J()` in `dynarec.h`
- Emit macros: `EMIT_LW()`, `EMIT_SW()`, `EMIT_MOVE()`, `EMIT_NOP()`, etc.
- CMake options pattern: `option(ENABLE_XXX "desc" ON/OFF)` + `target_compile_definitions(... PRIVATE ENABLE_XXX)`

## JIT Register Allocation

10 PSX registers are permanently pinned to EE hardware registers:

- Caller-saved: v0→T3, v1→T4, a0→T5, a1→T6, a2→T7
- Callee-saved: s0→S6, s1→S7, gp→FP, sp→S4, ra→S5
- Infrastructure: S0=cpu ptr, S1=RAM/TLB base, S2=cycles, S3=mask(0x1FFFFFFF)
- Dynamic slots: T0, T1, T2 (frequency-based per-block assignment, partial dirty tracking)
- Scratch: T8, T9 (with scratch cache for non-pinned regs), AT

The other 19 PSX GPRs go through `LW/SW` to `cpu.regs[]` (offset from S0).

Dynamic slots use partial dirty tracking: `emit_cpu_field_to_psx_reg`, `emit_materialize_psx_imm`, and `flush_dirty_consts` defer writes to cpu.regs[]. `emit_store_psx_reg` and `emit_sync_reg` use write-through (required for game correctness — root cause TBD).

## Code Buffer Layout

Trampolines at fixed offsets in `code_buffer[]`:

- [0]: slow-path, [2]: abort, [32]: full C-call, [68]: lite C-call, [96]: jump dispatch, [128]: mem slow-path
- JIT blocks start at [144+]
- `DYNAREC_PROLOGUE_WORDS = 26` (skip in direct block links)

## Current Roadmap

See `docs/jit_optimization_roadmap.md`. Next major task: refactor to 2-pass compilation (Scan + Emit).

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
