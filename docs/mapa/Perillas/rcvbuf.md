---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `rcvbuf`

El búfer de recepción de los sockets. En el servidor, el presupuesto de recepción lo recorta según `udp_mem`.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 4194304 (4 MiB)
- **Rango:** 0–2^30 bytes
- **Pieza:** [[Presupuesto de recepción (rcvbudget.h)]]
- **Historia:** [[011-hilos]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
