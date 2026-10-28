---
tags: [mapa, pieza]
archivos:
  - engine/src/subflow.h
  - engine/tests/test_subflow.c
sello: 9f434135384c4895
---
# Subflujo (subflow.h)

La primera pieza de MPGRIT ([[029-mpgrit]], paso U1): cada enlace como un
subflujo con su propia secuencia (`sseq`), del lado que recibe. Pura, sin E/S;
todavía no está conectada al motor (falta el `sseq` en el encabezado, que
sube el protocolo).

- **Pérdida solo dentro del enlace:** un hueco de un enlace se da por perdido
  cuando llegó por ese mismo enlace un `sseq` posterior hace más que su
  ventana (`reo_us`, RACK de un camino). El desorden entre enlaces, que en
  terreno se leía como pérdida en el túnel y en GRIT ([[028-grit-y-el-motor-como-uno]]
  «Terreno»), ya no cuenta.
- **La ventana aprende:** un hueco que llega después de darse por perdido fue
  un veredicto falso (`late`); la ventana sube 4 ms (como un DSACK agranda la
  de TCP) hasta su tope (`cap_us`, el presupuesto de latencia de 300 ms) y
  baja 0,2 ms por tic de 100 ms hasta su piso (`cg_sf_rx_decay`).
- **Ventana de 1024 `sseq`** por enlace; un salto más allá da por perdido lo
  que deja atrás (`overflow`); la secuencia da la vuelta en 32 bits.
- Funciones: `cg_sf_rx_init`, `cg_sf_rx_in` (`NEW`, `FILL`, `LATE`, `DUP`,
  `OLD`), `cg_sf_rx_poll` (los huecos perdidos, del más viejo al más nuevo),
  `cg_sf_rx_decay`; contadores `got`, `lost`, `late`, `fill`, `dup`,
  `overflow`.
- Lo que falta (historia 029): el `sseq` en el encabezado (protocolo 12), el
  lado que envía (qué llevó cada `sseq`, para reenviar esos datos por
  cualquier enlace y que el ritmo de ese enlace sepa de su pérdida), y que el
  flujo de GRIT tome esas pérdidas en vez de su RACK sobre la mezcla y los
  NACK del túnel.
