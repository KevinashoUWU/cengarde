---
tags: [mapa, pieza]
archivos:
  - engine/src/reorder.h
  - engine/tests/test_reorder.c
sello: b24bda4cec74363e
---
# Reordenador (reorder.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
Pone en orden de secuencia lo que llega antes de entregarlo a WireGuard o a la TUN. Retiene lo que llegó adelantado por un tiempo acotado; **nunca tira nada**: cada paquete sale una vez. Un hueco se abandona (1) si se fuerza (desborde, pool lleno), (2) nunca mientras no hay nada retenido, (3) por una retransmisión de TCP con `retx_release`, (4) por **RACK**: cada enlace de `rset` trajo un DATA posterior, o (5) en su **plazo**. El plazo sale del enlace más lento: su retardo más 4 desvíos (`CG_RO_DEV_K`) más el margen, acotado por `max_wait` (si hay reparto) o `dup_wait`.

## Lee
- Primeras copias verificadas del [[Anti-replay (replay.h)]]: secuencia, ts del emisor, flags (`CG_F_SPLIT`, `CG_F_PREV`, `CG_F_FREE`), enlace y hora de llegada.
- El retardo de cada enlace (`cg_ro_owd`) de las sondas verificadas ([[Sondas y cola de bonding (proto.h)]]).
- `rset`: los enlaces por los que el otro manda: los no silenciados (con `reorder_idle_ms`, sin los ociosos según [[Llegadas (arrival.h)]]), y de ellos solo los activos según la máscara del otro extremo y lo que llegó ([[Enlaces activos (active.h)]], `reorder_active`). `wide`, el mismo sin la máscara, sirve para contar.

## Decide / escribe
- La entrega en orden (`down_deliver` / `up_deliver`).
- Huecos a punto de abandonarse → [[Rescate (rescue.h)]] (NACK), por [[Reparación coherente (harq.h)]]; [[FEC (fec.h)]] reconstruye antes.
- Un hueco que la pista `CG_F_PREV` dio por perdido en todos los enlaces se pide al instante y retiene lo que sigue aunque el tope sea 0 (redundancia); un hueco pedido espera `rescue_us` desde el pedido, acotado por el tope **más** `rescue_us` (`cg_ro_ask_left`), no por el tope solo ([[023-reparacion-coherente]]).
- Lo rehecho (`rebuilt`) que llena un hueco pedido cuenta en `rescue.repaired`, no en `rescued`.
- Estadísticas `in_order`, `held_total`, `lost.*`, `late.*` → estado; `due_narrow` (huecos que vencieron por RACK o plazo y que con `wide` habrían esperado: lo que soltó antes la máscara) y `due_capped` (huecos que vencieron en el tope mismo) → `active.early` y `active.at_cap`.
- `cg_ro_link_until`: hasta cuándo puede llegar lo que un enlace llevó en un instante (su retardo, cuatro desvíos, su margen adaptativo y el margen), el horizonte de un enlace cerrado.
- `struct cg_ro_out` lleva también las banderas de la cabecera (`flags`): el extremo de salida lee la pista `CG_F_PREV` de lo que escribe ([[Liberación TCP (tcpfree.h)]]).

## Perillas
- [[Mapa/Perillas/reorder|reorder]]: sí. [[Mapa/Perillas/reorder_max_wait_ms|reorder_max_wait_ms]]: 200. [[Mapa/Perillas/reorder_dup_wait_ms|reorder_dup_wait_ms]]: 0.
- [[Mapa/Perillas/reorder_margin_ms|reorder_margin_ms]]: 10. [[Mapa/Perillas/reorder_bypass|reorder_bypass]]: 128. [[Mapa/Perillas/reorder_pool_kb|reorder_pool_kb]]: 8192.
- [[Mapa/Perillas/reorder_by_link|reorder_by_link]], [[Mapa/Perillas/reorder_adaptive|reorder_adaptive]]: no. [[Mapa/Perillas/reorder_idle_ms|reorder_idle_ms]], [[Mapa/Perillas/reorder_retx_release_ms|reorder_retx_release_ms]]: 0. [[Mapa/Perillas/reorder_active|reorder_active]]: sí.
- [[Mapa/Perillas/rescue_wait_ms|rescue_wait_ms]], [[Mapa/Perillas/rescue_tries|rescue_tries]], [[Mapa/Perillas/tcp_aware|tcp_aware]].

## Dónde se midió
- [[012-bonding]]: en redundancia no cambia nada medible; esperar 20 ms en duplicado empeora QUIC (por eso `dup_wait` 0).
- [[013-capacidad-y-dosificacion]]: se rendía antes de tiempo: daba por perdido el 4–8 % de lo repartido y todo llegaba (60 % menos de 10 ms tarde); con margen 50 la subida pasó de 16–17 a 33–48 Mbit/s. `reorder_by_link` y el adaptativo no mejoraron.
- [[013-capacidad-y-dosificacion]] (2026-10-26, C2): con un enlace ocioso (`idleslow`), los huecos que iban al tope bajan de 165 a 8 por corrida (bajada) y de 242 a 30 (subida); cordura, n = 3.

## Choques conocidos
- Antes de [[023-reparacion-coherente]], en redundancia no retenía nada y un hueco pedido al vencer su tope no esperaba nada: el rescate casi no actuaba.
- C2 (resuelto el 2026-10-26): `rset` incluía todo enlace no silenciado; ni G4 ni el ETA viajaban al receptor, y RACK esperaba a enlaces ociosos: cada hueco iba al plazo. Ahora la máscara de [[Enlaces activos (active.h)]]. Sacar de `rset` los enlaces sin retardo (probado el 2026-10-18) rompía la garantía de `sc_rack`: un enlace cerrado sin retardo se queda.
- [[Liberación TCP (tcpfree.h)]]: una retransmisión adelantada hacía reenviar todo; ahora va en su lugar.
- El comentario dice ventana N = 4096 bajo 8128; el código tiene 16384 y 32768 ([[Mapa del motor#Contradicciones]]).

## Archivos
- `reorder.h`; tests `engine/tests/test_reorder.c`. Config: `cg_ro_cfg_of` en `engine.h`.
- `client.c`: `down_deliver`, `ro_drain`, `ro_service`, `ro_timeout`, `send_nacks`. `server.c`: `up_deliver`, `up_drain`, `ro_service`, `ro_rset`.
