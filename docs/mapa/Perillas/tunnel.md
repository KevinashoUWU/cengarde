---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tunnel`

Qué lleva el túnel: paquetes IP de una TUN propia (autenticados, sin cifrar), el único modo desde 0.12.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `native`
- **Rango:** `native` (`wireguard` es un error desde 0.12)
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]], [[Liberación TCP (tcpfree.h)]]
- **Historia:** [[014-tunel-nativo]], [[027-adios-wireguard]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
