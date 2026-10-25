---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_cc`

Control de congestión del TCP del VPS hacia el destino; si el kernel no lo tiene, el suyo (un aviso).

- **Sección:** global
- **Extremo:** servidor
- **Por omisión:** `bbr`
- **Rango:** un control de congestión del kernel, o vacío
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
