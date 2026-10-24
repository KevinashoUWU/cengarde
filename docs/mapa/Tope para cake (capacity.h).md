---
tags: [mapa, pieza]
archivos:
  - engine/src/capacity.h
  - engine/tests/test_capacity.c
sello: 5daba1864deeebb2
---
# Tope para cake (capacity.h)

**Grupo:** Medir · Observar — [[Mapa del motor]]

## Qué hace
`cg_cap_total_tick` calcula la tasa que debería tener un `cake` delante del túnel, como cake-autorate pero con las medidas del motor. No da nada hasta que los enlaces con cola sobre `sat_us` llevan la mayor parte de lo entregado y eso es al menos 2 Mbit/s (`CG_CAP_SHAPE_MIN`). Entonces fija 9/10 de lo entregado, para que la cola se forme en `cake` y no en los módems; sube 1/32 por tick mientras entreguen al menos 4/5 sin cola, y se mantiene en otro caso. El motor solo dice la tasa; un servicio de fuera aplica `cake`.

## Lee
- La capacidad y la cola de cada enlace de [[Capacidad (capacity.h)]].

## Decide / escribe
- `shape_mbit` en el estado (`cg_cap_shape_bps`), para el servicio que aplica `cake`. Ninguna otra pieza del motor lo lee (comprobado con grep en `client.c` y `server.c`).

## Perillas
- [[Mapa/Perillas/capacity_sat_ms|capacity_sat_ms]] (la cola que dice «lleno»; por omisión `governor_target_ms`).

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: un `cake` con tope fijo costaba 35–40 % del caudal de bajada; la cola queda en `cake`, no en el motor ni en el módem.
- [[012-bonding]]: el gobernador de bajada y `cake` en el banco y en la Pi.

## Choques conocidos
- Con `capacity_expire_ms`, la entrega suavizada (`rate_bps`) de un enlace caducado baja con su capacidad: un silenciado vivo cuenta menos en lo entregado ([[016-celda-virtual]]).
- C10: el tope suma copias, rescates y paridad, que no son tráfico útil ([[audit-pending]], tarea #51).

## Archivos
- `capacity.h` (`cg_cap_total_tick`, `struct cg_cap_total`).
- `client.c` y `server.c`: `cap_tick`.
