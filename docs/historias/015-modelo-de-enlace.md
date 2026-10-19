# 015 — Modelo de enlace: cada enlace con su tipo

- **Fecha:** 2026-10-19
- **Estado:** vigente. Hecho: el tipo por enlace (`type` en `[link]`, `list uplink_type` en OpenWrt, el desplegable de LuCI), su envío al VPS en la cola de bonding de las sondas y los ajustes por tipo (`linkmodel.h`); el preset de `starlink` medido en laboratorio; las correcciones mixtas de la auditoría de relojería como campos del modelo por enlace, probadas por tipo (entra solo `pace_peers_ms` en `starlink`). Falta medirlo en campo (rama `claude/link-model-field` para el router de radio, protocolo 7).
- **Fuentes:**
  - la prueba de campo del 2026-10-18 en el router «nostalgica» (Starlink y dos LTE);
  - los casos `shallowq`, `shallowqd`, `shallowqdeep` y `starsat` de `bench/lab.d/bond.sh` (rama `claude/labq-nodes`, PR #77), la ablación del 2026-10-18 y el A/B de `eta_buffer` global (`~/.claude/jobs/1c68aec3/tmp/sat4/out/all.jsonl`);
  - código: `engine/src/linkmodel.h`, `capacity.h` (`cg_cap_tick`, `cg_cap_may_probe`, `cg_cap_eta`), `eta.h` (`limit_us`), `proto.h` (`CG_BOND_TYPE`);
  - las corridas de esta historia: `python3 -I bench/labq.py run PLAN --node local:6` (`local:3` en la verificación final), planes y salidas en `/tmp/lm/lab1` a `lab7` (scratch de la sesión, se pierde al reiniciar); las revisiones de los brazos experimentales quedan en las ramas `claude/link-model-exp*` (`-exp`: solo sin sondeo; `-exp2`: margen de 10 ms; `-exp-p40`, `-exp-p100`, `-exp-p200`: el costo en `eta.h`).

## TL;DR

- **El hallazgo:** en «nostalgica» (Starlink y dos LTE) el bonding bajó ~40 Mbit/s frente a 64,7 de Starlink solo [campo]. Todos los lazos tratan a todos los enlaces como celulares: esperan una cola para ver un enlace lleno, y Starlink, con ~15 ms de búfer, descarta antes. El reparto por llegada esperada nunca desborda a los celulares, que tienen 25–30 ms más de retardo base.
- **El diseño:** `type` en cada `[link]` (`auto`, `cellular`, `starlink`, `fiber`, `ethernet`, `radio`; `linkmodel.h`), un preset por tipo para los lazos de capacidad y reparto, y el tipo de cada camino al VPS en los bits 20–22 de la cola de bonding de las sondas (reservados: **sin versión nueva**, compatible con pares del protocolo 7). `auto` es exactamente el motor de antes; los demás tipos, salvo `starlink`, también, como ganchos.
- **El preset de `starlink`** [laboratorio, n = 8, control nulo]: mientras pierde (>0,3 %), no se sondea y se retiene en la cola que mostró; lo que pasa de ahí va a otro enlace solo mientras llegue a menos de **40 ms** de lo que tardaría por Starlink. En los casos de campo, +27 a +45 % (`shallowq` bajada 41,6 → 56,8; `starsat` bajada 36,0 → 52,1), por encima de Starlink solo; sin costo de caudal con retardos iguales (`shallowqd`) ni con cola profunda (`shallowqdeep`). La revisión final lo repite (+26 a +52 %, control nulo dentro de ±7 %). Lo que cuesta es latencia: el ping de 1200 bytes pasa de ~50 a ~100 ms, porque los celulares cargan más. Sin regresión con todo `auto` o todo `cellular` (`het`, `bloat`, `ltespikerand`, ±3 %), y compatible en los dos sentidos con un par del protocolo 7 sin el tipo.
- **Lo que no sirvió:** retenerlo como último recurso (`eta_buffer`, global o solo en Starlink): todo lo que sobra va a las colas de 300 ms de los celulares (ping grande de 52 a ~200 ms) y la subida pierde 17–18 %. Costos de 100 y 200 ms: más ganancia en `shallowq`, pero −10 % y −38 % con retardos iguales.
- **Las correcciones mixtas de la auditoría, por tipo** [laboratorio, n = 16–24, control nulo]: C3, C5, C9, C11, #33, `redund_pace_ms` y el corte por pérdida son ahora campos del modelo de cada enlace (perillas globales encima del preset, `exp_link_types` para probarlas por tipo). Para lo celular no entra ninguna: en `het`, `bloat`, `deepq`, `ltespikerand`, `spikeloss` y `lossy` nada pasa el criterio. En `starlink` entra una: en redundancia pide que los **otros** enlaces de su sesión lleven copia solo si llega a menos de 60 ms de la primera (`pace_peers_ms`). En `starsat`, el p99 del ping grande baja de ~360 a ~95 ms, con el caudal a ±5 % (−4 % en `shallowqd`) y 1,1–3× de retransmisiones. Aplicado a todos los celulares, `het` perdía 8–18 %. El corte por pérdida, C9 y C11 en Starlink no suman o cuestan (sección 6).
- **OpenWrt:** `list uplink_type 'ENLACE=TIPO'` y un desplegable por enlace en LuCI.
- **Falta:** campo; detectores (el diente de sierra de 15 s de Starlink, el planificador PF/RR de la celda) y presets afinados por tipo (búsqueda en PyTorch).

## Contexto

### El hallazgo de campo (2026-10-18)

En «nostalgica» (Starlink y dos LTE), el bonding bajó menos que Starlink solo: ~40 frente a 64,7 Mbit/s [medido en campo]. Starlink perdía el 15 % de lo que se le mandaba mientras su cola no pasaba de 40 ms, y la capacidad que el motor le atribuía seguía en 86 Mbit/s frente a los ~60 reales.

### La causa: todos los lazos tratan a todos los enlaces como celulares

- Un módem celular lleno **encola**, y mucho (cientos de ms). La capacidad (`capacity.h`) solo baja cuando ve esa cola, y el reparto por llegada esperada (`eta.h`, `cg_cap_eta`) pasa tráfico a otro enlace cuando la cola de este hace que un paquete llegue más tarde que por el otro.
- Starlink tiene un búfer corto (~15 ms) y, lleno, **descarta** en vez de encolar.
- El reparto solo desborda hacia un celular cuando la cola de Starlink supera la diferencia de retardo base (25–30 ms), y Starlink nunca llega ahí: descarta antes. Los celulares quedan ociosos y Starlink pierde las ráfagas de TCP.

### La ablación en el laboratorio (2026-10-18, n = 4, Mbit/s) [medido en laboratorio]

| Caso | Bonding | Starlink solo |
| --- | ---: | ---: |
| Cola corta y diferencia de retardo (`shallowq`) | 41,7 | 48,9 |
| Cola corta, retardos iguales (`shallowqd`) | 63–67 | — |
| Cola profunda y diferencia de retardo (`shallowqdeep`) | 72–74 | — |

Hace falta la combinación: cola corta **y** retardo base menor que el de los celulares. Con cualquiera de las dos sola, el bonding suma.

### El A/B de `eta_buffer` global (n = 4, cubic, mediana Mbit/s) [medido en laboratorio]

`eta_buffer` (rama `claude/cap-loss`): un enlace que perdió más del 0,3 % hace poco (`lossy`) no se sondea a 5/4 y, en el reparto por llegada esperada, se retiene en la cola más alta que mostró en 10 s. Aplicado a **todos** los enlaces:

| Caso | Sentido | sin (c6c7) | control nulo | `eta_buffer` |
| --- | --- | ---: | ---: | ---: |
| `starsat` | bajada | 26,6 | 29,3 | 38,4 (+38 %) |
| `starsat` | subida | 34,7 | 40,9 | 36,3 (ruido) |
| `shallowq` | bajada | 44,0 | 44,1 | 38,1 (−13 %) |
| `shallowq` | subida | 39,9 | 40,4 | 36,4 (−9 %) |
| `het` | subida | 66,7 | 65,8 | 56,9 (−15 %) |
| `het` | bajada | 63,6 | 65,5 | 68,8 |

Lo que cuesta en `het` (sin Starlink) se explica por aplicarlo a todos: los celulares también quedan marcados `lossy` (un 0,3 % de diferencia entre lo enviado y lo recibido en una muestra lo da también una cola que crece) y pierden el sondeo [razonado, no medido]. De ahí el tipo por enlace.

## Diseño

### El tipo

- `enum cg_link_type` en `linkmodel.h`: `auto` (0, por omisión), `cellular`, `starlink`, `fiber`, `ethernet` (cable, DSL, sin gestión) y `radio` (punto a punto, WAN inalámbrica). Los valores van en el cable: no se renumeran.
- Cada tipo tiene un preset (`struct cg_link_model`) con los parámetros por enlace que leen los lazos:
  - `lossy_noprobe`: un enlace `lossy` no se sondea a 5/4 (su cola es corta porque descarta, no porque tenga lugar);
  - `lossy_hold`: en el reparto por llegada esperada, un enlace `lossy` se retiene en la cola más alta que mostró;
  - `hold_over_ms`: lo que cuesta pasarse de esa cola (`over_us` de `eta.h`): el paquete va a otro enlace mientras llegue por él a menos de eso; 0 es el último recurso de `eta_buffer` (10 s);
  - `loss_pct`: el corte de capacidad por pérdida (`capacity_loss_pct`) para ese enlace.
- `auto` es todo ceros: exactamente el motor de antes. `cellular`, `fiber`, `ethernet` y `radio` también, por ahora: son ganchos para afinar con medidas, sin comportamiento inventado.
- Las perillas globales siguen: `eta_buffer = yes` enciende `lossy_noprobe` y `lossy_hold` en todos los enlaces (con el último recurso, salvo el costo propio de un enlace `starlink`); `capacity_loss_pct` distinto de 0 vale para todos, por encima del preset (`cg_link_model_of`).

### Por dónde pasa

- **Cliente:** `type = ...` en cada `[link NOMBRE]` (`config.c`, validado). `weights_update` guarda el tipo de cada enlace en uso; `cap_tick` arma el `cg_cap_cfg` de cada enlace con su modelo, y la máscara `lossy_hold`, con el costo de cada enlace, va a `cg_cap_eta` (`cg_link_hold`).
- **Protocolo:** el cliente manda el tipo de cada enlace en los bits 20–22 de la cola de bonding de cada sonda (`CG_BOND_TYPE`). Esos bits estaban reservados, «escritos en 0, ignorados al leer». Un par anterior los ignora (para él, `auto`, como antes), y uno nuevo lee 0 (`auto`) de uno anterior. Es el mismo caso que la propia cola de bonding, que se añadió sin versión nueva (comentario de `proto.h`). **No sube `CG_PROTO_VERSION`:** el router de radio habla el protocolo 7 y sale el martes 2026-10-20; los bits 20+ están reservados igual en las versiones 6 y 7. La respuesta del servidor escribe 0.
- **Servidor:** guarda el tipo de cada camino (`P->type`) desde la sonda más nueva y aplica el preset a la bajada de ese camino; `auto` o un tipo que no conoce es el comportamiento de antes.
- **Estado:** `type` en cada enlace (cliente) y camino (servidor); `lossy` y `lossy_noprobe` en `cap_diag`.
- **OpenWrt:** `list uplink_type 'ENLACE=TIPO'`, como `split_weight`; `cengarde-setup` escribe `type = ...` en cada `[link]` (nada para `auto`) y deja fuera, con un mensaje en el log, lo que no es válido. LuCI muestra un desplegable de tipo debajo de la lista de enlaces por cada enlace elegido, en redundancia y en bonding.
- **Laboratorio:** `BOND_LINK_TYPES="starlink cellular cellular"` (o `LINK_TYPES` en `lab.sh`) fija el tipo de los enlaces 1..N del cliente.

## Hallazgos

Todas las corridas: `labq.py` en este PC (6 laboratorios, y 3 en la verificación final), perfil de campo (`split` con `split_eta`, `capacity_weights`, `split_spread_ms = 250`, `split_spread_copy = no`, rescate; `governor` en el VPS), cubic, 30 s, `BOND_NETEM_INFLIGHT=1`, n = 8 por brazo. El control es la unión de los brazos sin tipos (el motor de antes `27c0c41` y `auto`/`auto2` del nuevo, idénticos en código: su dispersión entre ellos es el ruido). p: prueba de permutación sobre la diferencia de medianas. «1200 B RTT»: mediana del ping de 1200 bytes durante la transferencia, la cola que ve un paquete grande. Mbit/s en mediana. [medido en laboratorio]

### 1. El enlace Starlink retenido como último recurso: la bajada sube, la subida cae y las colas celulares se llenan

`sl`: `type = starlink` en el enlace 1, con `lossy_noprobe` y `lossy_hold` y la penalización de siempre (lo que pasa de la cola retenida va a otro enlace aunque espere segundos allí; es `eta_buffer` en un solo enlace). `slnp`: solo `lossy_noprobe`. `slm10`: la retención con 10 ms más de margen.

| Caso | Sentido | Control (n) | `sl` | `slnp` | `slm10` | 1200 B RTT control / `sl` |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `shallowq` | bajada | 42,7 (24) | 46,1 (+8 %) | 40,9 (−4 %) | 46,7 (+9 %) | 52 / 194 ms |
| `shallowq` | subida | 40,4 (24) | 33,7 (**−17 %**, p < 0,001) | 39,1 (−3 %) | 45,3 (+12 %) | 52 / 220 ms |
| `starsat` | bajada | 30,2 (23) | 42,6 (+41 %) | 33,5 (+11 %, ns) | 41,3 (+37 %) | 72 / 186 ms |
| `starsat` | subida | 39,1 (24) | 31,9 (−18 %) | 39,1 (0 %) | 36,5 (−7 %, ns) | 64 / 202 ms |

- No sondear al enlace que pierde, solo, no cambia nada (`slnp`): lo que cuenta es la retención.
- Con la retención, lo que no cabe en Starlink va **entero** a los celulares: su cola de 300 ms se llena (el ping grande pasa de 52 a ~200 ms) y la subida pierde. Es lo que costaba `eta_buffer` global en `shallowq`, y no se arregla con un margen sobre la cola de Starlink (`slm10`).

### 2. Un costo por pasarse, no un último recurso

`over_us` (`eta.h`) por enlace: un paquete que pasaría la cola retenida cuesta ese tiempo más en vez de 10 s, así que va a otro enlace solo mientras llegaría por él a menos de ese costo de lo que llegaría por Starlink; si no, va a Starlink, que lo descarta y TCP lo ve.

| Caso | Sentido | Control (n) | `sl` (último recurso) | 40 ms | 100 ms | 1200 B RTT control / 40 / 100 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `shallowq` | bajada | 41,6 (16) | 40,0 (−4 %) | **56,8 (+36 %)**, p < 0,001 | 60,4 (+45 %) | 52 / 100 / 130 ms |
| `shallowq` | subida | 40,3 (16) | 37,7 (−6 %) | **51,1 (+27 %)**, p < 0,001 | 57,9 (+44 %) | 53 / 97 / 126 ms |
| `starsat` | bajada | 36,0 (14) | 38,9 (+8 %) | **52,1 (+45 %)**, p = 0,01 | 56,8 (+58 %) | 71 / 87 / 123 ms |
| `starsat` | subida | 37,2 (16) | 32,7 (−12 %) | **49,1 (+32 %)**, p = 0,001 | 53,5 (+44 %) | 65 / 84 / 117 ms |

Con 40 ms el reparto ya supera a Starlink solo (48,9 en la ablación) en los dos casos de campo.

### 3. Lo que cuesta donde no hace falta

`shallowqd` (Starlink de cola corta con los celulares a su mismo retardo) y `shallowqdeep` (Starlink con cola profunda), donde el reparto ya sumaba:

| Caso | Sentido | Control (n) | 40 ms | 100 ms | 200 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| `shallowqd` | bajada | 65,2 (16) | 66,3 (+2 %) | 58,6 (**−10 %**, p = 0,003) | 40,7 (−38 %) |
| `shallowqd` | subida | 66,6 (16) | 69,1 (+4 %, p = 0,04) | 62,8 (−6 %) | 53,2 (−20 %) |
| `shallowqdeep` | bajada | 73,5 (16) | 74,2 (+1 %) | 71,7 (−2 %) | 55,3 (−25 %) |
| `shallowqdeep` | subida | 71,0 (16) | 74,3 (+5 %, p < 0,001) | 71,8 (+1 %) | 48,7 (−31 %) |

Cuanto mayor el costo, más se llenan las colas celulares, y con retardos iguales eso ya no compensa. **40 ms gana donde hace falta y no cuesta donde no**: es el preset de `starlink`. 100 ms gana más en `shallowq` pero cuesta en `shallowqd`; queda anotado para la búsqueda de presets.

### 4. El preset que queda

`starlink` = `lossy_noprobe` + `lossy_hold` + `hold_over_ms = 40`, sin corte por pérdida (`loss_pct = 0`): en el A/B de la sesión principal (`~/.claude/jobs/1c68aec3/tmp/sat3/`, n = 2, global), `eta_buffer` con `capacity_loss_pct = 5` bajó `shallowq` un 23 % de bajada y un 10 % de subida; no entra sin medirlo por enlace. `eta_buffer` global conserva su último recurso, como antes.

### 5. Verificación final

Con la revisión final (`a4d07c9`: el preset en `linkmodel.h`, no un parche de `eta.h`), 3 laboratorios, `BOND_LINK_TYPES="starlink cellular cellular"` (`finsl`) frente a dos brazos sin tipos de la misma revisión (`fin`, `fin2`), n = 8 cada uno:

| Caso | Sentido | Control (n = 16) | `finsl` | p | 1200 B RTT control / `finsl` | `fin2` frente a `fin` (control nulo) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `shallowq` | bajada | 42,5 | **56,3 (+33 %)** | < 0,001 | 54 / 102 ms | −3 % (p = 0,44) |
| `shallowq` | subida | 40,9 | **51,3 (+26 %)** | < 0,001 | 53 / 99 ms | −2 % (p = 0,32) |
| `starsat` | bajada | 34,5 | **52,5 (+52 %)** | 0,001 | 70 / 83 ms | −7 % (p = 0,61) |
| `starsat` | subida | 33,2 | **45,3 (+36 %)** | 0,005 | 68 / 81 ms | −1 % (p = 0,92) |
| `shallowqd` | bajada | 62,5 | 66,8 (+7 %) | 0,01 | 53 / 95 ms | +4 % (p = 0,15) |
| `shallowqd` | subida | 65,5 | 67,4 (+3 %) | 0,15 | 48 / 86 ms | +2 % (p = 0,30) |

El costo que queda es de latencia: el ping de 1200 bytes pasa de ~50 a ~100 ms en `shallowq` y `shallowqd`, porque los celulares cargan más y sus colas son de 300 ms en este laboratorio. Es el precio de usarlos; con `starsat` casi no se nota (70 → 83 ms).

Sin regresión donde no hay Starlink (`het`, `bloat`, `ltespikerand`, el motor de antes `27c0c41` frente a la revisión final con todos los enlaces en `auto` y en `cellular`, n = 8):

| Caso | Sentido | Antes (`27c0c41`) | todo `auto` | todo `cellular` |
| --- | --- | ---: | ---: | ---: |
| `het` | bajada | 67,1 | 66,4 (−1 %) | 65,5 (−2 %) |
| `het` | subida | 65,7 | 66,5 (+1 %) | 67,7 (+3 %, p = 0,02) |
| `bloat` | bajada | 35,4 | 35,4 (0 %) | 36,3 (+3 %) |
| `bloat` | subida | 34,3 | 34,5 (0 %) | 35,2 (+2 %) |
| `ltespikerand` | bajada | 66,1 | 65,4 (−1 %) | 66,2 (0 %) |
| `ltespikerand` | subida | 65,4 | 67,4 (+3 %) | 64,7 (−1 %) |

Todo dentro de ±3 %. `auto` y `cellular` hacen exactamente lo mismo que el motor de antes (sin seguimiento de pérdida ni retención; los bits del tipo en 0 o en 1 no cambian nada en el servidor), así que el +3 % con p = 0,02 de `het` en subida es ruido: con 18 comparaciones, una por debajo de 0,05 es lo esperable.

**Compatibilidad, en el laboratorio:** un cliente nuevo (rama `claude/link-model-field`, protocolo 7) contra un servidor de `claude/field-tickle` (`1739127`, sin el tipo), y al revés, en `shallowq` con `starlink cellular cellular`: los dos túneles andan (42,6 y 42,5 Mbit/s de bajada, lo de siempre sin el preset en el VPS). El servidor viejo ignora los bits; el cliente viejo avisa `unknown key 'type'` y su servidor nuevo ve `auto` en cada camino.

### 6. Las correcciones mixtas de la auditoría, por tipo (2026-10-19, tarde)

Las correcciones de la auditoría de relojería que habían salido «mixtas» (`~/campo/2026-10-17/relojeria/analysis.md`, `~/campo/2026-10-18/hallazgos.md`) pasaron a ser campos de `struct cg_link_model`: cada lazo las lee **por enlace**, la perilla global las fija en todos los enlaces encima del preset, y `exp_link_types` limita las perillas globales a algunos tipos. Así cada una se probó solo en el tipo de enlace que la hipótesis señalaba.

| Mecanismo | Perilla | Campo del modelo | Dónde actúa |
| --- | --- | --- | --- |
| C3, permanencia tras volver por una recuperación, con guarda de cola propia | `split_dwell_ms`, `split_dwell_oq_ms` | `dwell_ms`, `dwell_oq_ms` | G4 (`split.h`), por enlace |
| C5, una salida de G4 por cola cuenta para el arranque rápido (solo si sobrestimó) | `cap_g4_episode` | `g4_episode` | capacidad, por enlace |
| C9, el backlog del ETA sobre una cola vieja | `eta_keep`, `eta_keep_ms` | `eta_keep`, `eta_keep_ms` | ETA, por enlace |
| C11, histéresis del «lleno» del rescate (modo 2: solo mientras la cola no sube) | `rescue_full_hyst` | `rfull_hyst` | rescate, por enlace |
| #33, la cola prevista por su tendencia (modo 1: solo si sube) | `split_qpred_ms`, `split_qpred_mode` | `qpred_ms`, `qpred_mode` | ETA, por enlace |
| Copias a tiempo en redundancia | `redund_pace_ms` | `pace_ms`; `pace_peers_ms` para los demás enlaces de la sesión | `cg_eta_copies` (`eta.h`), por enlace |
| Corte de capacidad por pérdida | `capacity_loss_pct` | `loss_pct` | capacidad, por enlace |
| C10, el rescate del cliente por el OWD de subida | `rescue_by_owd` | — (global, solo el cliente) | rescate |

**Método.** `labq.py`, solo este PC, `--node local:6`, perfil de campo, cubic, 30 s, `BOND_NETEM_INFLIGHT=1`. Control: la revisión nueva sin perillas (`5c14ff3`). Control nulo: el PR #87 (`0751504`), compilado aparte; también sirve de control de «sin cambio», porque sin perillas los dos hacen lo mismo. Mann-Whitney de dos colas. Cribado con n = 8. Lo celular se juntó con las rondas del 2026-10-18 (`~/campo/2026-10-18/clk2/` A, B, D), donde el control era `c6c7` y `x0` el nulo: con `auto` y `cellular`, el código que corre es el mismo. Lo que entra se extendió a n = 16, y se verificó con la revisión del preset (`0706761`, n = 8 más). Planes y salidas en `/tmp/lmx` (scratch de la sesión). Criterio: entra con más del 5 % de caudal, retrans por debajo de 0,5× o una ganancia clara de latencia, con p < 0,05 y sin costar más del 5 % en los demás casos de su tipo. [medido en laboratorio]

#### Celular (`BOND_LINK_TYPES="cellular cellular cellular"`, bonding; n = 16–24 juntando rondas)

Razón frente al control (p). Lo significativo, en negrita:

| Mecanismo | Lo mejor | Lo peor | Veredicto |
| --- | --- | --- | --- |
| #33 `qp300r` (300 ms, solo cola que sube) | `bloat` subida retrans **0,60× (p = 0,009)**; `spikeloss` bajada retrans 0,69× (p = 0,016, n = 8: 54 → 38) | `het` bajada retrans **3,84× (p = 0,015)**; `deepq` subida caudal **0,98× (p = 0,001)** y bajada 0,99× (p = 0,046) | fuera: no llega a 0,5× y cuesta en `het` |
| C5 `c5g2` (solo si sobrestimó) | `lossy` bajada 1,11× (p = 0,07) | `bloat` bajada p99 del ping grande 1,12× (p = 0,04); `deepq` bajada retrans 1,19× (p = 0,05) | fuera: nada significativo a favor |
| C11 `c11g` (modo 2) | `spikeloss` 1,04–1,07× (p ≥ 0,18; el +59 % de la primera ronda era ruido: el nulo mueve `spikeloss` ±15–29 %) | nada significativo | fuera: neutro |
| C3 `dw300q60` (300 ms, cola propia ≤ 60 ms) | `het` subida retrans 0,44× (p = 0,12) | `lossy` bajada 0,81× (p = 0,08, n = 8) | fuera: neutro; lo del `ltespike` periódico era un artefacto |
| C9 `eta_keep = 2` (ronda del 2026-10-18, n = 8) | `bloat` **+19–21 %** (p = 0,001) | RTT p50 de 42 a 150–200 ms en `bloat` y de 55 a 100 en `deepq`; `het` subida −5 % | fuera: cambia latencia por caudal, y `het` pierde |

Ninguna mejora lo celular. `cellular` sigue con los ceros de `auto`.

#### Starlink con dos celulares, bonding (`BOND_LINK_TYPES="starlink cellular cellular"`, perilla solo en el enlace Starlink, n = 8)

| Mecanismo | `shallowq` ↓/↑ | `starsat` ↓/↑ | `shallowqd` ↓/↑ | `shallowqdeep` ↓/↑ | Veredicto |
| --- | --- | --- | --- | --- | --- |
| Corte por pérdida, `capacity_loss_pct = 5` | 1,08 (p = 0,02) / 0,97 | 1,01 / 0,94 (p = 0,13) | 1,01 / 0,98 | 1,02 / 0,98 | fuera: +8 % en un solo sentido, con el nulo en +5 %, y cuesta 6 % en `starsat` subida |
| C9, `eta_keep = 2` | **0,77 / 0,68** | **0,84 / 0,69** | **0,57 / 0,52** | **0,84 / 0,60** | fuera: −16 a −48 %, todo p ≤ 0,04 (el ETA cree a Starlink más lleno y vacía el enlace rápido) |
| C11, `rescue_full_hyst = 2` | 1,04 / 0,99 | 1,07 / 1,02 | 1,02 / 1,00 | 1,04 / 1,01 (retrans 0,19×, p = 0,03) | fuera: un solo caso, el de cola profunda (no es Starlink), y retrans 1,89× en `starsat` subida (p = 0,08) |
| Control nulo (`0751504`) | 1,05 / 1,02 | 1,04 / 0,99 | 1,02 / 1,04 | 1,03 / 1,00 | — |

Con la pérdida tomada solo en la frontera (`0751504`), el corte por pérdida ya no se dispara a lo loco, pero tampoco suma: lo que hace falta lo hace la retención del preset.

#### Redundancia: copias a tiempo (`redund_pace_ms`)

Cribado con la perilla solo en los enlaces celulares (`exp_link_types = cellular`), n = 8:

| Sesión | Caso | Caudal 30 ms / 60 ms | p99 del ping grande, 60 ms |
| --- | --- | ---: | ---: |
| Starlink + 2 celulares | `starsat` | 0,89–1,01 / 1,02–1,06 | 0,24–0,27× (p = 0,001) |
| solo celulares | `het` | **0,82–0,92 / 0,82–0,88** (p = 0,001) | 1,00 |
| solo celulares | `bloat` | **0,92–0,93 / 0,95–0,99** (p = 0,001) | 0,23–0,28×, retrans 3–7× |

Gana latencia con Starlink. En sesiones solo celulares, `het` pierde 8–18 %. De ahí la guarda `pace_peers_ms`: es el enlace `starlink` el que pide que **los demás enlaces de su sesión** lleven copia solo si llegaría a menos de 60 ms de la primera. Una sesión sin Starlink no cambia. 60 ms y no 30: la misma latencia, y en `starsat` 1,02–1,06 de caudal frente a 0,89. Junto (cribado y extensión con la perilla, más la verificación con el preset en `0706761`), n = 24 por brazo (16 del nulo):

| Caso | Sentido | Mbit/s control → preset (×, p) | Control nulo × (p) | Retrans × (p) | 1200 B p50 | 1200 B p99 control → preset (×, p) |
| --- | --- | --- | --- | --- | --- | --- |
| `shallowq` | bajada | 53,5 → 53,5 (1,00) | 1,00 (0,19) | 1,27 (< 0,001) | 54 → 54 ms | 58 → 58 ms (1,00) |
| `shallowq` | subida | 53,4 → 53,4 (1,00) | 1,00 (0,60) | 1,95 (< 0,001) | 54 → 53 ms | 58 → 58 ms (1,00) |
| `starsat` | bajada | 52,0 → 51,4 (0,99, p = 0,85) | 0,88 (0,09) | 1,08 (0,05) | 58 → 55 ms | **361 → 97 ms (0,27, p < 0,001)** |
| `starsat` | subida | 53,1 → 49,5 (0,93, p = 0,15) | 0,86 (0,02) | 1,05 (0,24) | 57 → 55 ms | **272 → 92 ms (0,34, p < 0,001)** |
| `shallowqd` | bajada | 53,5 → 51,5 (**0,96**, p < 0,001) | 1,00 (0,20) | 2,75 (< 0,001) | 54 → 46 ms | 58 → 57 ms (0,99) |
| `shallowqd` | subida | 53,4 → 51,2 (**0,96**, p < 0,001) | 1,00 (0,26) | 3,05 (< 0,001) | 54 → 44 ms | 58 → 57 ms (0,98) |
| `shallowqdeep` | bajada | 53,5 → 53,3 (1,00) | 1,00 (0,52) | 2,55 (< 0,001) | 281 → 138 ms | **358 → 232 ms (0,65, p < 0,001)** |
| `shallowqdeep` | subida | 53,4 → 53,3 (1,00) | 1,00 (0,94) | 2,42 (< 0,001) | 279 → 168 ms | **358 → 256 ms (0,71, p < 0,001)** |

- **Lo que gana:** el p99 del ping grande en `starsat`, de ~360 a ~95 ms. Es lo que pasa en campo cuando Starlink corta: las copias celulares ya no esperan detrás de 300 ms de cola. En `shallowqdeep`, también la mediana.
- **Lo que cuesta:**
  - caudal: −4 % en `shallowqd`, y el −7 % de `starsat` subida, no significativo (el nulo solo mueve `starsat` −14 %);
  - retransmisiones: 1,1–3× (de 20 a 40 en `shallowq`, de 60 a 150 en `shallowqd`), por los huecos que la copia celular ya no tapa.
- La verificación con la revisión del preset sola (`0706761`, n = 8) dio `starsat` subida −14 % (p = 0,03). Dentro del nulo de esta tanda (−14 %, p = 0,02), y junto con el resto queda en −7 % (p = 0,15).

**Sin cambio donde no toca** (`0706761` frente al PR #87, n = 8): todo `auto` en bonding, `het` 1,01/0,99 y `shallowq` 0,96/1,01 (p ≥ 0,27); `starlink cellular cellular` en bonding, `shallowq` 1,00/1,04 y `starsat` 0,88/1,05 (p ≥ 0,13): el preset solo toca las copias de redundancia; todo `cellular` en redundancia, `het` 1,00/1,00.

**El preset de `starlink` queda en:** `lossy_noprobe` + `lossy_hold` + `hold_over_ms = 40` + `pace_peers_ms = 60`. Lo demás queda como ganchos apagados: las perillas globales, `exp_link_types` y los campos del modelo en cero.

## Lo que falta

La visión: con un modelo por tipo, el reparto sabe cómo se comporta cada rama y encuentra la mejor manera de repartir entre un Starlink y celulares sin oscilar.

- **Starlink:**
  - medir su frontera: la tasa a la que empieza a perder, y un techo de capacidad aprendido del comienzo de la pérdida y no de la cola;
  - ser cauto en cuanto empieza a perder;
  - los microcortes y cambios de satélite: las ranuras de reconfiguración de 15 s, que se informan a los :12, :27, :42 y :57 s de cada minuto (UTC), por verificar con el registrador de «nostalgica»; armar la FEC o mover tráfico justo antes del borde de una ranura;
  - modelar la rampa de retardo dentro de una ranura (geometría, curvatura de la Tierra);
  - un detector que reconozca el diente de sierra de 15 s, para no depender de que el usuario elija el tipo.
- **Celular:**
  - saber cómo responde el planificador de la celda (PF, RR o tasa máxima): un detector, con ns-3 5G-LENA como referencia;
  - colas profundas y bufferbloat;
  - pérdida correlacionada con señal débil (RSRP/SINR leídos por AT en los módems).
- **Presets afinados por tipo** con la búsqueda en PyTorch (`~/campo/2026-10-17/gpusim/`), validados en laboratorio con n ≥ 8 y control nulo, y después en campo.
- Cada uno entra como campo de `struct cg_link_model` cuando mida como ganancia.

## Cambios

- 2026-10-19: creada.
- 2026-10-19 (tarde): las correcciones mixtas de la auditoría por tipo de enlace (sección 6, rama `claude/link-model-presets`); `pace_peers_ms = 60` en el preset de `starlink`.
