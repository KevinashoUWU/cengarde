---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tun_txqueuelen`

Cola de transmisión de la TUN.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (el del kernel, 500)
- **Rango:** 0–100000 paquetes
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]]
- **Historia:** [[audit-pending]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
