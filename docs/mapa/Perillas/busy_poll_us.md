---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `busy_poll_us`

Tras tráfico, sondea sin dormir este tiempo. `thrplan.h` lo limita si los hilos no caben en las CPU.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0
- **Rango:** 0–1000000
- **Pieza:** [[Hilos por enlace (thrplan.h)]]
- **Historia:** [[006-salud-de-enlaces]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
