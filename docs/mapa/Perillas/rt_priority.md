---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rt_priority`

Prioridad `SCHED_FIFO` de los hilos de datos.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (normal)
- **Rango:** 0–99
- **Pieza:** [[Hilos por enlace (thrplan.h)]]
- **Historia:** [[006-salud-de-enlaces]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
