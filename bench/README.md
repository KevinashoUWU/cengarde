# bench — laboratorio de rendimiento

Laboratorio reproducible para medir el motor cengarde en C (`engine/`, y en
el futuro eBPF), y compararlo con el engarde Go original, en una sola máquina
Linux, sin hardware. Dos network
namespaces unidos por tres pares veth que hacen de enlaces: `cli` hace de
Raspberry Pi y `srv` de VPS. WireGuard se sustituye por `udpgen`, un
emisor/receptor UDP que numera y marca con la hora cada paquete, así que se
miden pérdidas reales (paquetes sin ninguna copia), duplicados, reordenación y
latencia de un sentido.

```
netns cli (la Pi)                            netns srv (el VPS)
WG falso: udpgen 127.0.0.1:50000             servidor :59402 (engarde o cengarde)
cliente 127.0.0.1:59401                      WG falso: udpgen 127.0.0.1:59301
l1 10.0.1.1 ──────────────────────────────── s1 10.0.1.2
l2 10.0.2.1 ──────────────────────────────── s2 10.0.2.2
l3 10.0.3.1 ──────────────────────────────── s3 10.0.3.2
```

Las cifras citadas en [`ROADMAP.md`](../ROADMAP.md) y en las historias 001,
005, 006, 009 y 010 salen de aquí.

## Requisitos

root, iproute2 (`ip`, `tc` con `sch_tbf`), gcc, make y python3. Go solo hace
falta para la línea base del engarde Go (`ENGINE=go`); git, para esa línea
base y para `restart REF`; iptables, para `fallback`; y curl, para las demos
del Go.

## Uso

```sh
sudo bench/lab.sh build    # udpgen, protoclient, ringbench y cengarde en bench/bin/
sudo bench/lab.sh ci       # lo que corre el CI: smoke, health, control y los escenarios de lab.d con LAB_CI=1
sudo bench/lab.sh smoke    # prueba de humo de cengarde
sudo bench/lab.sh health   # salud de enlaces: un enlace con 500 ms de cola, subida y bajada (historia 006)
sudo bench/lab.sh control  # con tráfico: pausar un enlace, recargar dos veces, IP pass on/off; sin pérdidas (historia 009)
sudo bench/lab.sh latency  # latencia y CPU con busy_poll_us 0, 50 y 200, y el Go si está compilado (historia 006)
sudo bench/lab.sh restart  # reinicios del servidor con tráfico: todos los enlaces vivos en 4 s (lab.d, va en ci)
sudo bench/lab.sh restart ebe570b  # lo mismo con el motor de otro commit, p. ej. el de antes del arreglo
sudo bench/lab.sh multiip  # servidor con varias direcciones: responde desde la de llegada (lab.d, va en ci)
sudo bench/lab.sh fallback # varias direcciones del servidor por enlace: failover, IPv6 que se salta, vuelta tras un corte, MTU de camino (lab.d, va en ci)
sudo bench/lab.sh mtstall  # un hilo de enlace sin CPU no frena a los demás (link_threads = on; lab.d, va en ci)
sudo bench/lab.sh mtlat    # latencia y CPU de legacy, off, on y on con busy_poll a 2, 20 y 80 kpps (lab.d, a mano)
sudo bench/lab.sh soak     # 1 h por modo (off y on): tráfico variado, pérdidas, enlaces que caen, recargas (lab.d, a mano)
sudo CLIENT_EXTRA="link_threads = on" bench/lab.sh ci   # el ci con los hilos por enlace (el CI corre legacy, off y on)

sudo ENGINE=go bench/lab.sh build  # además, el engarde Go (normal y -race)
sudo bench/lab.sh suite    # línea base del engarde Go (historia 001, ~5 min)
sudo bench/lab.sh compare  # engarde Go frente a cengarde, cada uno en ambos extremos (historia 005)
```

`ci` corre cada escenario en una subshell y con el laboratorio limpio, sigue
aunque uno falle y acaba con `ci: ok (...)` o `ci: FAILED: ...`; borra los
namespaces al salir, también si se interrumpe. El CI (`engine.yml`) solo llama
a `build` y a `ci`.

