---
tags: [mapa, pieza]
archivos:
  - engine/src/linkcc.h
  - engine/tests/test_linkcc.c
sello: 4334f4237ae4baf6
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
- **Estimación:** `bw`, la mayor muestra de 1–2 s (dos baldes); `rtprop`, el
  menor RTT de 10 s. Antes de su primera muestra, la capacidad del motor como
  semilla (`cg_lc_seed`), si no 2 Mbit/s.
- **Modos:** STARTUP a ×2 hasta que `bw` deja de crecer un cuarto en tres
  rondas (una ronda: un RTT, al menos 10 ms); DRAIN a ×½ hasta que lo que va
  en vuelo cabe en `bw × rtprop`; PROBE en ciclo (5/4 una ronda, 3/4, y 1 seis).
- **Ventana:** en vuelo a lo más `2 × bw × (rtprop + objetivo de cola)`, al
  menos 16 000 bytes (`cg_lc_cwnd`, `cg_lc_room`).
- **Pérdida:** una ronda que pierde más del 5 % de lo que envió (con al menos
  20 paquetes) baja `bw` a 7/8 y termina STARTUP (la respuesta de BBRv2). Las
  pérdidas son las del enlace (`SFLOSS`), nunca el atraso de otro.
- **Sin datos:** con menos en vuelo que `bw × rtprop` el enlace estaba corto
  de datos (`cg_lc_idle`): su muestra cuenta solo si es mayor (el
  *app-limited* de BBR).
- Pura: sin reloj ni E/S; tiempos en µs, tasas en bytes/s.

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
    y su cola ahora (`cg_lc_queued`, lo que va en vuelo sobre `bw × rtprop`).
    Pasado `governor_target_ms` de cola, el enlace es último recurso.
  - El [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]] recibe como
    oferta la suma de los ritmos de los enlaces (`CG_SCC_OFFER_LINKS`), tenga
    o no cola: cada enlace sondea y cuida su propia cola.
- Estado: `subflow.up.rate` (router) y `subflow.down.rate` (VPS), por enlace:
  `mode`, `bw_bps`, `pacing_bps`, `rtprop_ms`, `srtt_ms`, `cwnd`,
  `inflight`, `cuts`, `sfacks`, `app_limited`.

## Perillas
- [[Mapa/Perillas/link_cc|link_cc]]: no. Exige `subflow` en los dos extremos.
- El objetivo de cola es `governor_target_ms` (60 con `subflow`).

## Dónde se midió
- Laboratorio, 2026-10-28 (`~/campo/2026-10-28/lc1`, todavía sin manejar el
  reparto): en `het` (60, 35 y 10 Mbit/s) de subida, `bw` 53, 25 y 7,8 Mbit/s,
  y `rtprop` 40, 55 y 80 ms (la ida de cada enlace más la vuelta por el más
  rápido); de bajada 49, 25 y 6 Mbit/s, con recortes por pérdida cuando las
  colas desbordan.

## Choques conocidos
- La [[Capacidad (capacity.h)]] sigue corriendo: con `link_cc` solo da la
  semilla y decide qué enlaces entran al reparto (los vivos con tasa).
- Si la capacidad del enlace cae, `bw` la sigue recién cuando la ventana de
  1–2 s olvida las muestras mayores; mientras tanto la cola crece, pero el
  [[ETA (eta.h)]] la ve en cada `SFACK` y desvía el tráfico.

## Archivos
- `linkcc.h`; tests `engine/tests/test_linkcc.c`.
- `subflow.h` (`struct cg_sf_tx`: `lc`); `client.c` y `server.c`: `lc_eta`,
  `lc_ref`, `on_sfack`, `pctl_hint`.
