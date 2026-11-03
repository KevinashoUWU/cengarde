---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_udp`

El resto del UDP de la LAN dentro de GRIT como datagramas no fiables (protocolo 20): un segundo socket TPROXY en el puerto siguiente a `protocontroller_port` (en OpenWrt, `udp_grit`), un flujo por origen y destino, cada datagrama entero en un mensaje DGM, sin reenvío ni orden, a lo más 300 ms en su cola, por turnos por equipo con el TCP (`fair.h`); el ritmo cuenta la parte que no miden los ACK (`cc.xs`). Laboratorio: con 30–60 Mbit/s de UDP de un equipo, el TCP de otro pasa de 3–9 a 25 Mbit/s en `cell4` (ping p99 de 0,8–1 s a 0,2 s) y de 42 a 51 en `equal` y `het`, con el mismo total.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** `no`
- **Rango:** `yes` o `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[031-clases-por-flujo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
