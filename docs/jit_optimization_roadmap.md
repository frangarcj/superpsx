# JIT Optimization Roadmap — superpsx

## Estado actual (lo que ya tenemos)

| # | Optimización | Estado | Detalles |
|---|---|---|---|
| 1 | Register Pinning | ✅ 4 pinned + 4 infra | gp→S6, sp→S4, fp→S7, ra→S5. Infra: S0=cpu, S1=RAM, S2=cycles, S3=mask |
| 2 | Dynamic Slots (T0-T7) | ✅ 8 slots + dirty writeback | Frequency-based `dyn_assign_slots()`. **Dirty writeback**: only modified slots are flushed to `cpu.regs[]` at sync points via `dyn_dirty_mask`. All 7 flush sites use dirty-only mode. |
| 3 | Constant Propagation | ✅ | `vregs[32]` con `dirty_const_mask`, lazy materialization |
| 4 | Dead Code Elimination | ✅ | Backward liveness en `block_scan()`, ventana 64 insn (`dce_dead_mask`) |
| 5 | Direct Block Linking | ✅ | J-based DBL con back-patching + SMC check (page_gen + hash) |
| 6 | Inline Hash Dispatch | ✅ | JR/JALR, 2-way set-associative `jit_ht[4096]`, ~20 insn trampoline |
| 7 | Cold Slow Paths | ✅ | 256-entry `cold_queue[]`, deferred al final del bloque |
| 8 | Const-Address Fast Paths | ✅ | RAM, scratchpad, I_STAT, I_MASK, GPU_ReadStatus — todos los anchos (B/H/W) |
| 9 | GTE Inline + VU0 | ✅ | 22 ops full inline, zero C-calls. VU0 macro mode (config `gte_use_vu0`) para C path |
| 10 | SMC Detection | ✅ | `jit_page_gen[512]` + djb2 hash + `jit_smc_handler` + inline NULL check |
| 11 | Idle Loop Detection | ✅ | Side-effect scan, `be->is_idle` flag, cycle skipping |
| 12 | Scratch Register Cache | ✅ | T8/T9 cached (`t8_cached_psx_reg`/`t9_cached_psx_reg`), evita LW redundantes |
| 13 | Per-Instruction Cycles | 🔄 Parcial | `r3000a_cycle_cost()` + `emit_cycle_offset`. Cold/abort paths correctos. Branch epilogue usa block total |
| 14 | SPU Batch ADSR | ✅ | Tight batch loop para volumen constante, skip per-sample tick |
| 15 | Super-Blocks (Fall-Through) | ✅ | `MAX_CONTINUATIONS=3`, `MAX_SUPER_INSNS=200`. Conditional branch fall-through inline + deferred taken-path |
| 16 | Ultra-Lite Trampoline | ✅ | Stack save/restore T0-T7 en `code_buffer[68]`. Zero dependency en cpu struct |
| 17 | TLB Backpatching | ✅ | 3-insn fast path (`and,addu,lw/sw`). `TLB_Backpatch()` + `tlb_patch_emit_all()` |
| 18 | Mem Slow-Path Trampoline | ✅ | `code_buffer[128]`: partial cycle accounting (`partial_block_cycles`) |
| 19 | BIOS HLE Native Injection | ✅ | A0/B0/C0 hooks compiled inline en bloques |
| 20 | Branch/Load Delay Slots | ✅ | `in_delay_slot` tracking + `pending_load_reg/apply_now` |
| 21 | CU Exception Dirty Mask Fix | ✅ | save/restore `dyn_dirty_mask` around conditional `emit_call_c(Helper_CU_Exception)` in 9 COP handler sites |

### GTE JIT Optimizations (P-series)

| # | Optimización | Estado | Detalles |
|---|---|---|---|
| P1 | CU2 Hoist to Prologue | ✅ | COP2 24x → ~10x. Single check per block |
| P3 | Inline MTC2/MFC2/LWC2/SWC2 | ✅ | Data transfers inline, no C call (24x → ~5x) |
| P4 | Branchless DIV/DIVU | ✅ | 15x → ~11x via pipeline 1 registers |
| P5 | FP($s8) for jit_ht dispatch | ✅ | JR/JALR fast path |
| P6 | Alignment tracking | ✅ | Skip alignment checks for known-aligned regs (LW 22x → 7.4x) |
| P7 | SWC2 cold slow path | ✅ | Via `cold_slow_push()` API |
| P9 | Delay slot fill | ✅ | Trampolines and cold stubs |
| P10 | Shared cold abort stub | ✅ | SW 24x → 22x |
| P11 | 11 simple GTE ops inline | ✅ | NCLIP, AVSZ3/4, SQR, OP, GPF, GPL, DPCS, INTPL, DCPL, DPCT |
| P12 | NCS family inline (8 ops) | ✅ | NCS/NCT/NCCS/NCCT/CC/CDP/NCDS/NCDT. 10 shared helpers |
| P13 | MVMVA standalone inline | ✅ | Decoded mx/v/cv, bugged paths fall back to C |
| P14 | RTPS/RTPT with C-call div | ✅ | Matrix multiply inline + emit_call_c_lite for UNR division |
| P15 | RTPS/RTPT fully inline | ✅ | Branchless CLZ16 + Newton-Raphson + 64-bit multiply. **Zero C-calls** |
| P16 | FPU DIV.S for RTPS/RTPT | ✅ | Replaces 52-word UNR (CLZ16+Newton+table) with 18-word FPU path. RTPS 155.6x→125.3x |
| P17 | VU0 matrix multiply in JIT | ✅ | VMULAX/VMADDAY/VMADDZ/VADD via VU0JITCache. ~30% faster multiply, C call for refresh |

