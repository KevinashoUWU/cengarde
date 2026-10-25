---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller`

El protocontrolador (solo con `tunnel = native`): en el router, con `a` o `b`, el motor toma el TCP que el firewall redirige a `protocontroller_port` y lo lleva al VPS como un TCP simple por el túnel (`a`) o como flujo fiable propio (`b`); en el VPS, `a` o `b` da lo mismo: abre al destino el TCP que pidan los routers con cualquiera de las dos. Con `no`, un mensaje de flujo que llega se cuenta y se descarta.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `no`, `a` o `b`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
