# Celda LTE virtual (srsRAN 4G + GNU Radio)

Una celda LTE completa en el PC, sin hardware de radio: núcleo (srsEPC),
estación base (srsENB), un canal de GNU Radio en medio y uno o más
teléfonos (srsUE), cada uno en su netns. Sirve para estudiar lo que el
netem no tiene: el SNR, el MCS, el HARQ, el scheduler y otros usuarios en la
misma celda. Historia: [016](../../docs/historias/016-celda-virtual.md).

## Instalar

`bench/celda/setup.sh` (Debian 13, con sudo): compila srsRAN 4G con ZMQ
(no está empaquetado), lo instala en `/usr/local` y deja las
configuraciones en `~/sdr/conf` (`CONF=...` para otra carpeta).

## Usar

```sh
PRB=75 EARFCN=1125 NUE=2 LOGS=/tmp/celda bench/celda/celda.sh start   # B2, 15 MHz, como Entel
sudo ip netns exec ue1 ping 172.16.0.1                               # el teléfono 0 hacia el núcleo
bench/celda/celda.sh stop
PERFIL=entel_b2 bench/celda/celda.sh start                           # o una celda calibrada (abajo)
```

- `PRB`: 50 (10 MHz), 75 (15 MHz) o 100 (20 MHz). `EARFCN`: 3350 (banda 7)
  por omisión; 1125 es la B2 de Entel. `NUE`: cantidad de teléfonos; el
  teléfono `k` está en el netns `ue(k+1)` y el núcleo en 172.16.0.1 (con
  varios, el teléfono 0 no siempre es 172.16.0.2).
- **La cola honda del módem** (`QUEUE_BYTES`): la cola de subida del
  teléfono, en bytes, con descarte por cola llena, donde la tiene un módem
  real (su búfer RLC/PDCP, que vacían las concesiones de la estación base).
  Un valor es la del teléfono 0 (`QUEUE_BYTES=220000`); una lista, una por
  teléfono (`QUEUE_BYTES=2000000,0`); 0 es la de srsRAN (256 paquetes,
  ~300 KB de paquetes de 1200 B: 0,5 s a 5 Mb/s). Necesita srsRAN con
  `srsran-cola.patch` (lo aplica `setup.sh`; sin la variable, srsRAN queda
  igual que el original).
- **Perfiles calibrados** (`perfiles.json`, `perfil.py lista`):
  `PERFIL=entel_b2` (B2 de Entel en el borde: ~2 dB, BLER ~5 %, ~1,3 Mb/s,
  cola de 220 KB) y `PERFIL=wom` (WOM: ~12 dB, codo cerca de 5 Mb/s, cola
  de 2 MB). Ponen `PRB`, `EARFCN`, `NUE` y `QUEUE_BYTES` (el entorno manda
  si se da), y al conectarse el canal (piso y ganancias), la carga de los
  otros usuarios (UDP desde el teléfono 1) y un retardo fijo de 35 ms del
  lado del núcleo (el camino al VPS). `celda.sh stop` quita la carga.
  Calibración en la historia 016 («Cola honda de operador»).
- **El canal** (`canal.py`, XML-RPC en 127.0.0.1:8765): en la subida, cada
  teléfono llega con su ganancia (`set_ul_gain(g, ue)`, su pérdida de
  trayecto) y la estación base suma un piso de ruido común
  (`set_ul_noise(v)`): el SNR de cada teléfono es su ganancia frente al
  piso, y uno débil no empeora a los demás. En la bajada, un ruido por
  teléfono (`set_dl_noise(v, ue)`). `get_power()` da la potencia en cada
  punto. Las muestras de srsRAN por ZMQ son grandes (≈250–450 rms): los
  ruidos útiles van de ~300 a ~5000.
- **Métricas:** `LOGS/enb_report.json` (por teléfono: `ul_snr`, `ul_mcs`,
  `ul_bler`, `ul_bitrate`, `ul_bsr`), `LOGS/ue*_metrics.csv`.
