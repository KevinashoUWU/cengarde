---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `server_failover_ms`

Sin respuesta verificada este tiempo, el enlace pasa a la siguiente dirección.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** el mayor de 10000 y 3 × `probe_idle_ms`
- **Rango:** 0 (nunca) o ≥ 3 × `probe_idle_ms`, hasta 3600000
- **Pieza:** [[Elección de servidor (srvpick.h)]]
- **Historia:** [[010-ipv6-varias-ip-multicliente-nombres]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
