# JIT Optimization Roadmap — superpsx vs pcsx_rearmed

## Estado actual (lo que ya tenemos)

| Optimización | Estado | Commit/Notas |
|---|---|---|
| Register Pinning | ✅ 13 regs | v0,v1,a0-a3,t0-t2,gp,sp,s8,ra → EE regs |
| Constant Propagation | ✅ | `vregs[32]` con lazy dirty tracking |
| Dead Code Elimination | ✅ | Backward liveness, ventana 64 insn (dce_prescan) |
| Direct Block Linking | ✅ | J-based DBL con back-patching |
| Inline Hash Dispatch | ✅ | JR/JALR, 2-way set-associative jit_ht[4096] |
| Cold Slow Paths | ✅ | Deferred slow-path al final del bloque |
| Const-Address Fast Paths | ✅ | RAM, scratchpad, I_STAT, I_MASK, GPU_ReadStatus inlined |
| GTE Inline + VU0 | ✅ | 22 comandos GTE con wrappers dedicados + VU0 macro para RTPS/RTPT/MVMVA/light |
| SMC Detection | ✅ | Page-generation + djb2 hash + jit_smc_handler |
| Idle Loop Detection | ✅ | Side-effect scan, be->is_idle flag |
| Scratch Register Cache | ✅ | T0/T1 cache, evita re-cargar regs no pinned recientes |
| Per-Instruction Cycles | ✅ Parcial | Cold/TLB abort paths usan offset correcto. Branch epilogue usa block total (correcto porque cada bloque acaba en branch) |
| SPU Batch ADSR | ✅ | Skip per-sample tick cuando volumen estable |
| Inline SMC Check | ✅ | Skip handler call para páginas sin bloques |

---

## Optimizaciones pendientes (ordenadas por impacto/esfuerzo)

### 1. Dirty Bitmask para flush_pinned
**Impacto:** Alto (5-10%)  
**Esfuerzo:** 1-2 horas  
**Descripción:** `emit_flush_pinned()` escribe 13 SW **siempre**, incluso si un registro solo se leyó dentro del bloque. Con un bitmask de 32 bits (`pinned_dirty_mask`) que se marca cuando un pinned reg se modifica (escritura a PSX reg), `emit_flush_pinned` puede emitir solo los SW de los regs realmente modificados. En un bloque típico, solo 3-6 de los 13 regs se modifican → ahorra 7-10 instrucciones por block exit y por C call.

**Implementación:**
- Variable global `uint32_t pinned_dirty_mask = 0`
- En `emit_store_psx_reg(r)` → `pinned_dirty_mask |= (1 << r)` si `psx_pinned_reg[r]`
- En `emit_flush_pinned()` → solo emitir SW para regs con bit set
- Reset mask al inicio de cada bloque y después de `emit_reload_pinned()`

---

### 2. SMRV (Speculative Memory Region Validation)
**Impacto:** Alto (5-8%)  
**Esfuerzo:** 2-4 horas  
**Descripción:** El hot path de LW/SW actualmente hace ~8 instrucciones: mask AND, range check (SRL+SLTIU), branch, ADDU+LW. Con predicción de que el base register apunta a RAM (>95% de accesos), se puede emitir un fast path optimista sin range check y un slow path de corrección si falla.

pcsx_rearmed hace esto con `smrv_regs[]` — trackea qué registros tienen alta probabilidad de apuntar a RAM basado en patrones de uso (LUI seguido de ADDIU, accesos repetidos al mismo base).

**Alternativa más simple:** En el cold path (después del range check), backpatchear el branch para saltar directamente al fast path la próxima vez. "Inline cache" por sitio de acceso.

---

### 3. FlushCache Batching
**Impacto:** Bajo-Medio (1-3%)  
**Esfuerzo:** 30 min  
**Descripción:** Actualmente hacemos `FlushCache(0); FlushCache(2);` después de cada bloque compilado. En PS2, cada FlushCache es un syscall (~50 ciclos). Con batching, se puede acumular un contador y hacer flush cada N bloques, o una vez al salir del compile_block cuando se detectan múltiples compilaciones seguidas.

**Riesgo:** Hay que asegurar que el flush ocurra antes de cualquier salto a código recién compilado. El `FlushCache` al final de `compile_block` ya está en la ruta correcta, pero si se quita hay que mover a justo antes de ejecutar.

---

### 4. Multi-Entry Blocks / Fall-Through Compilation
**Impacto:** Medio-Alto (5-10%)  
**Esfuerzo:** 1-2 días  
**Descripción:** Actualmente cada bloque termina en el primer branch. Eso significa que secuencias if/else que caen al through se parten en bloques separados, cada uno con su prólogo de 29 instrucciones. pcsx_rearmed compila más allá de branches internos, creando múltiples entry points por bloque.

