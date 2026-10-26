---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_pct`

FEC v3: las reparaciones, % de los bytes de DATA, haya o no pérdida. Un enlace de parte s detenido T se rehace unos s·T / (r·(1 − s)) después de empezar. Medido con cuatro enlaces: 20 % rinde en la pérdida al azar y en detenciones de 50 ms, 33 % en las de 100–200 ms; con BBR se paga la parte entera.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 20
- **Rango:** 1–50
- **Pieza:** [[FEC v3 (fec3.h)]]
- **Historia:** [[026-fec-v3]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
