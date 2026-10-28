---
tags: [mapa, pieza]
archivos:
  - engine/src/decision.h
  - engine/src/decrec.h
  - engine/tests/test_decision.c
sello: 725004e4b57a55d1
---
# Bitácora de decisiones (decision.h)

**Grupo:** Observar — [[Mapa del motor]]

## Qué hace
El registro de la «relojería»: las últimas 64 decisiones (`CG_DEC_N`) de un sentido (la subida del cliente, la bajada de cada sesión del servidor), para el estado. Cada guarda o medida que cambia cómo se reparten los paquetes deja una entrada cuando cambia de idea. Solo transiciones, desde los ticks (unos diez por segundo), nunca por paquete. `decrec.h` traduce lo que dicen las funciones puras a entradas, y dice qué decide el reparto en el estado (`weights_used`: «eta» cuando algún enlace del reparto tiene tasa, desde el 2026-10-28; antes, solo si todos la tenían).

## Lee
- [[Salud y silenciado (health.h)]] (G1: silenciar, volver, forzado; con `mute_queue`, el porqué es el atraso que comparó).
- [[G3 enlace sin respuesta (split.h)]] (callado / responde).
- [[G4 guarda de dispersión (split.h)]] (sale por cola, atasco o base; vuelve; estrés; recuperación).
- [[Copias bajo estrés (split.h)]] (sin presupuesto).
- [[Capacidad (capacity.h)]] (corte por lleno o por entregas cortas, arranque rápido, caducidad con `capacity_expire_ms`: `cap_expire`).
- [[Gobernador (govern.h)]] (cruce de la mitad del tope).
- [[Rescate (rescue.h)]] (presupuesto bajo).

## Decide / escribe
- Nada: solo el estado (`status.h`) y `cengarde ctl`.

## Perillas
- Ninguna.

## Dónde se midió
- [[012-bonding]]: la auditoría del 2026-10-17 («relojería») la pidió como métrica 1; PR #74 ([[audit-pending]]).

## Choques conocidos
- Ninguno: no decide.

## Archivos
- `decision.h`, `decrec.h`; tests `engine/tests/test_decision.c`.
- `client.c` y `server.c`: `health_tick`, `spread_tick`, `gov_tick`, `cap_tick`.
