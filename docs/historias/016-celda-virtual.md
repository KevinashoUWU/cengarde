# 016 — Celda LTE virtual: el acantilado del SNR

- **Fecha:** 2026-10-22 (cola honda de operador: 2026-10-23)
- **Estado:** vigente
- **Fuentes:** `bench/celda/` (srsRAN 4G `bef8680` con ZMQ, GNU Radio 3.10.12),
  resultados en `~/sdr/resultados/{calibracion,escenario,cengarde}.jsonl` del PC del
  banco (la cola honda: `~/sdr/resultados/cola-honda/tasa.jsonl`); terreno:
  nostalgica 20–22-10 (`cengarde-rec`, `cell-monitor` del cpe1); ns-3:
  `~/sdr/ns3/engranes/README.md` (tarea 5, búfer hondo).

## TL;DR

- Una celda LTE completa corre en el PC (núcleo, estación base, canal de
  GNU Radio y varios teléfonos, cada uno en su netns): `bench/celda/`.
- **El SNR tiene un acantilado.** Con un flujo chico (1 Mb/s, como las copias
  de cengarde), el RTT se mantiene en 20–50 ms hasta ~4 dB; con ~2 dB sube a
  44–84 ms con 8 % de pérdida; con ~1 dB el BLER llega a 64 % y el RTT a
  2,7–4,4 s, aunque no haya nadie más en la celda.
- **La carga de otros usuarios, sola, casi no daña un flujo chico** en srsRAN:
  con la celda pidiendo 45 Mb/s sobre ~25 de capacidad, a 4 dB nuestro RTT
  sigue en 20–50 ms. Cerca del acantilado, la carga sí lo empuja al lado malo.
- Encaja con Entel en B2 (terreno: RSRQ −18, SINR −6 a −2 por antena, RTT
  1–1,5 s, 5–19 % de pérdida): vivía en el borde. En B28 (SINR +13) quedó a
  42–112 ms, ~20 veces mejor (22-10, 11:39).
- **cengarde contra el acantilado** (LTE + un fijo de 10 Mb/s con 1 %):
  el bonding saca al LTE del reparto en 1,2–1,7 s y el TCP del túnel queda
  en 8–11 Mb/s; la redundancia no lo silencia nunca (con dos enlaces,
  `min_active_links = 2` no deja; lo que parecía silenciado eran mudos) y
  el TCP cae a 3,5–4,6 Mb/s (la copia tardía no repara la pérdida del fijo).
- **Silenciar por la cola y caducar la capacidad** (`mute_queue`,
  `capacity_expire_ms`, 22-10): con un enlace menos de mínimo, el LTE se
  silencia a los 3,3 s en vez de 3,8–7,9 s o nunca, y su capacidad vuelve
  baja; pero el TCP no mejora (lo limita el fijo), la vuelta de la señal
  cuesta (espera exponencial, capacidad que sube despacio) y el laboratorio
  queda igual. Las dos quedan como perillas apagadas.
- **Cola honda de operador** (23-10): la cola de subida del módem, en bytes
  (`QUEUE_BYTES`, `srsran-cola.patch`), y dos celdas calibradas contra el
  terreno (`PERFIL=entel_b2`, `PERFIL=wom`, `perfiles.json`). Entel: 5 %
  de pérdida sin cola (BLER del borde) y, con la carga sobre ~1,35 Mb/s,
  RTT 1,2–1,3 s y 8–33 % de pérdida. WOM: ~55 ms hasta ~4,5 Mb/s y, sobre
  ~5,25, 1,1–3 s; con 1,05× encola 20 s sin perder nada.
- Límites: el scheduler de srsRAN es más amable que el de un operador (no
  deja sin concesiones a un teléfono: la cola «de otro» de Entel con nuestra
  carga casi en cero no sale), su SNR no es el SINR del módem (1 antena
  frente a 4) y no hay 4×4 ni 256QAM en subida.

## Contexto

En terreno (historia 015 y nostalgica 20–22-10) los enlaces tienen firmas
distintas: Starlink pierde sin hacer cola, WOM encola sin perder (codo cerca
de 5 Mb/s) y Entel en B2 encolaba con nuestra carga casi en cero y además
perdía. El netem del laboratorio no reproduce el porqué: SNR, MCS, HARQ y un
scheduler compartido. Se armó una celda virtual para eso.

## Hallazgos

### La celda

