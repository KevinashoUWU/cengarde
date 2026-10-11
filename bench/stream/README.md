# Transmisión simulada

Prueba cómo se portan dentro del túnel los dos tráficos con los que se transmite
desde el terreno: RTMP sobre TCP (YouTube, o un servidor propio) y SRT sobre
UDP. cengarde duplica cada datagrama de WireGuard por todos los enlaces; lo que
ve el emisor es el túnel, no los módems. El kit sirve para medir, sin una
transmisión real, la redundancia de hoy y, más adelante, el reordenador y los
modos de reparto del bonding ([historia 012](../../docs/historias/012-bonding.md)):
se repiten los mismos comandos con el otro motor y se comparan los informes.

| Archivo | Qué hace |
| --- | --- |
| `send.sh` | el PC de la LAN de la Pi: imagen de prueba con reloj y contador, ffmpeg hacia RTMP/RTMPS o SRT, y los registros de lo que ve el emisor |
| `recv.sh` | el servidor propio: escucha RTMP o SRT, graba el flujo tal cual y anota la llegada de cada paquete de video |
| `report.py` | lee las corridas (y la exportación de `fieldrec.py`) y dice qué pasó y cuándo |
| `lib.sh` | lo que comparten los dos scripts: hay que copiar el directorio entero |
| `report_test.py` | la prueba de `report.py` con corridas inventadas |
| `../lab.d/stream.sh` | `lab.sh stream`: todo de punta a punta en el laboratorio de `bond` (a mano, no entra en `ci`) |

**El emisor.** La imagen es `testsrc2` (o barras SMPTE, `-p smptehdbars`) con
ruido leve para que el codificador llene el bitrate, y tres textos grabados:
`CLK`, la hora nominal del fotograma al milisegundo; `FRAME`, su número; y `NOW`,
la hora real al segundo en que se generó. Suena un seno de 1 kHz. `CLK` es la
hora de inicio más el pts, no la hora a la que salió el fotograma: si el emisor
se atasca, `CLK` sigue su línea y lo que se lee en la imagen recibida, frente a
un reloj propio, es el retraso. Su origen se adelanta `-L` segundos (1 por
defecto) para cubrir el arranque del codificador y de la conexión; el error que
quede lo estima `report.py` (`CLK` va adelantado o atrasado tantos segundos
frente a la hora real). El audio y el contenedor suman al bitrate del
multiplexor, así que lo medido queda un poco sobre el objetivo de video.

Mientras corre, el emisor guarda en un directorio de corrida (`stream-runs/`,
fuera de git por el `.gitignore` del directorio; `-o` lo cambia): `meta.json`
(comando con la clave enmascarada, codificador, bitrate, fechas, estado
inicial y final de cengarde si se pasa `-c`, `tcp_congestion_control` del
kernel), `progress.log` (el `-progress` de ffmpeg con la hora de cada línea),
`tcp.csv` (RTMP: `ss -tin` del socket del flujo cada 100 ms: cwnd,
ssthresh, rtt, rttvar, retransmisiones, perdidos, delivery rate, pacing rate,
no enviado, bytes retransmitidos, y los tiempos limitado por búfer de envío y por
ventana del receptor), `udp.csv` (SRT: cola y descartes del socket UDP),
`srt-stats.csv` (SRT con `-S`, abajo), `headers.pcap` (con `-P`, solo
cabeceras), `ffmpeg.log` y `send.log`.

**Codificadores.** Con `-e auto` (lo que se usa si no se dice otra cosa) se prueba,
en orden, `av1_nvenc`, `h264_nvenc` y `libx264`, y se toma el primero que pasa una
codificación de prueba de un fotograma con las mismas opciones del envío y a
través del contenedor del destino. La lista de codificadores de ffmpeg no
basta: muestra los `nvenc` aunque no haya GPU. Con `-e` se fija uno (`av1_nvenc`,
`h264_nvenc`, `hevc_nvenc`, `libx264` o `libsvtav1`, este último en CPU y solo
si se pide). `send.sh -T` muestra qué pasa cada prueba y por qué falla la que
falla. Todos van en CBR con búfer VBV de 1 s (`-B`), sin fotogramas B y con GOP fijo
de 2 s (`-g`); las opciones de baja latencia de NVENC van en un segundo nivel: si
una versión de ffmpeg no las conoce, se usan solo las básicas y se avisa.

