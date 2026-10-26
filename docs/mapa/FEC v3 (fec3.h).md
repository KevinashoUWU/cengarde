---
tags: [mapa, pieza]
archivos:
  - engine/src/fec3.h
  - engine/tests/test_fec3.c
sello: 80180356daa48def
---
# FEC v3 (fec3.h)

**Grupo:** Decidir · Recuperar — [[Mapa del motor]]

## Qué hace
Las decisiones del emisor de la **FEC v3** (`fec = yes` con `fec_code = rlc`, [[026-fec-v3]]): una ventana deslizante de RLC ([[Códigos de borrado (rs.h, rlc.h)]], `struct cg_rlcw_tx`) sobre **todo** DATA que sale, con reparaciones a una **parte fija** de los bytes (`fec_pct`), haya o no pérdida, para que un enlace que se detiene se rehaga con las reparaciones que llevan los demás: un enlace con parte s detenido T se rehace unos s·T / (r·(1 − s)) después de empezar, a tasa r (cuatro enlaces iguales, 50 ms y 25 %: ~67 ms), dentro de la ventana (`fec_window_ms`) y de lo que el receptor espera al código (`rescue_code_ms`, [[Reparación coherente (harq.h)]]).

- **Cuándo protege** (`cg_f3_mode_of`): con reparto sobre dos enlaces o más (`bonding`), o un **solo enlace** (`single`), con su puerta (`cg_f3_gate`, `fec_single = auto`): abierta mientras pierde **al azar** (el `loss_kind` de [[Observaciones del enlace (linksig.h)]], o una muestra con pérdida de 2 ‰ o más sin cola, `CG_F3_RAND_PM`: el 0,3 % de Starlink queda bajo la muestra con pérdida de `linksig.h`) y 30 s después mientras siga perdiendo 1 ‰ o más; cerrada en un enlace limpio, con pérdida **por congestión** (las caídas de una cola llena son la señal de TCP para frenar: repararlas le quitaría capacidad y la escondería) y en un **corte** (pérdida sobre la mitad de la tasa de reparación: el código no alcanza y las reparaciones mueren con el enlace). En redundancia con varios enlaces, apagada (ahí protege [[Lo que sobra (spare.h)]]).
- **Cuánto** (`cg_f3_pct`, `fec_pct_min`): la parte sigue a la pérdida que la pide, juzgada en muestras **sin cola** (la cola más alta de la muestra y de la anterior, `last_qmax` de [[Observaciones del enlace (linksig.h)]], dentro de 10 ms sobre el mínimo del minuto): una **ráfaga** (5 % o más en una muestra, `CG_F3_BURST_PM`: una detención, un corte, también en un enlace lleno) da `fec_pct` entero por 30 s; la pérdida **al azar**, además **bajo la capacidad del enlace** (`loaded_ms` de [[Capacidad (capacity.h)]], esta muestra y la anterior), `fec_pct_min` más 25 veces la pérdida pesada por los bytes de cada enlace, por 120 s (más que lo que tarda TCP, ya reparado, en llenar el enlace y callar la evidencia); los primeros 10 s, `fec_pct` entero (nada se sabe aún, y un cubic que se desploma al empezar tarda en volver); sin nada, `fec_pct_min` (0: ninguna reparación, el lote cuenta en `quiet`). Lo que **no** cuenta: la pérdida de un enlace con cola (BBR desbordando los de `clean4` hacía 15–70 ms) y la que un TCP que acaba de frenar reporta en la muestra siguiente, ya tranquila; con la parte fija, las reparaciones encima de esa pérdida la agrandaban y pedían más reparaciones. Un cubic con cola permanente (sube el mínimo del minuto) se lee sin cola y lleva la parte: no le cuesta nada (`clean4`, igual que sin FEC).
- **Cuántas** (`cg_f3_earn`, `cg_f3_owed`, `cg_f3_spend`): un crédito en bytes × 100; cada DATA gana su largo × la parte, cada reparación gasta su tamaño (cabecera y símbolo). La reparación de **cola** (`cg_f3_due`): `fec_wait_ms` después del primer DATA sin reparación (el fin de una ráfaga), con el crédito en 0 o más, hasta una reparación de deuda.
- **Dónde** (`cg_f3_pick`): round-robin suave ponderado por los bytes de DATA de cada enlace en los últimos ticks (`cg_f3_weigh`): cada enlace lleva ~`fec_pct` más que su parte; nunca un enlace que el reparto dejó afuera (silenciado, G3, G4) ni uno **lleno** (`cg_f3_guard`: el peso del rescate de [[Capacidad (capacity.h)]] en 0, su cola dentro de un segundo pasa 4 × `rescue_target_ms`: lo que pierde es congestión y un enlace ahogado no recibe reparaciones que le llenen la cola) o cuya cola pasa la ventana (sus reparaciones llegarían cuando ya se soltaron los huecos). Sin enlace, la reparación no sale (sus bytes se gastan igual): la parte es un techo. Con la mitad del peso (2,5 × el objetivo) cortaba las reparaciones que cubic necesita mientras llena un enlace solo, y tampoco frenaba a BBR, cuya cola queda chica mientras las reparaciones le quitan capacidad ([[026-fec-v3]]).
- **Sobre qué** (`cg_f3_span_us`, `cg_f3_lag_us`): una reparación de cada dos cubre la ventana entera (alcanza una detención) y la otra su último cuarto (al menos 20 ms: a los mismos miembros por reparación, una pérdida suelta cae en más de ellas y vuelve antes); la ventana termina un poco antes de ahora, en lo que tarda el enlace más lento más que el de la reparación (`behind_us` de [[Salud y silenciado (health.h)]]): lo enviado después llegaría detrás de la reparación y le costaría al receptor una columna para nada (bajo TCP, ~100 miembros por reparación en el laboratorio; con el retraso, la mitad de las incógnitas a la vez).