- srsRAN 4G compilado con ZMQ (Debian no lo empaqueta; gcc 14 obliga a
  `-DENABLE_WERROR=OFF`). Sin ruido, el ping del teléfono al núcleo da
  19–37 ms, como un LTE real.
- Las muestras de srsRAN por ZMQ son grandes (potencia media 6·10⁴–2·10⁵):
  los ruidos que importan van de ~300 a ~5000. Los primeros barridos (hasta
  1000) no movían nada por eso.
- **Modelo de subida:** el primero sumaba el ruido de cada teléfono antes de
  la estación base, y uno débil empeoraba a todos (el otro usuario bajó de
  24 a 6 dB). Corregido: ganancia por teléfono y un piso de ruido común.

### Calibración, un teléfono, 10 MHz (`calibrar.py`)

| Ruido | SNR | MCS | BLER | Capacidad | RTT con 1 Mb/s (p50/p90/máx) | RTT al 80 % |
|---|---|---|---|---|---|---|
| 0 | 48,7 dB | 23 | 0 | 21,4 Mb/s | 16 / 21 / 31 ms | 17 / 18 / 21 ms |
| 400 | 15,3 | 19 | 0 | 16,5 | 17 / 25 / 28 | 17 / 19 / 26 |
| 800 | 9,6 | 14 | 0 | 10,8 | 22 / 35 / 40 | 16 / 22 / 35 |
| 1600 | 4,8 | 7 | 0 | 5,4 | 19 / 33 / 57 | 19 / 184 / 307 |
| 3000 | 2,1 | 0 | 5 % | 1,45 | 20 / 25 / 31 | 328 / 466 / 482 (44 % perdido) |
| 4000 | 1,3 | 3 | 50 % | 0,84 | 2,5 s / 4,4 s / 4,6 s | 0,8 / 1,8 / 2,0 s |

Un solo barrido, n = 1 por punto: hay ruido entre puntos vecinos (2000 y
2500 salieron mejor que 1600 con carga). Hay que repetir cada punto.

### B2 de Entel, 15 MHz, dos teléfonos (`escenario.py`)

| Nuestro SNR | Otros | Nuestro RTT p50/p90/máx | Pérdida (ping / UDP) | Nuestro MCS / BLER |
|---|---|---|---|---|
| ~28 dB | 0–25 Mb/s | 18 / 27–35 / 32–43 ms | 0 / 0 % | 20 / 0 % |
| ~17 dB | 25 Mb/s | 23 / 48 / 69 ms | 6 / 4 % | 16 / 7 % |
| ~4 dB | 45 Mb/s | 20 / 34 / 52 ms | 2 / 3 % | 4 / 5 % |
| ~2 dB | 45 Mb/s | 44 / 78 / 84 ms | 8 % / — | sale del reporte |
| ~1 dB | 0 | 2,7 s / 4,3 s / 4,4 s | 40 / 25 % | 1 / 64 % |
| ~1 dB | 25–45 Mb/s | — | 88–98 % | se desconecta |

### cengarde contra el acantilado (`cengarde.py`, 22-10)

Cliente en `ue1` con dos enlaces: `lte` (el teléfono 0, `type = cellular`)
y `fijo` (veth con netem: 15 ms por sentido, 10 Mb/s, 1 % de pérdida,
~110 ms de cola); servidor en el netns raíz, túnel nativo. 10 MHz, piso
300. TCP cubic de subida 20 s; a los 3 s la ganancia del teléfono baja de 1
al valor de la fila. `redund`: las perillas de fábrica; `bond`: el perfil
de terreno (`split = on`, `split_eta`, `capacity_weights`, `governor` en el
servidor, `split_spread_ms = 250`...). Medianas de n = 3 (el TCP «cola» es
el promedio desde 3 s después de la caída; «fuera» es cuándo el LTE quedó
silenciado o fuera del reparto por primera vez, y qué parte del tiempo;
«parte LTE», su parte de los envíos después de la caída).

