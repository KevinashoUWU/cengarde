---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_mem_mb`

Memoria de todas las conexiones juntas (trozos de 16 KB). Cada conexión toma como mucho su parte pareja; lleno, lo que llega en orden cabe olvidando lo que está fuera de orden; sin trozos libres no se aceptan conexiones nuevas ni se lee más.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (un octavo de la RAM, 64–1024)
- **Rango:** 0–4096
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]], [[029-mpgrit]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
