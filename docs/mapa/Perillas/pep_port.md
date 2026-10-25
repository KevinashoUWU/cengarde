---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_port`

Puerto donde escucha el router (IPv4 e IPv6) lo que el firewall redirige (`REDIRECT`).

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** 7801
- **Rango:** 1–65535
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
