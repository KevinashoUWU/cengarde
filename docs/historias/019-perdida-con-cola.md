# 019 — Capacidad: pérdida con cola y cola permanente

- **Fecha:** 2026-10-23
- **Estado:** vigente
- **Fuentes:** `engine/src/capacity.h` (`cg_cap_lossq`, `cg_cap_stand`,
  `cg_cap_room`), `engine/tests/test_capacity.c`
  (`test_capacity_lossq`, `_stand`, `_room`); trazas de ns-3 con verdad
  conocida (`~/sdr/ns3/engranes/`, su `README.md`; historia 018) repetidas
  contra el motor en `~/sdr/resultados/lqs/` (`replay_eng.c`, `run.py`,
  `an.py`, `an_t3b.py`); celda virtual calibrada (historia 016, «Cola honda
  de operador»; `~/sdr/resultados/lqs/cell/`, `cell_an.py`); laboratorio de
  bonding (`~/campo/2026-10-23/lqs/`, `plan.json`, `all.jsonl`).

## TL;DR

- Dos fallas del estimador de capacidad medidas en ns-3 con verdad conocida:
  **(1)** con el búfer corto de un operador delante de un emisor que se
  adapta (TCP), la cola no pasa de 20–28 ms, el enlace nunca está «cargado»
  frente a la capacidad vieja y la pérdida (~3 %) no alcanza: la capacidad
  queda 3–6 veces arriba 40 s (+275 a +510 %); **(2)** una cola que dura
  más que `governor_window_ms` se vuelve base y el arranque rápido devuelve
  la capacidad a la mitad de la mejor (+523 %; choque C5 de la auditoría).
- Dos perillas nuevas, apagadas por omisión, en los dos extremos:
  `capacity_loss_queue_ms` (sugerido 10: pierde ≥ 1 %, manda más de lo que
  entrega y la cola sobre el mínimo de 60 s llega a eso, dos muestras →
  lleno a cualquier carga) y `capacity_stand` (1: una cola permanente frena
  el arranque rápido; 2: además el arranque rápido va antes si el enlace
  muestra lugar).
- **ns-3, el motor:** +5,10 → −0,02 y +2,75 → −0,02 en 1,0–1,4 s; la cola
  permanente +5,23 → +0,03; sin un solo corte en las 24 trazas de pérdida
  aleatoria sin cola; con otros usuarios a ráfagas (t3b) el tiempo
  sobreestimado baja de 15 % a 6 %. Igual o mejor que los prototipos.
- **Celda calibrada (Entel, WOM):** la capacidad se corrige antes (red.
  Entel: 1.ª vez bajo 1,5 C a los 13 s → 1,9 s; bonding Entel: queda bajo
  1,5 C desde 34 s → 6 s), pero el TCP no cambia (p ≥ 0,08, n = 3).
- **Laboratorio** (`shallowq`, `starsat`, `het`, `dominant`, `lossy`, subida
  y bajada, n = 8, con control nulo): ningún cambio de caudal ni de
  retransmisiones fuera de lo que mueve el control nulo; el corte por
  pérdida con cola se dispara 0–3 veces por corrida, en Starlink solo
  cargado (2 de 20 corridas).
- **Decisión:** las dos quedan **apagadas** por omisión: arreglan el
  estimador donde ns-3 tiene verdad, pero ni la celda ni el laboratorio
  muestran ganancia de TCP. Siguiente paso: un brazo de terreno con
  `capacity_loss_queue_ms = 10` y `capacity_stand = 1`.

## Contexto

La capacidad (`capacity.h`, historia 013) solo baja cuando el enlace está
lleno: cola sobre `capacity_sat_ms` cargado (2 muestras) o sin carga (4),
entregas cortas, o pérdida cargado (`capacity_loss_pct`, apagada). El
arranque rápido la devuelve a la mitad de su mejor tras 10 s sin llenarse.
ns-3 (LTE de LENA, historia 018) reprodujo enlaces con verdad conocida y un
harness que lee los encabezados del motor; dos casos fallaban y el informe
proponía dos prototipos fuera del motor, `lossq` y `stand`. Esta historia
los lleva al motor y los mide.

## Diseño

Funciones puras en `capacity.h`, llamadas por `cg_cap_tick` en cada
muestra (≥ 500 ms); nada por paquete.

