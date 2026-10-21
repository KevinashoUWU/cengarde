---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `server`

Lista ordenada de direcciones del servidor; un nombre lo resuelve el cliente por los enlaces.

- **Sección:** global, `[link NOMBRE]`
- **Extremo:** cliente
- **Por omisión:** (obligatoria, global o por enlace)
- **Rango:** hasta 8 entradas: IPv4, `[IPv6]` o nombres
- **Pieza:** [[Elección de servidor (srvpick.h)]], [[Nombres del servidor (resolve.h)]]
- **Historia:** [[010-ipv6-varias-ip-multicliente-nombres]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
