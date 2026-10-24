---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `capacity_stand`

EXPERIMENTO: 1, una cola permanente (sobre el mínimo de 60 s, sobre `capacity_sat_ms`, 4 muestras) frena el arranque rápido; 2, además el arranque rápido va antes de 10 s si el enlace entrega 9/8 de su capacidad sin pérdida ni cola, dos muestras.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (apagado)
- **Rango:** 0–2
- **Pieza:** [[Capacidad (capacity.h)]], [[Cola por enlace (cg_gov_base)]]
- **Historia:** [[019-perdida-con-cola]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