- **`cg_cap_lossq` (`capacity_loss_queue_ms`):** una muestra cuenta si
  perdió al menos el 1 % (`CG_CAP_LQ_PCT`) de lo que pudo llegar (la pérdida
  por secuencia de siempre, al menos 64 paquetes), la cola sobre el mínimo
  de 60 s (`ql_us`, el sensor del rescate) llegó a la perilla en algún tick
  desde la anterior, y se le mandó al menos 1 % más de lo que entregó. Dos
  seguidas (`CG_CAP_LQ_N`): la capacidad es lo que entrega (suavizado 1/4
  desde la segunda, como `sat_bps`), corte `loss`, y el enlace no se sondea
  mientras dure. Va después de los otros cortes y puede bajar más que uno
  lleno (cuyo promedio cae despacio: 0,87 → 0,25 Mb/s en 4 s contra 0,02–0,17
  entregados, ns-3 con desvanecimiento).
  - «Manda más de lo que entrega» salió de ns-3: una CBR de 18 y luego 4 Mb/s
    sobre 12,6 terminaba de vaciar su cola con pérdida vieja contada por
    secuencia y la capacidad caía a 4–6,9 (bajo ×0,67 el 71 % del tiempo);
    con la condición, 0 %.
  - Starlink: un enlace con el preset (`lossy_noprobe`/`lossy_hold`,
    `cg_link_tracks_loss`) solo cuenta la muestra cargado (`lossq_loaded`):
    su retardo salta por ranura y su pérdida a carga liviana es la del plato;
    ya lo retiene a su cola más alta el ETA. Las muestras con un borde de
    ranura (reloj de ranuras) no cuentan pérdida.
- **`cg_cap_stand` (`capacity_stand ≥ 1`):** `ql_us` sobre `capacity_sat_ms`
  4 muestras seguidas (`CG_CAP_SAT_LONG`) es una cola permanente: cuenta como
  muestra llena para el arranque rápido (`full_ms`), aunque `queue_us` (10 s)
  ya la tenga de base.
- **`cg_cap_room` (`capacity_stand = 2`):** dos muestras seguidas que
  entregan al menos 9/8 de la capacidad que había al empezar la racha, sin
  pérdida (≤ 0,3 %) y con `ql_us` ≤ `capacity_sat_ms`, dejan ir el arranque
  rápido sin esperar sus 10 s. Comparar contra la capacidad al empezar la
  racha y no contra la de cada muestra: la capacidad sigue a la entrega
  suavizada y nunca quedaba 9/8 abajo dos veces.
- Estado: `cut_lossq` y `restore_room` en `cap_diag`; los cortes van a las
  decisiones como los de pérdida (`cut_short`), las vueltas como
  `restore_rest`.

## Hallazgos

### ns-3 contra el motor (108 trazas × 9 perfiles)

`replay_eng.c` es el harness de la historia 018 compilado contra esta rama,
con los perfiles del motor (`eng_lossq`: 10 ms; `eng_lqs`: más
`capacity_stand = 1`; `eng_lqs2`: con 2) al lado de los prototipos. El
perfil `default` y los prototipos dan exactamente lo de la corrida original
(0 diferencias). Error durante el cambio frente a lo observable, mediana de
3 semillas (entre paréntesis, primer corte en s):

| Escenario | default | prototipo `lossq_stand` | `eng_lossq` | `eng_lqs2` |
|---|---|---|---|---|
| a, 10 KB, AIMD | +5,10 (—) | −0,02 (1,0) | −0,02 (1,0) | −0,02 (1,0) |
| a, 10 KB, CBR | +0,05 (2,0) | −0,01 (0,9) | −0,01 (0,9) | −0,01 (0,9) |
| b, 10 KB, AIMD | +2,75 (—) | −0,02 (1,4) | −0,02 (1,4) | −0,02 (1,4) |
| c, 10 KB, AIMD | +5,23 (1,0) | +0,03 (1,0) | +5,23 (1,0) | +0,03 (1,0) |
| c, 512 KB, AIMD | +0,11 (2,4) | +0,06 (2,4) | +0,02 (2,4) | +0,02 (2,4) |
| a/b/c, 512 KB, CBR | −0,01 a +0,00 | igual | igual | igual |

- Pérdida aleatoria sin cola (d: iid y Gilbert-Elliott, 1 y 3 %, 24
  trazas): 0 cortes con todos los perfiles.
- La cola permanente (c, 10 KB) la arregla `capacity_stand`, no la pérdida:
  `eng_lossq` solo queda en +5,23.
- **El costo de 20,5 s de vuelta** del informe de ns-3 es del emisor de la
  traza (lazo abierto): el AIMD de la traza tarda ~15 s en volver a pasar de
  8 Mb/s, y la capacidad nunca queda bajo lo que el enlace mostró (error
  después −0,01 a −0,05). Lo que sí se acorta es la vuelta a la mitad de la
  mejor: con `capacity_stand = 1` 10,1–10,5 s (a) y 5,8–10,3 s (c); con 2,
  1,8–2,2 s y 2,6–3,1 s.
