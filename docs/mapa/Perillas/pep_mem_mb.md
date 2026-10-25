---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_mem_mb`

Memoria de todos los flujos juntos (trozos de 16 KB). Sin trozos libres, no se aceptan flujos nuevos ni se lee más.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 64
- **Rango:** 4–4096
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
