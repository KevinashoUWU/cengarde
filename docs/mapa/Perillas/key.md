---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `key`

El secreto del par: de él salen las claves del MAC (`pair.h`). En `[client NOMBRE]`, la clave de ese router.

- **Sección:** global, `[client NOMBRE]`
- **Extremo:** los dos
- **Por omisión:** (obligatoria; con secciones `[client]`, una en cada una y ninguna global)
- **Rango:** base64 de 32 bytes
- **Pieza:** [[Configuración (config.c)]], [[Varios clientes (clients.h)]]
- **Historia:** [[008-luci-uci-y-emparejamiento]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
