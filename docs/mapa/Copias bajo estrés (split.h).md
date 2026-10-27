---
tags: [mapa, pieza]
archivos:
  - engine/src/split.h
  - engine/tests/test_split.c
sello: 907b1381554c3745
---
# Copias bajo estrés (split.h)

**Grupo:** Decidir · Enviar — [[Mapa del motor]]

## Qué hace
Mientras [[G4 guarda de dispersión (split.h)]] está en estrés, un paquete repartido puede ir copiado por todos los portadores, pagando con un presupuesto (`cg_sdup`). Cada paquete repartido suma `pct` % de sus bytes; cada copia gasta los bytes de sus copias extra; se guarda como mucho 1 MiB (`CG_SDUP_CAP`). Con 0 nunca copia; con 100 las copias pueden doblar los bytes mientras dura el estrés.

## Lee
- `stress` de [[G4 guarda de dispersión (split.h)]], el tamaño del paquete y cuántos portadores hay.

## Decide / escribe
- Si el paquete va copiado (elegibles 0 para `cg_split_pick_eta`) → [[Reparto (split.h)]].
- `CG_DEC_COPIES_DRY/OK` (sin presupuesto) → [[Bitácora de decisiones (decision.h)]].

## Perillas
- [[Mapa/Perillas/split_stress_dup|split_stress_dup]]: 50; 0–400.
- [[Mapa/Perillas/split_stress_single|split_stress_single]]: no (bajo estrés, solo el más rápido en vez de copiar).

## Dónde se midió
- `split.h`: en el laboratorio (`ltespike` bajada), el reparto vacío que copiaba todo por accidente tuvo 44 retransmisiones de TCP donde repartir tuvo 700–800.
- [[012-bonding]]: el contexto de la espiral de las guardas en la Pi.

## Choques conocidos
- C10: `cg_sdup` acumula hasta 1 MiB y lo gasta en ráfaga al empezar el estrés ([[audit-pending]], tarea #51).
- C13 (resuelto): es el paso 4 de `cg_split_plan` ([[Reparto (split.h)]]): una copia no elige enlace; la FEC v3 igual gana reparaciones por ella (bajo estrés rehacen los repartidos del enlace lento).

## Archivos
- `split.h` (`cg_sdup`, `CG_SDUP_CAP`, `cg_split_stress_elig`).
- `client.c`: `tun_read`, `spread_tick`. `server.c`: `down_prepare`.
