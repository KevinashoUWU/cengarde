---
tags: [mapa, pieza]
archivos:
  - engine/src/split.h
  - engine/tests/test_split.c
sello: 66ec508d9b9710ab
---
# G4 guarda de dispersión (split.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Solo comparten el reparto los enlaces de retardo parecido: el receptor entrega en orden, así que un reparto va al paso de su enlace más lento. Un candidato más de `split_spread_ms` detrás del más rápido que sigue dentro **sale** al instante; vuelve cuando se mantiene dentro de 3/4 de esa distancia durante `mute_settle_ms`. Fuera, lleva copia de todo (`split_spread_copy`) o solo los duplicados. Clasifica cada salida (cola propia que creció, atasco, base) y cada vuelta. Su **estrés**: el más rápido es uno que salió y todos los de dentro van más de la distancia detrás de él.

## Lee
- El `owd` filtrado de [[Salud y silenciado (health.h)]] y la cola propia ([[Cola por enlace (cg_gov_base)]], `CG_SPLIT_MIN_MS` 10 s).
- Los candidatos de [[G3 enlace sin respuesta (split.h)]]; `dwell_ms` del [[Modelo de enlace (linkmodel.h)]].

## Decide / escribe
- La máscara de salida (`cg_split_spread_out`, `cg_split_spread_apply`) → [[Reparto (split.h)]].
- `stress` → [[Copias bajo estrés (split.h)]]; con `split_stress_single`, solo el más rápido (`cg_split_stress_elig`).
- Una salida por cola → [[Capacidad (capacity.h)]] con `cap_g4_episode` (C5).
- Salidas, vueltas y estrés → [[Bitácora de decisiones (decision.h)]].

## Perillas
- [[Mapa/Perillas/split_spread_ms|split_spread_ms]]: 30; 0–2000 (0: sin guarda).
- [[Mapa/Perillas/split_spread_copy|split_spread_copy]]: `yes` salvo con gobernador, capacidad o ETA.
- [[Mapa/Perillas/mute_settle_ms|mute_settle_ms]]: 2000. [[Mapa/Perillas/split_stress_rejoin|split_stress_rejoin]]: sí.
- [[Mapa/Perillas/split_stress_single|split_stress_single]]: no. [[Mapa/Perillas/split_dwell_ms|split_dwell_ms]], [[Mapa/Perillas/split_dwell_oq_ms|split_dwell_oq_ms]]: 0.

## Dónde se midió
- [[012-bonding]]: con 50 ms de diferencia Cubic ~1,15×, BBR 1,47×; con 150 ms se hunden todos, con el reordenador limpio. En la Pi (4 enlaces 5G) la guarda entró en espiral: copias sobre los enlaces atrasados, hasta 2,6 s de atraso y miles de envíos perdidos.
- `split.h`: copiar todo bajo estrés fue 7–23 % más rápido que repartir en el laboratorio, a 1,5–2,4× los bytes.

## Choques conocidos
- C1: tomaba «la referencia se vació» por «mi cola creció»; arreglado (PR #75, la cola propia).
- C3: ping-pong entre la vuelta inmediata y la salida por cola; `split_dwell_ms` quedó neutro ([[015-modelo-de-enlace]] §6).
- C4: sus 2 s de vuelta resuenan con el período de los saltos y con la cosquilla ([[audit-pending]]).
- C5 con [[Capacidad (capacity.h)]]; C8 con [[Gobernador (govern.h)]].

## Archivos
- `split.h` (`cg_split_spread_eval_r`, `struct cg_spread`); tests `engine/tests/test_split.c`.
- `client.c` y `server.c`: `spread_tick`; `split_masks`.
