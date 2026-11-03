---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_burst_kb`

El crédito de ráfaga del ritmo de GRIT (`streamcc.h`): lo que rebalsa el balde lleno mientras el flujo pide menos que el ritmo y el camino no muestra cola se guarda, hasta esto y 100 ms al ritmo, y se gasta de una vez cuando llega una ráfaga (el keyframe de un video cada 2 s). Una pérdida, una cola o el flujo usando el ritmo lo vacían. Laboratorio, FLV a 8 Mbit/s tipo RTMP: el keyframe llega 40–80 ms antes en la mediana (`cell4`, subida, 169 → 89 ms), por 3–15 ms más de ping p99 del resto; con otro equipo cargando no se activa.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `256`
- **Rango:** 0–4096 (KB; 0: no)
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[031-clases-por-flujo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
