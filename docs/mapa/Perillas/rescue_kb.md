---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rescue_kb`

Anillo de lo enviado hace poco, para los NACK (servidor: bajada; cliente: subida).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (apagado)
- **Rango:** 0–262144
- **Pieza:** [[Rescate (rescue.h)]]
- **Historia:** [[013-capacidad-y-dosificacion]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
