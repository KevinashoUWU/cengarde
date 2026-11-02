---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `flow_learn`

Reconoce por flujo QUIC (su encabezado largo), SRT (su handshake) y RTP con NACK de RTCP (RIST, WebRTC) y, sin reparto, no los retiene (`direct`). En bonding nunca actúa (el QUIC sin retener perdió de la mitad al 85 %); en redundancia baja el retardo con carga y cuesta ~30 % del caudal en enlaces con pérdida.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes` o `no`
- **Pieza:** [[Reordenador (reorder.h)]]
- **Historia:** [[031-clases-por-flujo]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
