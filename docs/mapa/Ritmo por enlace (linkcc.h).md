---
tags: [mapa, pieza]
archivos:
  - engine/src/linkcc.h
  - engine/tests/test_linkcc.c
sello: c7c84cbdcada122d
---
# Ritmo por enlace (linkcc.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Un BBR por enlace (MPGRIT, [[029-mpgrit]], paso 2), como el que OpenMPTCProuter
corre en cada subflujo de MPTCP: en terreno sus módems guardaban 45–60 ms de
cola y daban 2–3 veces la capacidad que estimaba el motor (un módem da más
cuando su cola crece), mientras un solo controlador sobre la mezcla y la
[[Capacidad (capacity.h)]] (lo más que entregó un enlace, cortado al ver cola)
los dejaban a medias.

- **Muestras:** cada `SFACK` ([[Subflujo (subflow.h)]]) dice cuántos bytes
  tomó el receptor por el enlace; el emisor las convierte en la tasa de
  entrega de BBR (lo entregado desde que salió el `sseq` más nuevo, sobre el
  tiempo en el reloj del receptor) y en su RTT.
- **Estimación:** `bw`, en STARTUP la mayor muestra de 1–2 s (dos baldes);
  pasado el arranque, lo entregado en los últimos cinco baldes enteros de
  100 ms (la media de 0,5 s, `cg_lc_mean_add`, `cg_lc_mean`): con cola en
  el enlace es su capacidad y sube o baja, sin cola solo sube, y tras un
  silencio queda como estaba. A lo más 5/4 de lo que el enlace entregó
  mientras tenía cola (`sat`, promedio; caduca a los 2 s sin muestras así):
  el operador concede en ráfagas y la mayor muestra de las subidas sumaba
  140 Mbit/s para 86–100 entregados (terreno, 2026-10-28). En el VPS
  (`satq`, lo pone `server.c`) ese tope rige solo mientras el enlace tiene
  cola. `rtprop`, el menor RTT de 10 s. Antes de su primera muestra,
  2 Mbit/s: nace de su propio arranque, sin la capacidad del motor.
- **Su cola (`q_us`, `cg_lc_owd`):** el retardo de ida del `sseq` más nuevo
  que nombra un `SFACK` (el reloj del receptor menos la hora de envío: dos
  relojes, solo cuentan sus cambios) sobre su mínimo de 15–30 s (dos
  baldes); no depende de `bw` ni del camino de vuelta.
- **Modos, por la cola** (objetivo `target_us`, `governor_target_ms`):
  STARTUP a ×3/2 (×2 se pasaba) hasta
  que `bw` deja de crecer un cuarto en tres rondas (una ronda: un RTT, al
  menos 10 ms) o la cola pasa el objetivo; DRAIN a ×½ hasta que la cola baja
  de la mitad (sin retardo de ida: hasta que lo que va en vuelo cabe en
  `bw × rtprop`); PROBE en ciclo (5/4 una ronda, 3/4, y 1 seis), pero sobre el
  objetivo drena (3/4) y sondea a 5/4 solo bajo la mitad (`cg_lc_qstate`).
- **Ventana:** en vuelo a lo más `2 × bw × (rtprop + objetivo de cola)`, al
  menos 16 000 bytes (`cg_lc_cwnd`, `cg_lc_room`).
- **Pérdida:** una ronda que pierde más del 5 % de lo que envió (con al menos
  20 paquetes) baja `bw` a 7/8 y termina STARTUP (la respuesta de BBRv2). Las
  pérdidas son las del enlace (`SFLOSS`), nunca el atraso de otro.
- **Sin datos:** con menos en vuelo que `bw × rtprop` el enlace estaba corto
  de datos (`cg_lc_idle`): su muestra cuenta solo si es mayor (el
  *app-limited* de BBR).
- Pura: sin reloj ni E/S; tiempos en µs, tasas en bytes/s.

**Lo que quedó fijo** (2026-11-01, [[029-mpgrit]], «La limpieza del ritmo por enlace»): cada uso de la cola fue un bit de `link_cc_bits` hasta que se midió; los que ganaron son el motor y la perilla ya no existe (si una configuración vieja la dice, el motor avisa y sigue):
- Los modos por la cola y STARTUP a ×3/2 (laboratorio, 2026-10-28: +8,7 % y 3–10 veces menos pérdidas por enlace).
- El tope por lo entregado con cola: siempre en la subida del router (de 78 / 59 a 96 / 101 Mbit/s en terreno), solo con cola en la bajada del VPS (+23 % en `fade`, +11 % en terreno).
- La media de 0,5 s (contra la captura de techo el máximo leía 1,7–5,2 veces la capacidad de un módem saturado y la media 0,96–1,00; de madrugada la bajada +25 %).
- Sin la semilla de la [[Capacidad (capacity.h)]] y sin su respaldo en la pista a GRIT (`pctl_hint`); el [[Enlace dominante (dominant.h)]] por el `bw` de cada enlace; la guarda de la [[FEC v3 (fec3.h)]] por la cola propia (`cg_lc_f3_queue`) y su pérdida por el subflujo en el último tick (`cg_lc_tick_loss`). Laboratorio +4,1 %; con la FEC v3 encendida en terreno +9 % de bajada y de subida, con 7,2 % de reparaciones en la bajada en vez de 12,2 %.

