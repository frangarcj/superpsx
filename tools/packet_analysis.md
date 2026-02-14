# Análisis de GIF Packets

## LIBDRAW DRAW_RECT_FILLED (3 qwords, FUNCIONA ✅)
```
QW[00]: 0000000000005510 4400000000000001  <- GIF Tag
QW[01]: 3F800000800000FF 0000000000000006  <- PRIM=6, RGBAQ
QW[02]: 0000000092C992C9 0000000086398639  <- XYZ2, XYZ2
```

## MANUAL SPRITE (3 qwords actualmenteen memoria, NO FUNCIONA ❌)
```
QW[00]: 000000000005120E 4003400000008001  <- GIF Tag
QW[01]: 0000000086308630 00000000800000FF  <- RGBAQ, ???
QW[02]: 0000141900000000 0000000092C092C0  <- XYZ2, ???
```

## DIFERENCIAS CRÍTICAS ENCONTRADAS:

### 1. **REGS (Orden de registros)**
- **LIBDRAW**: `REGS = 0x5510` = [PRIM(0), RGBAQ(1), XYZ2(5), XYZ2(5)]
- **MANUAL**: `REGS = 0x5120E` = [A+D(E), PRIM(2), RGBAQ(1), XYZ2(5)]
  
  ❌ **MANUAL está COMPLETAMENTE MAL** - está usando A+D mode que es incorrecto

### 2. **PRE flag**
- **LIBDRAW**: Parece usar `PRE=0` y envía PRIM como PRIMER dato (valor 0x06)
- **MANUAL**: Usa `PRE=1` y PRIM en el tag

### 3. **NREG (Número de registros)**
- **LIBDRAW**: `NREG=4` (PRIM + RGBAQ + XYZ + XYZ)
- **MANUAL**: `NREG=4` pero con códigos incorrectos

### 4. **Datos**
- **LIBDRAW QW[01]**: Primer dato es `0x06` (PRIM_SPRITE), luego RGBAQ
- **MANUAL QW[01]**: Primer dato es RGBAQ, sin PRIM explícito

## CONCLUSIÓN:
El código manual tiene **MUL configuración incorrecta del GIFTAG**.
- Está usando REGS=0x5120E que incluye código 0xE (A+D mode)
- Debería usar REGS=0x5510 como libdraw
- Debe enviar PRIM=6 como primer dato, NO en el tag con PRE=1
