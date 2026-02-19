# AI / Copilot tips for this repository

This file provides quick instructions for AI assistants and GitHub Copilot when working on this repository.

Quick commands

```bash
# configure and build
cmake -S . -B build
cmake --build build

# run the timers test
make -C build run GAMEARGS=tests/timers/timers.exe
```

Important files

- `include/scheduler.h` — timing constants
- `src/hardware.c` — timer implementation
- `src/dynarec.c` — `next_vblank_cycle` anchor
- `tests/timers` — tests used during development

Notes for assistants

- Prefer using `next_vblank_cycle` as the frame anchor for all PSX timing calculations.
- Use the `.ai/assistant-config.yaml` for build and test commands.
- Use the VS Code tasks for quick runs.

Copilot documentation

Latest Copilot docs: https://docs.github.com/en/copilot
