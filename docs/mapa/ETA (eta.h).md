---
tags: [mapa, pieza]
archivos:
  - engine/src/eta.h
  - engine/src/capacity.h
  - engine/tests/test_eta.c
sello: 860f876a278b8c11
---
# ETA (eta.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
El reparto por **llegada esperada**: cada paquete va por el enlace donde llegaría antes, `eta_i = base_i + (backlog_i + len) / rate_i`. `base_i` es el retardo sin cola (relativo al menor), `rate_i` la capacidad y `backlog_i` una cola virtual que se vacía a `rate_i` entre paquetes y se vuelve a fijar cada tick con la cola medida. Así, paquetes enviados en orden llegan casi en orden, y un enlace lento solo recibe cuando la cola de los rápidos haría llegar el paquete más tarde. Un enlace sin tasa (ninguna todavía, o su capacidad se hundió) no recibe paquetes mientras otro tenga; solo si ninguno tiene se reparte por pesos (2026-10-28, [[029-mpgrit]]: antes un enlace sin tasa mandaba todo el reparto a los pesos, y en la testnet Claro, que perdía 28–78 %, seguía llevando un cuarto del tráfico).

## Lee
- `cg_cap_eta` (cada tick, desde [[Capacidad (capacity.h)]]): tasa, base y cola de cada enlace ([[Cola por enlace (cg_gov_base)]]). Un enlace con cola corta se toma a 5/4 de su capacidad (y 1/16 del mayor); uno ocioso 3 s cuenta el doble cada 3 s hasta `split_stale_max`.
- Del [[Modelo de enlace (linkmodel.h)]]: `lossy_hold` (retenido en la cola más alta que mostró en 10 s), `hold_over_ms` (costo de pasarse: `over_us`), `qpred_*`, `eta_keep*`.
- Lo que otros ponen en el enlace (`cg_eta_add`, p. ej. sondas).
- Con `link_cc` (experimento, [[029-mpgrit]] paso 2), el [[Ritmo por enlace (linkcc.h)]] reemplaza, para los enlaces que ya tienen `SFACK`, lo que puso `cg_cap_eta` (`cg_eta_own`, en cada tick y en cada `SFACK`): su ritmo, su retardo sin cola (`rtprop` sobre el menor) y su cola ahora (lo que va en vuelo sobre `bw × rtprop`, fresco cada ~10 ms: lo que C9 extrañaba); pasado `governor_target_ms` de cola es último recurso.

## Decide / escribe
- El enlace de cada paquete repartido → [[Reparto (split.h)]] (`cg_split_pick_eta`).
- Con `pace_any`, una segunda instancia sirve a [[Copias a tiempo (eta.h)]]; con `redund_spare` también, y de ella [[Lo que sobra (spare.h)]] toma el retardo sin cola de cada enlace para elegir el primero.

## Perillas
- [[Mapa/Perillas/split_eta|split_eta]]: no. [[Mapa/Perillas/split_stale_max|split_stale_max]]: 1; 1–64.
- [[Mapa/Perillas/capacity_sat_ms|capacity_sat_ms]] (la mitad es el umbral de sondeo).
- [[Mapa/Perillas/eta_buffer|eta_buffer]], [[Mapa/Perillas/split_qpred_ms|split_qpred_ms]], [[Mapa/Perillas/split_qpred_mode|split_qpred_mode]], [[Mapa/Perillas/eta_keep|eta_keep]], [[Mapa/Perillas/eta_keep_ms|eta_keep_ms]] (experimentos, 0).

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: por pesos, el 72 % de las retransmisiones de TCP de la bajada eran espurias (DSACK); de ahí el ETA.
- [[015-modelo-de-enlace]]: el ETA nunca desbordaba a los celulares, con 25–30 ms más de base que Starlink; lo resolvió el preset `starlink`. C9 (`eta_keep = 2`) en Starlink: −16 a −48 %.

- [[020-copias-parciales-y-paridad]]: con el enlace lento ahogado, el ETA de cada paquete se daba vuelta a él unos paquetes por tick; por eso [[Lo que sobra (spare.h)]] elige su primer enlace por el retardo sin cola, no por esto.

- Con el protocontrolador B, a GRIT (MPGRIT, paso U2a, 2026-10-31): `cg_eta_behind`, cuándo llega lo que el enlace elegido tiene ahora (su retardo sin cola sobre el menor y su cola sobre su ritmo), vuelve por `ops.send` mensaje a mensaje y fija el temporizador de cada segmento ([[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]).

## Choques conocidos
- Con `capacity_expire_ms`, cada mitad de la caducidad cuenta como medida (`loaded_ms`): `cg_cap_stale` no la vuelve a agrandar durante `CG_CAP_STALE_MS` ([[Capacidad (capacity.h)]], [[016-celda-virtual]]).
- C9: reinicia su cola cada tick con una cola de hace 300–500 ms, sin lo que está en vuelo; oscila con período de 0,6–1 s ([[audit-pending]]). `eta_keep` quedó apagado. Con `link_cc` la cola sale de lo que va en vuelo.
- C2 (resuelto el 2026-10-26): con el ETA, los enlaces ociosos no avanzaban y el RACK del [[Reordenador (reorder.h)]] los esperaba; ahora el receptor sabe por la máscara de [[Enlaces activos (active.h)]] qué enlaces usa el ETA.
- Sin carga un enlace no se mide: [[Cosquilla (tickle.h)]].
- Con ETA, los pesos del [[Gobernador (govern.h)]] y de la capacidad no deciden, pero salen en el estado (auditoría, métrica 10).

## Archivos
- `eta.h` (`cg_eta_own`, `cg_eta_wnd`: con `link_cc_bits` 64, un enlace con la ventana llena es último recurso), `capacity.h` (`cg_cap_eta`, `cg_cap_qpred`, `cg_cap_stale`); tests `engine/tests/test_eta.c`.
- `client.c`: `cap_tick`, `tun_read`, `lc_eta`. `server.c`: `cap_tick`, `down_prepare`, `lc_eta`.
