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

# GTE test (expect: 1150 passed, 0 failed)
rm -f build/superpsx.ini && printf "gte_vu0 = 0\n" > build/superpsx.ini && \
perl -e 'alarm 60; exec @ARGV' make -C build run GAMEARGS=tests/gte/test-all/test-all.exe 2>&1 | \
grep -E "Passed|Failed" | head -5; \
rm -f build/superpsx.ini && ln -s $(pwd)/superpsx.ini build/superpsx.ini

# CPU test (expect: 227)
perl -e 'alarm 120; exec @ARGV' make -C build run GAMEARGS=tests/psxtest_cpu/psxtest_cpu.exe 2>&1 | grep -c "error"

# Timer test
perl -e 'alarm 60; exec @ARGV' make -C build run GAMEARGS=tests/timers/timers.exe 2>&1 | tail -20

# Crash Bandicoot
make -C build run GAMEARGS=isos/CrashBandicoot/CrashBandicoot.cue
```

**IMPORTANT:** macOS has no `timeout`/`gtimeout`. Use `perl -e 'alarm N; exec @ARGV'` for timeouts.

## Testing Protocol

Before committing ANY change to the dynarec or emulation core:
1. Build must succeed with zero warnings (except known ones in tlb_handler.c when TLB disabled)
2. GTE: 1150 passed, 0 failed
3. CPU: 227 (known deviation count — must not change)
4. Timer test: must complete without hangs

## Code Conventions

- **C99** (compiled with ee-gcc for PS2 EE target)
- All dynarec source files are in `src/dynarec_*.c` with shared header `src/dynarec.h`
- MIPS instruction encoding macros: `MK_R()`, `MK_I()`, `MK_J()` in `dynarec.h`
- Emit macros: `EMIT_LW()`, `EMIT_SW()`, `EMIT_MOVE()`, `EMIT_NOP()`, etc.
- CMake options pattern: `option(ENABLE_XXX "desc" ON/OFF)` + `target_compile_definitions(... PRIVATE ENABLE_XXX)`

## JIT Register Allocation

13 PSX registers are permanently pinned to EE hardware registers:
- v0→S6, v1→V1, a0-a3→T3-T6, t0-t2→T7-T9, gp→FP, sp→S4, s8→S7, ra→S5
- Infrastructure: S0=cpu ptr, S1=RAM/TLB base, S2=cycles, S3=mask(0x1FFFFFFF)
- Scratch: T0, T1, T2, AT (T0/T1 have a scratch cache for non-pinned regs)

The other 19 PSX GPRs go through `LW/SW` to `cpu.regs[]` (offset from S0).

## Code Buffer Layout

Trampolines at fixed offsets in `code_buffer[]`:
- [0]: slow-path, [2]: abort, [32]: full C-call, [68]: lite C-call, [96]: jump dispatch, [128]: mem slow-path
- JIT blocks start at [144+]
- `DYNAREC_PROLOGUE_WORDS = 29` (skip in direct block links)

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
