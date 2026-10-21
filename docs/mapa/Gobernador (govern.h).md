---
tags: [mapa, pieza]
archivos:
  - engine/src/govern.h
  - engine/tests/test_govern.c
sello: c21e47af17e603fd
---
# Gobernador (govern.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Fija el peso de cada enlace en el reparto según su propia cola. Cada tick, mientras el extremo reparte y hay tráfico: si la cola de un enlace pasa en más de `target_us` la más corta de los candidatos, su peso baja 1/8; si queda bajo la mitad del objetivo, sube 20 (2 % del tope de 1000); entre medio se mantiene. Nunca baja de 10 (`CG_GOV_MIN`), para que el enlace siga diciendo cuándo se vació. Los pesos solo cuentan como proporción: los enlaces se reparten la cola.

## Lee
- El `owd` filtrado de [[Salud y silenciado (health.h)]] y la base de [[Cola por enlace (cg_gov_base)]].
- Los candidatos del [[Reparto (split.h)]] (elegibles con peso > 0) y si el extremo reparte ahora.

## Decide / escribe
- `gw[]`, los pesos → [[Reparto (split.h)]] (`split_weights`), o escalando los de [[Capacidad (capacity.h)]] con `capacity_weights`.
- Cruces de la mitad del tope → [[Bitácora de decisiones (decision.h)]] (`CG_DEC_GOV_DOWN/UP`).
- Con [[ETA (eta.h)]] activo, sus pesos no deciden (solo se usan si un enlace no tiene tasa).

## Perillas
- [[Mapa/Perillas/governor|governor]]: no.
- [[Mapa/Perillas/governor_target_ms|governor_target_ms]]: 20; 1–1000.
- [[Mapa/Perillas/governor_window_ms|governor_window_ms]]: 10000; 1000–120000.
- [[Mapa/Perillas/weight|weight]]: un peso 0 sigue siendo copia de todo.

## Dónde se midió
- [[012-bonding]]: con pesos fijos, en el banco dos enlaces 5G tenían 120–150 ms de cola de subida y los otros 55.
- [[013-capacidad-y-dosificacion]]: sirve en la bajada (+22 % a +108 %), no en la subida.

## Choques conocidos
- C8: sube el peso de un enlace que G4 tiene fuera, y eso amplifica C3 ([[audit-pending]]).
- C9: oscila con período de 1–2 s (auditoría).
- C12: `governor_target_ms` movía la capacidad y el rescate también; ahora tienen `capacity_sat_ms` y `rescue_target_ms`.
- Una regla sobre la cola propia de cada enlace mandaba todos al piso a la vez (laboratorio, 2026-10-14, `govern.h`).

## Archivos
- `govern.h`; tests `engine/tests/test_govern.c`.
- `client.c` y `server.c`: `gov_tick` (después de `spread_tick`, antes de `cap_tick`).
