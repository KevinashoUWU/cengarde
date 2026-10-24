---
tags: [mapa, pieza]
archivos:
  - engine/src/config.c
  - engine/src/config.h
  - engine/tests/test_config.c
sello: aa30eb1c0d284c3a
---
# Configuración (config.c)

**Grupo:** Configurar — [[Mapa del motor]]

## Qué hace
Lee el archivo INI: una parte global, secciones `[link NOMBRE]` (cliente) y `[client NOMBRE]` (servidor). Valida cada clave con su rango y avisa de las desconocidas sin fallar. Pone los valores por omisión. Una recarga (SIGHUP o `cengarde ctl reload`) aplica todo en caliente salvo lo que el bucle montó una vez; para eso `cg_config_restart_needed` dice qué reinicia el proceso.

## Lee
- El archivo de configuración y `/etc/hosts` (para los nombres).

## Decide / escribe
- `struct cg_config`, que leen todas las piezas. Valores derivados: `unmute_behind_ms` = 4/5 de `mute_behind_ms`; `capacity_sat_ms` y `rescue_target_ms` = `governor_target_ms` si son 0; `split_spread_copy` = no con gobernador, capacidad o ETA; `server_failover_ms` según `probe_idle_ms`.
- `cg_config_link_knobs` → [[Modelo de enlace (linkmodel.h)]]; `cg_config_slot` → [[Ranuras de Starlink (slot.h)]]; `cg_rescue_wait_ms` → [[Rescate (rescue.h)]] y [[FEC (fec.h)]].

## Perillas
- Todas: ver [[Perillas]].

## Dónde se midió
- [[009-recarga-control-e-ip-pass]]: recarga sin cortar.

## Choques conocidos
- C12: `governor_target_ms` fijaba cuatro lazos; ahora la configuración deriva los otros dos solo si son 0 ([[audit-pending]]).

## Archivos
- `config.c`, `config.h`; tests `engine/tests/test_config.c`. `main.c`, `client.c` (`apply_config`), `server.c` (`apply_config`).
