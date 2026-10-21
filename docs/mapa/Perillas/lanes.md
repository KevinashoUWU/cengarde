---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `lanes`

Sockets de escucha guiados por número de enlace (cBPF); 1 es el socket de antes.

- **Sección:** global
- **Extremo:** servidor
- **Por omisión:** `auto` (8)
- **Rango:** `auto`, 1, 2, 4, 8, 16
- **Pieza:** [[Colas del servidor (steer.h)]], [[Dosificación (pace.h)]]
- **Historia:** [[011-hilos]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
