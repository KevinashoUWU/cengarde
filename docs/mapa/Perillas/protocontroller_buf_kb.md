---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_buf_kb`

Búfer de cada conexión por sentido, como tope: lo leído del socket y no confirmado (al enviar), lo llegado y no escrito (al recibir: su ventana); en la variante A, lo leído de un lado y no escrito en el otro. Con el ritmo por enlace (`link_cc`) es el techo de la bajada: lo enviado queda retenido hasta su acuse (~150 ms con las colas de los enlaces), y en terreno 2 MB dieron 51–87 Mbit/s y 4 MB 105–119 (historia 029).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 1024 (con `subflow`, 4080)
- **Rango:** 64–8176
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]], [[029-mpgrit]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
