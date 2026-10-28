---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_cc`

MPGRIT (paso 2, experimento): con `subflow`, un BBR por enlace ([[Ritmo por enlace (linkcc.h)]]) alimentado por los `SFACK` decide el reparto por llegada (su ritmo, su retardo sin cola, su cola en vuelo; pasado `governor_target_ms` de cola, último recurso) y la oferta de GRIT es la suma de los ritmos de los enlaces, como OpenMPTCProuter corre un BBR por subflujo. Sin `subflow` no hace nada. Tienen que tenerla los dos extremos.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes`, `no`
- **Pieza:** [[Ritmo por enlace (linkcc.h)]], [[Subflujo (subflow.h)]], [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
