---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_vps`

Variante A: la dirección del VPS en el túnel, a la que el router abre el TCP del medio.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** vacío (la primera dirección del prefijo de `tun_addr`)
- **Rango:** una dirección IP
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
