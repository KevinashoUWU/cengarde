---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_mid_cc`

Variante A: el control de congestión del TCP del medio, en cada extremo para lo que envía (el router la subida, el VPS la bajada). Con cubic, la pérdida que el túnel no repara a tiempo lo frena (42 contra 54 Mbit/s en `protect`; 7 contra 50 de bajada); si el kernel no lo tiene, el suyo (un aviso).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `bbr`
- **Rango:** un control de congestión del kernel, o vacío (el del kernel)
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
