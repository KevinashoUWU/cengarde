---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `tun_route`

Prefijos de cada cliente en la TUN del servidor; también comprueba el origen de lo que manda (como AllowedIPs).

- **Sección:** global, `[client NOMBRE]`
- **Extremo:** servidor
- **Por omisión:** un cliente sin ella: `0.0.0.0/0` y `::/0`
- **Rango:** hasta los prefijos de `CG_TUN_ROUTES`
- **Pieza:** [[Rutas del túnel nativo (tunroute.h)]]
- **Historia:** [[014-tunel-nativo]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
