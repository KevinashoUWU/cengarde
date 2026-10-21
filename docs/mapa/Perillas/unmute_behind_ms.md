---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `unmute_behind_ms`

Atraso bajo el cual un enlace silenciado vuelve (tras 2 × `mute_settle_ms`).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 4/5 de `mute_behind_ms` (120)
- **Rango:** 0–`mute_behind_ms`
- **Pieza:** [[Salud y silenciado (health.h)]]
- **Historia:** [[006-salud-de-enlaces]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
