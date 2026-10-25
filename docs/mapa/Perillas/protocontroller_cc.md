---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_cc`

Control de congestión del TCP del VPS hacia el destino; `auto`: el del kernel (cubic) y BBR en cuanto ese tramo pierde (sin el ProbeRTT de BBR para el vídeo en vivo); si el kernel no lo tiene, el suyo (un aviso).

- **Sección:** global
- **Extremo:** servidor
- **Por omisión:** `auto`
- **Rango:** `auto`, un control de congestión del kernel, o vacío
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
