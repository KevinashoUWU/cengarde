---
tags: [mapa, pieza]
archivos:
  - engine/src/fwdtable.h
  - engine/tests/test_fwdtable.c
sello: 47e186218e788483
---
# Tabla de reenvío (fwdtable.h)

**Grupo:** Configurar — [[Mapa del motor]]

## Qué hace
Decide qué puertos públicos del VPS van a qué router: el IP pass (todo 1024–65000 a un solo router, el que lo pide en sus sondas y su sección lo permite) o reglas explícitas `forward`. El motor no toca el firewall: escribe la tabla en un archivo y `contrib/vps/cengarde-nat` la aplica.

## Lee
- Lo que pide el cliente en sus sondas (`CG_F_PASS_SET`, `CG_F_PASS`) y las reglas de cada `[client]`.

## Decide / escribe
- `forward_file` o `passthrough_file` (`fwd_sync`, `pass_sync`).

## Perillas
- [[Mapa/Perillas/passthrough|passthrough]], [[Mapa/Perillas/forward|forward]], [[Mapa/Perillas/forward_file|forward_file]], [[Mapa/Perillas/passthrough_file|passthrough_file]].

## Dónde se midió
- [[009-recarga-control-e-ip-pass]]: IP pass desde el router (v3).
- [[010-ipv6-varias-ip-multicliente-nombres]]: 3d2c, `forward add`.

## Choques conocidos
- Un puerto reenviado a dos clientes es error de configuración (`cg_fwd_overlap`).

## Archivos
- `fwdtable.h`; tests `engine/tests/test_fwdtable.c`. `server.c`: `fwd_sync`, `fwd_load`, `pass_sync`.