### `ringbench`: lo que cuesta pasar trabajo entre hilos

`bench/bin/ringbench [-d SEGUNDOS] [-q]` mide el anillo de los hilos del
motor (`engine/src/ring.h`) en la máquina donde corre: sin root ni
namespaces, así que sirve tal cual en la Pi, en un VPS o en un portátil.

- `spin`: un consumidor que nunca duerme; ns por entrada con lotes de 1, 8 y
  64 (el coste cuando los dos hilos están ocupados).
- `wake`: un productor que publica una entrada con su hora a 2000, 20 000 y
  80 000 por segundo y un consumidor que duerme en su timbre (un eventfd)
  cuando no hay nada, como el hub y las bombas del router a poco tráfico:
  latencia del traspaso (p50 y p99), despertares por entrada y CPU por
  entrada de cada hilo, de sus relojes de CPU (la del productor incluye el
  `nanosleep` que le marca el ritmo). Con `-q`, además con el consumidor
  sondeando (`poll`) en vez de dormir.

### Escenarios en `lab.d/`

Cada escenario nuevo va en su propio archivo, `bench/lab.d/NOMBRE.sh`, que
`lab.sh` carga al arrancar: define la función `NOMBRE` (se corre con
`sudo bench/lab.sh NOMBRE`), usa las funciones y variables de `lab.sh`
(`setup`, `start`, `teardown`, `jget`, `$RUN`, `$BIN`…) y pone `LAB_CI=1` si
el CI tiene que correrlo. Así, cambios en paralelo añaden escenarios sin tocar
`lab.sh`. Reglas:

- autocontenido: monta su laboratorio y lo desmonta con `teardown` al acabar,
  también si falla;
- termina con `NOMBRE: ok` o `NOMBRE: FAILED`, con una línea `FAIL: ...` que
  diga qué falló, y devuelve distinto de 0 si falló;
- pasa shellcheck como bash (`shellcheck -s bash -e SC2015 bench/lab.sh
  bench/lab.d/*.sh`, en el job lint de `openwrt.yml`).

### `restart`: el servidor se reinicia (`lab.d/restart.sh`)

Un servidor reiniciado abre la sesión de nuevo con una secuencia aleatoria,
y más o menos la mitad de las veces cae por detrás de la ventana anti-replay
del cliente. El escenario comprueba que el cliente rehace su ventana y la
bajada vuelve a fluir (la regla, en `engine/src/epoch.h` y la historia
[010](../docs/historias/010-ipv6-varias-ip-multicliente-nombres.md)).

- **Montaje:** 2000 pps de bajada (y 20 de subida, para que las sondas vayan
  cada 100 ms). l3 tiene 800 ms de cola siempre llena: tbf a 5 Mbit/s en el
  lado del cliente más un relleno de 7,8 Mbit/s, como `health` llena la suya
  de 500 ms. Así sus respuestas contestan sondas de 8 o más atrás. El
  WireGuard falso del VPS (`udpgen -l`) sigue al servidor nuevo a su puerto
  nuevo, como WireGuard cuando cambia el punto final del otro lado.
- **Fase 1:** 10 reinicios (`RESTARTS`). Cada vez, todos los enlaces tienen
  que estar vivos, con un paquete verificado después del reinicio, en 4 s
  (`RESTART_LIMIT`) desde que arranca el servidor nuevo (consultando el
  socket de control cada 50 ms), y la bajada tiene que volver a fluir. Si
  en 10 s (`WEDGE_S`) no lo están, cuenta como atascado y se reinicia el
  cliente para seguir.
- **Fase 2:** l1 y l2 en pausa (`cengarde ctl link … off`), así que solo las
  respuestas tardías de l3 pueden delatar el reinicio. Hasta 8 reinicios,
  hasta que uno caiga por detrás de la ventana.
- **Con `REF`** compila el motor de ese commit en `$RUN` y lo mide igual.
- **Si el laboratorio no está listo** (la comprobación de antes de reiniciar
  falla, o el socket de control no contesta), no reinicia nada: dice qué
  extremo no corre, con el final de su log, en vez de contarlo como atascado.

