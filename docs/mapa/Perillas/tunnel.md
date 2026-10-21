---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tunnel`

Qué lleva el túnel: datagramas de WireGuard o paquetes IP de una TUN propia (autenticados, sin cifrar).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `wireguard`
- **Rango:** `wireguard` o `native`
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]], [[Liberación TCP (tcpfree.h)]]
- **Historia:** [[014-tunel-nativo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