| Ganancia | SNR / BLER | Modo | TCP (cola) | Retx | RTT túnel p50/p90 | Fuera: s / tiempo | Parte LTE | Capacidad LTE |
|---|---|---|---|---|---|---|---|---|
| 1 | 17,7 dB / 0 | redund | 19,0 (20,2) Mb/s | 249 | 90 / 123 ms | nunca | 50 % | 20,9 Mb/s |
| | | bond | 22,7 (24,1) | 15 | 30 / 53 | nunca | 65 % | 18,1 |
| 0,15 | 3,6 dB / 5 % | redund | 7,1 (4,6) | 474 | 23 / 38 | nunca | 50 % | 2,9 |
| | | bond | 10,8 (10,7) | 38 | 41 / 70 | nunca | 24 % | 2,8 |
| 0,10 | 2,0 dB / 5 % | redund | 6,4 (3,7) | 406 | 25 / 33 | 2,7 s / 3 % | 50 % | 1,1 |
| | | bond | 9,1 (8,5) | 69 | 35 / 59 | 1,7 s / 3 % | 14 % | 1,0 |
| 0,07 | 1,2 dB / 62 % | redund | 6,1 (4,0) | 317 | 30 / 32 | 2,2 s / 11 % | 47 % | 1,4 |
| | | bond | 8,9 (8,2) | 79 | 40 / 62 | 1,7 s / 29 % | 7 % | 0,16 |
| 0,05 | 0,7 dB / 99 % | redund | 5,3 (3,5) | 473 | 30 / 33 | 2,2 s / 89 % | 24 % | 16 (vieja) |
| | | bond | 8,6 (8,2) | 104 | 61 / 81 | 1,2 s / 94 % | 8 % | 8 (vieja) |

- **El TCP sobrevive siempre** (ni un corte; el teléfono nunca se
  desconectó) y el túnel no pierde nada (`lost_all` 0, salvo 5–9 en `bond`
  con buena señal). La pérdida de ping es la del enlace fijo, ~1–2 %.
- **En redundancia, el LTE malo arrastra al túnel a lo que da el fijo
  solo con su pérdida:** 3,5–4,6 Mb/s de cola, lo que predice Mathis para
  1 % a 30 ms (~4,7), aunque el fijo da 10. La copia del LTE llega tarde
  (su cola sube a 1–4 s, RTT del enlace 0,9–6,8 s) y no repara la pérdida
  del fijo. Nunca se silencia (corregido el 22-10, sección siguiente: con
  dos enlaces `min_active_links = 2` no lo deja), y el ping del túnel va
  bien (lo trae el fijo). Con BLER ≥ 60 % enmudece (sondas sin respuesta)
  a los ~2,2 s, y vuelve y se va (fuera 11 % del tiempo con 1,2 dB, 89 %
  con 0,7 dB).
- **El bonding saca al LTE del reparto en 1,2–1,7 s** (su parte cae bajo
  10 % a los 1,7 s desde 2 dB hacia abajo) y saca **2× el TCP de la
  redundancia** (8–11 Mb/s, casi todo el fijo) con 4–6 veces menos
  retransmisiones. Con 3,6 dB deja al LTE un 24 % de los envíos según su
  capacidad (2,8 Mb/s): ahí todavía suma. Con buena señal suma los dos (24
  Mb/s frente a 20) con un tercio del RTT. Cuesta RTT de ping cerca del
  acantilado (35–61 ms frente a 25–30 de la redundancia): el reordenador
  espera.
- **El estimador de capacidad sigue al SNR** (21 → 2,8 → 1,0 → 0,15 Mb/s),
  pero con el enlace silenciado se congela: con 0,7 dB queda el último
  valor de antes de la caída (8–16 Mb/s), sin muestras nuevas. Un enlace
  que vuelve de silenciado con esa capacidad vieja recibe de más hasta que
  la mide de nuevo.

### Silenciar por la cola y caducar la capacidad (22-10)

**Lo que de verdad pasaba.** Releídas las muestras de la tabla anterior, el
LTE en redundancia nunca estuvo silenciado (`upload_mutes` 0 en las 30
corridas): con dos enlaces, `min_active_links = 2` (historia 006: «con 2
enlaces, nada se silencia») no deja silenciar ninguno. Lo que la tabla
contaba como «fuera» eran mudos (sondas sin respuesta 2 RTT + 1 s). La
guarda tampoco habría fallado por mirar el atraso y no la cola: con 1–4 s
de cola el LTE iba 0,9–2,4 s detrás del fijo. Lo que sí la frenaba: entre
dos ráfagas de respuestas el informe del LTE quedaba viejo (vale 5
intervalos de sonda, 500 ms) y cada hueco reiniciaba su espera de 2 s.

**Diseño** (sin lazos nuevos: la decisión de siempre con mejores entradas):

