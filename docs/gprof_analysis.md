# gprof Analysis — Crash Bandicoot (~4717 frames)

## Flat Profile Top 10

| # | Function | % Time | Calls | Calls/Frame | Notes |
|---|----------|--------|-------|-------------|-------|
| 1 | SIO_Write | 34.6% | 9.2M | 1,960 | Controller polling via emit_call_c_lite |
| 2 | get_psx_code_ptr | 14.3% | 88k | 18.6 | Compilation-only, per-instruction fetch |
| 3 | GPU_ReadStatus | 10.7% | 9.2M | 1,950 | DrawSync polling; 90% already inline |
| 4 | vu0_rt_is_dirty | 8.4% | 1.8M | 382 | 8-comparison loop, per vertex transform |
| 5 | dynarec_ensure_block | 5.7% | 300k | 63.6 | Per-block dispatch, 99.3% cache hit |
| 6 | Translate_GP0_to_GS | 3.9% | 882k | 187 | GPU primitive translation |
| 7 | GPU_TryFastEmit | 3.7% | 1.9M | 405 | GPU fast emit path |
| 8 | process_key_on | 3.6% | 124k | 26.3 | SPU voice key-on |
| 9 | Timers_Read | 3.4% | 9.2M | 1,960 | Timer polling (same loops as SIO/GPU) |
| 10 | vu0_bk_is_dirty | 1.4% | 958k | 203 | Background matrix dirty check |

**Note:** -pg overhead (~25 cycles/call for _mcount) inflates functions with high call counts.
Estimated real % for SIO_Write: ~25-28%. Still #1 by far.

## Optimization Opportunities

### O1: SIO_Write Inline Fast-Path (estimated -15-20% total)
- 9.2M calls = controller polling in tight BIOS loop
- Currently full C call via emit_call_c_lite for every SIO_DATA write
- **Idea:** Inline the common case (sio_state > 0, just advance response index)
- Fallback to C only for initial command bytes (0x01, 0x81)
- Or: detect SIO polling loop as P-SIO pattern → skip to completion

### O2: vu0 Matrix Dirty — MTC2 Invalidation (estimated -6-8% total)
- vu0_rt_is_dirty (8.4%) + vu0_bk_is_dirty (1.4%) + vu0_lt_is_dirty (1.2%) + vu0_lc_is_dirty (0.5%) = **11.5% total**
- Currently: 8-comparison loop called per GTE transform
- **Idea:** Set dirty flag at MTC2 time (write to control reg). Transform checks 1 flag instead of 8 compares.
- MTC2 to ctrl regs is rare vs transform frequency → massive savings

### O3: get_psx_code_ptr Inline (estimated -8-10% total)
- 99% of calls are RAM/BIOS → simple pointer arithmetic
- **Idea:** Move fast-path to static inline in header. Or use per-block opcode pointer.
- `compile_block()` already gets ptr at start; could pass it through analysis functions.

### O4: Timers_Read Fast-Path (estimated -2-3% total)
- 9.2M calls from polling loops
- Could cache timer value and only recalc on timer event

### O5: GPU_ReadStatus (already 90% inline)
- Remaining 10% goes to C for gpu_busy_until check
- Could extend inline path to handle busy-until skip
