---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_reo_ms`

La ventana de RACK sobre un cuarto de la ida y vuelta (la reparación del túnel va primero); un duplicado la agranda 4 ms (hasta 100) y vuelve sola.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 10
- **Rango:** 0–1000
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