- `mute_queue` (`health.h`, `cg_health_late`): el silenciado compara el
  **atraso**, el mayor entre lo que va detrás del más rápido, su cola propia
  (retardo sobre su mínimo de 60 s, el `ql_us` de la capacidad, el mismo
  sensor que el rescate) menos la del más rápido, y, en el cliente, cuánto
  lleva sin respuesta su sonda más vieja menos el RTT del enlace vivo más
  rápido (`cg_health_silent_us`). Un informe viejo con esa cota sobre
  `mute_behind_ms` no reinicia la espera. Volver pide el mismo atraso bajo
  `unmute_behind_ms`; la histéresis, la espera exponencial y
  `min_active_links` no cambian. No se usó la llegada esperada (ETA): en
  redundancia no se calcula, y la cola propia ya la contiene.
- `capacity_expire_ms` (`capacity.h`, `cg_cap_expire`): un enlace fuera del
  envío (silenciado, mudo o fuera del reparto) ese tiempo sin una muestra
  cargada ve su capacidad (y su entrega suavizada) a la mitad, y otra vez
  cada tanto, hasta 1/16 de su mejor. Reusa el arranque rápido: cada mitad
  cuenta como muestra llena (no vuelve a la mitad de su mejor hasta 10 s
  sin llenarse) y como medida para el ETA (`cg_cap_stale` no la agranda en
  3 s); al volver, el sondeo al doble de siempre la sube.

**Celda** (como arriba, 30 s, la ganancia baja a los 3 s y vuelve a 1 a
los 18; n = 3, medianas; `redund`: de fábrica; `red1`: con
`min_active_links = 1`, para que haya qué silenciar; `redq`: `red1` con
`mute_queue` y `capacity_expire_ms = 2000`; `bond` y `bondx`, el perfil de
terreno sin y con las dos). «TCP» es el promedio de la caída (desde 3 s
después hasta la vuelta); «silencio», cuándo quedó silenciado y qué parte
de la caída estuvo fuera (silenciado o mudo); «capacidad», la del LTE al
volver la señal; «vuelta», cuándo volvió a llevar y el TCP de los 10 s
siguientes. Resultados en `~/sdr/resultados/mqce/` (`cell.jsonl`,
`cell_an.txt`).

| Ganancia (SNR, BLER) | Modo | TCP | Retx | Silencio: s / fuera | Vaivenes | Capacidad | Vuelta: s / TCP |
|---|---|---|---|---|---|---|---|
| 0,15 (3,8 dB, 4 %) | redund | 5,0 | 404 | nunca / 0 % | 0 | 3,0 | 0,2 / 20,1 |
| | red1 | 4,7 | 410 | 3,8 / 55 % | 2 | 2,0 | 6,5 / 12,7 |
| | redq | 5,0 | 399 | 3,3 / 62 % | 2 | 1,3 | 5,9 / 13,4 |
| | bond / bondx | 10,5 / 10,6 | 37 / 35 | — | 0 | 2,8 / 2,8 | 0,2 / 23,7 – 21,9 |
| 0,10 (4,2 dB, 4 %) | redund | 3,8 | 466 | nunca (mudo 6,9) / 24 % | 0 | 2,3 | 0,2 / 20,0 |
| | red1 | 4,6 | 542 | 7,9 / 48 % | 1 | 1,5 | 0,2 / 19,5 |
| | redq | 4,3 | 406 | 3,3 / 62 % | 2 | 1,1 | 9,0 / 8,7 |
| | bond / bondx | 8,6 / 8,3 | 38 / 68 | — | 0 | 1,0 / 0,5 | 0,2 / 20,0 – 20,3 |
| 0,07 (1,3 dB, 60 %) | redund | 3,6 | 501 | nunca (mudo 3,8) / 14 % | 0 | 1,6 | 0,2 / 20,2 |
| | red1 | 3,9 | 597 | nunca (mudo 3,8) / 14 % | 0 | 2,0 | 0,2 / 19,9 |
| | redq | 4,8 | 628 | 3,3 / 79 % | 1 | 0,65 | 0,2 / 19,2 |
| | bond / bondx | 7,8 / 7,9 | 77 / 63 | — | 0 | 0,15 / 0,13 | 0,2 / 21,2 – 21,1 |
| 0,05 (0,7 dB, 99 %) | red1 | 3,6 | 526 | mudo 2,2 / 86 % | 0 | **20,9** | 1,2 / 19,9 |
| | redq | 3,4 | 604 | mudo 2,2 / 86 % | 0 | **1,3** | 1,3 / 19,8 |
| | bond / bondx | 8,0 / 8,0 | 108 / 85 | — | 0 | **3,6 / 0,35** | 0,7 / 23,0 – 20,8 |
| 0,05, vuelve a 0,15 | red1 / redq | 3,8 / 3,6 | | | | 20,9 / 1,3 | 1,3 / 4,3 – 4,4 |
| | bond / bondx | 8,1 / 8,3 | | | | 5,8 / 0,32 | 1,2 / 10,1 – 9,8 |

