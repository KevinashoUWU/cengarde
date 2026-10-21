---
tags: [mapa, pieza]
archivos:
  - engine/src/arrival.h
  - engine/tests/test_arrival.c
sello: 728b7b2be4b94adb
---
# Llegadas (arrival.h)

**Grupo:** Observar — [[Mapa del motor]]

## Qué hace
Estadísticas de recepción por enlace en un flujo redundante: qué enlace trajo primero cada paquete (`wins`), cuánto atrás llegaron los otros (`lag_us`) y qué enlaces nunca trajeron copia. También la **tardanza conjunta**: para plazos de 50, 100 y 150 ms tras la primera copia, un histograma por máscara de enlaces tardíos, lo que haría falta para decidir un k-de-N. Las copias duplicadas se anotan sin verificar el MAC: **solo para mostrar**.

## Lee
- Las primeras copias y los duplicados de [[Camino del router (clientpath.h)]] (cliente) y `listen_read` (servidor), con la hora de cada lote.

## Decide / escribe
- `wins`, `lag_us` en las sondas ([[Sondas y cola de bonding (proto.h)]]) y el estado; los histogramas para `bench/fieldrec.py`.
- Excepción: con `reorder_idle_ms`, `cg_arr_idle` recorta el `rset` del [[Reordenador (reorder.h)]].

## Perillas
- [[Mapa/Perillas/reorder_idle_ms|reorder_idle_ms]]: 0 (el único uso como entrada de decisión).

## Dónde se midió
- [[012-bonding]]: instrumentos pasivos del paso 0 (`joint`, `lost_all`, `sizes`) y el registrador de campo.
- [[011-hilos]]: `skew`, 33 % de primeras llegadas por enlace.

## Choques conocidos
- El comentario de cabecera dice que ninguna decisión lo lee, pero `reorder_idle_ms` (experimento de C2) sí lo usa (ver [[Mapa del motor#Contradicciones]]).
- Con bombas por enlace, el hub puede tomar una copia tardía antes que una temprana ([[Hilos por enlace (thrplan.h)]]).

## Archivos
- `arrival.h`; tests `engine/tests/test_arrival.c`.
- `clientpath.h` (`cg_rx_entry` anota primeras copias y duplicados); `client.c`: `ro_rset`. `server.c`: `listen_read`, `ro_rset`.
