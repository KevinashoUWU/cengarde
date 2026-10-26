---
tags: [mapa, pieza]
archivos:
  - engine/src/active.h
  - engine/tests/test_active.c
sello: 9f854e048df6b563
---
# Enlaces activos (active.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
Le dice al [[Reordenador (reorder.h)]] del otro extremo por qué enlaces se están mandando datos, para que un hueco no espere a un enlace vivo pero ocioso (un respaldo, uno que el [[ETA (eta.h)]] o [[G4 guarda de dispersión (split.h)]] dejan fuera): antes, RACK no saltaba nunca y cada hueco iba al tope (`reorder_max_wait_ms`). Es la auditoría de relojería C2 ([[audit-pending]]), en el protocolo 11 sin versión nueva ([[013-capacidad-y-dosificacion]], 2026-10-26).
- **Emisor (`cg_act_tx`):** la última vez que cada enlace llevó algo que puede llenar un hueco: DATA que fue solo por él, un reenvío por NACK, una paridad, una reparación; de lo que va por varios (los paquetes chicos por todos los portadores, las retransmisiones de TCP, las copias), solo el que debería llegar primero según los retardos de ida del emisor, y los de retardo desconocido (`cg_act_tx_counts`). Cada sonda o respuesta lleva la máscara de los que llevaron algo dentro de la retención (200 ms, `CG_ACT_HOLD_MS`) antes de su ts, y la retención. Tras un arranque o una recarga, todos durante la retención.
- **Receptor (`cg_act_rx`):** la máscara de la sonda o respuesta más nueva (por secuencia de control). Un enlace está **abierto** (cuenta en `rset`) si no hay máscara en vigor (ninguna, o de hace más de 2 s), si la máscara lo tiene, o si una evidencia dice que llevó algo en el ts de la máscara o después; **cerrado**, sigue en `rset` hasta su horizonte: el tiempo del emisor desde el que no llevó nada (`quiet`: ts − retención, que se mantiene mientras las máscaras siguientes se toquen) más su retardo, cuatro desvíos, su margen adaptativo y `reorder_margin_ms` (`cg_ro_link_until`). Sin retardo medido, se queda (la garantía del reordenador).
- **Evidencia (`cg_act_rx_data`):** cada primera copia verificada de DATA abre su enlace y el que nombra su `CG_F_PREV`; cada paridad o reparación, el suyo. Así un enlace que el emisor empieza a usar cuenta desde el primer paquete que llega por él, o desde el siguiente que lo nombra por un enlace más rápido, antes que la máscara. Lo que abre se junta en `fresh` para el `rset` de un lote ya calculado.

## Lee
- La cola de la máscara (`cg_probe_act`, 4 bytes tras la cola de bonding) de [[Sondas y cola de bonding (proto.h)]].
- Los retardos y desvíos de cada enlace del [[Reordenador (reorder.h)]].
- Las primeras copias verificadas (DATA, paridades, reparaciones).

## Decide / escribe
- `rset` = enlaces no silenciados ∩ activos, en `ro_rset` del cliente (bajada) y del servidor (subida). `wide` (sin la máscara) va al reordenador para contar `due_narrow`.
- El tiempo hasta que un enlace cerrado sale (`cg_act_rx_next_us`) entra en el plazo de `epoll`.
- Estado: objeto `active` (en el cliente y en cada sesión): la máscara del otro, su retención y edad, `base`, `rset`, `masks`, `opened`, `early` (huecos soltados antes gracias a la máscara), `at_cap` (huecos que fueron al tope) y `sent` (la máscara propia).

## Perillas
- [[Mapa/Perillas/reorder_active|reorder_active]]: sí (el receptor; el emisor siempre la manda).
- [[Mapa/Perillas/reorder_margin_ms|reorder_margin_ms]]: entra en el horizonte.

## Dónde se midió
- [[013-capacidad-y-dosificacion]] (2026-10-26, cordura, n = 3): caso nuevo `idleslow` (un tercer enlace de 150 ms que el ETA no usa): los huecos que iban al tope bajan de 165 a 8 (bajada) y de 242 a 30 (subida) por corrida, y se van por RACK. Sin FEC, ningún tardío nuevo en ningún caso; con la FEC v3 sin rescate, más huecos se sueltan por RACK y se rehacen tarde.

## Choques conocidos
- La cadena de evidencia necesita que cada paquete vaya por un enlace, o con copias en uno ya contado: si se pierde justo el paquete que nombraría a un enlace recién activado, su hueco puede soltarse antes (llega tarde, como un retardo fuera de los desvíos).
- Las copias que no son la primera esperada no cuentan: si la primera se pierde, la otra llega tarde.
- Con la FEC v3 sin rescate, RACK soltaba ya los huecos antes de que llegara la reparación (luego rehechos tarde); el enlace ocioso lo escondía en `idleslow`. Ver [[FEC v3 (fec3.h)]].

## Archivos
- `active.h`; tests `engine/tests/test_active.c` (la regla, la garantía de `sc_rack` y flujos aleatorios donde ningún hueco se suelta antes de tiempo; las reglas más débiles sí lo hacen).
- `client.c`: `ro_rset`, `ro_act_next`, `down_deliver`, `d_rep_take`, `on_probe_reply`, `send_probe`, `send_links`. `server.c`: `ro_rset_of`, `ro_rset`, `on_probe`, `queue_reply`, `down_send`, `send_path`, `ro_timeout`. `status.c`: `cg_json_active`.