Con ganancia 1 (sin caída) los cinco modos quedan iguales (redundancia
20,1–20,2 Mb/s; bonding 24,9 frente a 24,8). Con n = 3 ningún cambio de TCP
llega a p < 0,05 (Mann-Whitney; los más claros: `redq` frente a `red1` con
0,07 dB, 4,8 frente a 3,9 durante la caída, p = 0,07, y en la vuelta con
0,10, 8,7 frente a 19,5, p = 0,10; `bondx` frente a `bond` en la vuelta
con 0,05, 20,8 frente a 23,0, p = 0,10).

- **`mute_queue` hace lo que se pidió:** silencia a los 3,3 s siempre
  (también con BLER 60 %, donde sin ella nunca se silencia y solo enmudece
  a ratos), y el ping del túnel no cambia. **Pero no le sirve al TCP de la
  redundancia:** con el LTE silenciado el túnel es el fijo solo, con su 1 %
  de pérdida (4–5 Mb/s, lo mismo que con la copia tardía). Lo que repara
  esa pérdida es el bonding (el rescate), no silenciar.
- **Silenciar tiene un costo al volver la señal:** el silenciado vuelve tras
  2 × `mute_settle_ms` multiplicado por la espera exponencial (los vaivenes
  durante la caída la subieron), y el túnel pierde el LTE 6–9 s ya bueno
  (TCP 9–13 Mb/s frente a 20). Le pasa también a `red1`: es del
  silenciado, no de la perilla; `mute_queue` lo hace más frecuente porque
  silencia más.
- **`capacity_expire_ms` hace lo que se pidió:** el LTE vuelve con 0,3–1,3
  Mb/s y no con 3,6–21 de antes de caer. **Pero la capacidad vieja no hacía
  daño:** si la señal vuelve entera, la vieja acierta y la caducada tarda
  ~5 s en subir (bonding 20,8 frente a 23,0 Mb/s en los 10 s siguientes);
  si vuelve débil (a 0,15), los cortes de siempre (lleno, entregas cortas)
  bajan la vieja en 1,5–2,5 s y el TCP queda igual (10,1 frente a 9,8). El
  bonding ya sacaba al LTE del reparto por G3/G4 antes de que su capacidad
  importara.

**Laboratorio** (`bench/labq.py`, `lab.sh bond`, 5 laboratorios, casos
`shallowq`, `starsat`, `het` y `dominant`, subida y bajada, cubic, 30 s,
n = 8; `soff`/`snull`/`son`: el perfil de terreno con `split`, sin, sin otra
vez como control y con las dos perillas; `roff`/`ron`: la redundancia de
fábrica sin y con; plan y resultados en `~/campo/2026-10-22/mqce/`):
ningún caso cambia el caudal (de 0,96× a 1,06×, todos p ≥ 0,2; el control
nulo varía lo mismo, 0,96–1,05×). Lo único con p < 0,05: menos
retransmisiones en `starsat` de bajada con `son` (488 frente a 1034,
p = 0,015) y el p99 del ping en `het` de subida (0,84×, p = 0,05, pero el
control nulo da 0,85×, p = 0,015: ruido). En redundancia con tres enlaces
el silenciado ya actuaba igual (`ron` frente a `roff`: 1,00× en `het`,
`shallowq` y `starsat`; `dominant` de bajada 0,96×, p = 0,65).

**Decisión:** las dos quedan **apagadas** por omisión, como experimentos.
No empeoran nada en el laboratorio, pero en la celda no ganan TCP y
cuestan en la vuelta. Si algún día la redundancia de terreno usa
`mute_behind_ms`, `mute_queue` es el arreglo correcto del hueco de los
informes viejos; `capacity_expire_ms` solo tendría sentido con un enlace
que vuelve débil y sin los cortes de la capacidad.

### Cola honda de operador (23-10)

