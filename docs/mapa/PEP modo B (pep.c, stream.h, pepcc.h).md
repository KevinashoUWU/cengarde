---
tags: [mapa, pieza]
archivos:
  - engine/src/pep.c
  - engine/src/pepio.h
  - engine/src/pep.h
  - engine/src/stream.h
  - engine/src/pepcc.h
  - engine/tests/test_pep.c
  - engine/tests/test_stream.c
  - engine/tests/test_pepcc.c
sello: e23ed63a5786c329
---
# PEP modo B (pep.c, stream.h, pepcc.h)

**Grupo:** Enviar · Recibir · Recuperar — [[Mapa del motor]]

## Qué hace
El «protocontrolador» ([[025-protocontrolador]]): TCP partido en los dos extremos con el protocolo propio en el medio. El router (`pep = yes`, solo con `tunnel = native`) acepta el TCP que el firewall redirige a `pep_port` (destino original con `SO_ORIGINAL_DST`, o `getsockname` con TPROXY), y el VPS abre un TCP nuevo hacia ese destino (`pep_cc`, BBR por omisión). Entre los dos, cada conexión es un **flujo de bytes fiable** dentro del túnel: sus mensajes van como carga de DATA con el primer nibble en `0xF` (protocolo 10), así que el MAC, el anti-replay, las copias, la FEC y el rescate les aplican sin cambios. Lo que no se intercepta (UDP, QUIC) sigue por la TUN.

- **`pep.h`** (puro): el formato (`OPEN`, `DATA` con el reloj del emisor, `ACK` con el menor retardo de ida desde el anterior y la bandera `DUP`, `RST`), los destinos que el VPS no abre (loopback, sin especificar, multicast, enlace local: 169.254.169.254), la tabla del servidor por (sesión, id), el borde de la ventana (`cg_pep_edge`) y lo que un flujo guarda sin enviar (`cg_pep_backlog`).
- **`stream.h`** (puro): el búfer por trozos de 16 KB de un fondo común, el anillo de segmentos del emisor (un reenvío reusa su segmento; cada uno sabe si salió con cola), RACK y el temporizador desde el envío más viejo, el reensamblado del receptor (hasta 256 huecos; el ACK lleva primero el rango más nuevo y hasta 8), el FIN como un byte. Nunca se rinde.
- **`pepcc.h`** (puro): el ritmo de todos los flujos hacia un par: lo que el camino consume (entregado, y perdido de lo que salió sin cola, hasta 3 perdidos por 1 entregado); la cola por el **retardo de ida** que devuelve el receptor (`cg_pcc_owd`, sobre su mínimo de 10 s, el menor de cada ventana de una ida y vuelta; sin él, la ida y vuelta); arranque 3/2 que termina con la primera muestra con cola, ciclo 5/4–3/4–1, 3/4 mientras haya cola sobre `pep_queue_ms`, una ventana entera con cola baja el estimador a lo entregado; en vuelo dos veces `bw × (min_rtt + pep_queue_ms)` más `bw ×` la ventana de RACK; el estimador, el mayor de 1–2 s; arranque otra vez tras 1 s ocioso. De la [[Capacidad (capacity.h)]]: el comienzo (al menos 16 Mbit/s) y el techo cuando el motor halla lleno el enlace de los datos (`CG_CAP_CUT_FULL`), si no queda bajo la mitad del estimador (la capacidad de un enlace ocioso es la de sus sondas).
- **`pep.c`**: los sockets (no bloqueantes; `TCP_NOTSENT_LOWAT` 128 KB; `SO_RCVBUF` de 128 KB en el socket que escucha), la contrapresión, los tiempos (`pep_dead_s`, 30 s para un `connect`), `pep_cc = auto` (cubic hacia el destino, BBR desde 3 segmentos reenviados), los ACK (cada 4 DATA o 2 ms; al instante con un hueco nuevo o lleno, o el FIN), las ventanas de RACK por par (cada `DUP` +4 ms hasta 100, −2 ms por segundo), el estado (`pep` en el JSON, con los huecos del receptor por duración y las esperas de envío y de escritura).

## Lee
- Del túnel: `down_deliver` (router) y `up_deliver` (VPS) le pasan cada mensaje al instante; el [[Reordenador (reorder.h)]] solo cuenta su secuencia (`wgtype` no es dato: nunca lo retiene).
- Del motor, cada tick (`pep_hint`): la capacidad de los enlaces que llevan los datos ([[Lo que sobra (spare.h)]]: el primero; repartido, la suma) y si están llenos.

## Decide / escribe
- Mensajes al túnel por el camino de siempre (`up_one`/`send_links` en el router, `down_prepare`/`down_send` en el VPS). En [[Lo que sobra (spare.h)]], los DATA del flujo no se copian en las copias de un microcorte (`pnc`: su ARQ reenvía), y un segmento reenviado por segunda vez es una copia prioritaria que no va a un enlace con cola sobre `redund_spare_ms` (`prc`); repartido, va por todos los portadores.
- Contrapresión: el VPS cierra su borde cuando lo que tiene en orden sin escribir pasa `pep_backlog_ms` a su ritmo; el router deja de leer la LAN cuando lo que tiene sin enviar pasa eso (al menos 256 KB) o el búfer está lleno.
- Estado: `pep` (flujos, abiertos, rehusados por motivo, RST, reenvíos, copias, duplicados, `holes_ms`, `sendq_ms`, `writeq_ms`, memoria y su pico, `cc_to_bbr`, y por par el ritmo, `min_rtt`, `srtt`, en vuelo, ventana, modo, ventana de RACK).

## Perillas
- [[Mapa/Perillas/pep|pep]], [[Mapa/Perillas/pep_port|pep_port]], [[Mapa/Perillas/pep_buf_kb|pep_buf_kb]], [[Mapa/Perillas/pep_mem_mb|pep_mem_mb]], [[Mapa/Perillas/pep_max_flows|pep_max_flows]], [[Mapa/Perillas/pep_queue_ms|pep_queue_ms]], [[Mapa/Perillas/pep_backlog_ms|pep_backlog_ms]], [[Mapa/Perillas/pep_dead_s|pep_dead_s]], [[Mapa/Perillas/pep_reo_ms|pep_reo_ms]], [[Mapa/Perillas/pep_cc|pep_cc]]. Todas reinician el proceso al cambiar.

## Medido
- [[025-protocontrolador]]: el laboratorio de la 024 (`protect`, `beyond`, `protectcut`, vivo) contra el túnel transparente y el PEP A.
