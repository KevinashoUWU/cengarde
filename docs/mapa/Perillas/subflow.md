---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `subflow`

MPGRIT (paso U1): cada extremo numera lo que pone en cada enlace (`sseq`, protocolo 12) y sigue la secuencia de cada enlace que recibe; lo que un enlace perdió dentro de sí (un `sseq` posterior del mismo enlace llegó hace más que su ventana) va de vuelta en un `SFLOSS`; un mensaje de GRIT que fue solo por ese enlace se envía otra vez cuando nada lo reconstruye o el receptor también lo pidió. El NACK solo del túnel pasa a ser una pista (`protocontroller_nack` no cuenta), el RACK de GRIT sobre la mezcla se apaga y su temporizador espera al menos 300 ms más. En terreno: GRIT reenvía 0,6–1 % en vez de 10 %. Si la configuración no los dice, cambia los objetivos que ganaron en terreno (paso 2): `protocontroller_queue_ms` 50, `governor_target_ms` 60, `capacity_loss_pct` 5 y `protocontroller_buf_kb` 2048. Tienen que tenerla los dos extremos.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes`, `no`
- **Pieza:** [[Subflujo (subflow.h)]], [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