---

## Optimizaciones pendientes (ordenadas por impacto)

### GTE — Próxima fase

#### P18: Shared Matrix Loads en variantes ×3
**Impacto:** Medio (-36 palabras por NCDT, -18 por RTPT) · **Esfuerzo:** 1 hora · **Riesgo:** Bajo
**Estado:** ✅ Completado

Las variantes ×3 (NCT, NCCT, NCDT, RTPT) recargaban la matriz 3-6 veces.
Con P18, los callers ×3 precargan la(s) matriz(ces) en registros VU0 (VF1-4 y/o VF7-10)
una sola vez y las reutilizan. El array estático `vu0_preloaded[3]` indica a
`emit_inline_mvmva` que la matriz ya está cargada.

Resultados: NCDT 10800→10224 (-576w), NCS family ×3 ~-36w/op, RTPT ~-18w/op.
Standalone ops (RTPS, NCS, NCDS, MVMVA) sin cambios.

#### P19: MMI PMAXW/PMINW para Saturación Batch
**Impacto:** Bajo-Medio (-4 a -6 palabras por ir_sat) · **Esfuerzo:** 3-4 horas · **Riesgo:** Bajo
**Estado:** ✅ Completado

Reemplazar patrones SLT+MOVN (clamp 3 canales) con PMAXW/PMINW del R5900 (signed per-word min/max).
Aplicado a TODAS las saturaciones GTE inline: emit_ir_sat_store, emit_push_color_inline,
emit_interpolate_color, SZ/SXY/IR0 en RTPS, OP, SQR, AVSZ3/4, GPF.

Resultados: RTPS 2256→2032 (-224w, 125x→113x), NCS 2448→2160 (-288w),
NCDS 3632→3152 (-480w), NCDT 10224→8784 (-1440w, 568x→488x),
RTPT 5968→5360 (-608w), NCT 6672→5808 (-864w), NCCT 8496→7344 (-1152w),
MVMVA 1008→912 (-96w), GTE xform 571→529 (-42w).

---

### GPU Optimizations (G-series)

#### G0: VRAM Readback (C0)
**Impacto:** Correctness (BIOS memory card menu) · **Esfuerzo:** 2h · **Riesgo:** Bajo
**Estado:** ✅ Completado (commit 4a11f38)

GS→shadow VRAM sync via `GS_ReadbackRegion()` en handler C0. Strip STP bit 15 (`& 0x7FFF`).
TEXA fix: `GS_SET_TEXA(0x80, 1, 0x80)` para transparencia PSX correcta.

#### G0b: CLD=4 → CLD=1 Fix
**Impacto:** Correctness (palette glitches on real PS2) · **Esfuerzo:** 15 min · **Riesgo:** Muy Bajo
**Estado:** ❌ No empezado

CLD=4 skip-reload cuando round-robin CBP wraps y coincide con CBP0 del GS.
Fix: cambiar 9 sites de CLD=4 a CLD=1 (always reload).

#### G1: CLUT Content Caching
**Impacto:** Alto (~90% CLUT uploads eliminados) · **Esfuerzo:** 2h · **Riesgo:** Bajo
**Estado:** ❌ No empezado — Depende de G0b

Track CLUT region gen via `get_region_gen()`. Solo re-upload cuando CLUT data cambia.
Elimina ~100K uploads/frame en escenas con muchas texturas 8BPP.

#### G2: Wire in `get_tex_combined_gen()`
**Impacto:** Medio (prim_tex_cache 80%→95% hit rate) · **Esfuerzo:** 30 min · **Riesgo:** Bajo
**Estado:** ❌ No empezado

Reemplazar `vram_gen_counter` global con gen combinado page+clut en `prim_tex_cache`.
Elimina false invalidation por FillRect al framebuffer.

#### G3: Multi-Entry prim_tex_cache
**Impacto:** Medio (multi-texture scenes) · **Esfuerzo:** 1h · **Riesgo:** Bajo
**Estado:** ❌ No empezado

Expandir de 1 a 4 entradas direct-mapped. Mejora hit rate para games con 2-4 texturas alternantes.

