---
tags: [mapa, pieza]
archivos:
  - engine/src/tcpack.h
  - engine/tests/test_tcpack.c
sello: 445db96495336361
---
# Retransmisión según la evidencia (tcpack.h)

**Grupo:** Enviar · Recuperar — [[Mapa del motor]]

## Qué hace
Con `tcp_aware` en redundancia decide por cuántos enlaces va cada retransmisión del TCP de adentro, por lo que se sabe de su original, no por una conjetura ([[023-reparacion-coherente]], 2026-10-24):

| Lo que se sabe del original | Fila | La retransmisión va |
|---|---|---|
| un ACK o un SACK que lo cubre ya salió del túnel | `spurious` | por un enlace |
| el túnel lo perdió en todos los enlaces que podían traerlo | `tunnel` | copiada por todos los que la llevarían |
| el host lejano lo echa de menos, pero el túnel lo entregó (perdido más allá), todavía viene por algún enlace o el túnel no sabe | `delivered` | como `tcp_retx_one`: la primera por un enlace, la repetida copiada |
| nada todavía | `unknown` | como `tcp_retx_one` |

Un bit no distingue «entregado» de «en camino»: la repetida de algo que venía por un enlace lento volvía a ir por el rápido con pérdida (en `protectcut`, 12,3 → 10,8 Mbit/s), así que `delivered` copia la repetida como `tcp_retx_one`; las pérdidas más allá del túnel casi no se repiten.

«Sin ACK todavía» no es «perdido»: un ACK vuelve una ida y vuelta después del dato, así que una retransmisión de la que nada se sabe queda en la última fila. Una espuria nunca se descarta: su llegada da el D-SACK con el que el emisor deshace su recorte y RACK agranda su ventana de reordenamiento.

## Lee
- Los ACK de cada flujo cuando salen del túnel en el extremo que envía sus datos (`down_deliver` del router para la subida, `up_deliver` del VPS para la bajada): el ACK acumulado más alto, hasta 4 bloques SACK (fusionados), y el marcador del ACK más nuevo con SACK, `[su ACK, su borde SACK más alto)`, con la marca `CG_F_TUNHOLE` de su DATA.
- La marca la escribe el otro extremo, por donde TCP sale del túnel ([[Liberación TCP (tcpfree.h)]], `cg_tcpe_in`), solo cuando el hueco es suyo y perdido en todos los enlaces (todo enlace que podía traerlo trajo una secuencia posterior: el que nombra `CG_F_PREV` si falta exactamente una secuencia, si no todos los que trajeron alguna copia en 2 s, [[Llegadas (arrival.h)]]). Un hueco suyo que todavía puede llegar (`in_flight`) o uno por el que no puede responder (`unvouched`) no se marca: la fila es `delivered`.
- Que el par marca: el bit 23 de la cola de las sondas (`CG_BOND_TCPMARK`, [[Sondas y cola de bonding (proto.h)]]); sin él la marca no significa nada y la fila es `unknown`.
- La clase de la retransmisión (`CG_TCPF_RETX1`, `CG_TCPF_RETXN`) de [[Liberación TCP (tcpfree.h)]].

## Decide / escribe
- `cg_tcpa_copy(fila, clase)` → `retx_masks` (router) y `down_retx_masks` (VPS): copiada por los enlaces que la llevarían (las copias que dejan [[Copias a tiempo (eta.h)]]) o por el más rápido.
- Estado: `upload.tcp.rule` (router) y `tcp.rule` por sesión (VPS): `peer_marks`, las retransmisiones por fila y `copied` de cada una; `acks`, `acks_sack`, `acks_marked` en el router y en `tcp_rule` global del VPS.

## Perillas
- [[Mapa/Perillas/tcp_ack_rule|tcp_ack_rule]]: sí (con `tcp_retx_one`).
- [[Mapa/Perillas/tcp_retx_one|tcp_retx_one]]: sí; con `no`, todo copiado.
- [[Mapa/Perillas/tcp_aware|tcp_aware]]: tiene que estar, con `tunnel = native`.

## Dónde se midió
- [[023-reparacion-coherente]] (2026-10-24, laboratorio de bonding, túnel nativo, n = 8): caudal de TCP igual a la conjetura y a todo copiado; copia las retransmisiones de lo que el túnel perdió en todos los enlaces y no las de lo que perdió más allá ni las espurias.
- Las primeras versiones marcaban también los huecos que todavía venían o por los que no podía responder (con 4 huecos por flujo, olvidados en cada salto): en el caso `spurious` copiaban retransmisiones de lo que un enlace detenido solo atrasaba, y TCP retransmitía el doble.

## Choques conocidos
- En la subida la fila `spurious` casi no actúa: el ACK pasa por el router un instante antes de llegar al host, que retransmite sin haberlo visto. En la bajada actúa cuanto más lejos está el host del VPS.
- Aproximado: una entrada por cubo de la tabla de flujos, el marcador del ACK más nuevo con SACK para todos sus huecos.

## Archivos
- `tcpack.h`; tests `engine/tests/test_tcpack.c`.
- `client.c`: `down_deliver` (`cg_tcpa_ack`), `wg_read` (`cg_tcpa_retx`, `tcpe_view`), `retx_masks`. `server.c`: `up_deliver`, `down_prepare` (`tcpe_view`), `down_retx_masks`.
