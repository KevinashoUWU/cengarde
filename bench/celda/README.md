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
```

- `PRB`: 50 (10 MHz), 75 (15 MHz) o 100 (20 MHz). `EARFCN`: 3350 (banda 7)
  por omisión; 1125 es la B2 de Entel. `NUE`: cantidad de teléfonos; el
  teléfono `k` está en el netns `ue(k+1)` y el núcleo en 172.16.0.1.
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

## Cuidado

- El núcleo reescribe `user_db.csv` al detenerse: los usuarios nuevos se
  agregan con la celda detenida.
- No uses `pkill -f` con la ruta de `canal.py`: mata la shell que lo llama.
  `celda.sh stop` lo detiene por su PID.
- Límites del modelo en la historia 016: el scheduler de srsRAN es más
  amable que el de un operador, su SNR no es el SINR del módem y no hay 4×4
  ni 256QAM en subida.
