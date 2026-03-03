# JIT Optimization Roadmap — superpsx

## Estado actual (lo que ya tenemos)

| # | Optimización | Estado | Detalles |
|---|---|---|---|
| 1 | Register Pinning | ✅ 10 GPR + 4 infra | v0,v1,a0-a2→T3-T7 · s0,s1→S6,S7 · gp→FP · sp→S4 · ra→S5. Infra: S0=cpu, S1=RAM, S2=cycles, S3=mask |
| 2 | Dynamic Slots (T0/T1/T2) | ✅ 3 slots | Frecuencia-based `dyn_assign_slots()`. Write-through exclusivo. Lazy const desync fix en `emit_load_psx_reg` |
| 3 | Constant Propagation | ✅ | `vregs[32]` con `dirty_const_mask`, lazy materialization |
| 4 | Dead Code Elimination | ✅ | Backward liveness en `block_scan()`, ventana 64 insn (`dce_dead_mask`) |
| 5 | Direct Block Linking | ✅ | J-based DBL con back-patching + SMC check (page_gen + hash) |
| 6 | Inline Hash Dispatch | ✅ | JR/JALR, 2-way set-associative `jit_ht[4096]`, ~20 insn trampoline |
| 7 | Cold Slow Paths | ✅ | 256-entry `cold_queue[]`, deferred al final del bloque |
| 8 | Const-Address Fast Paths | ✅ | RAM, scratchpad, I_STAT, I_MASK, GPU_ReadStatus — todos los anchos (B/H/W) |
| 9 | GTE Inline + VU0 | ✅ | 22 `GTE_Inline_*` wrappers + VU0 macro mode (config `gte_use_vu0`) |
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

---

## Optimizaciones pendientes (ordenadas por impacto/esfuerzo)

### 1. ~~Dirty Bitmask para flush_pinned~~ → Impacto mínimo
**Impacto:** ~~Alto (5-10%)~~ → **<0.2%** tras optimizaciones de trampoline  
**Estado:** 🔄 Infraestructura lista, pero **no vale la pena cablear**

La estimación original asumía flush por bloque y por C call. Tras las optimizaciones actuales:
- Hot path (DBL): **zero flush** — regs se mantienen en HW
- Abort trampoline: compartido → no puede usar mask por bloque
- call_c trampoline: compartido → flush completo para seguridad
- Único flush emitido: `emit_block_epilogue()` (solo SYSCALL/BREAK = raro)

Overhead real: ~590 SWs/frame (~0.2% del tiempo de frame). No justifica el esfuerzo.

---

### 2. SMRV (Speculative Memory Region Validation)
**Impacto:** Alto (5-8%) · **Esfuerzo:** 2-4 horas · **Riesgo:** Medio  
**Estado actual:** ❌ No empezado

El hot path de LW/SW emite ~8 instrucciones: `AND S3` → `SRL+SLTIU` range check → branch → `ADDU+LW`. Con predicción de que el base register apunta a RAM (>95% de accesos), se emite un fast path optimista sin range check.

**Alternativa simple:** Inline cache per-site. Cold path backpatchea el branch para fast path directo la próxima vez.

---

### 3. FlushCache Batching
**Impacto:** Bajo-Medio (1-3%) · **Esfuerzo:** 30 min · **Riesgo:** Bajo  
**Estado actual:** ❌ No empezado

`FlushCache(0); FlushCache(2);` se llama después de cada `compile_block`. En PS2 cada FlushCache es un syscall (~50 ciclos EE). Con batching se acumula y se hace flush una vez antes de ejecutar.

---

### 4. LQ/SQ Bulk Flush/Reload (PS2-específico)
**Impacto:** Medio (3-5%) · **Esfuerzo:** 1 día · **Riesgo:** Bajo  
**Estado actual:** ❌ No empezado

EE tiene LQ/SQ (128-bit load/store). `emit_flush_pinned` pasaría de 10 SW a 3-4 SQ. `emit_reload_pinned` igual.

**Requisito:** `cpu.regs[]` 16-byte aligned. Pinned regs: 2-7,28-31 (casi consecutivos excepto gap 8-27). SQ para regs[0..3], regs[4..7], regs[28..31].

---

### 5. Dynamic Register Allocation (full)
**Impacto:** Crítico (15-25%) · **Esfuerzo:** 2-4 semanas · **Riesgo:** Alto  
**Estado actual:** 🔄 Parcial (3 dynamic slots T0/T1/T2)

Ya tenemos 3 write-through dynamic slots que cubren los regs más usados por bloque. Pero aún quedan ~16 PSX regs que siempre van por LW/SW a `cpu.regs[]`.

Full regalloc al estilo pcsx_rearmed: `regmap[HOST_REGS]` per-instruction con dirty bitmask, LRU eviction, mapping consistente en branch targets.

**Requiere reescribir** todos los emitters para pedir registros dinámicos en vez de asumir T0/T1/T2.

---

## Code Smells / Mejoras menores

| Issue | Descripción | Impacto |
|---|---|---|
| Prologue 29 words | 116 bytes overhead. Super-blocks mitigan (3 continuations = 1 prologue). | Bajo |
| Branch cond en stack | Trade-off por pin $gp. +1 SW/LW por conditional branch. | Aceptable |
| memset en buffer reset | `compile_block` hace memset de hasta 4MB. Innecesario. | Bajo |
| DCE ventana 64 insn | `uint64_t` bitmask. Expandible a 128 con `__uint128_t`. | Bajo |
| DMA linked-list 100K | Safety counter puede truncar display lists legítimas. | Medio |

---

## Prioridad recomendada

```
1. SMRV / inline cache          (2-4h — alto impacto, reduce hot path LW/SW)
2. FlushCache batching          (30 min — cambio trivial)
3. LQ/SQ bulk flush/reload      (1 día — medio impacto)
4. Dynamic regalloc full        (2-4 semanas — máximo impacto)
```

~~Dirty bitmask flush~~ — impacto <0.2% tras optimizaciones de trampoline.

**Nota:** El "multipass refactor" (Scan+Emit) listado antes como prerequisito YA ESTÁ HECHO — `block_scan()` es el pass 1 (backward liveness, DCE, reg demand, dirty mask) y el compile loop es el pass 2 (emit).

---

## Profile: Crash Bandicoot → Start Screen (53 reports, ~53s)

| Fase | Frames | Speed | Bottleneck principal | JIT% | GPU TexCache% |
|---|---|---|---|---|---|
| BIOS Boot | 0-240 | 45% | JIT (75-98%) | 95% | — |
| Game Init | 240-480 | 87% | JIT (93%) | 93% | — |
| Logo/Menú | 600-960 | **100%** | Balanced | 60% | <1% |
| Level Loading | 960-1080 | 38% | JIT (96%) | 96% | — |
| Cutscene/Menú | 1080-1440 | **100%** | Balanced | 58% | 2% |
| Gameplay temprano | 1680-2400 | 72% | JIT+GPU | 70% | 2% |
| **3D Gameplay** | **2640-3180** | **34-37%** | **JIT 50% + GPU TexCache 28-35%** | 50% | **28-35%** |

**Hotspot #1 siempre:** `PC=80034504` (kernel idle/scheduler) con 8-23M cycles.  
**JIT compilation:** <0.1% en steady-state (solo notable en report #1 con 4.2%).
