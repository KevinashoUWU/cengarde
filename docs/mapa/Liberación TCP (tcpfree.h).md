---
tags: [mapa, pieza]
archivos:
  - engine/src/tcpfree.h
  - engine/tests/test_tcpfree.c
sello: 2540879b7f291ef8
---
# Liberación TCP (tcpfree.h)

**Grupo:** Enviar · Recibir — [[Mapa del motor]]

## Qué hace
Con `tunnel = native` el emisor ve el TCP de adentro. Marca `CG_F_FREE` los segmentos sin datos (ACK puros, SYN, FIN, RST), que el reordenador del otro lado entrega al instante, y las retransmisiones. Detecta una retransmisión por flujo: guarda el final de la secuencia más alta enviada; un segmento que termina antes o ahí es reenvío. La tabla es de acceso directo (`CG_TCPF_N`, 4096 flujos), sin memoria por paquete.

Desde [[023-reparacion-coherente]] distingue la primera retransmisión de unos bytes (`CG_TCPF_RETX1`) de una repetida (`CG_TCPF_RETXN`), y en el extremo donde TCP sale del túnel (`struct cg_tcpe`) mide el tramo lejano (del SYN al SYN-ACK) y cuenta los huecos SACK del host lejano: los que el túnel había entregado son pérdidas más allá del túnel (`beyond`), los demás suyos.

## Lee
- El hash de 5-tupla de [[Flujos (flow.h)]] (el de la respuesta, invertido, para los ACK) y la cabecera TCP del paquete, con sus opciones SACK.

## Decide / escribe
- `CG_F_FREE` en la cabecera → [[Reordenador (reorder.h)]] del otro extremo, que distingue (`cg_tcp_data`) una retransmisión (con datos) de un ACK.
- Con `redund_spare`, la misma marca hace prioritario al paquete en lo que sobra de los otros enlaces ([[Lo que sobra (spare.h)]]).
- Una retransmisión va **en su lugar** de la secuencia, no adelantada; con `reorder_retx_release_ms` suelta los huecos retenidos antes.
- En redundancia, con `tcp_retx_one`, la primera retransmisión va por un enlace (`retx_masks`, `down_retx_masks`); la repetida, copiada.
- El RTT de los flujos → el presupuesto de TCP de [[Reparación coherente (harq.h)]]; `tcp_e2e` y `upload.tcp` en el estado.

## Perillas
- [[Mapa/Perillas/tcp_aware|tcp_aware]]: no.
- [[Mapa/Perillas/reorder_retx_release_ms|reorder_retx_release_ms]]: 0 (apagado); 0–2000.
- [[Mapa/Perillas/tunnel|tunnel]]: tiene que ser `native`.
- [[Mapa/Perillas/tcp_retx_one|tcp_retx_one]]: sí. [[Mapa/Perillas/rescue_tcp_race|rescue_tcp_race]]: sí.

## Dónde se midió
- [[014-tunel-nativo]]: una retransmisión entregada al instante se adelantaba a lo retenido y TCP lo reenviaba todo (135 veces las retransmisiones rápidas en la red de pruebas). Ahora ocupa su lugar y en el laboratorio `on` queda como `off`.
- `tcpfree.h`: TCP reparaba en ~30 ms y su retransmisión esperaba 500 ms (`reorder_max_wait_ms`).

## Choques conocidos
- El horizonte de liberación cuesta donde los huecos son paquetes tarde por un enlace lento, no perdidos: llegan después como duplicados ([[014-tunel-nativo]]).

## Archivos
- `tcpfree.h`; tests `engine/tests/test_tcpfree.c`.
- `client.c`: `wg_read`, `retx_masks`, `dout_add` (`cg_tcpe_out`). `server.c`: `down_prepare`, `down_retx_masks`, `q_add` (`cg_tcpe_out`), `tun_read` (`cg_tcpe_in`).
