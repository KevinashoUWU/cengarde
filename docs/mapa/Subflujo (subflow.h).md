---
tags: [mapa, pieza]
archivos:
  - engine/src/subflow.h
  - engine/tests/test_subflow.c
sello: c3bf335428a7bc21
---
# Subflujo (subflow.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
La primera pieza de MPGRIT ([[029-mpgrit]], paso U1): cada enlace es un
subflujo con su propia secuencia (`sseq`, protocolo 12, [[Sondas y cola de bonding (proto.h)]]). Con la perilla `subflow`, cada extremo numera el DATA que
pone en cada enlace y sigue la secuencia de cada enlace que recibe; lo que un
enlace perdió **dentro de sí** vuelve al emisor en un `SFLOSS` (los rangos del
NACK: base y máscara de 64 bits, hasta 16), por los dos enlaces más rápidos,
como el NACK: si uno lo pierde, la pérdida no espera al RACK ni al
temporizador de GRIT. El desorden
entre enlaces, que en terreno se leía como pérdida en el túnel y en GRIT
([[028-grit-y-el-motor-como-uno]] «Terreno»), ya no cuenta como pérdida.

- **Receptor (`cg_sf_rx`):** un hueco se da por perdido cuando por ese mismo
  enlace llegó un `sseq` posterior hace más que su ventana (`reo_us`: RACK de
  un camino). Un hueco que llega después de darse por perdido fue un veredicto
  falso (`late`): la ventana sube 4 ms, como un DSACK agranda la de TCP, hasta
  su tope (`CG_SF_CAP_US`, el presupuesto de latencia de 300 ms), y baja
  0,2 ms por tic de 100 ms hasta su piso (`CG_SF_FLOOR_US`, 10 ms).
- **Ventana de 8192 `sseq`** por enlace, en los dos extremos: lo que sale de
  ella sin llenarse se da por perdido sin decirlo (`overflow`). El emisor
  tiene que recordar qué llevó cada `sseq` hasta que llega su `SFLOSS` (la
  cola del enlace, la ventana y la vuelta): con 1024 (~110 ms a 100 Mbit/s)
  el VPS había olvidado dos tercios de lo que el NUC le dijo perdido en
  terreno (`forgotten`), y GRIT lo reenviaba solo por su temporizador. Un salto de una ventana o más, hacia
  adelante o hacia atrás, es un comienzo nuevo (`resync`: el otro extremo
  volvió a empezar, un enlace tomó la ranura de otro, o el enlace lo perdió
  todo) y no dice ninguna pérdida, porque un `SFLOSS` tiene que ser cierto. La
  secuencia da la vuelta en 32 bits y el 0 no existe (es «sin numerar»).
- **Emisor (`cg_sf_tx`):** numera desde 1, salta el 0 al dar la vuelta,
  recuerda qué secuencia del túnel llevó cada `sseq` (para `SFLOSS`; el segundo
  aviso de una misma `sseq`, su copia por el otro enlace, no cuenta: `repeat`,
  `CG_SF_LOST`, así la pérdida del enlace y su ritmo no la ven dos veces) y devuelve
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
  reordenador del receptor. El NACK solo nunca basta con `subflow` (en
  terreno, 4–8 mil por descarga de lo que ningún enlace había perdido). Lo
  copiado por varios enlaces nunca se da por perdido por uno solo.
- **Lo que entregó cada enlace (`SFACK`, paso 2):** el receptor cuenta los
  bytes que tomó por cada enlace (`NEW`, `FILL` o `LATE`; una copia no) y su
  `sseq` más nuevo (`cg_sf_rx_took`); a lo más cada 10 ms (`CG_SFACK_US`), un
  `SFACK` dice, enlace por enlace con novedades, el `sseq` más nuevo, los
  bytes tomados, cuándo llegó el más nuevo (en el reloj del receptor) y
  cuánto esperó (`cg_sf_rx_ack`). El emisor recuerda de cada
  `sseq` cuándo salió, su largo, los bytes enviados hasta él y el último
  `SFACK` conocido entonces; con cada respuesta (`cg_sf_tx_ack`) saca lo que
  va en vuelo (lo que salió después del más nuevo), una tasa de entrega en el
  reloj del receptor (así no cuentan la espera ni el lote de las respuestas),
  un RTT menos la espera y el retardo de ida del más nuevo (la cola del
  enlace), para el [[Ritmo por enlace (linkcc.h)]]. Una
  respuesta más vieja que la última (por `sseq`) no dice nada; una cuyos
  bytes retroceden es un receptor que volvió a empezar: la nueva base.
