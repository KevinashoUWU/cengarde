---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rescue_code_ms`

Con un código protegiendo (sus reparaciones o paridades llegan), cuánto espera un hueco a que lo rehaga antes de pedir; se suma a `rescue_wait_ms`. En la práctica decide el sello: una reparación cuya base pasó el hueco.

- **Sección:** global
- **Extremo:** servidor
- **Por omisión:** 100
- **Rango:** 0–1000
- **Pieza:** [[Reparación coherente (harq.h)]], [[Rescate (rescue.h)]]
- **Historia:** [[023-reparacion-coherente]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
