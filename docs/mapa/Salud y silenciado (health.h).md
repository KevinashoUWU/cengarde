---
tags: [mapa, pieza]
archivos:
  - engine/src/health.h
  - engine/tests/test_health.c
sello: 796d27b3c8a0770a
---
# Salud y silenciado (health.h)

**Grupo:** Medir · Decidir — [[Mapa del motor]]

## Qué hace
Decide qué enlaces llevan cada paquete en el sentido que envía este extremo (el cliente la subida, el servidor la bajada). Filtra los retardos de ida que reporta el otro extremo (EWMA de ganancia 1/4) y compara los enlaces. Un enlace activo que va más de `mute_behind_ms` detrás del más rápido durante `mute_settle_ms` queda **silenciado** (G1): conserva socket y sondas pero no lleva datos. El **mudo** (stall) es aparte e inmediato: un enlace que no responde no lleva nada. Con `mute_queue` (experimental, apagado) compara el **atraso** (`cg_health_late`): el mayor entre lo que va detrás, su cola propia sobre la del más rápido y la cota de sus sondas sin respuesta; un informe viejo de un enlace callado y atrasado no reinicia la espera.

## Lee
- `owd` de [[Sondas y cola de bonding (proto.h)]].
- Con `mute_queue`: la cola sobre el mínimo de 60 s de [[Cola por enlace (cg_gov_base)]] (`ql_us` de [[Capacidad (capacity.h)]], del tick anterior) y, en el cliente, la sonda más vieja sin respuesta menos el RTT del enlace vivo más rápido (`cg_health_silent_us`).
- Mudo: en el cliente, 3 sondas sin respuesta y la más vieja fuera hace 2 RTT + 1 s; en el servidor, 3 intervalos anunciados + 1 s sin paquete verificado.
- **Mudo pero trayendo** ([[025-protocontrolador]]): para lo que sube (los datos, sus reparaciones y paridades, el rescate, la PARQ y el primer enlace de [[Lo que sobra (spare.h)]]: `link_masks_tx` en `client.c`), un enlace con las sondas detenidas sigue vivo si trajo un paquete verificado en los últimos 2 RTT + 100 ms y la detención tiene menos de 5 s (`cg_link_rx_alive`); para el DATA de un flujo del protocontrolador, que tiene ARQ, si trajo algo en el último segundo (`cg_link_rx_alive_arq`). Un microcorte que pierde 60 % en cada sentido deja 3 sondas seguidas sin respuesta durante un segundo de vez en cuando (0,84 por sonda: ~17 % por ventana), y dejarlo mandaba todo a la cola del celular. El estado del enlace (`live`/`stalled`), el reordenador y la salud siguen con el mudo de siempre.

## Decide / escribe
- Estado por enlace (`CG_H_ACTIVE` / `CG_H_MUTED`), `behind_us`, el `owd` filtrado y su frescura.
- `cg_health_carriers`: los que llevan todo → [[Reparto (split.h)]], el envío y, en cada tick, el primer enlace de [[Lo que sobra (spare.h)]] (que además solo cuenta enlaces con un reporte en los últimos 3 s, `report_ms`).
- El `owd` filtrado lo leen [[Cola por enlace (cg_gov_base)]], [[G4 guarda de dispersión (split.h)]], [[Gobernador (govern.h)]], [[Capacidad (capacity.h)]] y [[ETA (eta.h)]].
- Los cambios van a la [[Bitácora de decisiones (decision.h)]] (G1).

## Perillas
- [[Mapa/Perillas/mute_behind_ms|mute_behind_ms]]: 150; 0–60000 (0: nunca silencia por retardo).
- [[Mapa/Perillas/unmute_behind_ms|unmute_behind_ms]]: 4/5 de lo anterior (120).
- [[Mapa/Perillas/mute_settle_ms|mute_settle_ms]]: 2000; 100–600000. Volver pide el doble; cada vuelta que no dura 30 s (`CG_H_FLAP_MS`) duplica la espera, hasta ×8.
- [[Mapa/Perillas/mute_trickle|mute_trickle]]: 0 (un paquete de cada N por un silenciado).
- [[Mapa/Perillas/min_active_links|min_active_links]]: 2; 1–16. Con dos enlaces, nada se silencia.
- [[Mapa/Perillas/mute_queue|mute_queue]]: no.

## Dónde se midió
- [[006-salud-de-enlaces]]: con un enlace de 500 ms de cola, se silencia en 3–3,6 s en los dos sentidos; el túnel no pierde nada y el enlace no recae.
- [[012-bonding]]: en bonding, el mudo de 1–1,5 s sirve para redundancia, no para repartir; de ahí G3.
- [[016-celda-virtual]]: un LTE cayendo al acantilado del SNR junto a un fijo, con `min_active_links = 1`: con `mute_queue` se silencia a los 3,3 s (sin ella, 3,8–7,9 s o nunca con BLER 60 %), pero el TCP de la redundancia no mejora (lo limita la pérdida del fijo) y, al volver la señal, la espera exponencial lo deja fuera 6–9 s; queda apagada.

## Choques conocidos
- C6/C7 (frescura por sentido): un informe seguía «fresco» 3 s; ahora la frescura es `CG_FRESH_PROBES` (5) intervalos de sonda. Mergeado (PR #79, [[audit-pending]]).
- Umbrales distintos sobre el mismo retardo: silenciado 150 ms, G4 30 ms, gobernador 20 ms (auditoría de relojería, [[audit-pending]]).
- [[G4 guarda de dispersión (split.h)]] usa `mute_settle_ms` para su vuelta.
- `mute_queue` usa la misma cola que [[Capacidad (capacity.h)]] y el [[Rescate (rescue.h)]] (`ql_us`); en bonding de terreno `mute_behind_ms = 0` la deja sin efecto.

## Archivos
- `engine/src/health.h`; tests `engine/tests/test_health.c`.
- `client.c`: `health_tick` (llama a `cg_health_eval` y luego a `spread_tick`, `gov_tick`, `cap_tick`), `on_probe_reply`. `server.c`: `health_tick`, `on_probe`.
- Config: `cg_hcfg_of` en `engine.h`.
