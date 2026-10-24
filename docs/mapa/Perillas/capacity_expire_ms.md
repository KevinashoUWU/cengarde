---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `capacity_expire_ms`

EXPERIMENTO: un enlace fuera del envío (silenciado, mudo o fuera del reparto) tanto tiempo sin medirse ve su capacidad a la mitad, y otra vez cada tanto, hasta 1/16 de su mejor.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (apagado)
- **Rango:** 0–600000
- **Pieza:** [[Capacidad (capacity.h)]]
- **Historia:** [[016-celda-virtual]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
