---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rescue_wait_ms`

Cuánto espera un hueco pedido. Con `fec`, al menos 2 × `fec_wait_ms` + 20; con un código protegiendo, más `rescue_code_ms`; con TCP adentro, nunca más que su presupuesto (`rescue_tcp_race`).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (no pide)
- **Rango:** 0–1000
- **Pieza:** [[Rescate (rescue.h)]], [[Reordenador (reorder.h)]], [[Reparación coherente (harq.h)]]
- **Historia:** [[013-capacidad-y-dosificacion]], [[023-reparacion-coherente]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
