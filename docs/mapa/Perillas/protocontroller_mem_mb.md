---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_mem_mb`

Memoria de todas las conexiones juntas (trozos de 16 KB). Sin trozos libres, no se aceptan conexiones nuevas ni se lee más.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 64
- **Rango:** 4–4096
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
