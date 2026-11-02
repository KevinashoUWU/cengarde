---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_stream`

FEC v3 también sobre los mensajes de GRIT (STREAM, `streamfec.h`), con su ventana propia y su CRC32C, y GRIT esperando el código antes de reenviar; cada extremo para lo que envía (el VPS, la bajada). Cuesta 4–20 % del caudal con los enlaces llenos (laboratorio 2026-11-02). En redundancia, con la protección de `redund_spare`, la subida las manda en lo que sobra a los otros enlaces y GRIT no las espera.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** no
- **Rango:** `yes` o `no`
- **Pieza:** [[FEC v3 (fec3.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[031-clases-por-flujo]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
