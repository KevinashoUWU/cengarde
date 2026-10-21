---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tun_addr`

Direcciones que cengarde pone en la TUN.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** ninguna (las pone el sistema)
- **Rango:** hasta 4 prefijos
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]]
- **Historia:** [[014-tunel-nativo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