Medidas (antes y después del arreglo, y con un anillo de 4 sondas):
historia [010](../docs/historias/010-ipv6-varias-ip-multicliente-nombres.md).

### `multiip`: el servidor con varias direcciones (`lab.d/multiip.sh`)

El servidor escucha en un comodín y responde desde la dirección a la que
llegó cada paquete (`engine/src/pktinfo.h`). Antes respondía desde la que
elegía la ruta, y el socket del enlace del cliente, conectado a la
dirección a la que envía, descartaba la respuesta.

- **Montaje:** `srv` tiene además 10.0.1.20/24 en `s1` (secundaria) y
  198.51.100.7/32 en `lo`, a la que `cli` llega por `l2`. l1 envía a la
  secundaria, l2 a la /32 y l3 a 10.0.3.2, como siempre.
- **Dos pasadas,** con `listen = 0.0.0.0:59402` y con `*:59402` (doble pila
  con direcciones v4-mapped donde el kernel tiene IPv6; IPv4 si no, como en
  el contenedor sin IPv6).
- **Comprueba:**
  - todos los enlaces vivos en los dos extremos en 3 s, y en cada camino
    del servidor `links[].local` es la dirección a la que envía su enlace
    (también en la columna LOCAL de `cengarde ctl links`);
  - 2000 pps de bajada y de subida, cada paquete una vez (como `smoke`) y
    al menos el 90 % por cada enlace;
  - al borrar la /32 con 2000 pps de bajada, solo crecen los
    `local_errors` de l2, su camino sigue y el túnel no pierde nada por l1
    y l3; con la /32 de vuelta, l2 vuelve a estar vivo en 5 s.
- **Con el motor de antes** (`CENGARDE_BIN` de `3bcf673`), l1 y l2 nunca
  llegan a vivos: el escenario falla.
- **Con la lista de direcciones del cliente:** l2 con
  `server = 198.51.100.7:59402 10.0.2.2:59402` y `server_failover_ms = 3000`
  pasa a la segunda en 4 s como mucho tras borrar la /32 (medido: 2,8 s), y
  el camino del servidor la sigue (`links[].local` 10.0.2.2).

### `fallback`: varias direcciones del servidor (`lab.d/fallback.sh`)

Cada enlace manda a una dirección de su lista `server` y pasa a la siguiente
tras `server_failover_ms` sin respuesta (`engine/src/srvpick.h`). Con 4 s de
failover (`FAILOVER_MS`):

- **l1**, `10.0.1.99 10.0.1.2`: la primera no lleva a ningún sitio; tiene
  que vivir en la segunda dentro del tiempo de failover y quedarse. Después,
  dos recargas que no lo cambian de dirección: una añade a su lista una
  entrada IPv6, que no es de su familia, y otra intercambia sus dos
  direcciones, así que la nueva primera es donde ya está. Ninguna puede
  cerrar su socket.
- **l2**, `[2001:db8::2] 10.0.2.2`: sin IPv6 en el enlace (ni en el
  laboratorio), la entrada IPv6 no cuenta y vive en la IPv4 al momento.
- **l3**, `10.0.3.2 10.0.3.20`: las dos llevan al servidor (10.0.3.20 por un
  DNAT de iptables en el netns `srv`, así que basta con que el servidor
  escuche en una). Con 10.0.3.2 bloqueada pasa a 10.0.3.20, y se queda
  aunque 10.0.3.2 vuelva. Después, un corte: el lado del servidor pierde
  sus direcciones durante al menos 25 s (`OUTAGE_S`), hasta que l3 acaba de
  pasar a 10.0.3.20, y l3 conserva la suya, como un módem que mantiene su
  concesión. Recorre las dos sin respuesta (una ronda
  muerta), y la primera respuesta lo devuelve a 10.0.3.2. Por último, otra
  vez en 10.0.3.20, l3 pierde su propia dirección y recupera la misma: eso
  también lo devuelve a la primera, al momento.
- **MTU de camino:** l2 baja a MTU 1400 mientras suben datagramas de 1400
  bytes; el estado tiene que dar `path_mtu` 1400 en l2 (1500 en l1) y el log
  tiene que pedir un MTU de WireGuard de 1316 solo para l2, sin perder
  paquetes.

