---
tags: [mapa, pieza]
archivos:
  - engine/src/eta.h
  - engine/src/capacity.h
  - engine/tests/test_eta.c
sello: fc7c8b2ef17d5655
---
# ETA (eta.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
El reparto por **llegada esperada**: cada paquete va por el enlace donde llegaría antes, `eta_i = base_i + (backlog_i + len) / rate_i`. `base_i` es el retardo sin cola (relativo al menor), `rate_i` la capacidad y `backlog_i` una cola virtual que se vacía a `rate_i` entre paquetes y se vuelve a fijar cada tick con la cola medida. Así, paquetes enviados en orden llegan casi en orden, y un enlace lento solo recibe cuando la cola de los rápidos haría llegar el paquete más tarde. Si un enlace no tiene tasa, reparte por pesos.

## Lee
- `cg_cap_eta` (cada tick, desde [[Capacidad (capacity.h)]]): tasa, base y cola de cada enlace ([[Cola por enlace (cg_gov_base)]]). Un enlace con cola corta se toma a 5/4 de su capacidad (y 1/16 del mayor); uno ocioso 3 s cuenta el doble cada 3 s hasta `split_stale_max`.
- Del [[Modelo de enlace (linkmodel.h)]]: `lossy_hold` (retenido en la cola más alta que mostró en 10 s), `hold_over_ms` (costo de pasarse: `over_us`), `qpred_*`, `eta_keep*`.
- Lo que otros ponen en el enlace (`cg_eta_add`, p. ej. sondas).

## Decide / escribe
- El enlace de cada paquete repartido → [[Reparto (split.h)]] (`cg_split_pick_eta`).
- Con `pace_any`, una segunda instancia sirve a [[Copias a tiempo (eta.h)]].

## Perillas
- [[Mapa/Perillas/split_eta|split_eta]]: no. [[Mapa/Perillas/split_stale_max|split_stale_max]]: 1; 1–64.
- [[Mapa/Perillas/capacity_sat_ms|capacity_sat_ms]] (la mitad es el umbral de sondeo).
- [[Mapa/Perillas/eta_buffer|eta_buffer]], [[Mapa/Perillas/split_qpred_ms|split_qpred_ms]], [[Mapa/Perillas/split_qpred_mode|split_qpred_mode]], [[Mapa/Perillas/eta_keep|eta_keep]], [[Mapa/Perillas/eta_keep_ms|eta_keep_ms]] (experimentos, 0).

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: por pesos, el 72 % de las retransmisiones de TCP de la bajada eran espurias (DSACK); de ahí el ETA.
- [[015-modelo-de-enlace]]: el ETA nunca desbordaba a los celulares, con 25–30 ms más de base que Starlink; lo resolvió el preset `starlink`. C9 (`eta_keep = 2`) en Starlink: −16 a −48 %.

## Choques conocidos
- Con `capacity_expire_ms`, cada mitad de la caducidad cuenta como medida (`loaded_ms`): `cg_cap_stale` no la vuelve a agrandar durante `CG_CAP_STALE_MS` ([[Capacidad (capacity.h)]], [[016-celda-virtual]]).
- C9: reinicia su cola cada tick con una cola de hace 300–500 ms, sin lo que está en vuelo; oscila con período de 0,6–1 s ([[audit-pending]]). `eta_keep` quedó apagado.
- C2: con el ETA, los enlaces ociosos no avanzan y el RACK del [[Reordenador (reorder.h)]] los espera.
- Sin carga un enlace no se mide: [[Cosquilla (tickle.h)]].
- Con ETA, los pesos del [[Gobernador (govern.h)]] y de la capacidad no deciden, pero salen en el estado (auditoría, métrica 10).

## Archivos
- `eta.h`, `capacity.h` (`cg_cap_eta`, `cg_cap_qpred`, `cg_cap_stale`); tests `engine/tests/test_eta.c`.
- `client.c`: `cap_tick`, `wg_read`. `server.c`: `cap_tick`, `down_prepare`.
