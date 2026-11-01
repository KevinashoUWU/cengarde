---
tags: [mapa, pieza]
archivos:
  - engine/src/config.c
  - engine/src/config.h
  - engine/tests/test_config.c
sello: 4e77cd38f9bf18ff
---
# Configuración (config.c)

**Grupo:** Configurar — [[Mapa del motor]]

## Qué hace
Lee el archivo INI: una parte global, secciones `[link NOMBRE]` (cliente) y `[client NOMBRE]` (servidor). Valida cada clave con su rango y avisa de las desconocidas sin fallar. Pone los valores por omisión. Una recarga (SIGHUP o `cengarde ctl reload`) aplica todo en caliente salvo lo que el bucle montó una vez; para eso `cg_config_restart_needed` dice qué reinicia el proceso.

## Lee
- El archivo de configuración y `/etc/hosts` (para los nombres).

## Decide / escribe
- `struct cg_config`, que leen todas las piezas. Valores derivados: `unmute_behind_ms` = 4/5 de `mute_behind_ms`; `capacity_sat_ms` y `rescue_target_ms` = `governor_target_ms` si son 0; `split_spread_copy` = no con gobernador, capacidad o ETA; `server_failover_ms` según `probe_idle_ms`; con `subflow`, si no se dicen, los objetivos que ganaron en terreno ([[029-mpgrit]], paso 2): `protocontroller_queue_ms` 50, `governor_target_ms` 60, `capacity_loss_pct` 5, `protocontroller_buf_kb` 8176 (2048 y luego 4080 hasta que el ritmo por enlace llegó a esos techos); sin `subflow`, `link_cc` se apaga (necesita los `SFACK`); `link_cc_bits` por omisión es 14 en el router y 46 en el VPS.
- `redund_spare` se lee por nombre (`no`/`off`, `copies`, `parity`, `mix`, `rs`, `rlc` → 0–5, `get_spare`) → [[Lo que sobra (spare.h)]]; `rs` se rechaza en un binario sin Leopard (`make LEOPARD=0`) → [[Códigos de borrado (rs.h, rlc.h)]].
- `redund_spare_first` tiene que nombrar un `[link NOMBRE]` del archivo (con alguna clave: una sección vacía no existe para el INI); si no, error → [[Lo que sobra (spare.h)]].
- `fec_code` se lee por nombre (`xor`, `rlc`, `get_fec`) y `fec_single` (`auto`, `yes`, `no`); `fec_pct` (20), `fec_pct_min` (0) y `fec_window_ms` (300) se aplican sin reiniciar → [[FEC v3 (fec3.h)]].
- `cg_config_link_knobs` → [[Modelo de enlace (linkmodel.h)]]; `cg_config_slot` → [[Ranuras de Starlink (slot.h)]]; `cg_rescue_wait_ms` → [[Rescate (rescue.h)]] y [[FEC (fec.h)]].

## Perillas
- Todas: ver [[Perillas]].

## Dónde se midió
- [[009-recarga-control-e-ip-pass]]: recarga sin cortar.
- [[020-copias-parciales-y-paridad]]: `redund_spare` y `redund_spare_ms`.
- [[021-fec-v2]]: `redund_spare = rs | rlc` y `redund_spare_code_ms`.
- [[026-fec-v3]]: `fec_code`, `fec_pct`, `fec_pct_min`, `fec_window_ms`, `fec_single`.

## Choques conocidos
- C12: `governor_target_ms` fijaba cuatro lazos; ahora la configuración deriva los otros dos solo si son 0 ([[audit-pending]]).

## Archivos
- `config.c`, `config.h`; tests `engine/tests/test_config.c`. `main.c`, `client.c` (`apply_config`), `server.c` (`apply_config`).