Paso a paso (pasa las mismas variables a `setup` y a `start`):

```sh
sudo NLINKS=3 bench/lab.sh setup     # netns, enlaces y configs en bench/run/
sudo NLINKS=3 bench/lab.sh start     # cengarde en ambos extremos (ENGINE=go: engarde)
sudo bench/lab.sh down 10000 5       # 10.000 pps de bajada (VPS -> Pi) durante 5 s
sudo bench/lab.sh up 10000 5         # subida (Pi -> VPS)
sudo bench/lab.sh shape l3 5mbit     # enlace lento con cola local (módem USB, WiFi)
sudo bench/lab.sh teardown
```

| Variable | Valor por defecto | Uso |
| --- | --- | --- |
| `NLINKS` | 3 | enlaces activos (1–3) |
| `SIZE` | 1400 | tamaño de paquete en bytes |
| `ENGINE` | `c` | `c`: cengarde (`engine/`) en ambos extremos, con configs INI en `bench/run/`; `go`: el engarde Go |
| `GO_REF` / `GO_REPO` | `3492df9…` / porech/engarde | de qué commit sale el engarde Go: del historial de este repositorio o, si no está (clon superficial), de `GO_REPO` |
| `WRITE_TIMEOUT` | 10 | `writeTimeout` del cliente Go, en ms (`-1` lo desactiva) |
| `PROTO` | vacío | con `ENGINE=go`, `c` o `dedup`: usa `protoclient` (C) en vez del cliente Go |
| `CLIENT_BIN` / `SERVER_BIN` | `bench/bin/engarde-*` | probar otros binarios de engarde |
| `CLIENT_EXTRA` / `SERVER_EXTRA` | vacío | ajustes extra de cengarde, `clave = valor` separados por `;` (p. ej. `busy_poll_us = 50`) |
| `CENGARDE_BIN` | `bench/bin/cengarde` | otro binario de cengarde, p. ej. un envoltorio que ejecuta la compilación de OpenWrt con su musl (historia 007) |
| `RUN` | `bench/run` | configs, logs, JSON de estado y sockets de control. Un `RUN` relativo se toma desde el directorio actual y pasa a ruta absoluta, porque el motor solo acepta un `control_socket` absoluto. La ruta de un socket Unix no pasa de 107 bytes (contando la ruta absoluta): si el repositorio está muy hondo, `control` falla con `socket path too long` y hay que usar un `RUN` más corto, p. ej. bajo `/tmp` |

Demos de los problemas del engarde Go descritos en el roadmap (necesitan
`ENGINE=go bench/lab.sh build`):
- `demo_stranger`: el servidor envía el tráfico del túnel a cualquiera que le
  mande un paquete. Con el valor por defecto, `ENGINE=c`, muestra en cambio
  que cengarde no responde a nadie sin autenticar.
- `demo_webpanic`: el cliente se cae si el puerto web está ocupado.
- `demo_races`: detector de carreras de Go, con tráfico y uso normal de la
  web.

### `mtstall`: un hilo de enlace sin CPU (`lab.d/mtstall.sh`)

Con `link_threads = on` cada enlace tiene un hilo que lee su socket, así que
uno que no recibe CPU llena solo su socket y las copias de los otros llegan
a tiempo (historia [011](../docs/historias/011-hilos.md)).

- **Montaje:** 2000 pps en los dos sentidos durante 12 s (`MTSTALL_S`). Un
  bucle `SCHED_FIFO` 99 ocupa la última CPU 300 ms de cada 2 s; el hilo que
  lee l3 (`cg-l3`, buscado por nombre con `cengarde ctl threads`) pasa a esa
  CPU y todo lo demás (los otros hilos, el servidor, los WireGuard falsos)
  se queda fuera de ella. `rcvbuf = 256 KiB`: cada atasco de 300 ms (unos
  600 datagramas de l3) desborda el socket de l3.