- Otros usuarios a ráfagas (t3b: 0/62 Mb/s con media de 4 s, o nuestra CBR
  4/18; 36 trazas sin verdad ajena: con cola o pérdida la entrega es la
  capacidad, sin ellas la referencia es lo más entregado en 10 s): tiempo
  sobreestimado (× 1,5) 0,151 → 0,059, subestimado (× 0,67) 0,197 → 0,222;
  todo el aumento es de `oth_b10_aimd` (0 → 0,31): cuando los otros se van,
  la capacidad sigue a la entrega del AIMD, que vuelve a crecer despacio (en
  lazo cerrado lo sube el sondeo). El prototipo: 0,055 / 0,237.
- Con desvanecimiento EPA (36 trazas que ya sobrecargaban antes del
  cambio): sobre 0,209 → 0,178, bajo 0,249 → 0,277.

### Celda calibrada (`cengarde.py`, 23-10)

La celda de la historia 016 con la cola honda: el teléfono empieza bueno
(ganancia 1) y a los 5 s cae a la ganancia del perfil (`entel_b2`: C ≈ 1,33
Mb/s, cola de 220 KB; `wom`: C ≈ 5,25, 2 MB); el fijo de siempre (10 Mb/s,
1 %). 40 s de TCP cubic de subida, n = 3. `bond` es el perfil de terreno,
`red1` la redundancia con `min_active_links = 1` (con dos enlaces, el
mínimo de 2 no deja silenciar); `x`: con `capacity_loss_queue_ms = 10` y
`capacity_stand = 2` en los dos extremos. Error de la capacidad del LTE
desde 5 s después de la caída contra C; «1.ª» y «queda»: cuándo bajó de
1,5 C por primera vez y desde cuándo no volvió a pasarlo. Medianas [mín–máx].

| Perfil | Modo | TCP Mb/s | Retx | RTT p50/p90 | Error | t > 1,5 C | 1.ª ≤ 1,5 C | Queda ≤ 1,5 C |
|---|---|---|---|---|---|---|---|---|
| entel_b2 | bond | 7,1 [6,0–8,4] | 110 | 31 / 50 | −0,20 | 7 % | 3,0 s | 34,2 [2,9–34,8] s |
| | bondx | 8,1 [6,1–8,2] | 162 | 32 / 50 | −0,38 | 3 % | 3,0 s | 6,1 [5,6–27,9] s |
| | red1 | 5,3 | 189 | 30 / 33 | −0,27 | 25 % | 13,0 s | 13,0 s |
| | red1x | 5,0 | 201 | 30 / 33 | −0,38 | 20 % | 1,9 s | 12,0 s |
| wom | bond | 11,3 | 47 | 40 / 64 | −0,13 | 0 | 0,3 s | 0,3 s |
| | bondx | 11,5 | 48 | 42 / 68 | −0,17 | 0 | 0,3 s | 0,3 s |
| | red1 | 5,2 | 482 | 30 / 33 | −0,04 | 0 | 2,4 s | 2,4 s |
| | red1x | 5,3 | 530 | 30 / 33 | −0,02 | 0 | 1,3 s | 1,3 s |

- Ningún cambio de TCP ni de retransmisiones llega a p < 0,05
  (Mann-Whitney, n = 3; el más bajo, `red1x` frente a `red1` en Entel, 5,0
  frente a 5,3 Mb/s, p = 0,08).
- Entel: en `bond` el arranque rápido devolvió una vez el LTE a 5,9 Mb/s a
  los 18 s de la caída (C 1,33) y otra a 2,6; con las perillas no pasó. En
  `red1x` el corte por pérdida con cola bajó la capacidad a 1,3 en 3 s, donde
  el corte lleno tardaba 8 s en bajar de 6,8 a 2,3.
- WOM: la cola honda la ve el corte lleno de siempre (cola de segundos), y
  las perillas no tienen qué hacer.
- La capacidad del LTE pesa poco en el TCP: en bonding G3/G4 lo sacan del
  reparto en ~1 s y lleva 11 % de los envíos; en redundancia la copia va
  igual (como en la historia 016).

### Laboratorio (`bench/labq.py`, 23-10)

`lab.sh bond` en 5 laboratorios, cubic, 30 s, n = 8, enlaces `starlink
cellular cellular` (el preset de Starlink en el enlace 1). Brazos con el
perfil de terreno (`split`): `soff` y `snull` (sin perillas: el segundo es
el control nulo), `son` (`capacity_loss_queue_ms = 10`, `capacity_stand =
2`) y `slq` (solo la primera); y la redundancia de fábrica sin y con
(`roff`, `ron`). Medianas frente a `soff`, p de Mann-Whitney (plan y
resultados en `~/campo/2026-10-23/lqs/`, `an_split.txt`, `an_redund.txt`):

