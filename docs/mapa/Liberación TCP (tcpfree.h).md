---
tags: [mapa, pieza]
archivos:
  - engine/src/tcpfree.h
  - engine/tests/test_tcpfree.c
sello: b0793e22bf93f79f
---
# Liberación TCP (tcpfree.h)

**Grupo:** Enviar · Recibir — [[Mapa del motor]]

## Qué hace
El emisor ve el TCP de adentro (el túnel lleva paquetes IP). Marca `CG_F_FREE` los segmentos sin datos (ACK puros, SYN, FIN, RST), que el reordenador del otro lado entrega al instante, y las retransmisiones. Detecta una retransmisión por flujo: guarda el final de la secuencia más alta enviada; un segmento que termina antes o ahí es reenvío. La tabla es de acceso directo (`CG_TCPF_N`, 4096 flujos), sin memoria por paquete.

Desde [[023-reparacion-coherente]] distingue la primera retransmisión de unos bytes (`CG_TCPF_RETX1`) de una repetida (`CG_TCPF_RETXN`), y en el extremo donde TCP sale del túnel (`struct cg_tcpe`) mide el tramo lejano (del SYN al SYN-ACK) y cuenta los huecos SACK del host lejano: los que el túnel había entregado son pérdidas más allá del túnel (`beyond`), los demás suyos.

Desde el 2026-10-24 ese extremo juzga cada ACK con SACK y lo marca para el otro (`CG_F_TUNHOLE`, [[Retransmisión según la evidencia (tcpack.h)]]): guarda por flujo hasta 8 huecos propios (rangos de TCP que saltó al escribir), que una copia tardía o la retransmisión recortan, parten o borran, con la secuencia del túnel del segmento que los reveló y la pista `CG_F_PREV` de ese paquete; pasados esos, solo sabe el rango donde caen (ni perdido ni entregado: fundir dos huecos tomaba por faltante lo escrito entre ellos, y un salto de demora que abría decenas de huecos se leía como perdido en todos los enlaces). Veredicto (`cg_tcpe_verdict`): `THROUGH` (lo entregó: perdido más allá), `INFLIGHT` (suyo, pero algún enlace que podía traerlo no trajo todavía nada posterior: `struct cg_tcpe_view`, la secuencia más nueva de cualquier copia por enlace y los que trajeron algo en 2 s, de [[Llegadas (arrival.h)]]), `UNVOUCHED` (debajo del primer segmento visto, SACK por encima de lo escrito, flujo desconocido) u `OWN` (suyo y perdido en todos); solo `OWN` se marca.

## Lee
- El hash de 5-tupla de [[Flujos (flow.h)]] (el de la respuesta, invertido, para los ACK) y la cabecera TCP del paquete, con sus opciones SACK.
- En el extremo de salida: la secuencia del túnel y la pista `CG_F_PREV` de cada DATA escrito (`cg_ro_out.flags` del [[Reordenador (reorder.h)]]), y por enlace la secuencia más nueva de cualquier copia (`top` de [[Llegadas (arrival.h)]]) y cuándo trajo la última.

## Decide / escribe
- `CG_F_FREE` en la cabecera → [[Reordenador (reorder.h)]] del otro extremo, que distingue (`cg_tcp_data`) una retransmisión (con datos) de un ACK.
- Con `redund_spare`, la misma marca hace prioritario al paquete en lo que sobra de los otros enlaces ([[Lo que sobra (spare.h)]]).
- Una retransmisión va **en su lugar** de la secuencia, no adelantada (soltar los huecos retenidos antes, `reorder_retx_release_ms`, está en `docs/desvan/`).
- En redundancia, con `tcp_retx_one`, la primera retransmisión va por un enlace (`retx_masks`, `down_retx_masks`); la repetida, copiada.
- El RTT de los flujos → el presupuesto de TCP de [[Reparación coherente (harq.h)]]; `tcp_e2e` y `upload.tcp` en el estado.
- `CG_F_TUNHOLE` en el DATA de un ACK (`OWN`) → [[Retransmisión según la evidencia (tcpack.h)]] del otro extremo; `tcp_e2e`: `in_flight`, `unvouched`, `marked`.

## Perillas
- [[Mapa/Perillas/tcp_aware|tcp_aware]]: no.
- [[Mapa/Perillas/tcp_retx_one|tcp_retx_one]]: sí. [[Mapa/Perillas/rescue_tcp_race|rescue_tcp_race]]: sí. [[Mapa/Perillas/tcp_ack_rule|tcp_ack_rule]]: sí.

## Dónde se midió
- [[014-tunel-nativo]]: una retransmisión entregada al instante se adelantaba a lo retenido y TCP lo reenviaba todo (135 veces las retransmisiones rápidas en la red de pruebas). Ahora ocupa su lugar y en el laboratorio `on` queda como `off`.
- `tcpfree.h`: TCP reparaba en ~30 ms y su retransmisión esperaba 500 ms (`reorder_max_wait_ms`).

## Choques conocidos
- El horizonte de liberación cuesta donde los huecos son paquetes tarde por un enlace lento, no perdidos: llegan después como duplicados ([[014-tunel-nativo]]).

## Archivos
- `tcpfree.h`; tests `engine/tests/test_tcpfree.c`.
- `client.c`: `tun_read` (`cg_tcpe_in`, la marca), `retx_masks`, `dout_add` (`cg_tcpe_out`). `server.c`: `down_prepare` (`cg_tcpe_in`, la marca), `down_retx_masks`, `q_add` (`cg_tcpe_out`).
