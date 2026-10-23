---
tags: [mapa, pieza]
archivos:
  - engine/src/linkmodel.h
  - engine/tests/test_linkmodel.c
sello: 607b3150c0b3c126
---
# Modelo de enlace (linkmodel.h)

**Grupo:** Configurar · Decidir — [[Mapa del motor]]

## Qué hace
Dice qué clase de enlace es cada uno (`type` en su `[link]`) y le da un **preset**: los parámetros que leen los lazos de capacidad y de reparto para ese enlace. `auto` es exactamente el motor de antes. Hoy solo `starlink` cambia algo; `cellular`, `fiber`, `ethernet` y `radio` son ganchos con los ceros de `auto`. Las perillas globales de la auditoría se ponen encima del preset, en todos los enlaces o solo en los tipos de `exp_link_types` (`cg_link_model_k`).

## Lee
- `type` de cada `[link]` (cliente). El servidor lo recibe por camino en la cola de bonding (`CG_BOND_TYPE`) de [[Sondas y cola de bonding (proto.h)]].
- Las perillas globales (`cg_config_link_knobs`).

## Decide / escribe
Campos de `struct cg_link_model`, por enlace:
- `lossy_noprobe`, `lossy_hold`, `loss_pct`, `g4_episode`, `rfull_hyst`, `slot_clock` → [[Capacidad (capacity.h)]] y [[Ranuras de Starlink (slot.h)]].
- `hold_over_ms`, `qpred_*`, `eta_keep*` → [[ETA (eta.h)]].
- `dwell_ms`, `dwell_oq_ms` → [[G4 guarda de dispersión (split.h)]].
- `pace_ms`, `pace_peers_ms` → [[Copias a tiempo (eta.h)]] (`cg_link_pace_windows`).
- `dominant_pct`, `dominant_hold` → [[Enlace dominante (dominant.h)]].

Preset `starlink`: `lossy_noprobe`, `lossy_hold`, `hold_over_ms = 40`, `pace_peers_ms = 60`, `dominant_pct = 250`, `slot_clock = 1`.

## Perillas
- [[Mapa/Perillas/type|type]] (`[link]`): `auto`.
- [[Mapa/Perillas/exp_link_types|exp_link_types]]: vacía (todos).
- Encima del preset: [[Mapa/Perillas/eta_buffer|eta_buffer]], [[Mapa/Perillas/capacity_loss_pct|capacity_loss_pct]], [[Mapa/Perillas/cap_g4_episode|cap_g4_episode]], [[Mapa/Perillas/rescue_full_hyst|rescue_full_hyst]], [[Mapa/Perillas/split_qpred_ms|split_qpred_ms]], [[Mapa/Perillas/split_qpred_mode|split_qpred_mode]], [[Mapa/Perillas/split_dwell_ms|split_dwell_ms]], [[Mapa/Perillas/split_dwell_oq_ms|split_dwell_oq_ms]], [[Mapa/Perillas/eta_keep|eta_keep]], [[Mapa/Perillas/eta_keep_ms|eta_keep_ms]], [[Mapa/Perillas/redund_pace_ms|redund_pace_ms]], [[Mapa/Perillas/pace_peers_ms|pace_peers_ms]] (`pace_peers` = ms + 1, solo donde el preset tiene uno), [[Mapa/Perillas/split_dominant_pct|split_dominant_pct]], [[Mapa/Perillas/split_dominant_hold|split_dominant_hold]], [[Mapa/Perillas/starlink_slot_clock|starlink_slot_clock]].

## Dónde se midió
- [[015-modelo-de-enlace]]: el preset `starlink` dio +27 a +45 % en los casos de campo del laboratorio (`shallowq` bajada 41,6 → 56,8 Mbit/s), sin costo con retardos iguales; cuesta latencia (ping de 1200 B de ~50 a ~100 ms).
- §6: ninguna corrección mixta de la auditoría mejora lo celular; `cellular` sigue en ceros.
- `hold_over_ms`: 100 y 200 perdían 6–10 % y 20–38 % con retardos iguales; 40 quedó parejo o mejor.

## Choques conocidos
- `eta_buffer` global (último recurso) manda lo que sobra a las colas de 300 ms de los celulares; la subida pierde 17–18 % ([[015-modelo-de-enlace]]).
- Antes del modelo, todos los lazos trataban todo enlace como celular: esperaban una cola para ver «lleno» ([[link-model-priority]]).

## Archivos
- `linkmodel.h`; tests `engine/tests/test_linkmodel.c`.
- `client.c`: `link_models` (en `health_tick`). `server.c`: `path_models`.
