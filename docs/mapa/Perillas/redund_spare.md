---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `redund_spare`

EXPERIMENTO: en redundancia, lo que llevan los enlaces que no son el primero, en lo que les sobra (su cola bajo `redund_spare_ms`): copias parciales, paridad XOR sobre k paquetes del primero (el servidor necesita `fec = yes`), la mezcla (copias si cabe todo o en un microcorte, paridad si no), o como la mezcla con un código de borrado en vez del XOR: bloques Reed-Solomon (`rs`, Leopard) o una ventana deslizante (`rlc`), con k y m según la pérdida del primero (protocolo 8; el servidor necesita `fec = yes`).

- **Sección:** global
- **Extremo:** cliente
- **Por omisión:** `no`
- **Rango:** `no`, `copies`, `parity`, `mix`, `rs` o `rlc`
- **Pieza:** [[Lo que sobra (spare.h)]], [[Copias a tiempo (eta.h)]], [[FEC (fec.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[020-copias-parciales-y-paridad]], [[021-fec-v2]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