**Por qué.** En terreno las colas de subida duran segundos (Entel en B2,
WOM sobre su codo) y la celda no las mostraba. Medido: la cola de subida de
srsRAN es la de SDU del RLC UM del teléfono (QCI 7, sin `discard_timer` de
PDCP), de **256 paquetes** fijos al compilar (`RLC_TX_QUEUE_LEN`,
`rlc_interface_types.h`; no hay opción en `ue.conf` ni en `rb.conf`); el
PDCP descarta lo que llega con la cola llena. Con paquetes de 1200 B son
~300 KB: 0,49 s con 5,2 Mb/s de capacidad (ping 491 ms, 77 % de los pings
perdidos) y 2,2 s con 1,3 Mb/s. Cuenta paquetes, no bytes: un ping o un ACK
ocupa lo mismo que 1200 B, y la cola es honda en tiempo solo con poca
capacidad.

**Dónde va la cola** (descartado y por qué):

- Un `tc` en el netns del teléfono, delante de `tun_srsue`: la TUN no frena
  al que escribe (si su anillo se llena, descarta) y srsUE la lee tan rápido
  como llega; una qdisc ahí solo retiene paquetes si limita a una tasa fija,
  y entonces el cuello es esa tasa y no la radio (el SNR, el HARQ y el
  scheduler dejan de importar).
- Del lado del núcleo: en la subida el cuello es el aire y la cola se forma
  en el teléfono; una cola después del aire no se llena nunca.
- **Elegido:** la cola del módem, donde la tiene uno real (su búfer
  RLC/PDCP), vaciada por las concesiones de la estación base.
  `bench/celda/srsran-cola.patch` (4 archivos, lo aplica `setup.sh`): con
  `SRSRAN_UM_TX_QUEUE_BYTES=N` en el entorno de un srsUE, su cola RLC UM
  descarta por cola llena en bytes (N), con lugar para N/400 paquetes (hasta
  6144) y una reserva de 8192 búferes en vez de 4096; sin la variable,
  srsRAN queda como el original. `celda.sh` la pasa por teléfono
  (`QUEUE_BYTES=220000` para el 0, o `QUEUE_BYTES=2000000,0`), así que es
  por teléfono, como en una celda real.

**Perfiles** (`bench/celda/perfiles.json`; `PERFIL=nombre celda.sh start`
pone la celda, el canal, la carga de los otros y el camino; `celda.sh stop`
los quita). La receta viene de ns-3 (tarea 5: sobre la capacidad, RTT ≈
búfer / C y pérdida ≈ 1 − C / tasa):

| Perfil | Celda | Nuestro teléfono | Otros | Cola | Camino |
|---|---|---|---|---|---|
| `entel_b2` | B2 (EARFCN 1125), 15 MHz | ganancia 0,075 sobre piso 300: 2,3–2,9 dB, MCS 0, BLER ~5 %; C ≈ 1,33 Mb/s | 1 teléfono, 15 Mb/s UDP | 220 KB (1,3 s × C) | 35 ms |
| `wom` | banda 7, 10 MHz | ganancia 0,28: 8–12 dB, MCS 11–12, BLER ~5 %; C ≈ 5,25 Mb/s | 1 teléfono, 10 Mb/s UDP | 2 MB | 35 ms |

C es lo que llega al núcleo con la cola llena (después del BLER y las
cabeceras); la cola se vacía algo más rápido (~1,43 Mb/s en Entel, lo que
la estación base ve como 1,52 en MAC). El camino es un retardo fijo en
`srs_spgw_sgi` (hacia los teléfonos) para que el RTT en vacío sea el del
terreno hasta el VPS (p5 53–54 ms) y no el de la celda (~15–20).

**Calibración** (`PERFIL=... escenario.py tasa`, UDP de 1200 B durante
30 s con 4 pings por segundo; RTT y pérdida desde los 10 s, con la cola ya
llena; medianas [mín–máx], n = 3 con la cola honda y n = 2 con la de
srsRAN; 3 de 48 puntos se repitieron porque el iperf3 falló, ninguno por
una caída de radio):

