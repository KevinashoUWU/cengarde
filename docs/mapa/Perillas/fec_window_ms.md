---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_window_ms`

FEC v3: hasta dónde alcanza una reparación (una de cada dos cubre su último cuarto); como mucho `CG_RLCW_SPAN` (1024) secuencias, y lo útil queda dentro de lo que el receptor espera un hueco (su plazo más `rescue_code_ms`).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 300
- **Rango:** 10–1000
- **Pieza:** [[FEC v3 (fec3.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[026-fec-v3]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
