---
tags: [mapa, pieza]
archivos:
  - engine/src/reorder.h
  - engine/tests/test_reorder.c
sello: bf852640aa1dba7f
---
# Reordenador (reorder.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
Pone en orden de secuencia lo que llega antes de entregarlo a WireGuard o a la TUN. Retiene lo que llegó adelantado por un tiempo acotado; **nunca tira nada**: cada paquete sale una vez. Un hueco se abandona (1) si se fuerza (desborde, pool lleno), (2) nunca mientras no hay nada retenido, (3) por una retransmisión de TCP con `retx_release`, (4) por **RACK**: cada enlace de `rset` trajo un DATA posterior, o (5) en su **plazo**. El plazo sale del enlace más lento: su retardo más 4 desvíos (`CG_RO_DEV_K`) más el margen, acotado por `max_wait` (si hay reparto) o `dup_wait`.

## Lee
- Primeras copias verificadas del [[Anti-replay (replay.h)]]: secuencia, ts del emisor, flags (`CG_F_SPLIT`, `CG_F_PREV`, `CG_F_FREE`), enlace y hora de llegada.
- El retardo de cada enlace (`cg_ro_owd`) de las sondas verificadas ([[Sondas y cola de bonding (proto.h)]]).
- `rset`: los enlaces por los que el otro manda (no silenciados; con `reorder_idle_ms`, sin los ociosos según [[Llegadas (arrival.h)]]).

## Decide / escribe
- La entrega en orden (`down_deliver` / `up_deliver`).
- Huecos a punto de abandonarse → [[Rescate (rescue.h)]] (NACK), por [[Reparación coherente (harq.h)]]; [[FEC (fec.h)]] reconstruye antes.
- Un hueco que la pista `CG_F_PREV` dio por perdido en todos los enlaces se pide al instante y retiene lo que sigue aunque el tope sea 0 (redundancia); un hueco pedido espera `rescue_us` desde el pedido, acotado por el tope **más** `rescue_us` (`cg_ro_ask_left`), no por el tope solo ([[023-reparacion-coherente]]).
- Lo rehecho (`rebuilt`) que llena un hueco pedido cuenta en `rescue.repaired`, no en `rescued`.
- Estadísticas `in_order`, `held_total`, `lost.*`, `late.*` → estado.

## Perillas
- [[Mapa/Perillas/reorder|reorder]]: sí. [[Mapa/Perillas/reorder_max_wait_ms|reorder_max_wait_ms]]: 200. [[Mapa/Perillas/reorder_dup_wait_ms|reorder_dup_wait_ms]]: 0.
- [[Mapa/Perillas/reorder_margin_ms|reorder_margin_ms]]: 10. [[Mapa/Perillas/reorder_bypass|reorder_bypass]]: 128. [[Mapa/Perillas/reorder_pool_kb|reorder_pool_kb]]: 8192.
- [[Mapa/Perillas/reorder_by_link|reorder_by_link]], [[Mapa/Perillas/reorder_adaptive|reorder_adaptive]]: no. [[Mapa/Perillas/reorder_idle_ms|reorder_idle_ms]], [[Mapa/Perillas/reorder_retx_release_ms|reorder_retx_release_ms]]: 0.
- [[Mapa/Perillas/rescue_wait_ms|rescue_wait_ms]], [[Mapa/Perillas/rescue_tries|rescue_tries]], [[Mapa/Perillas/tcp_aware|tcp_aware]].

## Dónde se midió
- [[012-bonding]]: en redundancia no cambia nada medible; esperar 20 ms en duplicado empeora QUIC (por eso `dup_wait` 0).
- [[013-capacidad-y-dosificacion]]: se rendía antes de tiempo: daba por perdido el 4–8 % de lo repartido y todo llegaba (60 % menos de 10 ms tarde); con margen 50 la subida pasó de 16–17 a 33–48 Mbit/s. `reorder_by_link` y el adaptativo no mejoraron.

## Choques conocidos
- Antes de [[023-reparacion-coherente]], en redundancia no retenía nada y un hueco pedido al vencer su tope no esperaba nada: el rescate casi no actuaba.
- C2: `rset` incluye todo enlace no silenciado; ni G4 ni el ETA viajan al receptor, y RACK espera a enlaces ociosos: cada hueco va al plazo. Abierto; necesita protocolo ([[audit-pending]]).
- [[Liberación TCP (tcpfree.h)]]: una retransmisión adelantada hacía reenviar todo; ahora va en su lugar.
- El comentario dice ventana N = 4096 bajo 8128; el código tiene 16384 y 32768 ([[Mapa del motor#Contradicciones]]).

## Archivos
- `reorder.h`; tests `engine/tests/test_reorder.c`. Config: `cg_ro_cfg_of` en `engine.h`.
- `client.c`: `down_deliver`, `ro_drain`, `ro_service`, `ro_timeout`, `send_nacks`. `server.c`: `up_deliver`, `up_drain`, `ro_service`, `ro_rset`.
