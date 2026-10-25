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

- **`pep.h`** (puro): el formato (`OPEN`, `DATA`, `ACK`, `RST`), los destinos que el VPS no abre (loopback, sin especificar, multicast, enlace local: 169.254.169.254), la tabla del servidor por (sesión, id), el borde de la ventana (`cg_pep_edge`) y lo que un flujo guarda sin enviar (`cg_pep_backlog`).
- **`stream.h`** (puro): el búfer por trozos de 16 KB de un fondo común, el anillo de segmentos del emisor (un reenvío reusa su segmento), RACK y el temporizador desde el envío más viejo, el reensamblado del receptor (hasta 256 rangos; el ACK lleva primero el rango más nuevo y hasta 8), el FIN como un byte. Nunca se rinde.
- **`pepcc.h`** (puro): el ritmo de todos los flujos hacia un par: lo que el camino consume (entregado, y perdido mientras la cola es corta, hasta 3 perdidos por 1 entregado), arranque 3/2 que termina con la primera muestra con cola, ciclo 5/4–3/4–1, 3/4 mientras una ventana de ida y vuelta muestre cola sobre `pep_queue_ms`, y en vuelo a lo más dos veces `bw × (min_rtt + pep_queue_ms)`. De la [[Capacidad (capacity.h)]]: el arranque (si la hay) y el techo cuando el motor halla lleno el enlace de los datos (`CG_CAP_CUT_FULL`; un recorte por pérdida no).
- **`pep.c`**: los sockets (no bloqueantes; `TCP_NOTSENT_LOWAT` 128 KB; `SO_RCVBUF` chico en el socket que escucha), la contrapresión, los tiempos (`pep_dead_s`, 30 s para un `connect`), el estado (`pep` en el JSON).

## Lee
- Del túnel: `down_deliver` (router) y `up_deliver` (VPS) le pasan cada mensaje al instante; el [[Reordenador (reorder.h)]] solo cuenta su secuencia (`wgtype` no es dato: nunca lo retiene).
- Del motor, cada tick (`pep_hint`): la capacidad de los enlaces que llevan los datos ([[Lo que sobra (spare.h)]]: el primero; repartido, la suma) y si están llenos.

## Decide / escribe
- Mensajes al túnel por el camino de siempre (`up_one`/`send_links` en el router, `down_prepare`/`down_send` en el VPS); un segmento reenviado por segunda vez va copiado por todos los enlaces (`pcopy`).
- Contrapresión: el VPS cierra su borde cuando lo que tiene en orden sin escribir pasa `pep_backlog_ms` a su ritmo; el router deja de leer la LAN cuando lo que tiene sin enviar pasa eso o el búfer está lleno.
- Estado: `pep` (flujos, abiertos, rehusados por motivo, RST, reenvíos, copias, duplicados, memoria y su pico, y por par el ritmo, `min_rtt`, `srtt`, en vuelo, ventana, modo, ventana de RACK).

## Perillas
- [[Mapa/Perillas/pep|pep]], [[Mapa/Perillas/pep_port|pep_port]], [[Mapa/Perillas/pep_buf_kb|pep_buf_kb]], [[Mapa/Perillas/pep_mem_mb|pep_mem_mb]], [[Mapa/Perillas/pep_max_flows|pep_max_flows]], [[Mapa/Perillas/pep_queue_ms|pep_queue_ms]], [[Mapa/Perillas/pep_backlog_ms|pep_backlog_ms]], [[Mapa/Perillas/pep_dead_s|pep_dead_s]], [[Mapa/Perillas/pep_reo_ms|pep_reo_ms]], [[Mapa/Perillas/pep_cc|pep_cc]]. Todas reinician el proceso al cambiar.

## Medido
- [[025-protocontrolador]]: el laboratorio de la 024 (`protect`, `beyond`, `protectcut`, vivo) contra el túnel transparente y el PEP A.
