---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `pep_backlog_ms`

Lo que un flujo guarda sin enviar o sin escribir, en tiempo a su ritmo (al menos 64 KB): la contrapresión hacia el emisor.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** 200
- **Rango:** 10–10000
- **Pieza:** [[PEP modo B (pep.c, stream.h, pepcc.h)]]
- **Historia:** [[025-protocontrolador]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
