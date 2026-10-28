---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_cc_bits`

Experimento del paso 2b: qué usos de la cola de cada enlace (su retardo de ida) están encendidos en el [[Ritmo por enlace (linkcc.h)]], sumando: 1, la cola que ve el reparto; 2, los modos por la cola (STARTUP termina, DRAIN termina, PROBE drena o sondea); 4, el tope de `bw` por lo que entregó con cola; 8, STARTUP a ×3/2 (si no, ×2). Los cuatro juntos perdieron 20–46 % en el laboratorio; esto mide cada uno.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `15`
- **Rango:** 0–15
- **Pieza:** [[Ritmo por enlace (linkcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
