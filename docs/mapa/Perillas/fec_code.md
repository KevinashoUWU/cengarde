---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec_code`

Lo que manda `fec = yes`: `xor`, la paridad espacial de siempre (un paquete por enlace por grupo, rehace una pérdida por grupo); `rlc`, la FEC v3: reparaciones de una ventana deslizante sobre todo DATA, una parte fija de los bytes repartida por los enlaces, con reparto o con un solo enlace (protocolo 11; el servidor con `fec = yes`). En el servidor, las mismas reparaciones sobre la bajada de cada sesión (~2 MB más por sesión; `cengarde-vps-setup fec rlc`), que el router rehace con `fec = yes`. Se cambia sin reiniciar.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `xor`
- **Rango:** `xor` o `rlc`
- **Pieza:** [[FEC v3 (fec3.h)]], [[FEC (fec.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[026-fec-v3]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
