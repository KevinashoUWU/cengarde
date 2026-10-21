---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `cpu`

Fija el proceso (global) o el hilo de un enlace (`[link]`, con `link_threads = on`) a una CPU.

- **Sección:** global, `[link NOMBRE]`
- **Extremo:** los dos
- **Por omisión:** -1 (cualquiera)
- **Rango:** 0–1023
- **Pieza:** [[Hilos por enlace (thrplan.h)]]
- **Historia:** [[006-salud-de-enlaces]], [[011-hilos]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
