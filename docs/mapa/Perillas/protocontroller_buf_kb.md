---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_buf_kb`

Búfer de cada conexión por sentido, como tope: lo leído del socket y no confirmado (al enviar), lo llegado y no escrito (al recibir: su ventana); en la variante A, lo leído de un lado y no escrito en el otro.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 1024
- **Rango:** 64–4080
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
