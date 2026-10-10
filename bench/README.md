# bench — laboratorio de rendimiento

Laboratorio reproducible para medir el motor cengarde en C (`engine/`, y en
el futuro eBPF), y compararlo con el engarde Go original, en una sola máquina
Linux, sin hardware. Dos network
namespaces unidos por tres pares veth (o `NLINKS`, si son más) que hacen de
enlaces: `cli` hace de
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
005, 006, 009, 010 y 011 salen de aquí.

## Requisitos

root, iproute2 (`ip`, `tc` con `sch_tbf`), gcc, make y python3. Go solo hace
falta para la línea base del engarde Go (`ENGINE=go`); git, para esa línea
base y para `restart REF`; iptables, para `fallback`; iperf3,
wireguard-tools y `sch_netem`, para `bond`; y curl, para las demos del Go.

## Uso

```sh
sudo bench/lab.sh build    # udpgen, protoclient, ringbench, cgprobe, macbench y cengarde en bench/bin/
sudo bench/lab.sh ci       # lo que corre el CI: smoke, health, control y los escenarios de lab.d con LAB_CI=1
sudo bench/lab.sh smoke    # prueba de humo de cengarde
sudo bench/lab.sh health   # salud de enlaces: un enlace con 500 ms de cola, subida y bajada (historia 006)
sudo bench/lab.sh control  # con tráfico: pausar un enlace, recargar dos veces, IP pass on/off; sin pérdidas (historia 009)
sudo bench/lab.sh muteoff  # una recarga con mute_behind_ms = 0 devuelve al instante un enlace silenciado, subida y bajada (lab.d, va en ci)
sudo bench/lab.sh multiclient  # tres routers en un servidor: tráfico sin cruces, IP pass y tabla de reenvío, recargas (lab.d, va en ci)
sudo bench/lab.sh latency  # latencia y CPU con busy_poll_us 0, 50 y 200, y el Go si está compilado (historia 006)
sudo bench/lab.sh restart  # reinicios del servidor con tráfico: todos los enlaces vivos en 4 s (lab.d, va en ci)
sudo bench/lab.sh restart ebe570b  # lo mismo con el motor de otro commit, p. ej. el de antes del arreglo
sudo bench/lab.sh multiip  # servidor con varias direcciones: responde desde la de llegada (lab.d, va en ci)
sudo bench/lab.sh fallback # varias direcciones del servidor por enlace: failover, IPv6 que se salta, vuelta tras un corte, MTU de camino (lab.d, va en ci)
sudo bench/lab.sh lanes    # las colas del servidor: una por enlace, la basura aparte, un segundo servidor rechazado; y lanes = 1 (lab.d, va en ci)
sudo bench/lab.sh deepq    # un camino del servidor con 20 MB de cola local no afecta a los demás (lab.d, va en ci)
sudo bench/lab.sh skew     # primeras llegadas repartidas entre enlaces idénticos, puerta C5 (lab.d, va en ci)
sudo bench/lab.sh wgpoke   # un router con la hora atrasada: el servidor hace que WireGuard inicie el handshake (lab.d, va en ci)
sudo LAB_UDPMEM=1 bench/lab.sh udpmem  # el presupuesto de recepción frente a net.ipv4.udp_mem (lab.d; baja el sysctl de toda la máquina unos segundos: solo en el CI)
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

### `macbench`: lo que cuesta servir a varios routers

`bench/bin/macbench [-n SEGUNDOS] [-r CORRIDAS] [-s SEMILLA]`
(`bench/macbench.c`) mide en ns por operación, sin root ni namespaces, lo
que el servidor hace por paquete: leer la cabecera, buscar la sesión por la
pista del router (`engine/src/clients.h`) con 1, 32 y 64 routers, el MAC,
admitir una sesión probando 1, 2 o 4 claves, y las cookies. La última tabla
compara buscar y verificar ahora con lo de antes de varios routers
(`cg_idmap_get` y el MAC). Imprime la mediana y el mínimo de cada medida:
en una máquina compartida, citar el mínimo.

Medido el 2026-10-10 en este contenedor (un núcleo de un Xeon de 2,8 GHz,
gcc 13.3, mínimos de 5 corridas de 0,2 s; dos corridas coinciden en ±10 %):

| Operación | ns |
| --- | --- |
| MAC de un DATA de 1400 B (`cg_hdr_verify`) | 720 |
| MAC de una sonda | 57 |
| buscar la sesión: 1 router, 32 (2 sesiones cada uno), 64 (4) | 3 / 5,4 / 10 |
| descartar una pista que no es de nadie | 1 |
| admitir una sonda: 1, 2 o 4 claves probadas | 58 / 122 / 252 |
| buscar y verificar un DATA: antes, y ahora con 64 routers | 721 / 738 |
| buscar y verificar una sonda: antes, y ahora con 1 router | 60 / 67 |

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

### `lanes`: las colas del servidor (`lab.d/lanes.sh`)

El servidor escucha con un grupo `SO_REUSEPORT` de 8 sockets (*lanes*) y
uno de basura, y un programa BPF mete cada datagrama en el de su número de
enlace (`engine/src/steer.h`; [README del motor](../engine/README.md#colas-del-servidor-lanes)).
Con las direcciones de `multiip` (l1 a una secundaria, l2 a una /32, l3 a
10.0.3.2):

- todos los enlaces vivos en 3 s, y en cada camino del servidor
  `links[].local` es la dirección a la que envía su enlace: las respuestas
  salen desde la dirección de llegada en cada cola;
- 2000 pps de bajada y de subida, cada paquete una vez;
- las colas 0, 1 y 2 reciben, cada una solo de su enlace (`lanes[].links`),
  y las demás y la de basura nada;
- un datagrama corto y uno de protocolo 3 (un router anterior) van a la basura (`rx.junk`,
  `rx.short`, `rx.bad_version`), no a una cola;
- un segundo servidor en el mismo puerto no arranca ("Address already in
  use") y el primero conserva sus 8 colas;
- con `lanes = 1`, un solo socket y sin basura, y el mismo tráfico.

### `deepq`: un camino del servidor con una cola larga (`lab.d/deepq.sh`)

s3 envía a 5 Mbit/s detrás de una cola de 20 MB (`tbf limit 20mb`) mientras
bajan 20.000 pps durante 10 s. La cola es más larga que el búfer de envío
del socket, que se llena. Con un solo socket para todos los caminos ese
búfer era de todos: los caminos sanos perdían sus copias y el túnel
también. Con colas, el túnel no pierde nada y los caminos sanos no tiran
ninguna copia. Medido aquí: con `lanes = 1` (`SERVER_EXTRA="lanes = 1"`)
el túnel perdió un 10,48 % y los caminos sanos 21.970 y 20.972 copias; con
8 colas, el túnel nada y los caminos sanos ninguna copia.

### `skew`: primeras llegadas (`lab.d/skew.sh`)

Tres enlaces idénticos, 2000 y 40.000 pps, 5 s en cada sentido. Un extremo
envía cada lote camino a camino y el primero llega primero: cuando era
siempre el camino 0, l1 ganaba el 90–100 % de las primeras llegadas. El
servidor ahora rota el primero; en bajada ningún enlace puede pasar del
60 % (`SKEW_MAX`, la puerta C5). La subida solo se informa hasta que el
router envíe en paralelo. Medido aquí: bajada 33,0–33,9 % por enlace a las
dos tasas; subida, l1 con el 92–99,7 %.

### `udpmem`: el presupuesto de recepción (`lab.d/udpmem.sh`)

Todos los sockets UDP de la máquina comparten `net.ipv4.udp_mem`: pasado su
primer valor el kernel deja un solo datagrama en la cola de cada uno, y
pasado el último, ninguno. Las colas del servidor se reparten la mitad del
umbral de presión. El escenario baja `udp_mem` a `8192 12288 16384`
páginas (y lo devuelve con un trap), para el servidor con `SIGSTOP`, llena
todas las colas con datagramas de protocolo 3 y, mientras, un par UDP
aparte intercambia 100 pps leyendo cada 250 ms (con algo de cola, como
cualquier socket UDP ocupado):

- el servidor informa `rcvbuf_capped` con el valor por socket del
  presupuesto (24 MiB para 10 sockets);
- todas las colas se llenan, y entre todas no pasan del presupuesto (sus
  colas en `/proc/net/udp` y `/proc/net/udp6`: con `*:59402` las colas son
  sockets AF_INET6 de doble pila donde el kernel tiene IPv6, y esos solo
  salen en `udp6`);
- el par no pierde nada;
- tras `SIGCONT`, el túnel funciona en los dos sentidos.

`udp_mem` es global y solo se ve en el primer namespace de red, así que
el servidor de `srv` lo estimaría desde la RAM: el escenario se lo enseña
con un tmpfs sobre `/proc/sys/net/ipv4` en su propio namespace de montaje,
como lo leería en un VPS. `UDPMEM_SHOW` le enseña otros valores: con los
reales no recorta, las colas dejan de llenarse en el primer valor de
`udp_mem` y el par pierde la mayoría (medido: llegaron 163 de 599). Solo
corre con `LAB_UDPMEM=1` (el CI lo pone): baja el sysctl de toda la
máquina unos segundos.

### `restart`: el servidor se reinicia (`lab.d/restart.sh`)

Hasta el protocolo 3, un servidor reiniciado abría la sesión de nuevo con
una secuencia aleatoria. Más o menos la mitad de las veces caía por detrás
de la ventana anti-replay del cliente, que tenía que rehacerla
(`engine/src/epoch.h` hasta la 0.4).

Con el protocolo 4 el servidor nuevo contesta las sondas con un HELLO y
retoma la sesión con la sonda que trae su cookie. Sigue 2^20 por delante de
lo que la sonda dice que recibió el cliente, así que no hay ventana que
rehacer (historia
[010](../docs/historias/010-ipv6-varias-ip-multicliente-nombres.md)). El
escenario comprueba que los enlaces vuelven y la bajada fluye.

- **Montaje:** 2000 pps de bajada (y 20 de subida, para que las sondas vayan
  cada 100 ms). l3 tiene 800 ms de cola siempre llena: tbf a 5 Mbit/s en el
  lado del cliente más un relleno de 7,8 Mbit/s, como `health` llena la suya
  de 500 ms. El WireGuard falso del VPS (`udpgen -l`) sigue al servidor
  nuevo a su puerto nuevo, como WireGuard cuando cambia el punto final del
  otro lado.
- **Fase 1:** 10 reinicios (`RESTARTS`). Cada vez, todos los enlaces tienen
  que estar vivos, con un paquete verificado después del reinicio, en 4 s
  (`RESTART_LIMIT`) desde que arranca el servidor nuevo (consultando el
  socket de control cada 50 ms), y la bajada tiene que volver a fluir. Si
  en 10 s (`WEDGE_S`) no lo están, cuenta como atascado y se reinicia el
  cliente para seguir.
- **Fase 2:** l1 y l2 en pausa (`cengarde ctl link … off`), así que el HELLO
  y la cookie solo pueden pasar por l3, detrás de su cola. Son `ALONE`
  reinicios (3).
- **Al final** dice cuántos HELLO tomó el cliente (`download.hellos`).
- **Con `REF`** compila el motor de ese commit en `$RUN` y lo mide igual. Un
  motor del protocolo 3 dice además `window_resets`.
- **Si el laboratorio no está listo** (la comprobación de antes de reiniciar
  falla, o el socket de control no contesta), no reinicia nada: dice qué
  extremo no corre, con el final de su log, en vez de contarlo como atascado.

Medidas: historia
[010](../docs/historias/010-ipv6-varias-ip-multicliente-nombres.md) (las del
protocolo 3, antes y después del arreglo de la 0.4.1, y las del 4).

### `replay`: cookies y reenvíos (`lab.d/replay.sh`)

`bench/bin/cgprobe` (`bench/cgprobe.c`) hace de segundo router con la clave del laboratorio, desde
direcciones del netns `cli` y junto al cliente real. Fabrica una sonda (con
o sin cookie), un DATA o un datagrama guardado, lo envía y dice qué contestó
el servidor: `hello COOKIE`, `refused`, `reply` o `none`. Comprueba lo
siguiente:

1. Una sonda de una sesión que el servidor no tiene recibe un HELLO, y no
   crea nada. La misma sonda con la cookie recibe respuesta y crea la
   sesión, con el IP pass que pide (se guarda como `p.bin`).
2. `p.bin` otra vez, desde otra dirección, es un duplicado: no hay
   respuesta y el camino no se mueve.
3. Una sonda nueva desde otra dirección, con la cookie vieja, recibe un
   HELLO para la dirección nueva y no mueve el camino. Con la cookie nueva,
   lo mueve (un NAT que cambió el puerto).
4. Un DATA de una sesión que no existe, una sonda con la pista de otro
   router y una con otra clave no reciben nada, y quedan contadas
   (`rx.no_session`, `rx.other_hint`, `rx.auth_failures`).
5. Con el servidor reiniciado, `p.bin` desde cualquier dirección, también
   la suya, recibe solo un HELLO, porque las claves de las cookies son
   nuevas: no crea la sesión ni escribe el IP pass.
6. El cliente real sigue sin pérdidas antes del reinicio y vuelve después,
   en menos de 5 s, sin ningún paquete `too_old`.

### `multiclient`: varios routers en un servidor (`lab.d/multiclient.sh`)

Tres motores cliente en el netns `cli`, cada uno un router con sus enlaces:
alpha (l1 y l2), bravo (l3 y l4) y charlie (l5), con una clave elegida para
que su pista sea la de alpha (el servidor prueba las dos claves). Un
servidor con una sección `[client NOMBRE]` por router y un `udpgen` como
WireGuard de cada uno. Comprueba:

1. Los tres routers vivos en todos sus enlaces, de los dos lados, en 3 s,
   con una sesión cada uno; `ctl links` con la columna `CLIENT`.
2. 1000 pps de subida y luego de bajada para los tres a la vez: lo de cada
   router llega una vez a su WireGuard y nada al de otro (otro router se
   vería como duplicados o como más de lo enviado).
3. IP pass: alpha lo pide y la tabla de reenvío dice `pass alpha` y sus dos
   reglas; bravo lo pide también y espera; alpha lo suelta y pasa a bravo.
   Cada router lo ve en sus respuestas (`passthrough.server`).
4. Recargas del servidor mientras alpha pasa 1000 pps: bravo desactivado
   (rechazado: `vps_refusing`, sin sesión) y activado otra vez (de vuelta en
   5 s), charlie quitado (su sesión se cierra). Alpha no pierde nada ni
   cambia de sesión, y el servidor no se reinicia.

Medido el 2026-10-10: los tres vivos en 1,1 s; 2999 de 2999 paquetes por
router en cada sentido; el IP pass pasa a bravo 0,8 s después de que alpha
lo suelta; bravo rechazado 0,5–0,6 s después de desactivarlo y de vuelta en
1,9–2,0 s; alpha, 8999 de 8999 a través de las tres recargas.

### `muteoff`: apagar el silenciado en caliente (`lab.d/muteoff.sh`)

Con 2000 pps por 3 enlaces, l3 recibe la misma cola de 500 ms que en
`health` y se silencia. Una recarga (SIGHUP) agrega `mute_behind_ms = 0`:
l3 tiene que volver a llevar datos en menos de `MUTEOFF_MAX_MS` (1000 ms),
con la cola puesta, decir por qué en el log («unmuted, muting is off») y no
volver a silenciarse mientras el silenciado siga apagado. Otra recarga quita
la línea y l3 se silencia de nuevo. Se prueba en los dos sentidos: la subida
la silencia el cliente y la bajada el servidor. El túnel no pierde nada ni
cambia de sesión.

Medido el 2026-10-10: l3 vuelve 58–139 ms después de la recarga. Con el
motor anterior al arreglo seguía silenciado 3 s después en los dos sentidos:
`unmute_behind_ms` no puede pasar de `mute_behind_ms`, así que quedaba en 0,
y un enlace atrasado no volvía mientras siguiera atrasado (historia 006).

### `wgpoke`: un router con la hora atrasada (`lab.d/wgpoke.sh`)

WireGuard ignora las iniciaciones de un router que volvió con la hora
atrasada; el servidor lo nota y hace que WireGuard inicie él
(`engine/src/wgwatch.h`, historia
[002](../docs/historias/002-wireguard-para-cengarde.md)). `bench/fakewg.py`
hace de los dos WireGuard: el del VPS ignora toda iniciación e inicia una
cuando lo empujan; el del router la contesta.

- **1:** con datos, WireGuard aprende la sesión del cliente y nadie empuja.
- **2:** el cliente se reinicia y llama. En 6 s (`KNOCK_S`) WireGuard recibe
  el empujón, su iniciación (hacia la sesión vieja) va por la nueva y llega
  al router, y la respuesta deja la sesión nueva como su endpoint.
- **3:** lo mismo, pero WireGuard inicia 7 s después del empujón (el real
  espera 15 s si su sesión sigue válida). Para entonces la sesión vieja
  expiró y la que llama heredó su puerto: la iniciación llega igual, sin
  redirección.
- **4:** el cliente para 8 s, más que `session_timeout_ms` (5 s aquí), y
  vuelve a llamar: la sesión nueva retoma el puerto de la anterior y la
  iniciación le llega directo.
- **5:** con `wireguard_poke = none` (recarga), un cliente que llama tras un
  reinicio no provoca empujones.

El motor de antes del arreglo falla las fases 2, 3 y 4; sin el traspaso del
puerto, falla la 3.

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

### `bond`: el laboratorio de bonding (`lab.d/bond.sh`)

Es el paso 0 de la Fase 5 ([historia 012](../docs/historias/012-bonding.md)).
A diferencia del resto, aquí no hay WireGuard falso: un WireGuard real
(`wgc` 10.79.0.2 en `cli`, `wgs` 10.79.0.1 en `srv`) pasa por cengarde, y
dentro va TCP real. En cada corrida, iperf3 sube o baja (`-R`) con Cubic y
con BBR, mientras ping mide el RTT del túnel cargado cada 100 ms. Los
enlaces se moldean con netem en los dos sentidos: retardo de un sentido,
tasa y una cola de 100 ms a esa tasa.

| Caso | Enlaces | Qué pasa |
| --- | --- | --- |
| `equal` | 2 × 50 Mbit/s, 25 ms | nada; también con el primer enlace solo |
| `het` | 60/20, 30/35 y 10/60 (Mbit/s / ms) | nada; también con el primer enlace solo |
| `reorder50` | 2 × 50 Mbit/s, 20 y 70 ms | 100 ms de diferencia de RTT |
| `reorder150` | 2 × 50 Mbit/s, 20 y 170 ms | 300 ms de diferencia de RTT |
| `linkdeath` | 3 × 40 Mbit/s, 25 ms | l2 pierde todo durante el tercio central |
| `starlink15` | 50/25 y 100/20 | cada 15 s, l2 pierde todo 1,5 s y su retardo cambia entre 20 y 50 ms (un patrón supuesto: el real está por medir) |
| `ltespike` | 50/25 y 50/30 | cada 5 s, l1 sube 300 ms durante 1 s |
| `deepq` | 20/25 con 500 ms de cola y 50/25 con 100 ms | nada |

Así queda medido lo que hoy da la redundancia (cada paquete por todos los
enlaces, gana la primera copia) y, en `equal` y `het`, lo que daría el mejor
enlace solo. El bonding tendrá que superar esas cifras. El caso `deepq` de
aquí no es el escenario `deepq` de arriba, que mide las colas del servidor.

Por cada caso, variante (`bonded` o `solo`), sentido y control de congestión
se escribe una línea JSON en `$RUN/bond.jsonl`. Cada línea lleva Mbit/s,
retransmisiones de TCP, p50 y p99 del RTT y CPU de cada cengarde. Al final
se imprime una tabla con las medianas, que también queda en `$RUN/bond.md`.

El escenario no juzga la velocidad: solo falla si falla la fontanería, es
decir, si no hay handshake, tráfico o pings. Necesita iperf3,
wireguard-tools y ping. Si el kernel no trae WireGuard (el contenedor
cloud), sirve wireguard-go con `WG_GO=/ruta/wireguard`. Sin netem, los
enlaces solo tienen tasa (`tbf`), sin retardos ni eventos, y el escenario
lo avisa; con `BOND_NEED_NETEM=1` eso pasa a ser un fallo.

```sh
sudo bench/lab.sh bond                                  # todos los casos, ~10 min
sudo BOND_CASES="equal reorder150" BOND_S=20 BOND_N=3 bench/lab.sh bond
sudo BOND_CC=bbr BOND_DIRS=down BOND_OUT=/tmp/b.jsonl bench/lab.sh bond
```

| Variable | Por defecto | Qué hace |
| --- | --- | --- |
| `BOND_CASES` | todos | casos a correr |
| `BOND_S` | 10 | segundos por corrida de iperf3 (`starlink15`, al menos 31) |
| `BOND_N` | 1 | repeticiones; la tabla da la mediana |
| `BOND_CC` | `cubic bbr` | controles de congestión |
| `BOND_DIRS` | `up down` | sentidos (`down` es iperf3 `-R`) |
| `BOND_OUT` | vacío | archivo al que añadir las líneas JSON |
| `WG_GO` | vacío | wireguard-go, si el kernel no tiene WireGuard |
| `BOND_NEED_NETEM` | 0 | 1: sin netem, falla en vez de avisar |

No entra en `ci`: lo corre su propio workflow, `bond.yml`, en cada cambio de
`engine/` o `bench/`, con todos los casos y una repetición. La tabla queda
en el resumen del job y el JSON en un artefacto. A mano (`workflow_dispatch`)
se pueden pedir más repeticiones, otros segundos y otros casos. En un runner
compartido las cifras de una sola repetición varían: para comparar dos
versiones, usa varias repeticiones en la misma máquina. Falta un cliente
QUIC dentro del túnel, que la historia 012 pide junto a TCP.

### `fieldrec`: el registrador de campo (`lab.d/fieldrec.sh`, `fieldrec.py`)

El otro instrumento del paso 0 mide el terreno: cómo se portan de verdad
los enlaces de la Pi. En el router, `cengarde-rec`
([guía de OpenWrt](../openwrt/README.md#registrar-los-enlaces-durante-días))
guarda cada versión nueva del archivo de estado del motor como una línea
JSON, en archivos por hora comprimidos con gzip. Aquí,
`fieldrec.py DIR|ARCHIVO...` los lee en orden y escribe un informe en
Markdown (`--json` guarda además el resumen). El informe trae:

- por enlace: tiempo arriba, caídas (cuántas, cuánto duran), RTT p50/p95/p99,
  su variación, pérdida de bajada y de subida, y silenciados;
- cuántos enlaces quedan vivos a la vez;
- por cada par: P(B caído | A caído), su *lift* frente a la independencia,
  qué parte de las caídas de A tuvo una de B a menos de 2 s, y la correlación
  de sus RTT;
- el spread de RTT entre los enlaces vivos y entre los dos mejores;
- las caídas y los saltos de RTT plegados sobre el reloj, módulo 15 s
  (Starlink reconfigura cada 15 s, en los segundos 12, 27, 42 y 57);
- por hora del día (`--utc-offset`), las caídas, el RTT y la pérdida de cada
  enlace.

Un enlace está caído si su estado no es `live`, o si no llegó nada por él
en tres intervalos de sonda (300 ms como mínimo). Los enlaces en pausa no
cuentan. La caída empieza en la última respuesta, que estampa el motor, y
termina a mitad de camino entre la última muestra caída y la primera de
vuelta. La resolución es `status_interval_ms`.

La hora de cada muestra es `time_ms` del motor. Las muestras de antes de que
NTP corrija el reloj (la Pi no tiene RTC) quedan fuera de los pliegues y
del perfil por hora. Con un motor anterior, sin `time_ms`, se usa la hora
del registrador, al segundo.

La pérdida de bajada de un enlace se calcula por intervalo: es lo que no
trajo de lo que entregó el túnel, cuando bajaron 20 paquetes o más. La de
subida es lo que el servidor dice haber recibido por él frente a lo que se
envió, en ventanas de 10 s, porque su cuenta llega con la última respuesta
de sonda. Ninguna de las dos mide capacidad: en redundancia, cada enlace
lleva el mismo tráfico.

`lab.d/fieldrec.sh` (en `ci`) lo prueba de dos maneras:

- `fieldrec_test.py`, sobre una grabación inventada con respuestas
  conocidas: a y b caen juntos, c cae solo y sin que el motor lo marque
  todavía, y Starlink corta en los segundos 12/27/42/57. La grabación trae
  además un reinicio del motor, un hueco, el reloj antes de NTP, un archivo
  de un motor sin `time_ms` y otro cortado por un apagón. Se comprobó que la
  prueba falla si se rompe el pliegue, la correlación, el corte entre
  corridas, la detección por silencio o el inicio de la caída.
- De punta a punta: el motor y `cengarde-rec` reales (bajo `sh`), a 4
  muestras por segundo, mientras s2 cae 3 s. Tienen que quedar solo
  archivos comprimidos, cada muestra una vez, una caída de 2 a 5 s en l2 y
  ninguna en l1 ni l3.

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
| `NLINKS` | 3 | enlaces activos; con más de 3 crea más veth (l4 10.0.4.1…) |
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

## Medidas más finas: `mt.py`, `jitter` y `mgen`

`bench/mt.py` es el driver del estudio de hilos: envuelve `lab.sh` (setup,
start, teardown) y repite su tráfico, y apunta por ejecución lo que la
línea de `lab.sh` no da: CPU por paquete en ns (de
`/proc/PID/task/*/schedstat`, por proceso y por hilo), descartes por socket
según su papel (los enlaces del cliente, las colas del servidor por orden
de creación, los sockets de WireGuard), los errores UDP de cada namespace y
los contadores de los dos motores, con las colas del servidor. Cada
ejecución es una línea JSON en `RESULTS` (por defecto `mt.jsonl`).

```sh
# gate S1: subida 80–110 kpps, lanes 1 y 8 intercaladas, 4 rondas, búferes
# del router a 32 MiB para que solo pueda tirar el servidor
sudo RUN=/tmp/cg-mt RESULTS=$PWD/s1.jsonl flock /tmp/cengarde-netns.lock python3 bench/mt.py s1 4
# 5 enlaces, 4 y 8 colas, por debajo del techo del router
sudo RUN=/tmp/cg-mt RESULTS=$PWD/s1.jsonl flock /tmp/cengarde-netns.lock python3 bench/mt.py s1 4 40000,50000,60000 4,8 5 5 up
python3 bench/mt.py table s1.jsonl
# barrido, enlace lento, perf y varias sesiones (ver el docstring)
sudo RUN=/tmp/cg-mt flock /tmp/cengarde-netns.lock python3 bench/mt.py sweep e1 down,up 40000,80000 5 3 1400 3
```

`bench/jitter.c` (`bin/jitter -d SEGUNDOS`) mide cuánto tarda en despertar
un sueño de 1 ms en cada CPU y marca los despertares tardíos con su hora
`CLOCK_MONOTONIC`: así se distinguen las pausas de la VM entera (tardíos a
la vez en varias CPU) de los atascos del motor. `bench/mgen.c` es el
WireGuard falso de varias sesiones que usan los comandos `multi` de
`mt.py`. `build` compila los dos.

### La puerta S1

`mt.py s1` intercala `lanes` ronda a ronda (1 y 8; con 5 enlaces, 4 y 8),
con los búferes del servidor por defecto y los del router a 32 MiB, 5 s y
4 rondas por punto. Cuatro sesiones con el mismo código (4 vCPU
compartidas con otros agentes; ≈: estimado por resta, porque en esa ronda
también tiró otro socket). `mt.py table` da por ronda lo que se tiró antes
de duplicar (en subida, en el socket de WireGuard del router; en bajada,
en el de la sesión en el servidor) y cuántas rondas quedan en ≤ 0,1 % sin
eso:

| Criterio | Medido | Veredicto |
| --- | --- | --- |
| 110 kpps con ≤ 0,1 % de pérdida en 4 de 4 rondas | 8 colas: 3/4, 2/4 y 2/4 (sin lo que tiró el router, 4/4, 3/4 y 3/4); 1 cola: 1/4, 0/4 y 0/4 | **no se cumple** |
| paquetes enteros perdidos en las colas, ≥ 10× menos que con una | 0 / 18.011, ≈12.643 / ≈107.977 (8,5×) y 1.650 / 42.747 (26×); sumadas, 11,8× | se cumple en 2 de 3 sesiones y en la suma |
| µs/paquete del servidor a ±5 % de una cola (40–110 kpps) | de −4,4 % a +4,9 % | se cumple |
| bajada a saturación (110 y 120 kpps) a ±5 % | µs/paquete de −2,4 % a +0,5 %; pérdida media menor con 8 colas en 3 de 4 puntos, y +0,25 puntos en el cuarto (3,44 → 3,69 %) | µs, sí; pérdida, no peor salvo ese punto, dentro del ruido entre rondas |
| 5 enlaces, 8 colas no peor que 4 (40–60 kpps) | ningún paquete entero perdido en el servidor; copias tiradas en las colas, 4.374 (50 kpps) y 7.526 (60 kpps) con 4 colas, ninguna con 8; µs/paquete de −0,3 % a +2,8 % | se cumple |

Tal como está escrita, S1 no pasa. El router de este laboratorio tiene un
solo hilo y a 110 kpps va al 81–102 % de CPU: en 4 de las 5 rondas que
fallaron con 8 colas tiró en su propio socket de WireGuard, antes de
duplicar. Y en dos de ellas el servidor perdió paquetes enteros, con las
tres colas desbordadas a la vez: una cola absorbe un parón más corto que
su búfer (~33 ms a 110 kpps *(cálculo)*), no un hilo único que se queda
atrás más tiempo. Con 5 enlaces, el router satura desde 60–70 kpps.
Detalle, la bajada y las opciones que quedan por decidir: historia
[011](../docs/historias/011-hilos.md).

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
- **Pasa con `on`:** el túnel no pierde ni un paquete en ningún sentido,
  los sockets de l1 y l2 no descartan nada (`socket_drops`), y como mucho el
  5 por mil (`MTSTALL_LATE_PM`) de los paquetes llega con 50 ms o más de
  retraso (`over50ms` de `udpgen`; la VM se para sola hasta 38 ms; el
  escenario aún no cruza esos retrasos con las pausas que marca
  `bench/jitter.c`).
- **`off` y `legacy`:** el único bucle lee todos los enlaces, así que el
  bucle ocupado se lleva el hilo principal: se informa, no se juzga.
- **Modos:** con `link_threads` en `CLIENT_EXTRA` (cada trabajo `lab` del
  CI pone uno) corre solo ese modo, con el resto de esos ajustes; sin él,
  los tres, cada uno en un subshell con todas las CPU.
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
  tramos de 60 s a 2000, 10 000, 20 000 y 40 000 pps y 80, 400 y 1400
  bytes por turnos, mientras l3 cambia de pérdida y retardo cada 2 min
  (netem; `tbf` si el kernel no lo tiene), el cliente recarga cada 5 min y
  l2 cae 10 s cada 10 min. Cada tramo imprime lo que perdió cada sentido y
  los descartes contados para ese sentido mientras corría, los que pueden
  explicarlo: de paquetes enteros, los del kernel al recibir en los sockets
  de ese sentido (colas llenas y puertos sin socket, `/proc/net/snmp` y
  `/proc/net/udp`), lo que el motor no pudo entregar a WireGuard y los
  errores de envío del WireGuard falso que envía; de copias, por enlace, lo
  que el motor no pudo enviar y lo que tiraron después el veth o la `qdisc`
  de l3. Un paquete solo se pierde si se pierden sus copias en dos enlaces
  o más, así que cuentan las copias de todos los enlaces menos el que más
  perdió: l3, limitado a propósito, o l2 mientras está caído, no explican
  solos una pérdida. Pasa (diseño D.4: entregado = enviado menos descartes
  contados) si ningún tramo pierde en un sentido más paquetes que los
  descartes contados para él, no hay avisos de hilos parados ni de sockets
  que no se pudieron vigilar, todas las recargas dicen `ok` y el RSS del
  cliente no crece más de 1 MiB tras los primeros 5 min. Al final de cada
  modo dice cuántas recargas, caídas de l2 y cambios de l3 hubo.

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
