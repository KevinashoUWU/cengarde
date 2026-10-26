---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_single`

FEC v3 con un solo enlace: `auto` repara mientras pierde al azar (sin cola, como Starlink) y no con pérdida por congestión ni en un corte (pérdida sobre la mitad de la tasa); `yes` siempre, `no` nunca.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** `auto`
- **Rango:** `auto`, `yes` o `no`
- **Pieza:** [[FEC v3 (fec3.h)]], [[Observaciones del enlace (linksig.h)]]
- **Historia:** [[026-fec-v3]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
