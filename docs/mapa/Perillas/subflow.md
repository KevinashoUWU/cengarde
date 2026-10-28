---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `subflow`

MPGRIT (paso U1): cada extremo numera lo que pone en cada enlace (`sseq`, protocolo 12) y sigue la secuencia de cada enlace que recibe; lo que un enlace perdió dentro de sí (un `sseq` posterior del mismo enlace llegó hace más que su ventana) va de vuelta en un `SFLOSS`, y un mensaje de GRIT que fue solo por ese enlace se envía otra vez como pérdida cierta, aun con `protocontroller_nack no`. Tienen que tenerla los dos extremos.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes`, `no`
- **Pieza:** [[Subflujo (subflow.h)]], [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
