---
tags: [mapa, pieza]
archivos:
  - engine/src/subflow.h
  - engine/tests/test_subflow.c
sello: 67a54fbaac843b4b
---
# Subflujo (subflow.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
La primera pieza de MPGRIT ([[029-mpgrit]], paso U1): cada enlace es un
subflujo con su propia secuencia (`sseq`, protocolo 12, [[Sondas y cola de bonding (proto.h)]]). Con la perilla `subflow`, cada extremo numera el DATA que
pone en cada enlace y sigue la secuencia de cada enlace que recibe; lo que un
enlace perdió **dentro de sí** vuelve al emisor en un `SFLOSS`. El desorden
entre enlaces, que en terreno se leía como pérdida en el túnel y en GRIT
([[028-grit-y-el-motor-como-uno]] «Terreno»), ya no cuenta como pérdida.

- **Receptor (`cg_sf_rx`):** un hueco se da por perdido cuando por ese mismo
  enlace llegó un `sseq` posterior hace más que su ventana (`reo_us`: RACK de
  un camino). Un hueco que llega después de darse por perdido fue un veredicto
  falso (`late`): la ventana sube 4 ms, como un DSACK agranda la de TCP, hasta
  su tope (`CG_SF_CAP_US`, el presupuesto de latencia de 300 ms), y baja
  0,2 ms por tic de 100 ms hasta su piso (`CG_SF_FLOOR_US`, 10 ms).
- **Ventana de 1024 `sseq`** por enlace: lo que sale de ella sin llenarse se da
  por perdido sin decirlo (`overflow`). Un salto de una ventana o más, hacia
  adelante o hacia atrás, es un comienzo nuevo (`resync`: el otro extremo
  volvió a empezar, un enlace tomó la ranura de otro, o el enlace lo perdió
  todo) y no dice ninguna pérdida, porque un `SFLOSS` tiene que ser cierto. La
  secuencia da la vuelta en 32 bits y el 0 no existe (es «sin numerar»).
- **Emisor (`cg_sf_tx`):** numera desde 1, salta el 0 al dar la vuelta,
  recuerda qué secuencia del túnel llevó cada `sseq` (para `SFLOSS`) y devuelve
  los números de lo que el socket no tomó (`cg_sf_tx_unsend`), o el receptor
  leería una pérdida falsa.
- **La cola:** una pérdida al final de lo que llevó un enlace no la delata
  ningún DATA posterior. La sonda del router y la respuesta del VPS van por el
  mismo socket que el DATA y llevan en su `sseq` el siguiente del enlace
  (`cg_sf_rx_sent`): lo de abajo que no llegó es un hueco desde ese momento
  (`tail`), como la sonda de cola de TCP.
- **Lo que cuenta como llegada:** cada DATA verificado, también una copia que
  llega por otro enlace después de la primera (dice que ese enlace no la
  perdió); por eso, con `subflow`, se verifica el MAC de las copias (antes se
  descartaban sin verificar). Lo reconstruido por un código no es una llegada.
- **Una pérdida del enlace no es siempre del receptor:** un código (FEC v3, la
  paridad XOR, `redund_spare`) puede haberla reconstruido. El emisor junta la
  evidencia en el anillo del [[Rescate (rescue.h)]] (`cg_resc_sure`): sin
  código, el `SFLOSS` basta; con código, hace falta además el NACK del
  reordenador del receptor. Lo copiado por varios enlaces nunca se da por
  perdido por uno solo.
- Funciones: `cg_sf_rx_init`, `cg_sf_rx_in` (`NEW`, `FILL`, `LATE`, `DUP`,
  `OLD`), `cg_sf_rx_poll` (los huecos vencidos, del más viejo al más nuevo),
  `cg_sf_rx_due_us` (cuándo vence el más viejo, para despertar el bucle),
  `cg_sf_rx_decay`; `cg_sf_tx_init`, `cg_sf_tx_take`, `cg_sf_tx_unsend`,
  `cg_sf_tx_find`.

