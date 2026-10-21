---
tags: [mapa, pieza]
archivos:
  - engine/src/clientpath.h
  - engine/tests/test_client_rx.c
sello: 54997df1fc332646
---
# Camino del router (clientpath.h)

**Grupo:** Recibir · Enviar — [[Mapa del motor]]

## Qué hace
Las decisiones por paquete del router, iguales en todos los modos de `link_threads`. `cg_rx_entry` juzga cada datagrama recibido, en este orden: (1) entrada de un enlace cerrado o de un socket viejo: fuera; (2) truncado o malformado: fuera; (3) otra sesión: fuera; (4) un HELLO solo vale si responde a una sonda propia aún sin respuesta ([[Eco de sondas (epoch.h)]]); (5) la ventana anti-replay antes del MAC; (6) el MAC, solo para lo nuevo; (7) marcar la ventana; (8) una respuesta de sonda vuelve al llamador (salud) y un DATA es la primera copia. `cg_up_header` escribe la cabecera de subida: secuencia y MAC una vez, el byte del enlace se parcha por enlace.

## Lee
- Cada datagrama de los enlaces; la sesión; [[Anti-replay (replay.h)]].

## Decide / escribe
- El veredicto → `client.c`: respuestas a [[Salud y silenciado (health.h)]], DATA a [[Llegadas (arrival.h)]] y al [[Reordenador (reorder.h)]].

## Perillas
- [[Mapa/Perillas/link_threads|link_threads]] (quién lee; las decisiones no cambian).

## Dónde se midió
- [[011-hilos]]: todos los modos deciden con el mismo código (secuencia, ventana antes del MAC, marca solo tras el MAC, reinicio del servidor).

## Choques conocidos
- Un servidor reiniciado sigue 2^20 adelante; el reordenador lo toma como paquete lejano y se resincroniza (no hace falta reiniciar la ventana).

## Archivos
- `clientpath.h`; tests `engine/tests/test_client_rx.c`.
- `client.c`: `link_read` y `hub_slot` (llaman a `cg_rx_entry`), `wg_read`, `fec_send`, `send_nacks` y `on_nack` (`cg_up_header`).
