---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller_offer`

Con `subflow`, GRIT ofrece la capacidad del motor y un cuarto mientras no estén llenos todos los enlaces que llevan los datos (`cg_scc_offer`): cada enlace recibe más que su capacidad y lo que entrega la sube donde hay espacio (un módem da más cuando su cola crece). Experimento del paso 2 (historia 029).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `yes`
- **Rango:** `yes`, `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[029-mpgrit]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