| Máquina | Qué elegiría `auto` | Origen |
| --- | --- | --- |
| el PC del estudio (RTX 2060 SUPER, Turing) | `h264_nvenc`: Turing no codifica AV1; con `-e hevc_nvenc` también sirve HEVC | dato del dueño; la elección sale de la prueba, que aquí no se pudo correr (sin GPU) |
| el portátil de campo (RTX 4050, Ada) | `av1_nvenc`: Ada codifica AV1, H.264 y HEVC; las transmisiones reales usan AV1 hasta 8 Mbit/s (`-b 8M`) | dato del dueño; igual que arriba |
| una máquina sin GPU (este contenedor, CI) | `libx264`; `-e libsvtav1` da AV1 en CPU | medido aquí con `send.sh -T` |

- **AV1 y HEVC por RTMP** necesitan Enhanced RTMP en la compilación de ffmpeg
  (ffmpeg 6.1 o más nuevo; el ffmpeg 6.1.1 de este contenedor lo trae y se
  comprobó con `libx265` y `libsvtav1`: entran en FLV y vuelven a salir). Si el
  ffmpeg local no puede, la prueba falla con «this ffmpeg cannot put it in flv:
  RTMP needs Enhanced RTMP…», `auto` pasa al siguiente y `-e` se detiene con ese
  mensaje; con un ffmpeg viejo hay que usar H.264 o SRT. Que
  YouTube acepte AV1 o HEVC por RTMPS es cosa de su ingesta [no verificado
  aquí]: se ve en el panel de YouTube Studio.
- **SRT con cualquier códec** solo es cierto de la parte del emisor: el
  multiplexor MPEG-TS de ffmpeg 6.1 escribe AV1, pero su desmultiplexor no lo
  vuelve a leer [medido aquí: la grabación falla con «Tag [6][0][0][0]
  incompatible»]. Por eso `-C auto` manda H.264 y HEVC en MPEG-TS y AV1 en
  Matroska en vivo (también se puede `-C nut`); un receptor que no sea ffmpeg
  tiene que entender ese contenedor. H.264 y HEVC en MPEG-TS se comprobaron de
  ida y vuelta.
- Las opciones de NVENC de los tres codificadores no se pudieron probar aquí
  (sin GPU, «Cannot load libcuda.so.1»): hay que correr `send.sh -T` en cada
  máquina antes de fiarse.

**Qué instalar en Debian 13.** En el PC: `ffmpeg` (con libsrt, x264, SVT-AV1 y,
si hay controlador, NVENC; Debian 13 trae ffmpeg 7.1 [estimación: comprobar con
`send.sh -T`]), `iproute2` (`ss`), `fonts-dejavu-core` (el texto de la imagen) y
`python3`; opcionales: `tcpdump` (`-P`), `srt-tools` (`-S`), `python3-matplotlib`
(gráficos) y `chrony` (el reloj). Para NVENC, el controlador de NVIDIA de Debian
(`nvidia-driver`) con `libnvidia-encode1` [estimación: nombres de paquete sin
comprobar aquí]. En el VPS: `ffmpeg`, `iproute2`, y `srt-tools` si se usa `-S`.
El laboratorio necesita lo de `bond` más `ffmpeg`.

**La clave.** Se lee de un archivo de una línea, con modo 600 o 400; `send.sh`
se niega si otros pueden leerlo y si el archivo está dentro de un repositorio
git que no lo ignora. Para RTMP es la clave de transmisión; para SRT, la
contraseña de cifrado (10 a 79 caracteres, opcional). Nunca va en la línea de
comandos, ni en `meta.json`, ni en los registros (se enmascara en los dos). Pero
ffmpeg recibe la URL con la clave en su `argv`: en ese PC cualquier usuario
puede leerla con `ps` mientras dure la corrida. Úsalo en un PC propio o monta
`/proc` con `hidepid=2`.

```sh
mkdir -p ~/.config/cengarde-stream && chmod 700 ~/.config/cengarde-stream
read -rs -p 'Clave: ' K; echo     # sin eco y sin quedar en el historial
(umask 077; printf '%s\n' "$K" >~/.config/cengarde-stream/youtube.key); unset K
```

**El PC.**

