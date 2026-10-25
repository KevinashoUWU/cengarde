---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_dead_s`

Un flujo sin avance este tiempo, con algo pendiente (el túnel caído), se cierra con RST en los dos lados; 0: nunca.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 60
- **Rango:** 0–3600
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
