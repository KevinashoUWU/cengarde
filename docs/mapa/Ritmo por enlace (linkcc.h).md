---
tags: [mapa, pieza]
archivos:
  - engine/src/linkcc.h
  - engine/tests/test_linkcc.c
sello: 484193bada7bd70d
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
- **Estimación:** `bw`, la mayor muestra de 1–2 s (dos baldes), a lo más 5/4
  de lo que el enlace entregó mientras tenía cola (`sat`, promedio; caduca a
  los 2 s sin muestras así): el operador concede en ráfagas y la mayor
  muestra de las subidas sumaba 140 Mbit/s para 86–100 entregados (terreno,
  2026-10-28). `rtprop`, el menor RTT de 10 s. Antes de su primera muestra,
  la capacidad del motor como semilla (`cg_lc_seed`), si no 2 Mbit/s.
- **Su cola (`q_us`, `cg_lc_owd`):** el retardo de ida del `sseq` más nuevo
  que nombra un `SFACK` (el reloj del receptor menos la hora de envío: dos
  relojes, solo cuentan sus cambios) sobre su mínimo de 15–30 s (dos
  baldes); no depende de `bw` ni del camino de vuelta.
- **Modos, por la cola** (objetivo `target_us`, `governor_target_ms`):
  STARTUP a ×3/2 (×2 se pasaba; la capacidad del motor es la semilla) hasta
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
- **MTPT** (*Maximum Transfer Point Tracking*, `CG_LC_MTPT`, `link_cc_bits`
  128; el nombre es del usuario): cada enlace busca su propia cola objetivo
  perturbando y observando, como el MPPT de un cargador solar
  (`cg_lc_mtpt`). Cada segundo mueve su objetivo un paso de 20 ms: en el mismo
  sentido si lo entregado creció 3 % o más, al revés si cayó otro tanto, dos
  pasos abajo si perdió más de 5 %, y se queda quieto si quedó plano o si su
  cola no llegó a la mitad del objetivo (tenía menos que llevar). Entre el
  objetivo base (`governor_target_ms`: solo explora por encima; la primera
  versión, que bajaba hasta 20 ms y bajaba con lo plano, perdió 22 % de la
  bajada en terreno) y el techo que da quien lo usa
  (`cg_lc_target`, `cg_lc_mtpt_cap`): `link_cc_budget_ms` menos cuánto más
  lento es el enlace que el más rápido, así un enlace chico suma lo que puede
  dentro de la ventana del grupo y no estira la espera del reordenador.
- Pura: sin reloj ni E/S; tiempos en µs, tasas en bytes/s.

**La media en vez del máximo** (`link_cc_bits` 256, 2026-10-31, [[029-mpgrit]]): pasado el arranque, `bw` es lo entregado en los últimos cinco baldes enteros de 100 ms (`cg_lc_mean_add`, `cg_lc_mean`; `mean_bps` en el estado); con cola en el enlace es su capacidad y sube o baja, sin cola solo sube, y tras un silencio queda como estaba. Contra la captura de techo el máximo de 1–2 s leía 1,7–5,2 veces la capacidad de un módem saturado (las ráfagas del operador) y la media 0,96–1,00; en terreno, la bajada +21 % y la subida +6 %. En prueba antes de quedar por omisión.

## Lee
- Cada `SFACK` (`cg_sf_tx_ack`): bytes entregados, tasa, RTT.
- Cada envío (`cg_sf_tx_take`: `cg_lc_sent`) y cada `SFLOSS`
  (`cg_sf_tx_find`: `cg_lc_lost`).
- La capacidad del motor en cada tic, como semilla.

## Decide / escribe
- Con `link_cc` (experimento, apagado):
  - [[ETA (eta.h)]] (`cg_eta_own`, en cada tic y cada `SFACK`): el ritmo del
    enlace, su retardo sin cola (su `rtprop` sobre el menor: los `SFACK`
    vuelven por el enlace más rápido, así que las diferencias son las de ida)
    y su cola ahora (`cg_lc_queued`: su retardo de cola al ritmo del enlace;
    sin retardo de ida, lo que va en vuelo sobre `bw × rtprop`).
    Pasado `governor_target_ms` de cola (con MTPT, su propio objetivo), el
    enlace es último recurso.
  - El [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]] recibe como
    oferta la suma de los ritmos de los enlaces (`CG_SCC_OFFER_LINKS`), tenga
    o no cola: cada enlace sondea y cuida su propia cola.
- Estado: `subflow.up.rate` (router) y `subflow.down.rate` (VPS), por enlace:
  `mode`, `bw_bps`, `pacing_bps`, `rtprop_ms`, `srtt_ms`, `cwnd`,
  `inflight`, `cuts`, `queue_ms`, `sat_bps`, `sfacks`, `app_limited`,
  `target_ms`, y con MTPT `mtpt_cap_ms`, `mtpt_up`, `mtpt_down`.

## Perillas
- [[Mapa/Perillas/link_cc|link_cc]]: no. Exige `subflow` en los dos extremos.
- El objetivo de cola es `governor_target_ms` (60 con `subflow`); con MTPT
  es de donde parte, y [[Mapa/Perillas/link_cc_budget_ms|link_cc_budget_ms]]
  (300) da su techo.
- [[Mapa/Perillas/link_cc_bits|link_cc_bits]] (experimento): qué usos de la cola están encendidos (`CG_LC_QETA`, `CG_LC_QMODE`, `CG_LC_SAT`, `CG_LC_S32`, `CG_LC_QMAX`, `CG_LC_SATQ`, `CG_LC_WND`, `CG_LC_MTPT`); por omisión los modos por la cola y STARTUP a ×3/2 (10, `CG_LC_BITS`), y en el router además el tope por lo entregado con cola (14): su subida se concede en ráfagas; en el VPS el tope solo mientras el enlace tiene cola (46: siempre, dejaba al enlace rápido corto).

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
- La [[Capacidad (capacity.h)]] sigue corriendo: con `link_cc` solo da la
  semilla y decide qué enlaces entran al reparto (los vivos con tasa).
- Si la capacidad del enlace cae, `bw` la sigue recién cuando la ventana de
  1–2 s olvida las muestras mayores; mientras tanto la cola crece, la ve el
  retardo de ida (el enlace drena y `sat` recorta `bw`) y el
  [[ETA (eta.h)]] desvía el tráfico.
- Sin PROBE_RTT: una cola que nunca baja en 30 s haría del mínimo del
  retardo de ida uno con cola; el drenaje sobre el objetivo lo evita en la
  práctica.

## Archivos
- `linkcc.h`; tests `engine/tests/test_linkcc.c`.
- `subflow.h` (`struct cg_sf_tx`: `lc`); `client.c` y `server.c`: `lc_eta`,
  `lc_ref`, `on_sfack`, `pctl_hint`.
