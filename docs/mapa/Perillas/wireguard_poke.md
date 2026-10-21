---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `wireguard_poke`

Dirección del router en el túnel: un datagrama ahí hace que WireGuard inicie un handshake.

- **Sección:** global, `[client NOMBRE]`
- **Extremo:** servidor
- **Por omisión:** `10.79.0.2:9`
- **Rango:** una dirección o `none`
- **Pieza:** [[Reloj atrasado de WireGuard (wgwatch.h)]]
- **Historia:** [[002-wireguard-para-cengarde]], [[010-ipv6-varias-ip-multicliente-nombres]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
