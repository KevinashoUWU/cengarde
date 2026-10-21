---
tags: [mapa, pieza]
archivos:
  - engine/src/govern.h
  - engine/src/capacity.h
  - engine/src/split.h
sello: 7cbd50e75c6cdd46
---
# Cola por enlace (cg_gov_base)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
La «cola» de un enlace es su retardo de ida menos su propio mínimo de una ventana. Los relojes de los dos extremos no están en hora, pero el desfase es constante en una sesión, así que la diferencia es lo que espera en la cola del módem. El mínimo es de ventana: dos medias ventanas, la actual y la anterior. No es un archivo propio: es una medida que comparten varias piezas (inferido: la nota existe para mostrar ese sensor común).

## Lee
- El `owd` filtrado de [[Salud y silenciado (health.h)]].

## Decide / escribe
- [[Gobernador (govern.h)]]: la cola sobre la más corta de los candidatos (su exceso).
- [[Capacidad (capacity.h)]]: «lleno» cuando la cola pasa `sat_us`; además una cola sobre el mínimo de 60 s (`CG_CAP_LONG_MS`) y su tendencia, para el peso del [[Rescate (rescue.h)]].
- [[ETA (eta.h)]]: la base (el mínimo, relativo al menor) y la cola, que fija el backlog cada tick.
- [[G4 guarda de dispersión (split.h)]]: desde C1, la cola propia (`CG_SPLIT_MIN_MS`, 10 s) para clasificar por qué sale un enlace.
- [[Enlace dominante (dominant.h)]]: la base para saber qué enlaces son más lentos.

## Perillas
- [[Mapa/Perillas/governor_window_ms|governor_window_ms]]: 10000; 1000–120000. Ventana del mínimo para el gobernador y la capacidad.

## Dónde se midió
- [[012-bonding]]: el gobernador por cola en el banco y la Pi.
- [[015-modelo-de-enlace]] (sección 9): `bond.sh` arrancaba los motores antes que netem, el mínimo se aprendía con ~0 ms y duraba una ventana (10 s); era un artefacto del laboratorio.

## Choques conocidos
- Una cola que dura una ventana entera se vuelve base (lo dice `govern.h`); un enlace lleno más que `window_ms` mide contra un mínimo que ya tiene la cola (por eso la capacidad usa además el de 60 s).
- Starlink casi no hace cola (~15 ms) y descarta: la cola no dice que está lleno ([[Modelo de enlace (linkmodel.h)]], [[015-modelo-de-enlace]]).
- La misma cola cuenta tres veces en modo pesos: la puerta de sonda, el corte de capacidad y el ×7/8 del gobernador (auditoría C5, [[audit-pending]]).

## Archivos
- `govern.h` (`cg_gov_base`, `struct cg_gov_link`), `capacity.h` (`q`, `ql`), `split.h` (cola propia de G4).
- Tests: `engine/tests/test_govern.c`, `test_capacity.c`, `test_split.c`.
