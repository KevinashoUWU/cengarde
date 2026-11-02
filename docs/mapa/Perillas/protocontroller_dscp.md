---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_dscp`

DiffServ a través de GRIT (protocolo 21, `dscp.h`): el DSCP con que un equipo de la LAN marcó su flujo (el SYN de su TCP, su primer datagrama) va en el OPEN, ambos extremos marcan sus sockets con él (GRIT termina la conexión y antes borraba la marca) y elige el nivel del flujo en los envíos: EF, VOICE-ADMIT, CS5, AF4x, CS4, CS3, CS6 y CS7 primero mientras manden menos de un cuarto del ritmo; LE y CS1 solo con lo que deja el resto, más un segmento cada 250 ms. Laboratorio: un equipo con 8 conexiones en CS1 al lado de otro con una sin marca le deja el 99 % del total (antes 27–41 %); sin marcas, igual que apagada.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes` o `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[031-clases-por-flujo]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
