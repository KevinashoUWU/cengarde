---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `listen`

Dónde escucha el servidor a los enlaces. En el cliente es un error desde 0.12 (era la dirección para el WireGuard local).

- **Sección:** global
- **Extremo:** servidor
- **Por omisión:** `*:59402`
- **Rango:** una dirección
- **Pieza:** [[Configuración (config.c)]]
- **Historia:** [[027-adios-wireguard]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