```
GPU (prioridad):
  G0b. CLD=1 fix                    (15 min — palette correctness)
  G1.  CLUT caching                  (2h — biggest perf win)
  G2.  Combined gen                  (30 min — reduce false invalidation)
  G3.  Multi-entry cache             (1h — multi-texture scenes)
```

---

### Memoria/CPU (pendientes anteriores)

#### SMRV Memory Fast-Path
**Impacto:** Muy Alto (LW/SW: 23-27x → ~8-10x) · **Esfuerzo:** 2-4 horas · **Riesgo:** Medio
**Estado actual:** ❌ No empezado

Current LW/SW emits ~25 words per instruction (ISC + align + range + scratchpad + slow).
With SMRV (base reg known RAM), skip ISC/range/scratchpad: 4 words instead of 25.

---

### 4. DIV Simplification
**Impacto:** Medio (DIV: 15x → ~5x) · **Esfuerzo:** 1 hora · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

Simplify div-by-zero and overflow checks. DIVU only needs BEQ+NOP+DIV+MFHI+MFLO.

---

### 5. SQ/LQ Prologue/Epilogue
**Impacto:** Bajo-Medio (32 → ~20 words per block) · **Esfuerzo:** 2-4 horas · **Riesgo:** Bajo
**Estado actual:** ❌ No empezado

Use PS2 128-bit SQ/LQ to batch register saves/restores (4 SW → 1 SQ).

---

### 6. FlushCache Batching
**Impacto:** Bajo (1-3%) · **Esfuerzo:** 30 min · **Riesgo:** Muy Bajo
**Estado actual:** ❌ No empezado

Batch FlushCache calls across multiple compile_block invocations.

---

## Code Smells / Mejoras menores

| Issue | Descripción | Impacto |
|---|---|---|
| Prologue 26 words | 104 bytes overhead. Super-blocks mitigan (3 continuations = 1 prologue). | Bajo |
| Branch cond en stack | Trade-off por pin $gp. +1 SW/LW por conditional branch. | Aceptable |
| memset en buffer reset | `compile_block` hace memset de hasta 4MB. Innecesario. | Bajo |
| DCE ventana 64 insn | `uint64_t` bitmask. Expandible a 128 con `__uint128_t`. | Bajo |
| DMA linked-list 100K | Safety counter puede truncar display lists legítimas. | Medio |

---

## Prioridad recomendada

```
GTE (próxima fase):
  P16. FPU DIV.S para división      ✅ DONE (RTPS: 155x → 125x, RTPT: 438x → 348x)
  P17. VU0 matrix multiply en JIT   ✅ DONE (~30% faster multiply via VMULAX/VMADDAY/VMADDZ/VADD)
  P18. Shared matrix loads ×3       ✅ DONE (NCDT: 600x→568x, 4 fewer C calls per ×3 op)

CPU/Memoria (pendientes):
  1. SMRV memory fast-path           (2-4h — LW/SW: 23-27x → 8-10x)
  2. DIV simplification              (1h — DIV: 15x → 5x)
  3. SQ/LQ prologue/epilogue         (2-4h — prologue: 32 → 20 words)
  4. FlushCache batching             (30 min — runtime only)
```

**Expansion ratio data:** see `tests/jit/test_expansion.c` (playground compile-only measurement).

---

## Profile: Crash Bandicoot (post texture cache overhaul)

Measured after direct-map texture cache + CLUT round-robin (commit 3fcf97b).

| Categoría | % del frame | Notas |
|---|---|---|
| JIT Execution | 75.1% | Principal bottleneck — incluye todo el R3000A emulado |
| GPU TexCache | 6.1% avg, 17-25% picos | Decode PSMT8/4 + CLUT upload. Picos en escenas con muchas texturas |
| GPU Primitives | 5.4% | Traducción GP0 → GS |
| GPU GIF Flush | 4.2% | DMA batch al GS |
| SPU | ~2% | Batch ADSR optimizado |
| Otros | ~7% | Scheduler, SIO, CDROM, etc. |

**Velocidad general:** 55.3% (30.1ms/frame vs 16.6ms target @ 60fps).

### Fases de ejecución (detalle)

| Fase | Speed | Bottleneck | JIT% | GPU TexCache% |
|---|---|---|---|---|
| BIOS Boot | 45% | JIT (75-98%) | 95% | — |
| Game Init | 87% | JIT (93%) | 93% | — |
| Logo/Menú | **100%** | Balanced | 60% | <1% |
| Level Loading | 38% | JIT (96%) | 96% | — |
| **3D Gameplay** | **34-55%** | **JIT + GPU TexCache** | 50-75% | **6-25%** |

**Hotspot #1:** `PC=80034504` (kernel idle/scheduler) con 8-23M cycles.
**JIT compilation:** <0.1% en steady-state.

### Comparación antes/después del texture cache overhaul

El cambio de LRU hash-based a direct-map eliminó búsqueda lineal y redujo el overhead
promedio de TexCache. Los picos siguen altos porque CLUT decode + CSM1 shuffle son
operaciones inherentemente costosas (256 entries × byte shuffle + GS upload).
