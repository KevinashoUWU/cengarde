---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `reliable_zstd`

El carril fiable comprimido con zstd: cada datagrama sobre la historia del flujo (`zlane.h`), sin diccionario que acordar porque el carril es ordenado y fiable. Solo si el motor se compiló con zstd (y el VPS también: si no, rechaza el flujo). Telemetría JSON en el laboratorio: 5 veces más chica.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[031-clases-por-flujo]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
