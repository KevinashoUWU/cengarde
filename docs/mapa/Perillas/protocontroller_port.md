---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_port`

Puerto donde escucha el router (IPv4 e IPv6) lo que el firewall redirige (`REDIRECT`); en el VPS, el de la variante A en su dirección del túnel.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 7801
- **Rango:** 1–65535
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
