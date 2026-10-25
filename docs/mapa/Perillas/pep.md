---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep`

PEP modo B (solo con `tunnel = native`): en el router, el motor toma el TCP que el firewall redirige a `pep_port` y lo lleva como flujo fiable propio; en el VPS, abre al destino el TCP que pidan los routers. Con `no`, un mensaje de flujo que llega se cuenta y se descarta.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes` o `no`
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