## Lee
- Cada DATA que sale (`client.c`, el lote de `sendmmsg` por enlace;
  `server.c`, el de cada camino): su `sseq`, junto al byte del enlace, fuera
  del MAC.
- Cada DATA verificado que llega ([[Camino del router (clientpath.h)]] en el
  router; la ruta de subida en `server.c`).

## Decide / escribe
- `SFLOSS` (tipo 9): un byte con el enlace y los rangos de `sseq` en el formato
  del NACK; por el enlace vivo de menor RTT (router) o de menor retardo (VPS).
  Al vencer un hueco: después de cada pasada del bucle y cuando
  `cg_sf_rx_due_us` dice (el tic de 100 ms es muy grueso).
- Con `subflow`, el RACK de GRIT sobre la mezcla de los enlaces se apaga
  (`cg_pctl_rack`): un segmento enviado antes que el último entregado no está
  perdido porque fue por un enlace más lento; quedan las pérdidas de cada
  enlace y el temporizador como respaldo, con al menos 300 ms sobre el RTT de
  la mezcla (`cg_scc_rto_extra`).
- Al recibir un `SFLOSS`, para un mensaje de GRIT que fue solo por ese enlace:
  `cg_pctl_nack` con la certeza de `cg_resc_sure` ([[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]) y `CG_F_GONE` al otro extremo. El DATA
  del túnel (IP) sigue con el reordenador y sus NACK.
- Estado, por enlace (router) o por camino (VPS), objeto `subflow`: lo que
  sigue este extremo (`got`, `lost`, `late`, `reordered`, `copies`,
  `overflow`, `resync`, `tail`, `window_ms`) y lo que el otro dijo de lo enviado
  (`sent`, `unsent`, `lost`, `forgotten`, `to_grit`, `copied`, `not_kept`,
  `other`, `unsure`); `sfloss_sent` y `sfloss_rx` en `rescue`.

## Perillas
- [[Mapa/Perillas/subflow|subflow]]: no. Tienen que tenerla los dos extremos.

## Dónde se midió
- Laboratorio, 2026-10-28 (`~/campo/2026-10-28/sf-smoke`): sin la regla de la
  evidencia, en `lossy` de bajada el subflujo halló 900 pérdidas, GRIT reenvió
  906 y el router recibió 904 duplicados: la FEC v3 ya las había
  reconstruido. Con la regla, 0–3.
- `~/campo/2026-10-28/sf1` (n = 3, 7 casos × 2 sentidos): con el RACK de la
  mezcla encendido, casi todo lo que GRIT reenvió fue por RACK y llegó
  duplicado, también con su ventana en 250 ms (`protocontroller_reo_ms`);
  de ahí apagarlo con `subflow`.
- `sf2` (n = 4, `7fba66d`): sin el RACK de la mezcla se fueron sus
  reenvíos (`reorder50` de subida: 2094 → 210 y 75), pero el temporizador
  ocupó su lugar, casi todo falso (`cell4` de subida: 386 por él, 514
  duplicados; `ratswitch`: 864): el RTT de la mezcla es más corto que el del
  enlace más lento. De ahí `cg_scc_rto_extra`.

## Choques conocidos
- Lo copiado por varios enlaces (redundancia) no tiene un enlace que responda
  por su pérdida: sin el RACK de la mezcla, GRIT lo recupera por el NACK (con
  `protocontroller_nack yes`) o por el temporizador.
- Memoria: en el VPS, 13 KiB por camino con `subflow` (se toma al primer uso);
  en el router, 13 KiB por enlace dentro de `struct client`.

## Archivos
- `subflow.h`; tests `engine/tests/test_subflow.c`.
- `client.c`: el sello en el lote, `on_sfloss`, `sf_service`, `sf_timeout`,
  `fastest_link`. `server.c`: `struct sfpath`, `sf_get`, `sf_up`,
  `sf_service`, `on_sfloss`, `say_gone`. `clientpath.h`: la llegada.
