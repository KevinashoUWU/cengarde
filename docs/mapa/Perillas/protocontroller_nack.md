---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_nack`

Variante B: el NACK del túnel de un mensaje de GRIT es una pérdida que se envía otra vez (`yes`) o solo una pista y decide el RACK de GRIT (`no`); en terreno lo que el túnel dio por perdido llegó tarde (historia 029). Con `subflow` no cuenta: el NACK solo es siempre una pista y decide la palabra del enlace.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes`, `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[029-mpgrit]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