- **Pasa con `on`:** el túnel no pierde nada (99,9 %) en ningún sentido,
  los sockets de l1 y l2 no descartan nada (`socket_drops`), y como mucho el
  5 por mil (`MTSTALL_LATE_PM`) de los paquetes llega con 50 ms o más de
  retraso (`over50ms` de `udpgen`; la VM se para sola hasta 38 ms, y
  `bench/jitter.c`, que marca esas pausas, llega con el PR 3a).
- **`off` y `legacy`:** el único bucle lee todos los enlaces, así que el
  bucle ocupado se lleva el hilo principal: se informa, no se juzga.
- Necesita `chrt` y `taskset` (util-linux) y 2 CPU o más.

### `mtlat` y `soak`: a mano (`lab.d/mtlat.sh`, `lab.d/soak.sh`)

- **`mtlat`:** `MTLAT_RUNS` (5) pasadas intercaladas de cada modo de
  `MTLAT_MODES` (`legacy off on on+busy`; `on+busy` es `on` con
  `busy_poll_us = 50`) a `MTLAT_RATES` (2000, 20 000 y 80 000 pps) en cada
  sentido de `MTLAT_DIRS`, `MTLAT_S` (5) s cada una. Cada línea es la de
  `up`/`down` (pérdida, p50/p99, CPU por paquete de cada extremo, de todos
  sus hilos) y, en bajada, el salto que mide el propio motor (`hop_us`: lo
  que espera un lote entre el hilo que lo leyó y el principal). Al final,
  la mediana de cada punto. Con `MTLAT_S` de 10 o más, la
  ventana de 5 s de `hop_us` cae entera dentro del tráfico.
- **`soak`:** `SOAK_S` (3600) s por modo de `SOAK_MODES` (`off on`), en
  tramos de 60 s a 2000, 10 000 y 20 000 pps y 80, 400 y 1400 bytes por
  turnos, mientras l3 cambia de pérdida y retardo cada 2 min (netem; `tbf`
  si el kernel no lo tiene), l2 cae 10 s cada 10 min y el cliente recarga
  cada 5 min. Pasa si cada tramo entrega el 99,9 %, no hay avisos de hilos
  parados ni de sockets que no se pudieron vigilar, todas las recargas dicen
  `ok` y el RSS del cliente no crece más de 1 MiB tras los primeros 5 min.

## Cómo leer la salida

```
pps=10000 (112 Mbit/s) sent=49997 uniq=49997 loss=0.00% dup=99994 p50=110us p99=912us p99.9=2969us cpu client=93.0% server=30.8% | us/pkt client=93.0 server=30.8
```

- `loss`: paquetes de los que no llegó **ninguna** copia.
- `dup`: copias extra entregadas al "WireGuard". Con engarde Go son N−1 por
  paquete, y el WireGuard real las descifra todas antes de descartarlas;
  cengarde deduplica y entrega 0.
- `us/pkt`: CPU (usuario + kernel en el contexto del proceso) por paquete
  WireGuard. Incluye el trabajo del kernel al enviar y, en veth/loopback, parte
  de la recepción del otro extremo; es igual para todas las variantes, así que
  sirve para compararlas, no como coste absoluto.

## Limitaciones

- No hay cifrado: el coste de WireGuard (que con engarde Go descifra N copias
  de cada paquete) no aparece en las cifras.
- veth no es un módem: sin `sch_netem` no hay retardo, jitter ni pérdida en los
  enlaces; `tbf` solo emula un enlace lento con la cola en la propia máquina.
  Esa cola la limita también el `sndbuf` del socket (~190 ms a 5 Mbit/s por
  defecto), así que `health` lo sube a 4 MiB para tener 500 ms.
- Los valores absolutos dependen de la CPU (una VM x86 no es una Pi). Compara
  variantes en la misma máquina y, si puedes, ejecútalo también en la Pi.
- `protoclient.c` es el prototipo de ~200 líneas que sirvió para la primera
  medida, antes del motor; queda como referencia. El motor real está en
  `engine/`.
- `udpgen` envía paquetes con forma de mensaje de datos de WireGuard (tipo 4),
  porque cengarde solo aprende el puerto local de WireGuard de datagramas con
  esa forma.
