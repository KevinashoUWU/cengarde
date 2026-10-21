---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rescue_target_ms`

Cola donde empieza a bajar el peso del rescate (cero a 4 veces). Auditoría C12.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 20 (el de `governor_target_ms`; 0 lo sigue)
- **Rango:** 0–1000
- **Pieza:** [[Rescate (rescue.h)]], [[Capacidad (capacity.h)]]
- **Historia:** [[audit-pending]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
