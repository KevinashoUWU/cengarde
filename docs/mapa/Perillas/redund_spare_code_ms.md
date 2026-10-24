---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `redund_spare_code_ms`

Con `redund_spare = rs` o `rlc`, cuánto abarca un bloque (o la ventana) de paquetes fuente: k son los paquetes de ese tiempo (de 4 a 64), y un bloque que no se llena cierra a ese plazo (la ventana manda una reparación).

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** 30
- **Rango:** 1–500
- **Pieza:** [[Códigos de borrado (rs.h, rlc.h)]], [[Lo que sobra (spare.h)]]
- **Historia:** [[021-fec-v2]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
