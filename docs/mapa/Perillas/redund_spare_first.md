---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `redund_spare_first`

Con `redund_spare`, el primer enlace (el que los demás protegen) fijado: ese mientras esté vivo y llevando, medido ahora y sin ahogarse; si se ahoga como primero queda vetado 15 s y manda la elección automática, y vuelve cuando se despeja; mientras los demás copian toda la subida, solo tras 2 s seguidos de ahogo (un salto de retardo más corto no lo saca). El estado dice por qué no es el primero (`upload.spare.pin`).

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** vacío (el de menor retardo sin cola)
- **Rango:** el nombre de un `[link NAME]` del archivo
- **Pieza:** [[Lo que sobra (spare.h)]]
- **Historia:** [[020-copias-parciales-y-paridad]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