**Beneficio:** Elimina el overhead de prologue para fall-through paths. Los DBL ya saltan pasado el prologue, pero para el primer dispatch o hash misses, el prologue es brutal (116 bytes).

**Complicación:** Requiere tracking de estado de registros en cada entry point. Si un branch target dentro del bloque asume ciertos regs loaded, hay que garantizarlo.

**NOTA:** Si se implementa, habría que cambiar `emit_branch_epilogue` para usar `emit_cycle_offset` en vez de `block_cycle_count`, porque entonces los branches condicionales internos ya no estarían al final del bloque.

---

### 5. Dynamic Register Allocation
**Impacto:** Crítico (15-25%)  
**Esfuerzo:** 2-4 semanas  
**Descripción:** La mayor ventaja de pcsx_rearmed. Mantiene `regmap[HOST_REGS]` por instrucción con `dirty` bitmask, `lsn()` eviction, y mapping consistente en branch targets. Superpsx tiene pinning estático — los 19 PSX regs no pinned ($s0-$s7, $t3-$t7, $at, $k0-$k1, etc.) **siempre** van por LW/SW a `cpu.regs[]`.

En Crash Bandicoot, $s0-$s7 son los regs más usados en inner loops. Cada acceso = 1 LW + posible cache miss.

**Requiere:** Reescribir el compiler a 2-pass (primer pass = análisis de uso, segundo = emisión). Todos los emitters necesitan cambiar para pedir un registro dinámico en vez de asumir T0/T1/T2.

**Riesgo:** Altísimo. Regresiones en todo. Semanas de depuración.

---

### 6. LQ/SQ Bulk Flush/Reload (PS2-específico)
**Impacto:** Medio (3-5%)  
**Esfuerzo:** 1 día  
**Descripción:** EE tiene LQ/SQ (128-bit load/store). Con 4 regs flush/reload por instrucción, `emit_flush_pinned` pasa de 13 SW a 4 SQ (más cleanup). `emit_reload_pinned` igual.

**Requisito:** `cpu.regs[]` debe estar 16-byte aligned y los regs pinned deben mapearse a posiciones consecutivas en el struct. Actualmente los pinned regs son: 2,3,4,5,6,7,8,9,10,28,29,30,31 — casi consecutivos excepto el gap 11-27. Se podría hacer SQ para regs[0..3], regs[4..7], regs[8..11], regs[28..31].

---

## Code Smells / Mejoras menores

| Issue | Descripción |
|---|---|
| Prologue 29 words | 116 bytes de overhead. Con multi-entry blocks o lazy prologue se reduciría. |
| Branch cond en stack | Trade-off por pin $gp. Cuesta 1 SW + 1 LW extra por conditional branch. Aceptable. |
| memset en buffer reset | `compile_block` hace memset de hasta 4MB al resetear code_buffer. Innecesario — se sobreescribe igualmente. |
| DCE limitada a 64 insn | `DCE_MAX_SCAN = 64`, usa `uint64_t` como bitmask. Bloques GTE grandes pueden superar esto. Expandible a 128 con `__uint128_t` o bitfield array. |
| FlushCache 2× syscall | Ya mencionado arriba. |
| DMA linked-list 100K | Safety counter trunca display lists legítimas silenciosamente. |

---

## Prioridad recomendada

```
0. Multipass refactor (Scan+Emit) — PREREQUISITO (1-2 días)
   Partir compile_block() en:
   - Pass 1 (Scan): liveness backward, reg demand, branch targets, DCE
   - Pass 2 (Emit): emitir con toda la info del scan
   Overhead: ~1.5µs extra por bloque. DCE existente ya es el 80% del scan.
   Desbloquea: dirty mask gratis, SMRV, multi-entry, eventualmente dynamic regalloc.

1. Dirty bitmask flush      (1-2h, alto impacto, bajo riesgo)
2. FlushCache batching       (30min, bajo-medio impacto, bajo riesgo)
3. SMRV / inline cache       (2-4h, alto impacto, medio riesgo)
4. LQ/SQ bulk flush          (1 día, medio impacto, bajo riesgo)
5. Multi-entry blocks         (1-2 días, medio-alto, medio riesgo)
6. Dynamic regalloc          (2-4 semanas, crítico, alto riesgo)
```

Total estimado para 0-4: ~4-5 días de trabajo.
Total estimado para 0-5: ~6-7 días.
Total estimado para todo: 4-6 semanas.
