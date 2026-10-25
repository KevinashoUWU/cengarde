---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_buf_kb`

Búfer de cada flujo por sentido, como tope: lo leído del socket y no confirmado (al enviar), lo llegado y no escrito (al recibir: su ventana).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 1024
- **Rango:** 64–4080
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
