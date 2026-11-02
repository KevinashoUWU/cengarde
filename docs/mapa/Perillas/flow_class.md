---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `flow_class`

La clase de un flujo para el reordenador del que recibe, sobre lo que adivina `dgclass.h`: `ordered` espera su rescate (como MPEG-TS), `deadline` la regla del plazo (como RTP), `direct` nunca retenido ni rescatado (QUIC, SRT, WebRTC, juegos), `auto` lo adivinado. Calza el origen o el destino del paquete; gana la primera regla.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** ninguna
- **Rango:** repetible, hasta 32: `ADDR[/LEN][:PORT]`, `[IPv6][/LEN][:PORT]` o `:PORT`, y `auto`, `ordered`, `deadline` o `direct`
- **Pieza:** [[Reordenador (reorder.h)]]
- **Historia:** [[031-clases-por-flujo]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