| Perfil, cola | Ofrecido (× C) | Llega | RTT p50 / p90 / máx (ms) | Pérdida UDP |
|---|---|---|---|---|
| `entel_b2`, 220 KB | 0,4 Mb/s (0,30) | 0,40 | 62 / 157 / 374 | 3,8 % |
| | 0,8 (0,60) | 0,77 | 49 / 96 / 400 | 5,3 % |
| | 1,2 (0,90) | 1,16 | 53 / 135 / 344 | 4,9 % |
| | 1,5 (1,13) | 1,32 | 1207 [1145–1216] / 1270 / 1344 | 8,5 % |
| | 1,7 (1,28) | 1,34 | 1254 / 1287 / 1341 | 21,4 % |
| | 2,0 (1,50) | 1,33 | 1254 / 1296 / 1342 | 33,5 % |
| `entel_b2`, 256 paquetes | 0,8 (0,60) | 0,77 | 51 / 84 / 108 | 5,5 % |
| | 1,5 (1,12) | 1,33 | 1106 / 1526 / 1632 | 6,0 % |
| | 2,0 (1,50) | 1,33 | 1768 / 1790 / 1810 (63 % de los pings perdidos) | 33,2 % |
| `wom`, 2 MB | 1 (0,19) | 1,00 | 54 / 65 / 120 | 0,1 % |
| | 3 (0,57) | 3,03 | 56 / 65 / 80 | 0 |
| | 4 (0,76) | 3,98 | 55 / 71 / 83 | 0 |
| | 4,5 (0,86) | 4,55 | 53 / 88 / 180 | 0 |
| | 5,5 (1,05) | 5,24 | 1091 [1075–1136] / 1447 / 1529 | **0** (la cola aún se llena) |
| | 6,5 (1,24) | 5,26 | 3019 / 3069 / 3090 | 15,7 % |
| `wom`, 256 paquetes | 3 (0,57) | 3,00 | 55 / 66 / 84 | 0 |
| | 5,5 (1,05) | 5,23 | 506 / 526 / 615 (10 % de los pings) | 5,2 % |
| | 6,5 (1,24) | 5,25 | 508 / 550 / 586 (49 % de los pings) | 19,5 % |

Entre repeticiones casi no hay dispersión (C 1,31–1,34 y 5,22–5,28 Mb/s;
los RTT sobre el codo, ±5 %).

**Contra el terreno** (nostalgica 20–21-10, subida, muestras de 5 s del
`cengarde-rec`: el RTT del enlace hasta el VPS y la pérdida por enlace;
`curvas.py` de `~/campo/2026-10-22/`):

| | Terreno | Celda |
|---|---|---|
| Entel, RTT | p5 54 ms; p50 957 (tarde 1103), p90 1546–1570, máx 3,9 s, con nuestra carga en 0,1 Mb/s | 49–62 ms bajo C; 1,21–1,25 s sobre C (p90 1,27–1,30, máx 1,34) |
| Entel, pérdida | 3,8–4,8 % (5 % sin cola, 4,8 % con cola ≥ 100 ms); 5–19 % en las tardes malas | 3,8–5,3 % bajo C (BLER residual del borde); 8,5 % con 1,13 × C, 21 % con 1,28 × C |
| WOM, RTT | p5 53; p50 94 (tarde 117), p90 214–259; con 4–6 Mb/s p50 113–118, p90 212–259, máx 4–7 s | 53–56 ms hasta 0,86 × C (p90 65–88); 1,1–3,0 s sobre C (codo ~5,25 Mb/s) |
| WOM, pérdida | 0,2–0,4 %; 1,6–7,6 % sobre 5 Mb/s | 0–0,1 % bajo C; 0 % con 1,05 × C durante 30 s; 15,7 % con 1,24 × C |

- **Entel queda dentro de lo pedido:** RTT 1,2–1,3 s y pérdida 8–21 % con
  1,05–1,3 × C (la receta de ns-3), y además el 5 % de pérdida sin cola que
  mostraba el terreno sale solo del borde del acantilado (MCS 0, BLER 5 %),
  sin pérdida agregada. La cola de srsRAN (256 paquetes) daba un RTT
  parecido a esta capacidad, pero perdía el 63 % de los pings: con bytes,
  un paquete chico entra donde uno grande no.
- **WOM queda dentro de lo pedido:** el codo cae en ~5,25 Mb/s, bajo él el
  RTT es el del camino más 20 ms, y sobre él la cola crece a segundos sin
  perder mientras el exceso cabe (con 1,05 × C, 20 s después sigue en 0 %);
  con 1,24 × C se llena en ~3 s de cola y pierde 1 − C / tasa. Con la cola
  de srsRAN el mismo exceso daba 0,5 s y 5 % de pérdida desde el principio.
