---
tags: [mapa, pieza]
archivos:
  - engine/src/pace.h
  - engine/tests/test_pace.c
sello: 126fa57a370ab81e
---
# Dosificación (pace.h)

**Grupo:** Enviar — [[Mapa del motor]]

## Qué hace
Fija la tasa a la que el kernel dosifica el socket de cada enlace (`SO_MAX_PACING_RATE`, con el qdisc `fq` a la salida). El motor manda un lote con un `sendmmsg` a la velocidad de la red local; esa ráfaga espera en el módem y hace saltar su retardo. La tasa es el doble de la capacidad y nunca menos de 20 Mbit/s (`CG_PACE_FLOOR`); sin capacidad, sin límite. El motor no guarda cola: la guarda el kernel. El servidor dosifica sus carriles (`lanes`), cada uno a la suma de sus caminos.

## Lee
- La capacidad de [[Capacidad (capacity.h)]].
- En el servidor, qué caminos van por cada carril de [[Colas del servidor (steer.h)]].

## Decide / escribe
- La tasa de cada socket (`pace_set`, `lane_pace_set`). Ninguna pieza la lee: actúa en el kernel.

## Perillas
- [[Mapa/Perillas/pacing|pacing]]: no.
- [[Mapa/Perillas/lanes|lanes]] (servidor: dosifica solo con más de un carril).

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: en la red de pruebas los retardos sin cola diferían 2–6 ms y el reordenador aun retenía el 80 % de la bajada. Una regla a 5/4 de la capacidad se hundió (la subida a 0–2 Mbit/s): tras un reinicio la capacidad es casi nada y las sondas, dosificadas en el mismo socket, la mantenían ahí.

## Choques conocidos
- Las sondas salen por el mismo socket: dosificadas, toman la dosificación por cola del enlace (lo dice `pace.h`). El doble de la capacidad lo evita.
- Sin `fq` en la interfaz, no dosifica nada ([[engine-README]]).

## Archivos
- `pace.h`; tests `engine/tests/test_pace.c`.
- `client.c`: `cap_tick` → `pace_set`. `server.c`: `pace_lanes`, `lane_pace_set`.