- `calibrar.py RUIDO...`: con un teléfono, SNR, MCS, BLER, capacidad de
  subida y RTT con carga liviana y al 80 %.
- `escenario.py "GANANCIA,..." "MBIT_OTROS,..." [PISO]`: nuestro teléfono
  (1 Mb/s y pings, como las copias de cengarde) con cada ganancia, mientras
  el teléfono 1 carga la celda.
- `PERFIL=... escenario.py tasa "TASA,..." [N]`: con la celda levantada
  con ese perfil, mide nuestra capacidad (lo que llega con la cola llena)
  y, para cada tasa ofrecida (Mb/s, o `x1.2`: 1,2 veces la capacidad), un
  flujo UDP de `SECS` s (30) con 4 pings por segundo; el RTT y la pérdida
  cuentan desde `WARM` s (10), cuando la cola ya se llenó.
- `cengarde.py run "GANANCIA,..." "redund,bond" [N]`: cengarde encima de
  la celda (con `NUE=1`, o con `PERFIL=...`: su piso, su ganancia del
  teléfono 0 como punto de partida, `BASE_GAIN`, y su `NUE` al reiniciar
  la celda). El cliente, en `ue1`, tiene dos enlaces: `lte`
  (`tun_srsue`, hacia 172.16.0.1) y `fijo` (un veth al netns raíz con
  netem, `FIJO_NETEM`); el servidor corre en el netns raíz, con túnel
  nativo (`cgcel`, 10.81.0.1/24). En cada punto sube TCP 20 s por el túnel
  con pings dentro y, a los 3 s, baja la ganancia del teléfono 0: mide si
  cengarde saca al LTE del acantilado y en cuánto tiempo, el TCP, el RTT,
  la capacidad que estima y lo que ve la estación base. `cengarde.py up
  [redund|bond]` y `down` lo levantan y lo quitan a mano.
- `PERFIL=wom proteger.py run "BRAZO,..." "udp,tcp" [N]`: el mismo montaje
  con `fijo` como el enlace principal que pierde (como Starlink: `delay 5ms
  rate 30mbit loss 0.7%`, más rápido que la subida de la celda) y la celda
  como el respaldo ahogado, en redundancia (historia 020). Por corrida, 40 s
  de un flujo UDP fijo (`cbr.py`, 7 Mbit/s de 1200 B, con secuencia y hora
  de envío: lo perdido por segundo y el retardo de ida) o TCP, pings en el
  túnel y un microcorte (`EP_AT`, `EP_SECS`, `EP_LOSS`: 15 s, 10 s, 60 %).
  Brazos (`proteger.py arms`): `all`, `full` (250), `cut` (60), `fec`,
  `pcopy`, `parity`, `mix`, `rs`, `rlc` (`redund_spare`, historias 020 y
  021) y los de `ARMS_EXTRA`. Del flujo UDP también los paquetes que
  llegaron después de uno posterior (rehechos, o copias por el enlace lento):
  cuántos y cuánto más tarde que la mediana (`late`; `late_out` fuera del
  corte, o todos con `EP_SECS=0`); el ping, p50/p95/p99.
  `proteger_an.py ARCHIVO.jsonl [REF]` hace las tablas con Mann-Whitney.

## Cuidado

- El núcleo reescribe `user_db.csv` al detenerse: los usuarios nuevos se
  agregan con la celda detenida.
- No uses `pkill -f` con la ruta de `canal.py`: mata la shell que lo llama.
  `celda.sh stop` lo detiene por su PID.
- Este PC tiene su propio servicio `iperf3` en el puerto 5201, en todas
  las direcciones: los guiones usan 5231–5232 (`escenario.py`,
  `calibrar.py`), 5241.. (los otros usuarios) y 5290 (`cengarde.py`), y
  detienen sus servidores por su PID, nunca por nombre.
- La celda es una sola (puertos ZMQ fijos): un solo dueño a la vez.
- Límites del modelo en la historia 016: el scheduler de srsRAN es más
  amable que el de un operador, su SNR no es el SINR del módem y no hay 4×4
  ni 256QAM en subida.
