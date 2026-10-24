---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rescue_tcp_race`

Con TCP adentro (túnel nativo), ningún hueco se retiene ni se pide más allá del presupuesto de TCP (su RTT más un cuarto, RACK): lo que no llega antes queda para la retransmisión de TCP.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Reparación coherente (harq.h)]], [[Liberación TCP (tcpfree.h)]]
- **Historia:** [[023-reparacion-coherente]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
