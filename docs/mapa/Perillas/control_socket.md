---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `control_socket`

El socket Unix de `cengarde ctl` (`ctl.h`).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** vacía (sin socket)
- **Rango:** ruta absoluta
- **Pieza:** [[Configuración (config.c)]]
- **Historia:** [[009-recarga-control-e-ip-pass]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
