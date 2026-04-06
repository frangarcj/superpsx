# Auditoría Estática de Rendimiento: Implementación GPU PSP

Esta auditoría analiza la implementación actual de la GPU para PSP en `superpsx`, enfocándose en identificar cuellos de botella y proponer optimizaciones para mejorar los cuadros por segundo (FPS).

> **Última revisión:** Post-audit cleanup. Originally post-commit 10829f8.
> **Status:** Audit mostly complete — 4/5 items done, #4 not viable on PPSSPP.

## 1. Gestión de Estado y Llamadas al Backend (GU)

### Hallazgos
- **Cambios de Estado Redundantes:** `apply_blend()` y `apply_dither()` se llaman incondicionalmente por cada primitiva, emitiendo `sceGuEnable/Disable` aunque el estado no cambie.
  - *Impacto:* Cada llamada a `sceGu` escribe en el Display List. Acumula overhead significativo.
- **`sceGuTexFlush()` en cada `setup_psx_texture`:** Fuerza invalidación de la caché de texturas del GE.
  - *Impacto:* En cache hit del tcache EDRAM, la textura no cambió en EDRAM — el flush es innecesario.
- **`sceGuEnable(GU_COLOR_TEST)` + `sceGuColorFunc()` per-primitive:** Se configura en cada polígono/rectángulo texturizado.
  - *Impacto:* 3 llamadas sceGu extra por cada primitiva texturizada.
- **`gs_state` existe pero NO se usa:** La estructura `gs_state_t` está declarada con campos para textura, blend, dither, etc., y `Prim_InvalidateGSState()` limpia `gs_state.valid`. Pero ningún path de renderizado consulta `gs_state.valid` antes de re-aplicar estado.
  - *Impacto:* Todo el overhead de estado se emite redundantemente.

### Recomendaciones
- [ ] **Lazy State Management:** Usar `gs_state` para trackear el estado actual de GU. Solo emitir `sceGu*` si el valor cambió. Campos: blend_mode, dither, texture_enabled, color_test, stencil.
- [x] ~~Batching de Primitivas~~ (futuro — requiere acumular vértices en un buffer compartido).

## 2. Procesamiento de Vértices y Primitivas

### Hallazgos
- **`sceGuGetMemory` + `sceGuDrawArray` por primitiva:** Cada polígono, línea y rectángulo hace su propia asignación de vértices y draw call.
  - *Impacto:* Overhead de llamada dominante. Batching reduciría drásticamente el número de draw calls.
- **`PSX_to_PSP` por vértice:** Ahora es solo 2 sumas (offset_x + vx, offset_y + vy) — inline y barato.
  - *Impacto:* Negligible comparado con overhead de sceGu.

### Recomendaciones
- [ ] **Vertex Buffer Batching:** Agrupar primitivas con mismo estado en un solo `sceGuDrawArray`. Requiere lazy state tracking primero.

## 3. Gestión de VRAM y Texturas

### Hallazgos
- **EDRAM Tex Cache (7 slots LRU):** T4/T8 usan un cache de 7 slots × 64KB en EDRAM. En cache miss, `memcpy` fila-por-fila de shadow RAM a EDRAM. En cache hit, GE lee directamente del slot EDRAM (rápido).
  - *Impacto:* Gran mejora vs. diseño anterior (decode a main RAM). Hits son gratuitos; misses cuestan ~32-64KB de memcpy.
- **15bpp directo desde EDRAM:** Sin CPU, GE lee de `PSP_VRAM_OFFSET + tpy*1024 + tpx` con TBW=1024.
  - *Impacto:* Zero-copy, óptimo.
- **T4/T8 NO pueden ser directos:** El GE requiere CLUT lookup (sceGuClutLoad). Los datos deben ser contiguos con stride ≤ 1024 (TBW limit). El cache EDRAM resuelve esto.
- **CLUT cacheado por `cached_clut_word`:** Solo re-lee CLUT de shadow cuando el clut_word cambia.
  - *Impacto:* Evita re-lectura de CLUT en texturas que comparten paleta.
- **Region-based cache invalidation:** `Prim_InvalidateTexCache_Region()` solo invalida slots cuya página se superpone al área escrita. O(7) por llamada.
  - *Impacto:* Mucho mejor que invalidar todo. El `cached_clut_word` se invalida conservativamente.

### Recomendaciones
- [ ] **DMA para cache fills:** Usar `sceGuCopyImage` o DMA para copiar shadow→EDRAM en cache miss (en vez de memcpy fila-por-fila). Baja prioridad — cache hits dominan.
- [x] ~~T4/T8 directo desde EDRAM~~ **NO FACTIBLE**: CLUT requiere datos contiguos + stride ≤ 1024.

## 4. Sincronización y Blitting

### Hallazgos
- **`sceGuFinish` + `sceGuSync` en UpdateDisplay y VRAMReadback:** Detiene CPU hasta que GE termine.
  - *Impacto:* Rompe paralelismo CPU/GPU. Necesario para readback, pero UpdateDisplay podría optimizarse.
- **Display blit en tiras de 512px:** Recalcula `tex_pw`/`tex_ph` (power-of-2) cada frame.
  - *Impacto:* Menor — pero podría pre-calcularse.

### Recomendaciones
- [ ] Reducir `sceGuSync` — solo sincronizar en readback.
- [ ] Pre-calcular parámetros de blit si resolución no cambia entre frames.

## Resumen de Prioridades (Actualizado)

| # | Optimización | Impacto | Esfuerzo | Estado |
|---|-------------|---------|----------|--------|
| 1 | **Lazy State Tracking** (blend/dither/texture/color_test) | ALTO | Medio | **Hecho** (f98e80f) |
| 2 | **Eliminar sceGuTexFlush en cache hit** + cache tex func/filter | MEDIO | Bajo | **Hecho** (553e69c) |
| 3 | **Vertex Buffer Batching** | ALTO | Alto | **Hecho** (75e70f1) |
| 4 | **DMA para cache fills** | BAJO | Medio | No viable en PPSSPP |
| 5 | **Pre-calcular display blit params** + integer scaling | MÍNIMO | Bajo | **Hecho** (9d1b352) |