- Funciones: `cg_sf_rx_init`, `cg_sf_rx_in` (`NEW`, `FILL`, `LATE`, `DUP`,
  `OLD`), `cg_sf_rx_took`, `cg_sf_rx_ack`, `cg_sf_rx_poll` (los huecos
  vencidos, del más viejo al más nuevo), `cg_sf_rx_due_us` (cuándo vence el
  más viejo, para despertar el bucle), `cg_sf_rx_decay`; `cg_sf_tx_init`,
  `cg_sf_tx_take`, `cg_sf_tx_unsend`, `cg_sf_tx_find` (y `cg_sf_tx_find_cls`, que dice si la `sseq` llevaba un STREAM: `CG_SF_STREAM`, puesto con `cg_sf_tx_stream`, protocolo 13), `cg_sf_tx_ack`;
  `cg_sfack_put`, `cg_sfack_get`.

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
- `SFACK` (tipo 10): una cuenta (1–16) y 15 bytes por enlace (el enlace, el
  `sseq` más nuevo, los bytes tomados, su hora y la espera; `cg_hdr_parse`
  exige ese largo); por el mismo enlace que el `SFLOSS`, a lo más cada 10 ms
  y solo si algún enlace tiene novedades (el bucle despierta para eso,
  `sf_timeout`).
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
  `other`, `unsure`, y `rate`, el [[Ritmo por enlace (linkcc.h)]]);
  `sfloss_sent`, `sfloss_rx`, `sfack_sent` y `sfack_rx` en `rescue`.
  `window_ms` salía mil veces más chico hasta el paso 2.

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
- Terreno (2026-10-28, `~/campo/2026-10-28/field-u1`, NUC y VPS): GRIT
  reenvía 0,6–1 % en vez de 10 %; los duplicados en el NUC, 3–13 en vez de
  6–8 mil ([[029-mpgrit]]).
- `sf2` (n = 4, `7fba66d`): sin el RACK de la mezcla se fueron sus
  reenvíos (`reorder50` de subida: 2094 → 210 y 75), pero el temporizador
  ocupó su lugar, casi todo falso (`cell4` de subida: 386 por él, 514
  duplicados; `ratswitch`: 864): el RTT de la mezcla es más corto que el del
  enlace más lento. De ahí `cg_scc_rto_extra`.

## Choques conocidos
- Lo copiado por varios enlaces (redundancia) no tiene un enlace que responda
  por su pérdida: sin el RACK de la mezcla, GRIT lo recupera por el NACK (con
  `protocontroller_nack yes`) o por el temporizador.
- Memoria: en el VPS, 33 KiB por camino con `subflow` (se toma al primer uso);
  en el router, otro tanto por enlace dentro de `struct client` (27 KiB el
  emisor, con lo de cada `sseq` para las muestras del `SFACK`; 5 KiB el
  receptor).

## Archivos
- `subflow.h`; tests `engine/tests/test_subflow.c`.
- `client.c`: el sello en el lote, `on_sfloss`, `on_sfack`, `sfack_send`,
  `sf_service`, `sf_timeout`, `fastest_link`. `server.c`: `struct sfpath`,
  `sf_get`, `sf_up`, `sf_service`, `sfack_service`, `on_sfloss`, `on_sfack`,
  `say_gone`. `clientpath.h`: la llegada (`CG_RXV_SFACK`).