| Caso | Sentido | soff Mb/s | son | slq | snull | soff retx | son | slq | snull |
|---|---|---|---|---|---|---|---|---|---|
| shallowq | subida | 52,3 | 1,00× (p 0,65) | 1,00× (0,88) | 1,03× (0,43) | 705 | 1,13× (0,57) | 1,87× (0,19) | 1,38× (0,80) |
| shallowq | bajada | 51,3 | 0,99× (0,65) | 1,05× (0,72) | 1,03× (0,96) | 544 | 1,55× (0,33) | 1,39× (0,57) | 1,07× (0,72) |
| starsat | subida | 48,3 | 1,04× (0,65) | 1,05× (0,33) | 1,03× (0,28) | 976 | 1,06× (1,00) | 0,81× (0,44) | 0,98× (0,88) |
| starsat | bajada | 50,4 | 1,10× (0,33) | 1,13× (0,16) | 1,01× (0,38) | 778 | 0,92× (1,00) | 1,25× (0,28) | 1,86× (0,08) |
| het | subida | 58,4 | 1,08× (0,19) | 1,07× (0,44) | 1,11× (0,23) | 153 | 1,55× (0,75) | 0,95× (0,80) | 1,16× (1,00) |
| het | bajada | 68,6 | 0,99× (0,44) | 1,03× (0,44) | 1,01× (0,96) | 278 | 3,92× (0,19) | 0,64× (0,32) | 0,93× (0,80) |
| dominant | subida | 49,1 | 1,07× (0,51) | 1,12× (0,51) | 1,08× (0,65) | 212 | 0,93× (0,80) | 1,01× (1,00) | 0,75× (0,16) |
| dominant | bajada | 215,0 | 1,00× (0,57) | 0,98× (0,88) | 0,95× (0,80) | 2930 | 1,18× (0,28) | 1,23× (0,80) | 0,98× (0,65) |
| lossy | subida | 22,9 | 1,02× (0,71) | 1,03× (0,88) | 0,92× (0,33) | 17 | 1,06× (0,40) | 1,32× (0,03) | 1,09× (0,34) |
| lossy | bajada | 22,1 | 1,20× (0,08) | 1,35× (0,08) | 1,17× (0,19) | 16 | 1,00× (0,92) | 1,25× (0,05) | 1,34× (0,25) |

- Caudal: de 0,98× a 1,35×, ninguno con p < 0,05; el control nulo se mueve
  de 0,92× a 1,17× (`lossy` de bajada es ruidoso: 17–30 Mb/s en `soff`, 22–47 en
  `snull`).
- Retransmisiones: las únicas con p ≤ 0,05 son `slq` en `lossy` (17 → 22 y
  16 → 20 por corrida, pocas en números absolutos). `het` de bajada con
  `son` da 1088 frente a 278 (p 0,19; 69–1780 contra 79–1187): `slq` no lo
  muestra (176), así que si es algo es de `capacity_stand = 2`; queda como
  la razón para preferir `capacity_stand = 1` en terreno.
- Redundancia (`ron` frente a `roff`): 0,93–1,01× de caudal, nada con
  p < 0,05 salvo la cola de `het` de subida (53,7 frente a 54,2, p 0,01).
- Cuántas veces actuó (`son`, 20 corridas más con el estado guardado,
  `diag/`): el corte por pérdida con cola, 0–3 veces por corrida, casi
  siempre en un celular (100–300 ms de cola), que el corte lleno también ve; en
  Starlink 2 veces (`dominant` y `shallowq` de subida, cargado). La vuelta
  por lugar (`restore_room`) no se disparó nunca: en el laboratorio la
  capacidad no queda baja.

## Qué hacemos con esto

- Las dos perillas quedan en el motor, **apagadas**. Arreglan lo que ns-3
  mostró con verdad (el búfer corto con TCP, la cola permanente) sin cortes
  falsos en pérdida aleatoria ni en Starlink, pero en la celda y en el
  laboratorio el TCP no cambia: hoy G3/G4 sacan del reparto al enlace malo
  antes de que su capacidad pese.
- Donde sí deberían pesar es en el terreno, con un Entel de búfer corto y
  `capacity_weights` repartiendo por capacidad: probar un brazo con
  `capacity_loss_queue_ms = 10` y `capacity_stand = 1` (el modo 2 no se
  disparó en el laboratorio y deja la duda de `het` de bajada).

## Pendiente

- Probar en terreno con el registrador (Entel en B2 y WOM a la tarde), con
  `capacity_loss_queue_ms = 10` y `capacity_stand = 2` en un brazo.
- El dueño de la cola (informe de ns-3, tarea 3: la entrega del segundo
  siguiente cae > 15 % → la cola es de otros) no está en el motor: con él,
  la pérdida con cola podría distinguir «me llené yo» de «llegaron otros».

## Cambios

- 2026-10-23: creada.
