---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_pct_min`

FEC v3: la parte de las reparaciones cuando no hay pérdida que la pida (`cg_f3_pct`). Lo que la pide, en muestras **sin cola** (la más alta de la muestra y de la anterior, 10 ms sobre el mínimo del minuto): una **ráfaga** (5 % o más en la muestra: una detención, un corte) da `fec_pct` entero por 30 s; la pérdida **al azar**, además **bajo la capacidad del enlace**, esto más 25 veces la pérdida (pesada por los bytes de cada enlace) hasta `fec_pct`, por 120 s. Los primeros 10 s, `fec_pct` entero (nada se sabe aún). La pérdida de un enlace con cola o lleno (TCP que desborda; BBR hace 15–70 ms de cola) no cuenta: las reparaciones sobre ella pedían más reparaciones. Igual a `fec_pct` o más: la parte fija.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0
- **Rango:** 0–50
- **Pieza:** [[FEC v3 (fec3.h)]], [[Observaciones del enlace (linksig.h)]]
- **Historia:** [[026-fec-v3]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