- **Lo que no reproduce:** el terreno es más disperso (p90 de Entel 1,55 s
  frente a 1,3; picos de 4–9 s en WOM) porque su capacidad y nuestra carga
  cambian (TCP a ráfagas, otros usuarios); en la celda la capacidad es casi
  fija y la carga es UDP constante. Y la cola de Entel con nuestra carga
  casi en cero (el 89 % de sus colas grandes, «cola de otro»; el motor le
  estimaba 0,1 Mb/s) no sale: el scheduler de srsRAN reparte entre los
  teléfonos y no deja a uno sin concesiones. Aquí aparece solo cuando lo
  que ofrecemos pasa C, como en ns-3.
- **En el borde la radio se cae a ratos:** con ganancia 0,07 (~2 dB) los
  pedidos de concesión fallaron dos veces en ~10 min (falla de enlace de
  radio y reconexión en 1–2 s); con 0,075 (perfil) no hubo ninguna en las
  48 mediciones (~45 min), pero sí una en la prueba de `cengarde.py` sobre
  `wom` (TCP de subida, 15 s), con ~11 dB: srsRAN las tiene también lejos
  del borde. `escenario.py` las cuenta (`rlf`: pedidos fallidos y fallas
  de enlace) y repite el punto si el iperf3 falla (`tries`).
- **Al pasar:** este PC tiene su servicio `iperf3` en el puerto 5201 en
  todas las direcciones; `calibrar.py` y `escenario.py` le mandaban a él
  (los números de arriba de esta historia valen: era el mismo núcleo) y
  `cengarde.py` lo mataba con `pkill -x iperf3`. Ahora los guiones usan
  sus puertos (5231–5232, 5241.., 5290) y detienen sus servidores por PID.
  Y el reporte JSON de la estación base trae `-inf` para un teléfono que
  no transmite, que no es JSON: con dos teléfonos se cortaba la lectura
  (el «sale del reporte» de la tabla de B2 de arriba); `perfil.py` lo lee
  bien.

## Qué hacemos con esto

- Para un enlace celular, lo que decide si sus copias sirven es la distancia
  al acantilado del SNR, más que la carga de la celda: un módem cerca del
  borde (B2 de Entel en la tarde) se fuerza a una banda baja (B28) o se deja
  de esperar por él. WOM queda en automático, como seguro si Entel pierde su
  B28.
- La celda virtual complementa a las trazas de terreno: srsRAN explica los
  mecanismos; las trazas de nostalgica reproducen nuestras celdas tal como
  son. Para escala (cientos de usuarios, 4×4, 256QAM) queda ns-3 con LENA
  (tarea #54); para fidelidad total, un comercial como Amarisoft.

## Pendiente

- Repetir la calibración con n ≥ 3 por punto y los dos tamaños de celda.
- Cola honda: carga variable (otros usuarios a ráfagas, `on_s`/`off_s` en
  `perfiles.json`, sin calibrar) para la dispersión del terreno, y probar
  cengarde encima de los dos perfiles (`PERFIL=... cengarde.py`).
- Varios teléfonos como enlaces de cengarde (hoy uno, más un fijo con
  netem), el rescate, y una caída lenta del SNR en vez de un escalón.
- Con dos enlaces, `min_active_links = 2` impide silenciar: una copia
  1–4 s tarde no es redundancia. Si importa, que un enlace atrasado más de
  lo que sirve deje de contar para el mínimo; en terreno hay 4–5 enlaces y
  no pesa.
- La espera exponencial del silenciado deja fuera 6–9 s a un enlace que ya
  volvió: mirar una vuelta más rápida cuando el atraso cae de golpe.
- 64QAM en subida, 2×2 en bajada y agregación de portadoras en srsRAN, para
  acercarse a una macro de Entel.

## Cambios

- 2026-10-22: creada.
- 2026-10-22: cengarde encima de la celda (`cengarde.py`): redundancia
  frente a bonding con el LTE cayendo al acantilado, n = 3.
- 2026-10-22: `mute_queue` y `capacity_expire_ms` (apagadas), la fase de
  vuelta en `cengarde.py` (`RET`, `RET_GAIN`, `ARMS`) y la corrección: en
  redundancia con dos enlaces el LTE nunca estuvo silenciado (eran mudos).
- 2026-10-23: cola honda de operador: `srsran-cola.patch` (cola de subida
  del módem en bytes, `QUEUE_BYTES` por teléfono), perfiles `entel_b2` y
  `wom` calibrados contra el terreno (n = 3), `perfil.py`, `escenario.py
  tasa`; los guiones ya no matan el `iperf3` del sistema.
