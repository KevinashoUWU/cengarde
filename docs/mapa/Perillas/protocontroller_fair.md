---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_fair`

Los envíos del protocontrolador por equipo antes que por flujo (`fair.h`): cada vuelta da un segmento por dispositivo de la LAN y sus flujos se turnan; un equipo con muchas conexiones (un gestor de descargas, una actualización) se lleva la parte de uno. En el VPS el equipo viene en el OPEN (protocolo 19). Laboratorio: el equipo de una conexión al lado de otro de ocho pasa de 13–18 % a 31–41 % en `equal` y `het`, sin bajar el total.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[031-clases-por-flujo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
