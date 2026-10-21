---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `exclude`

Interfaces que nunca llevan el túnel.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** vacía (más las de fábrica: `lo wg* docker* veth* br-* virbr* tun* tap* ifb* dummy*`)
- **Rango:** patrones
- **Pieza:** [[Configuración (config.c)]]
- **Historia:** [[009-recarga-control-e-ip-pass]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
