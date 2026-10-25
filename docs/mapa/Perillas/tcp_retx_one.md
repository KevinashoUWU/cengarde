---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tcp_retx_one`

Con `tcp_aware`, en redundancia la primera retransmisión de un segmento va por un solo enlace (el más rápido), no copiada; una segunda del mismo, copiada.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Liberación TCP (tcpfree.h)]], [[Reparación coherente (harq.h)]]
- **Historia:** [[023-reparacion-coherente]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
