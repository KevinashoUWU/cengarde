---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `redund_spare_first`

Con `redund_spare`, el primer enlace (el que los demás protegen) fijado: ese mientras esté vivo, medido y sin ahogarse; si se ahoga queda vetado 60 s y manda la elección automática, y vuelve cuando se despeja.

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** vacío (el de menor retardo sin cola)
- **Rango:** el nombre de un `[link NAME]` del archivo
- **Pieza:** [[Lo que sobra (spare.h)]]
- **Historia:** [[020-copias-parciales-y-paridad]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
