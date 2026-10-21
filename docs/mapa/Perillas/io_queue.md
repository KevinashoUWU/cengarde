---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `io_queue`

Entradas de cada anillo hub → bomba.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** 256
- **Rango:** 64–1024, potencia de dos
- **Pieza:** [[Hilos por enlace (thrplan.h)]]
- **Historia:** [[011-hilos]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