- **La bajada** (`server.c`: `sf3_apply`, `sf3_begin`, `sf3_feed`, `sf3_repair`, `sf3_send`, `sf3_service`): con `fec = yes` y `fec_code = rlc` en el servidor (`cengarde-vps-setup fec rlc`), las mismas decisiones por sesión sobre los caminos de la bajada (una ventana de ~2 MB por sesión, asignada con la perilla, nunca por paquete), con su guarda en el peso del rescate de cada camino; el router las rehace con `fec = yes` (su anillo de 2048 DATA y el receptor de la RLC, ~5 MB) y pide la PARQ por la subida ([[Reparación coherente (harq.h)]]).
## Lee
- De [[Reparto (split.h)]]: los enlaces elegibles del lote y los de peso 0 (`u->k`), o los portadores en redundancia; `split_active` y el dominante.
- De [[Capacidad (capacity.h)]]: la pérdida de cada muestra (`loss_pm`, `loss_pm_ms`) y la cola sobre el mínimo del minuto (`ql_us`); de [[Observaciones del enlace (linksig.h)]], `kind`.
- Los bytes de DATA de cada enlace por tick (`tx_bytes`) y su atraso (`behind_us`).

## Decide / escribe
- Reparaciones `CG_T_REPAIR` de tipo `CG_REP_RLCW` (protocolo 11, `repair.h`: base, largo de la ventana, densidad y semilla) en la cola de paridades (`fecq`, `f3 = 1`), por el enlace elegido; cuentan en `tx_fec_*` del enlace (no como DATA, [[FEC (fec.h)]]) y en el [[ETA (eta.h)]] con `split_eta`.
- Estado del servidor por sesión: `download_fec` (`code`, `rlc` con los mismos campos que el cliente) y por camino `fec_repairs`; del cliente, `download_fec` (lo rehecho).
- Estado del cliente: `upload.fec.code` y `upload.fec.rlc` (`mode`, `pct`, `window_ms`, `single`, `links`, `window`, `lag_ms`, `density`, `repairs`, `repair_bytes`, `data_bytes`, `overhead_pm`, `tail`, `guarded`, `no_link`, `empty`, `gated`, `quiet`; `pct` es la parte ahora, con `pct_max`, `pct_min` y `bursts`); por enlace `fec_repairs`, `fec_repair_bytes`, `fec_loss_pm`.
- La ventana también responde la paridad pedida (PARQ) sin anillo de rescate (`on_parq`), con el presupuesto del rescate (`rescue_pct`) que gana cada DATA.

## Perillas
- [[Mapa/Perillas/fec|fec]]: `yes`; [[Mapa/Perillas/fec_code|fec_code]]: `rlc` (por omisión `xor`, la paridad de [[FEC (fec.h)]]).
- [[Mapa/Perillas/fec_pct|fec_pct]]: 20; 1–50. [[Mapa/Perillas/fec_pct_min|fec_pct_min]]: 0; 0–50. [[Mapa/Perillas/fec_window_ms|fec_window_ms]]: 300; 10–1000. [[Mapa/Perillas/fec_single|fec_single]]: `auto`, `yes`, `no`.
- [[Mapa/Perillas/fec_wait_ms|fec_wait_ms]]: el plazo de la reparación de cola.

## Dónde se midió
- [[026-fec-v3]]: laboratorio (túnel nativo, perfil de terreno, n = 6, contra `origin/master` con `fec = no` y `fec = yes`): detenciones de uno de cuatro enlaces, un solo enlace con 1 % de pérdida, con microcorte y limpio, `lossy`, `spikeloss`, `ltespikerand`, `starsat`, `protect`, en la subida y en la bajada; costo con `bench/fecbench.c` y la CPU del servidor con tres routers (`bench/lab.sh fec3cpu`).

## Choques conocidos
- Una detención más larga que la que cubre la parte (s·T / (r·(1 − s)) más allá de la espera del receptor) no se rehace con la ventana sola: lo que falta lo pide la PARQ, una ida y vuelta ([[Reparación coherente (harq.h)]]); con TCP adentro el hueco no se retiene (la regla de la historia 023) y lo rehecho llega tarde.
- Bajo saturación, mientras la parte está arriba (una ráfaga o pérdida al azar), las reparaciones toman su parte de la capacidad de los enlaces y rehacen también lo que una cola llena tiró; con `fec_pct_min` igual a `fec_pct`, siempre (la parte fija).
- La ventana guarda una copia de cada DATA (~2 MB en el router); al receptor le cuesta el anillo de 2048 y ~600 KB de ecuaciones por sesión.

## Archivos
- `fec3.h`; tests `engine/tests/test_fec3.c`.
- `client.c`: `f3_on`, `f3_begin`, `f3_feed` (en `up_one`), `f3_repair`, `f3_send` (tras `send_links`), `f3_tail` (en `fec_service`), `f3_apply`; `on_parq` (la ventana como fuente); `cap_tick` (`cg_f3_weigh`, `cg_f3_loss`).
