---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `capacity_loss_queue_ms`

EXPERIMENTO: un enlace que pierde al menos 1 % de lo enviado, mandándole más de lo que entrega, con la cola sobre su mínimo de 60 s en al menos esto, dos muestras seguidas, está lleno a cualquier carga (con el preset de Starlink, solo cargado). Sugerido: 10.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 0 (apagado)
- **Rango:** 0–1000
- **Pieza:** [[Capacidad (capacity.h)]], [[Cola por enlace (cg_gov_base)]], [[Modelo de enlace (linkmodel.h)]]
- **Historia:** [[019-perdida-con-cola]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