```sh
bench/stream/send.sh -T                                  # herramientas y codificadores que andan aquí
K=~/.config/cengarde-stream
bench/stream/send.sh -k $K/youtube.key -b 6M -d 600 rtmps://a.rtmps.youtube.com:443/live2   # YouTube (dirección de ejemplo: la de tu evento)
bench/stream/send.sh -k $K/own.key -e hevc_nvenc -b 8M rtmp://10.79.0.1:1935/live            # tu servidor, por RTMP
bench/stream/send.sh -k $K/own.key -l 200 -S -b 6M srt://10.79.0.1:9000                      # tu servidor, por SRT con estadísticas
bench/stream/send.sh -D -k $K/own.key rtmp://10.79.0.1:1935/live                            # solo muestra el comando, con la clave enmascarada
```

Ctrl-C o TERM mandan INT a ffmpeg, nunca TERM, y esperan a que termine (si no
responde, INT de nuevo a los 15 s, tres veces, y KILL a los 30 s más); con `-d SEGUNDOS`
ffmpeg acaba solo (`-t`) y un vigilante lo detiene así si pasan 20 s de más. Un script
que lance `send.sh` o `recv.sh` en segundo plano debe pararlos con TERM: bash no deja
atrapar un INT que ya llega ignorado.

Con `-c root@192.168.1.1:/var/run/cengarde.json` guarda el estado del motor
de la Pi al empezar y al acabar. Con `-P` captura solo las cabeceras, 96 bytes por
paquete (necesita root o `sudo -n`): unos 100 KB/s a 6 Mbit/s [estimación]. RTMP
sin TLS lleva la clave en claro por el cable, en sus primeros mensajes: contra
YouTube usa RTMPS.

**El servidor propio.** `recv.sh` escucha con ffmpeg (RTMP con `-listen 1`, SRT
con `mode=listener`), graba el flujo con `-c copy` en `rec.mkv` y, en una
segunda salida que lee `ffprobe` línea por línea, anota la hora de llegada, el
pts, el tamaño y si es fotograma clave de cada paquete de video (`packets.csv`);
también `progress.log`, y el estado del socket aceptado cada 100 ms
(`tcp.csv`: segmentos fuera de orden, DSACK, RTT de recepción) o del UDP
(`udp.csv`). Atiende un flujo tras otro, cada uno en su directorio (`-1`: uno
y sale). La clave es la misma del emisor. Ábrelo solo por el túnel: en el
firewall del VPS, 1935/tcp o 9000/udp en la interfaz de WireGuard y nada más.

```sh
# en el VPS: apt install ffmpeg srt-tools
bench/stream/recv.sh -b 10.79.0.1 -k ~/.config/cengarde-stream/own.key              # RTMP, puerto 1935
bench/stream/recv.sh -p srt -S -b 10.79.0.1 -l 200 -k ~/.config/cengarde-stream/own.key   # SRT con estadísticas del receptor
```

