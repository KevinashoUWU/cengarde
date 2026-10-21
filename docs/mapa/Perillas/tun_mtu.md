---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tun_mtu`

MTU de la TUN.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 1420
- **Rango:** 576–2024
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]]
- **Historia:** [[014-tunel-nativo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
