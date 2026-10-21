---
tags: [mapa, pieza]
archivos:
  - engine/src/wgsize.h
  - engine/tests/test_wgsize.c
sello: 88f9f6e9a4218452
---
# Tamaños (wgsize.h)

**Grupo:** Observar — [[Mapa del motor]]

## Qué hace
Cuenta lo que lleva el túnel en un sentido: datagramas de WireGuard por tipo de mensaje (handshake, respuesta, cookie, datos) y, los de datos, por tamaño en cubetas (32 un keepalive, 96 un ACK de TCP vacío, y así hasta 1440 y más). Solo medida: nada decide con ella.

## Lee
- El primer byte (tipo) y el largo de cada datagrama.

## Decide / escribe
- Contadores para el estado y el registrador de campo (`bench/fieldrec.py`). Ninguna pieza los lee.

## Perillas
- Ninguna.

## Dónde se midió
- [[012-bonding]]: cuántas copias valen los ACK y los handshakes (k-de-N).

## Choques conocidos
- Ninguno.

## Archivos
- `wgsize.h`; tests `engine/tests/test_wgsize.c`. `client.c`: `wg_read` (subida), `wg_msg` (bajada). `server.c`: `listen_read`, `down_prepare`.
