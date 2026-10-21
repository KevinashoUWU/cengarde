---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_threads`

Cómo lee el cliente sus enlaces: el bucle de 0.4, un hilo por enlace o la estructura nueva en un hilo.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** `legacy`
- **Rango:** `auto`, `on`, `off`, `legacy`
- **Pieza:** [[Hilos por enlace (thrplan.h)]]
- **Historia:** [[011-hilos]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