`10.79.0.1` es la dirección del VPS en el túnel del laboratorio; en el VPS
real es la que dejó `cengarde-vps-setup`. Si el receptor es otro servidor y no
el VPS de cengarde, el tráfico sale por el NAT del VPS: la corrida suma un
salto de internet que el laboratorio no tiene. [mediamtx](https://github.com/bluenviron/mediamtx)
también recibe RTMP y SRT y tiene una API con estadísticas por conexión, pero
exige bajar un binario y una configuración: no hace falta para nada de esto.

**SRT y sus estadísticas.** La libsrt de ffmpeg no imprime estadísticas. Con
`send.sh -S` y `recv.sh -S` el flujo pasa por `srt-live-transmit` (paquete
`srt-tools`), que escribe `srt-stats.csv` con los contadores acumulados de SRT
(pérdidas, retransmisiones, descartes por llegar tarde, RTT, búferes) unas cinco
veces por segundo, y el informe da la vista del emisor y la del receptor. Sin `-S`
solo hay el socket UDP (`udp.csv`). El relé fija `maxbw` en 3 veces el bitrate:
sin eso libsrt reparte el envío según una estimación de la tasa de entrada que
arranca baja, y el primer fotograma clave (una ráfaga) envejece más que la
latencia en el búfer y se descarta [medido en el laboratorio: no llegaba ningún
paquete al receptor]. El relé deja una tubería entre ffmpeg y SRT, así que no
mide exactamente lo mismo que la libsrt directa de ffmpeg.

**YouTube (privado u oculto) o servidor propio.** El destino decide qué se puede ver:

| | YouTube | Servidor propio (el VPS de cengarde u otro) |
| --- | --- | --- |
| Lado del emisor (`ss`, `-progress`, cwnd, RTT, retransmisiones) | sí | sí |
| Lado del receptor (`packets.csv`, huecos de llegada, fotogramas tarde) | no: no hay visibilidad del servidor | sí, con `recv.sh` |
| SRT | YouTube no recibe SRT [no verificado]: solo RTMP/RTMPS | sí |
| Qué se ve de la imagen | la transmisión privada u oculta, con `CLK` frente a un reloj propio; incluye el búfer de YouTube (normal, baja o ultrabaja latencia) | la grabación `rec.mkv` y los registros |
| Qué incluye el camino | hasta YouTube, con su ingesta y sus reconexiones | solo hasta el servidor (con el VPS de cengarde, sin salir del túnel) |

Con YouTube el análisis queda en el emisor: la contrapresión (`speed` y
fotogramas descartados), el colapso de cwnd, el RTT sobre su mínimo, el tiempo
limitado por búfer de envío; y, a ojo, la imagen recibida. Lo que YouTube
muestra de salud (YouTube Studio) no se lee desde aquí.

**Con `cengarde-rec` y `fieldrec.py`.** El grabador de campo
([más abajo](../README.md#fieldrec-el-registrador-de-campo-labdfieldrecsh-fieldrecpy)) ya
registra los enlaces de la Pi. Para correlacionar:

1. Sincroniza el reloj del PC, del router y del servidor (NTP: chrony o
   systemd-timesyncd). El error de la correlación es el desfase de los relojes;
   `report.py --fieldrec-offset S` suma `S` segundos a los tiempos del router si
   se conoce.
2. En la Pi, `status_interval_ms = 250` y `probe_idle_ms = 100` en la config de
   cengarde dan caídas con resolución de 300 ms, como en el laboratorio. Los cortes
   que más interesan duran un segundo o menos.
3. Después de la corrida: `bench/fieldrec.py /ruta/a/cengarde-rec --export campo/`
   y `bench/stream/report.py RUN_DEL_PC --rx RUN_DEL_SERVIDOR --fieldrec campo/`.
   `report.py` solo mira las caídas y silenciados (`events.csv`) que caen en la
   ventana de la corrida y, si hay `tunnel.csv`, los paquetes perdidos en todos
   los enlaces (`lost_all`).

**El análisis (`report.py`).** Texto, `--json ARCHIVO` con todos los números y, si
`matplotlib` está instalado (`apt install python3-matplotlib`), `report.png` en
la corrida con la tasa, cwnd, RTT y retraso de los fotogramas en una escala cada
uno, con los eventos de enlace como franjas (`--no-plots` lo evita). Informa:

- emisor: bitrate objetivo frente a logrado por segundo (p5, p50, p95, segundos
  bajo la mitad); velocidad del codificador por segundo (menor de 1 y cuántos
  segundos), fotogramas descartados o duplicados, y los episodios en que el
  emisor quedó retenido (la hora de salida atrás de la hora real);
- TCP: retransmisiones (segmentos, % de los segmentos y de los bytes, ráfagas),
  RTT sobre su mínimo (p50, p95, p99, máximo; el RTT del kernel es el suavizado,
  así que las puntas salen más chicas de lo que fueron), colapsos de cwnd (una
  caída a menos de la mitad del máximo del último segundo, con su hora; los
  descensos de ProbeRTT de BBR se marcan aparte y no cuentan como incidentes),
  tiempo limitado por el búfer de envío y bytes sin enviar;
- SRT: pérdidas, retransmisiones, descartes (paquetes que llegaron tarde), RTT,
  búferes; y el socket UDP si no hay estadísticas;
- receptor: paquetes y bitrate por segundo, **paradas** (huecos de llegada de
  video de 200 ms o más: cuántas, duración total, p50, p95, la más larga),
  fotogramas tarde frente a la línea de tiempo del emisor (llegada menos pts,
  sobre su piso: el desfase de los dos relojes más el menor retraso del camino;
  p50, p95, p99 y episodios de más de 500 ms), saltos de pts y fotogramas que
  faltan, y segmentos TCP fuera de orden que vio el servidor;
- con `--fieldrec`: por cada tipo de evento (caída, silenciado de subida o de
  bajada) y enlace, cuántos incidentes (parada, fotogramas tarde, retención,
  colapso, punta de RTT, ráfaga de retransmisiones, pérdida o descarte de SRT)
  caen a menos de `--window` segundos (3 por defecto; el solapamiento de los
  intervalos), frente al azar: lo que daría el mismo número de incidentes
  repartidos sin relación con los eventos, según la parte de la corrida que
  cubren las ventanas. También, de cada evento, si tuvo algún incidente cerca,
  y la lista de incidentes cercanos con su posición («2,0 s después»).

Los umbrales (`--stall-ms`, `--late-ms`, `--hold-ms`, `--rtt-spike-ms`, `--skip`, `--skip-end`)
son propuestas [razonado], no medidas: las cifras absolutas dependen de ellos.
Los primeros 2 s se dejan fuera de las estadísticas de tasa y de velocidad del
codificador (`--skip`). En el receptor no se salta nada del arranque salvo el
primer grupo de paquetes que ffmpeg entrega de golpe tras sondear la entrada
(unos 10 a 20 con RTMP, más de 100 con SRT, medido aquí): una parada pocos
segundos después se cuenta, y con SRT esa parada puede caer cuando el
arranque aún no termina. Los últimos 2 s también quedan fuera (`--skip-end`: al
acabar el flujo, el multiplexor vuelca lo que retiene y ese último hueco no es
una parada; medido con SRT en el laboratorio).
Ante dos corridas que se comparan, repite cada una varias veces: una corrida
no basta (historia 012, «Medidas con dispersión»).

```sh
bench/stream/report.py stream-runs/20261014-210000-prueba --rx stream-runs/20261014-210001-rx --fieldrec campo/ --json r.json
python3 bench/stream/report_test.py     # la prueba: corridas inventadas con respuestas conocidas
```

**El laboratorio (`lab.sh stream`).** Corre todo esto dentro de los namespaces de
`bond` (WireGuard real, cengarde, netem por enlace) con `libx264` a 640x360 y
1,5 Mbit/s, para que quepa en un runner o en este contenedor sin GPU: el emisor
en `cli`, el receptor en `srv`, `cengarde-rec` sobre el archivo de estado del
cliente y `fieldrec.py --export` para el cruce. Los casos son los de `bond` más
`blackout` (todos los enlaces cortados 2 s a un tercio de la corrida, que ninguna
redundancia esconde: sirve para ver que el kit encuentra una parada). Solo falla si
falla la fontanería (no hay flujo, llegan menos de la mitad de los paquetes, `report.py`
falla) y, en `blackout`, si el receptor no ve una parada de un segundo o más.

```sh
sudo bench/lab.sh stream                                  # equal, linkdeath y blackout, por RTMP y por SRT (unos 7 min)
sudo STREAM_CASES="starlink15" STREAM_PROTOS=rtmp STREAM_S=40 bench/lab.sh stream
sudo STREAM_ENC=libsvtav1 STREAM_QUIET=1 STREAM_KEEP=/tmp/stream bench/lab.sh stream
```

| Variable | Por defecto | Qué hace |
| --- | --- | --- |
| `STREAM_CASES` | `equal linkdeath blackout` | casos (los de `bond` y `blackout`) |
| `STREAM_PROTOS` | `rtmp srt` | protocolos |
| `STREAM_S` | 30 | segundos por corrida (`starlink15`, al menos 31) |
| `STREAM_SIZE`, `STREAM_KBPS` | `640x360`, 1500 | imagen y bitrate de video |
| `STREAM_ENC` | `libx264` | codificador |
| `STREAM_KEEP` | vacío | directorio donde copiar las corridas (cada una con su `report.txt`, `report.json` y `field/`) |
| `STREAM_QUIET` | 0 | 1: una línea por corrida en vez del informe completo |

**Una corrida de ejemplo.** Medida el 2026-10-11 en este contenedor (4 núcleos, sin
netem ni WireGuard en el kernel: wireguard-go y un netem en espacio de usuario,
como en la historia 012), `lab.sh stream` con `libx264` a 640x360 y 1,5 Mbit/s,
BBR, 30 s por corrida y una repetición de cada caso. Las cifras de una corrida sola varían
(historia 012): sirven para ver que el kit distingue los casos, no como línea
base de nada.

| Caso | Receptor | TCP (RTMP) o SRT (con `-S`) | Eventos de enlace (cengarde-rec) |
| --- | --- | --- | --- |
| `equal`, RTMP | 0 paradas | 0 retransmisiones de 4998 segmentos; 0 colapsos de cwnd (uno de ProbeRTT) | ninguno |
| `equal`, SRT | 0 paradas | emisor y receptor: 0 perdidos, 0 retransmitidos, 0 descartados | ninguno |
| `linkdeath`, RTMP | 0 paradas | 0 retransmisiones de 5008; 0 colapsos (uno de ProbeRTT) | una caída de l2: ningún incidente cerca |
| `linkdeath`, SRT | 0 paradas | emisor y receptor: 0 perdidos, 0 retransmitidos, 0 descartados | una caída de l2: ningún incidente cerca |
| `blackout`, RTMP | 1 parada de 2,94 s; fotogramas tarde hasta 2,9 s | 178 retransmisiones de 5145 (3,46 %); 1 colapso de cwnd (176 a 1 segmento) | caídas de l1 y de l2: los 4 incidentes caen dentro de ellas |
| `blackout`, SRT | 1 parada de 2,13 s; 53 fotogramas que faltan | emisor: 170 perdidos, 59 retransmitidos, 17 descartados; receptor: 344 perdidos, 301 descartados por tarde | caídas de l1 y de l2: los 6 incidentes caen dentro de ellas |

La parada de `blackout` varió entre corridas: 2,28, 2,90 y 2,94 s por RTMP, y
2,07, 2,23 y 2,13 s por SRT (tres corridas de este laboratorio).

Con redundancia, la muerte de un enlace (`linkdeath`) no se nota en la
transmisión, y lo que sí se nota es que mueran todos a la vez. Es lo que la
historia 012 dice del paso 0; el kit lo da ahora con RTMP y SRT, y las mismas
corridas con el reordenador o el reparto dirán qué cambia.

Un fragmento del informe de `blackout` por RTMP, el cruce con los eventos de enlace:

```
| event | link | events | seconds | with an incident near | rx_late near/total (chance) | rx_stall near/total (chance) | tcp_collapse near/total (chance) | tcp_retrans near/total (chance) |
| outage | all | 2 | 4.2 | 2 of 2 | 1/1 (0.3) | 1/1 (0.3) | 1/1 (0.3) | 1/1 (0.3) |
- 20:59:45.741 (+9.7 s) rx_stall: no video packet for 2943 ms; outage l1 during it, outage l2 during it
- 20:59:46.856 (+10.9 s) tcp_retrans: 178 segments retransmitted; outage l1 during it, outage l2 during it
- 20:59:46.983 (+11.0 s) tcp_collapse: cwnd 176 -> 1 segments, back in 2.0 s; outage l1 during it, outage l2 during it
```

**Límites.**

- `CLK` no es la hora a la que salió el fotograma (arriba); la latencia absoluta
  que se lee a ojo solo vale si los relojes están sincronizados, y la latencia
  de YouTube incluye su propio búfer.
- El kernel da el RTT suavizado de TCP; una punta de menos de un RTT no se ve. El
  `ss` de cada versión de iproute2 imprime distinto algunos campos (`bps`,
  `Mbps`); el analizador entiende ambos, y los contadores que el kernel omite
  cuando son 0 salen como 0 (`ssthresh` ausente queda vacío).
- Con BBR, el cwnd baja a 4 durante 200 ms cada ~10 s (ProbeRTT) [conocimiento
  general, visto en las corridas de aquí]: no es una pérdida y el informe lo marca.
- Las paradas se miden donde ffmpeg del servidor termina de leer un fotograma
  (un fotograma de RTMP llega completo, no por paquete de red), no en el cable.
- No se mide lo que hace el decodificador o la pantalla del espectador.
- Las opciones de NVENC (los tres códecs) y todo lo de YouTube quedan por
  comprobar en el hardware real: aquí no hay GPU ni cuenta de YouTube.

Se probó `report.py` con `report_test.py` (corridas inventadas con una parada
del emisor de 2,95 s, un colapso de cwnd, estadísticas de SRT con la cabecera
real de `srt-live-transmit`, y una exportación de `fieldrec`); se comprobó que la
prueba falla si se rompe el umbral de colapso, la recuperación del colapso, la
unión de ráfagas, el recorte del arranque, el relleno de la ventana, el azar, la
resta de contadores acumulados, el mínimo de RTT, la omisión de los primeros segundos,
el desfase del router, la resta de contadores con la primera fila vacía o el recorte
del final (18 mutantes cazados; el de la línea base de la retención sobrevive con esos
datos, porque su mediana y su p5 coinciden). `send.sh`, `recv.sh`, `lib.sh` y
`lab.d/stream.sh` pasan shellcheck con las opciones del job lint de `openwrt.yml`
(que solo cubre `lab.d/`), y los Python, pyflakes.
