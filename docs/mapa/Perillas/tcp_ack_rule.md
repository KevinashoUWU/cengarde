---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tcp_ack_rule`

Con `tcp_retx_one`, los enlaces de una retransmisión según lo que se sabe de su original: ya ACK o SACK, por un enlace (también repetida); perdida por el túnel en todos los enlaces, copiada (también la primera); entregada por el túnel, en camino o nada sabido, como `tcp_retx_one`. Lo dicen los ACK que vuelven y la marca `CG_F_TUNHOLE` del otro extremo.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Retransmisión según la evidencia (tcpack.h)]], [[Liberación TCP (tcpfree.h)]]
- **Historia:** [[023-reparacion-coherente]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
