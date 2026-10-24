---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec`

Paridad XOR de cada grupo de DATA repartido. En el servidor, también la memoria con la que usa las paridades y las reparaciones de `redund_spare` (`parity`, `mix`, `rs`, `rlc`).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes` o `no`
- **Pieza:** [[FEC (fec.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[013-capacidad-y-dosificacion]], [[021-fec-v2]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
