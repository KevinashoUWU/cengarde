---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `listen`

Dónde escucha: el cliente, a WireGuard local; el servidor, a los enlaces.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** cliente `127.0.0.1:59401`; servidor `*:59402`
- **Rango:** una dirección
- **Pieza:** [[Configuración (config.c)]]
- **Historia:** —
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
