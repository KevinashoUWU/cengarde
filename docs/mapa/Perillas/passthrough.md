---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `passthrough`

Cliente: pide IP pass al servidor. `[client NOMBRE]`: si ese router puede tenerlo.

- **Sección:** global, `[client NOMBRE]`
- **Extremo:** los dos
- **Por omisión:** cliente: sin pedir (-1); `[client]`: `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Tabla de reenvío (fwdtable.h)]]
- **Historia:** [[009-recarga-control-e-ip-pass]], [[010-ipv6-varias-ip-multicliente-nombres]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
