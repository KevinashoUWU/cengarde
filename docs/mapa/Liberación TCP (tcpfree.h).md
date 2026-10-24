---
tags: [mapa, pieza]
archivos:
  - engine/src/tcpfree.h
  - engine/tests/test_tcpfree.c
sello: e94ce31610831224
---
# Liberación TCP (tcpfree.h)

**Grupo:** Enviar · Recibir — [[Mapa del motor]]

## Qué hace
Con `tunnel = native` el emisor ve el TCP de adentro. Marca `CG_F_FREE` los segmentos sin datos (ACK puros, SYN, FIN, RST), que el reordenador del otro lado entrega al instante, y las retransmisiones. Detecta una retransmisión por flujo: guarda el final de la secuencia más alta enviada; un segmento que termina antes o ahí es reenvío. La tabla es de acceso directo (`CG_TCPF_N`, 4096 flujos), sin memoria por paquete.

## Lee
- El hash de 5-tupla de [[Flujos (flow.h)]] y la cabecera TCP del paquete.

## Decide / escribe
- `CG_F_FREE` en la cabecera → [[Reordenador (reorder.h)]] del otro extremo, que distingue (`cg_tcp_data`) una retransmisión (con datos) de un ACK.
- Con `redund_spare`, la misma marca hace prioritario al paquete en lo que sobra de los otros enlaces ([[Lo que sobra (spare.h)]]).
- Una retransmisión va **en su lugar** de la secuencia, no adelantada; con `reorder_retx_release_ms` suelta los huecos retenidos antes.

## Perillas
- [[Mapa/Perillas/tcp_aware|tcp_aware]]: no.
- [[Mapa/Perillas/reorder_retx_release_ms|reorder_retx_release_ms]]: 0 (apagado); 0–2000.
- [[Mapa/Perillas/tunnel|tunnel]]: tiene que ser `native`.

## Dónde se midió
- [[014-tunel-nativo]]: una retransmisión entregada al instante se adelantaba a lo retenido y TCP lo reenviaba todo (135 veces las retransmisiones rápidas en la red de pruebas). Ahora ocupa su lugar y en el laboratorio `on` queda como `off`.
- `tcpfree.h`: TCP reparaba en ~30 ms y su retransmisión esperaba 500 ms (`reorder_max_wait_ms`).

## Choques conocidos
- El horizonte de liberación cuesta donde los huecos son paquetes tarde por un enlace lento, no perdidos: llegan después como duplicados ([[014-tunel-nativo]]).

## Archivos
- `tcpfree.h`; tests `engine/tests/test_tcpfree.c`.
- `client.c`: `wg_read`. `server.c`: `down_prepare` (desde `tun_read`).