Lo que perdió está en `docs/desvan/`, fuera del motor: la cola del reparto por el retardo (1 y 16: −20 a −53 %), la ventana como el planificador de OMR (64: neutro), el MTPT, que buscaba la cola objetivo de cada enlace perturbando y observando (128, con `link_cc_budget_ms`: −12 % y −6 % en terreno), y que el ritmo propio decida solo (512: subida −8 % en terreno).

## Lee
- Cada `SFACK` (`cg_sf_tx_ack`): bytes entregados, tasa, RTT.
- Cada envío (`cg_sf_tx_take`: `cg_lc_sent`) y cada `SFLOSS`
  (`cg_sf_tx_find`: `cg_lc_lost`).

## Decide / escribe
- Con `link_cc` (encendido por los modos del router y del VPS; en el motor, apagado):
  - [[ETA (eta.h)]] (`cg_eta_own`, en cada tic y cada `SFACK`): el ritmo del
    enlace, su retardo sin cola (su `rtprop` sobre el menor: los `SFACK`
    vuelven por el enlace más rápido, así que las diferencias son las de ida)
    y su cola ahora (`cg_lc_queued`: lo que va en vuelo sobre `bw × rtprop`).
    Pasado `governor_target_ms` de cola, el enlace es último recurso.
  - La [[FEC v3 (fec3.h)]] (su guarda y la pérdida de cada enlace) y el
    [[Enlace dominante (dominant.h)]] (su tasa).
  - El [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]] recibe como
    oferta la suma de los ritmos de los enlaces (`CG_SCC_OFFER_LINKS`), tenga
    o no cola: cada enlace sondea y cuida su propia cola.
- Estado: `subflow.up.rate` (router) y `subflow.down.rate` (VPS), por enlace:
  `mode`, `bw_bps`, `pacing_bps`, `rtprop_ms`, `srtt_ms`, `cwnd`,
  `inflight`, `cuts`, `queue_ms`, `sat_bps`, `mean_bps`, `sfacks`,
  `app_limited`, `target_ms`.

## Perillas
- [[Mapa/Perillas/link_cc|link_cc]]: no. Exige `subflow` en los dos extremos.
- El objetivo de cola es `governor_target_ms` (60 con `subflow`).

## Dónde se midió
- Laboratorio, 2026-10-28 (`~/campo/2026-10-28/lc1`, todavía sin manejar el
  reparto): en `het` (60, 35 y 10 Mbit/s) de subida, `bw` 53, 25 y 7,8 Mbit/s,
  y `rtprop` 40, 55 y 80 ms (la ida de cada enlace más la vuelta por el más
  rápido); de bajada 49, 25 y 6 Mbit/s, con recortes por pérdida cuando las
  colas desbordan.

- Laboratorio, 2026-10-28, por piezas (`lc6`, `lc7`, n = 2): los modos por
  la cola y STARTUP a ×3/2 (10) ganan +8,7 % de media geométrica sobre el
  `link_cc` sin cola, con 3–10 veces menos pérdidas por enlace (equal de
  bajada 66 → 81 Mbit/s y de 5841 a 542 perdidos; lossy +15–17 %;
  reorder50 +18–26 %); la cola del reparto por retardo (1) pierde 20–53 %.
- Terreno, 2026-10-28 (`field-l2`, 2 rondas): con el tope (14) la subida 95,5 / 101,4
  Mbit/s contra 77,9 / 59,4 sin él (10), OMR 92,7 / 94,8, ping p95 110–114 (OMR
  144–168); la bajada 83,0 / 75,4 con el tope contra 85,0 / 97,6 sin él (OMR
  165,8 / 89,8).
- Terreno, 2026-10-28 (`field-l1`, antes de la cola por retardo): la bajada
  con `link_cc` 124,6 / 97,7 Mbit/s contra 62,8 / 64,7 sin él (OMR 145 /
  155), ping p95 45; la subida igual (86 / 84), en la capacidad de subida de
  los módems, con el ping p95 de 65 a 128 ms ([[029-mpgrit]]).

## Choques conocidos
- La [[Capacidad (capacity.h)]] sigue corriendo: con `link_cc` decide qué
  enlaces entran al reparto (los vivos con tasa), sus pesos, el rescate
  lleno y el tope de `cake`.
- Si la capacidad del enlace cae, `bw` la sigue en medio segundo (la
  media, con cola); mientras tanto la cola crece, la ve el retardo de ida
  (el enlace drena y `sat` recorta `bw`) y el [[ETA (eta.h)]] desvía el
  tráfico.
- Sin PROBE_RTT: una cola que nunca baja en 30 s haría del mínimo del
  retardo de ida uno con cola; el drenaje sobre el objetivo lo evita en la
  práctica.

## Archivos
- `linkcc.h`; tests `engine/tests/test_linkcc.c`.
- `subflow.h` (`struct cg_sf_tx`: `lc`); `client.c` y `server.c`: `lc_eta`,
  `lc_ref`, `on_sfack`, `pctl_hint`.
