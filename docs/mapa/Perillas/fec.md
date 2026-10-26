---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `fec`

Paridad XOR de cada grupo de DATA repartido, o con `fec_code = rlc` la ventana deslizante de la FEC v3. En el servidor, también la memoria con la que usa las paridades y las reparaciones de `redund_spare` (`parity`, `mix`, `rs`, `rlc`) y de la FEC v3 (el anillo de 2048 DATA y las ecuaciones: ~5 MB por sesión). En el router, con un servidor que manda reparaciones en la bajada (`fec_code = rlc` allí), el mismo anillo y las ecuaciones para rehacerlas.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `no`
- **Rango:** `yes` o `no`
- **Pieza:** [[FEC (fec.h)]], [[FEC v3 (fec3.h)]], [[Códigos de borrado (rs.h, rlc.h)]]
- **Historia:** [[013-capacidad-y-dosificacion]], [[021-fec-v2]], [[026-fec-v3]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
