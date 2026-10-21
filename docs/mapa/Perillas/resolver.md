---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `resolver`

Servidores DNS para los nombres de `server`; los del enlace van primero.

- **Sección:** global, `[link NOMBRE]`
- **Extremo:** cliente
- **Por omisión:** global: `1.1.1.1 9.9.9.9 [2606:4700:4700::1111] [2620:fe::fe]`; enlace: ninguno
- **Rango:** hasta 8 direcciones
- **Pieza:** [[Nombres del servidor (resolve.h)]]
- **Historia:** [[010-ipv6-varias-ip-multicliente-nombres]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
