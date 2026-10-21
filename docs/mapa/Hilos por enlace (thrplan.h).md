---
tags: [mapa, pieza]
archivos:
  - engine/src/thrplan.h
  - engine/src/pump.h
  - engine/src/pump.c
  - engine/src/ring.h
  - engine/tests/test_thrplan.c
  - engine/tests/test_ring.c
  - engine/tests/test_threads.c
sello: e48b032363c05d2f
---
# Hilos por enlace (thrplan.h)

**Grupo:** Recibir · Configurar — [[Mapa del motor]]

## Qué hace
Decide cómo lee el router sus enlaces. `legacy` (por omisión) es el bucle de 0.4. `on` da a cada enlace una **bomba** (pump, hasta `CG_MAX_PUMPS`) que lee su socket a un anillo SPSC (`ring.h`); el **hub**, el único hilo con la sesión (secuencia, MAC, ventana, salud, sondas), toma los anillos por turnos. `off` es esa estructura en un hilo. `auto` es `legacy` en esta versión. También pone guardas: tiempo real o espera activa en más hilos que CPU, dos bombas en una CPU.

## Lee
- La clase de máquina (arquitectura, CPU de la afinidad), las perillas de CPU.

## Decide / escribe
- El modo, qué bomba toma cada enlace, avisos de perillas que dejarían sin CPU a la máquina → `client.c` (`pumps_init`, `guards_check`, `link_pump`). Las decisiones por paquete siguen en [[Camino del router (clientpath.h)]].

## Perillas
- [[Mapa/Perillas/link_threads|link_threads]]: `legacy`. [[Mapa/Perillas/io_queue|io_queue]]: 256; 64–1024.
- [[Mapa/Perillas/cpu|cpu]] (global y por `[link]`), [[Mapa/Perillas/busy_poll_us|busy_poll_us]]: 0, [[Mapa/Perillas/rt_priority|rt_priority]]: 0. Todas reinician el proceso.

## Dónde se midió
- [[011-hilos]]: con `on`, un hilo de enlace sin CPU llena solo su socket (`mtstall`: el túnel no pierde nada). A poco tráfico cada lote despierta un hilo (≈ 12 µs por despertar en la VM): la puerta C1 de CPU no pasa, y `on` sigue a mano.
- [[006-salud-de-enlaces]]: `busy_poll_us = 200` baja la mediana de 107–142 µs a 57–74 µs con ~75 % de CPU.

## Choques conocidos
- Un atasco del hub sigue frenando todos los enlaces a la vez (inherente a un solo dueño de la ventana).
- [[Llegadas (arrival.h)]]: el hub toma los lotes por turnos, así que las horas de llegada pueden salir desordenadas.

## Archivos
- `thrplan.h`, `pump.h`, `pump.c`, `ring.h`; tests `engine/tests/test_thrplan.c`, `test_ring.c`, `test_threads.c`.
- `client.c`: `pumps_init`, `guards_check`, `link_pump`, `hub_drain`, `threads_second`.
