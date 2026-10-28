---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_cc_bits`

Experimento del paso 2b: qué usos de la cola de cada enlace (su retardo de ida) están encendidos en el [[Ritmo por enlace (linkcc.h)]], sumando: 1, la cola que ve el reparto; 2, los modos por la cola (STARTUP termina, DRAIN termina, PROBE drena o sondea); 4, el tope de `bw` por lo que entregó con cola; 8, STARTUP a ×3/2 (si no, ×2). En el laboratorio (n = 2, 7 casos × 2 sentidos), 2 + 8 ganó: +8,7 % de media geométrica sobre ninguno y 3–10 veces menos pérdidas por enlace; 1 perdió 20–53 % (su retardo es el del paquete más nuevo con acuse y deja fuera lo enviado después: el reparto llenó de más los enlaces); 4, algo peor en netem, que no tiene ráfagas de concesiones; en terreno subió la subida de 78 / 59 a 96 / 101 Mbit/s (OMR 93–95) y no la bajada: por eso el router lo trae y el VPS no.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `14` en el router, `10` en el VPS
- **Rango:** 0–15
- **Pieza:** [[Ritmo por enlace (linkcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
