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
wireguard-tools y `sch_netem`, para `bond`; ffmpeg (con libsrt para SRT), `ss` y python3,
para `stream`; y curl, para las demos del Go.

## Uso

```sh
sudo bench/lab.sh build    # udpgen, protoclient, ringbench, robench, cgprobe, macbench y cengarde en bench/bin/
sudo bench/lab.sh ci       # lo que corre el CI: smoke, health, control y los escenarios de lab.d con LAB_CI=1
sudo bench/lab.sh smoke    # prueba de humo de cengarde
sudo bench/lab.sh health   # salud de enlaces: un enlace con 500 ms de cola, subida y bajada (historia 006)
sudo bench/lab.sh control  # con tráfico: pausar un enlace, recargar dos veces, IP pass on/off; sin pérdidas (historia 009)
sudo bench/lab.sh muteoff  # una recarga con mute_behind_ms = 0 devuelve al instante un enlace silenciado, subida y bajada (lab.d, va en ci)
sudo bench/lab.sh multiclient  # tres routers en un servidor: tráfico sin cruces, IP pass y tabla de reenvío, recargas (lab.d, va en ci)
sudo bench/lab.sh stream   # transmisión simulada: RTMP y SRT por el túnel, con las grabaciones del motor y el informe de report.py (lab.d, a mano; ver bench/stream/)
sudo bench/lab.sh latency  # latencia y CPU con busy_poll_us 0, 50 y 200, y el Go si está compilado (historia 006)
sudo bench/lab.sh restart  # reinicios del servidor con tráfico: todos los enlaces vivos en 4 s (lab.d, va en ci)
sudo bench/lab.sh restart ebe570b  # lo mismo con el motor de otro commit, p. ej. el de antes del arreglo
sudo bench/lab.sh multiip  # servidor con varias direcciones: responde desde la de llegada (lab.d, va en ci)
sudo bench/lab.sh fallback # varias direcciones del servidor por enlace: failover, IPv6 que se salta, vuelta tras un corte, MTU de camino (lab.d, va en ci)
sudo bench/lab.sh names    # el servidor por nombre, con un DNS falso (fakedns.py): cambio de IP con tráfico (corte de ~3 s), DNS caído, TTL 0, NXDOMAIN (lab.d, va en ci)
sudo bench/lab.sh lanes    # las colas del servidor: una por enlace, la basura aparte, un segundo servidor rechazado; y lanes = 1 (lab.d, va en ci)
sudo bench/lab.sh native   # tunnel = native: ping (IPv4 e IPv6) e iperf3 TCP por el TUN con 2 enlaces y split, y dos routers con sus tun_route (lab.d, va en ci)
sudo bench/lab.sh deepq    # un camino del servidor con 20 MB de cola local no afecta a los demás (lab.d, va en ci)
sudo bench/lab.sh skew     # primeras llegadas repartidas entre enlaces idénticos, puerta C5 (lab.d, va en ci)
sudo bench/lab.sh wgpoke   # un router con la hora atrasada: el servidor hace que WireGuard inicie el handshake (lab.d, va en ci)
sudo bench/lab.sh wgport   # el socket del servidor hacia WireGuard nunca se queda con el puerto de WireGuard, con el rango efímero reducido a dos puertos (lab.d, va en ci)
sudo bench/lab.sh srcroute # un enlace cuya IPv6 más nueva solo tiene ruta por otro enlace: sale desde otra suya, y vuelve de la IPv4 cuando la ruta vuelve (lab.d, va en ci; se omite sin IPv6)
sudo LAB_UDPMEM=1 bench/lab.sh udpmem  # el presupuesto de recepción frente a net.ipv4.udp_mem (lab.d; baja el sysctl de toda la máquina unos segundos: solo en el CI)
sudo bench/lab.sh mtstall  # un hilo de enlace sin CPU no frena a los demás (link_threads = on; lab.d, va en ci)
sudo bench/lab.sh mtlat    # latencia y CPU de legacy, off, on y on con busy_poll a 2, 20 y 80 kpps (lab.d, a mano)
sudo bench/lab.sh soak     # 1 h por modo (off y on): tráfico variado, pérdidas, enlaces que caen, recargas (lab.d, a mano)
sudo bench/lab.sh reorder  # el reordenador del receptor y el reparto (paso 1 de la Fase 5): IMIX con desbordes y cabeza de línea; sin ellos en el motor, "skip" (lab.d, a mano)
bench/bin/robench          # ns por paquete de engine/src/reorder.h solo: en orden, 50/50 con 50 ms de diferencia, IMIX con bypass y desborde (1 s, sin root)
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
gcc 13.3, mínimos de 5 corridas de 0,2 s; donde dos corridas no
coinciden, las dos):

| Operación | ns |
| --- | --- |
| MAC de un DATA de 1400 B (`cg_hdr_verify`) | 720 |
| MAC de una sonda | 57 |
| buscar la sesión: 1 router, 32 (2 sesiones cada uno), 64 (4) | 3 / 5,4 / 10 |
| descartar una pista que no es de nadie | 1 |
| admitir una sonda: 1, 2 o 4 claves probadas | 57 / 111–122 / 222–252 |
| buscar y verificar un DATA: antes, y ahora con 64 routers | 721–729 / 727–738 |
| buscar y verificar una sonda: antes, y ahora con 1 router | 59–60 / 66–67 |

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
- **5:** el servidor se reinicia (una actualización) mientras el cliente
  llama con la hora todavía atrasada: lee de `wireguard_port_file` el puerto
  que conoce WireGuard, su sesión nueva lo toma y la iniciación de WireGuard
  llega al router.
- **6:** con `wireguard_poke = none` (recarga), un cliente que llama tras un
  reinicio no provoca empujones.

El motor de antes del arreglo falla las fases 2, 3 y 4; sin el traspaso del
puerto, falla la 3; sin `wireguard_port_file` (0.6.1 y antes), falla la 5
(comprobado quitando la clave del escenario el 2026-10-13).

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
dentro va tráfico real. En cada corrida, iperf3 sube o baja (`-R`) con Cubic
y con BBR, y un emisor QUIC (`quicperf.py`, abajo) hace lo mismo, mientras
ping mide el RTT del túnel cargado cada 100 ms. Los
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
| `lossy` | 60/20, 30/35 y 50/25 | cada enlace pierde el 1 % al azar en los dos sentidos: la pérdida que el rescate (`rescue_kb`, `rescue_wait_ms`) tapa; no está en la lista por defecto (`BOND_CASES`); córrelo con `CLIENT_EXTRA` `rescue_wait_ms = 60` y `SERVER_EXTRA` `rescue_kb = 32768` |
| `starlink15sw` | 100/25 y 50/20 | como `starlink15` con las capacidades cambiadas; no está en la lista por defecto (`BOND_CASES`) |

Así queda medido lo que hoy da la redundancia (cada paquete por todos los
enlaces, gana la primera copia) y, en `equal` y `het`, lo que daría el mejor
enlace solo. El bonding tendrá que superar esas cifras. El caso `deepq` de
aquí no es el escenario `deepq` de arriba, que mide las colas del servidor.

**Variantes (`BOND_VARIANTS`).** Qué corre en cada caso. Los ajustes de la
variante van a los dos extremos por `CLIENT_EXTRA` y `SERVER_EXTRA` (los
tuyos, si los hay, vienen después):

| Variante | Qué es |
| --- | --- |
| `bonded` | la redundancia de hoy con los valores por defecto del motor; lo que corre el CI. Es la misma que `redund`, que es el nombre de las tablas del paso 1 |
| `solo` | el primer enlace solo (`NLINKS` 1) |
| `redund` | los valores por defecto |
| `redund-ro0` | `reorder = no` |
| `redund-dw20` | `reorder_dup_wait_ms = 20` |
| `split` | `split = force`, `mute_behind_ms = 0` y `reorder_max_wait_ms = 400` (con el silenciado el enlace lento de `reorder150` saldría del reparto y se escondería la diferencia). Con la guarda de retardo del motor (`split_spread_ms`, 30 ms por omisión), en `reorder50` y `reorder150` el enlace lento sale igual del reparto y lleva copias (`split_spread_copy`, por omisión): es el modo de campo; el reparto sin filtro es `split-sp0` |
| `splitnoro` | `split` con `reorder = no`: el control de `split` |
| `NOMBRE-bpN` | cualquiera de ellas con `reorder_bypass = N` |
| `NOMBRE-spN` | cualquiera de ellas con `split_spread_ms = N`. `split-sp0` es el reparto sin la guarda de retardo, el round-robin crudo: con el valor por omisión del motor (30 ms), en `reorder50` y `reorder150` el enlace lento deja su parte y lleva copias, así que `split` ya no mide el reparto sin filtro |
| `NOMBRE-nc` | cualquiera de ellas con `split_spread_copy = no`: el enlace que la guarda de retardo deja fuera no lleva copias, y si queda uno solo en el reparto, ese lleva todo (`split-sp30-nc` frente a `split-sp30`). Los sufijos van en cualquier orden, una vez cada uno (`split-sp0-bp64`, `split-sp250-nc`) |

Sin `BOND_VARIANTS` corren `bonded` y `solo`, esta última solo en `equal` y
`het`: exactamente lo que había antes de las variantes, así que las tablas
viejas y las 10 repeticiones de la historia 012 siguen siendo comparables y la
estimación de tiempo del CI vale. Con `BOND_VARIANTS` dado, `solo` corre también
en `reorder50` y `reorder150`, las referencias de `split`; `BOND_SOLO_CASES` lo
fija a mano. Antes de empezar, el motor revisa los ajustes de cada variante
(`cengarde -t`), para que una errata no cueste una corrida entera. Los ajustes
de reorden y reparto los lee el motor del paso 1; uno anterior solo los avisa
como desconocidos y las variantes `split` no harían nada.

**El límite de netem [razonado; sin medir, el contenedor no tiene netem].**
netem cuenta contra su `limit` los paquetes que van dentro del retardo: un
enlace de 170 ms y 50 Mbit/s lleva unos 760 paquetes de 1400 bytes en vuelo,
más que los 456 de límite de una cola de 100 ms, y tira lo que no cabe (sobre
todo en `reorder150`; en `reorder50`, con 310 en vuelo, la cola útil queda en
una tercera parte). Las tablas del paso 0 son así y la redundancia lo esconde
tras el otro enlace; un reparto no tiene otra copia, y esas pérdidas serían
retransmisiones del `split` que no son suyas. `BOND_NETEM_INFLIGHT=1` suma al
límite lo que cabe en el retardo: cambia la línea base del paso 0 (por eso
está apagado por defecto), y `reorder_hol` lo enciende. Para comparar `split`
con `solo` y `splitnoro` en `reorder50` y `reorder150`, enciéndelo en las tres
(`variants` y `netem_inflight` en `bond.yml`).

Por cada caso, variante, sentido y control de congestión se escribe una línea
JSON en `$RUN/bond.jsonl`. Cada línea lleva Mbit/s, retransmisiones y su tasa
(`retrans_pct`: sobre los segmentos que escribió el emisor, que son los bytes de
iperf3 entre el MSS, o sobre los paquetes enviados en QUIC), p50 y p99 del RTT
y CPU de cada cengarde, y RTT suavizado y mínimo del emisor. Un segundo ping de
1200 bytes (el de 84 pasa de largo por el reordenador; este es de la clase que
se retiene) da `rtt_big_p50` y `rtt_big_p99`. Para QUIC (`cc` = `quic`),
`retrans` son los paquetes que el emisor declaró perdidos, y la línea suma los
paquetes enviados y los espurios (los declarados perdidos que llegaron después:
puro desorden); en TCP esos campos van en `null`.

Antes y después de cada corrida se copian los archivos de estado de los dos
extremos (el motor los escribe cada segundo: por eso hay una espera de 1,1 s por
corrida; el «después» de una es el «antes» de la siguiente) y los contadores de
las interfaces de WireGuard (`ip -s -j link show`). La línea lleva las
diferencias en `eng.cli` y `eng.srv`: de `reorder.*` (`held_total`, `in_order`,
`bypassed`, `split_rx`, `lost_rack`, `lost_cap`, `lost_overflow`, `lost_flush`,
`late_after_rack`, `late_after_cap`, `late_after_overflow`, `late_old`, `far`,
`resyncs`, `pool_full`, y los histogramas `wait` y `late_by`), `too_old`
(`download.too_old` del cliente, `rx.too_old` del servidor), `wireguard_drops`
y `lost_all`; `held_max` y `max_burst` son máximos desde que arrancó el caso, no
diferencias. En `wg_rx_dropped`, la diferencia de `rx_dropped` de `wgc` y `wgs`.
Lo que el motor no tiene (hoy, todo `reorder`) va en `null`, nunca es un error.
Con `BOND_SMALL` > 0 (pps) corre además un flujo de datagramas de
`BOND_SMALL_SIZE` bytes (160) por el túnel, en el sentido de la corrida, y la
línea trae `small` (enviados, recibidos, p50 y p99 de su retardo en µs).

Al final se imprimen las tablas (Markdown), que también quedan en
`$RUN/bond.md`; las hace `bondtab.py`, que prueba `bondtab_test.py`:

- **La principal**, por caso, variante, sentido y control de congestión. Con
  una repetición da el valor de la corrida; con `BOND_N` > 1 da la mediana y,
  entre paréntesis, el rango p25–p75 (mínimo–máximo con menos de 4
  repeticiones), para Mbit/s, retransmisiones y su tasa, RTT de 84 y de 1200
  bytes y el flujo pequeño. La columna `spurious` aparece cuando hay corridas
  QUIC.
- **Dos modos.** Una celda de 6 corridas o más se marca con † si sus valores
  son dos grupos y no ruido (historia 012, «Medidas con dispersión»), y debajo
  se listan los dos con cuántas corridas tiene cada uno. La regla: se ordenan
  los valores y se cortan por el hueco más ancho que deje al menos 2 corridas a
  cada lado; son dos modos si ese hueco es mayor que el rango (máximo menos
  mínimo) del más ancho de los dos grupos y no menor que un mínimo por
  métrica, lo que la medida no distingue (1 Mbit/s, 10 retransmisiones o
  pérdidas espurias, 5 ms de RTT). Una corrida rara es una anomalía, no un modo;
  es una descripción de 10 corridas, no una prueba.
- **Contadores del motor**, la mediana de las diferencias de cada corrida en el
  extremo que recibió (el servidor en `up`, el cliente en `down`), y `too_old`,
  `wireguard_drops`, `lost_all` y `rx_dropped` de los dos. Debajo, si `too_old`,
  `pool_full` o `wireguard_drops` no son 0, lista las corridas.
- **Contra la referencia**: cada variante frente a las referencias de su
  familia que haya en su grupo (mismo caso, sentido y control), una fila por
  cada una: `bonded`, `redund` y `redund-dw20` frente a `redund-ro0`; `split`
  frente a `solo` y frente a `splitnoro`, su control; `redund-ro0` y
  `splitnoro` frente a `solo`; una variante con sufijo (`-bpN`, `-spN`),
  frente a la misma sin su último sufijo si corrió (así `split-sp0` frente a
  `split`: lo que cambia la guarda), y si no frente a la más cercana sin
  sufijos que haya, o como la base. Sin ninguna de sus referencias en el
  grupo, frente a la primera que haya de `solo`, `redund-ro0` y `splitnoro`
  (así `bonded` sigue contra `solo` en la corrida de CI). Así una sola corrida
  con `solo` y las dos familias da las filas de los dos criterios de salida del
  paso 1: tasa ≤ 1,2× un enlace solo (`split` contra `solo`) y CPU ≤ 1,05× y
  +0 ms (`redund` contra `redund-ro0`). `--ref A,B` vuelve a la regla de antes:
  todas frente a la primera de esas que tenga el grupo. Cada fila da la razón
  de las medianas de Mbit/s, de la tasa de retransmisiones y de la CPU de cada
  extremo, la diferencia de los RTT y el valor p de Mann–Whitney (desde 5
  corridas por lado) del goodput y de la tasa de retransmisiones.

El escenario no juzga la velocidad: solo falla si falla la fontanería, es
decir, si no hay handshake, tráfico o pings. Necesita iperf3,
wireguard-tools y ping, y aioquic para QUIC (abajo). Si el kernel no trae
WireGuard (el contenedor cloud), sirve wireguard-go con
`WG_GO=/ruta/wireguard`. Sin netem, los enlaces solo tienen tasa (`tbf`), sin
retardos ni eventos, y el escenario lo avisa; con `BOND_NEED_NETEM=1` eso
pasa a ser un fallo.

**QUIC (`quicperf.py`).** Un servidor y un cliente de transferencia masiva
sobre aioquic, para que los mismos enlaces vean también un emisor QUIC (lo
que usan los navegadores y los teléfonos, aunque no con esta biblioteca): un
flujo durante N segundos, subiendo o bajando, que imprime un objeto JSON con el
goodput, los bytes, los paquetes enviados, los declarados perdidos y los
espurios, el RTT suavizado y mínimo y el control de congestión (`cubic` o
`reno`, los dos que trae aioquic). Su detección de pérdidas no se adapta al
desorden (3 paquetes o 9/8 del RTT), así que un camino que reordena más que
eso le cuenta como perdido lo que solo llegó tarde. El servidor genera un
certificado autofirmado al arrancar, en memoria. Instalación y prueba a
mano, sin el laboratorio:

```sh
pip install aioquic==1.2.0       # en Ubuntu 24.04, como root: --break-system-packages --ignore-installed
python3 -I bench/quicperf.py server --bind 127.0.0.1 &
python3 -I bench/quicperf.py client 127.0.0.1 --dir down --secs 5
```

Es Python puro, en un solo núcleo: a más de 100–200 Mbit/s lo limita su CPU
(unos 200 Mbit/s por loopback en este contenedor), por debajo de iperf3. La
versión de aioquic está fijada porque `quicperf.py` usa dos de sus
internos (la recuperación de pérdidas y el emisor del flujo).

```sh
sudo bench/lab.sh bond                                  # todos los casos, ~18 min
sudo BOND_CASES="equal reorder150" BOND_S=20 BOND_N=3 bench/lab.sh bond
sudo BOND_CC=bbr BOND_DIRS=down BOND_OUT=/tmp/b.jsonl bench/lab.sh bond
sudo BOND_VARIANTS="split solo splitnoro" BOND_CASES="equal reorder50 reorder150" BOND_N=10 bench/lab.sh bond
BOND_N=10 BOND_VARIANTS="split solo" bench/lab.sh bond_plan   # cuánto tardaría, sin correr (sin root)
python3 bench/bondtab_test.py                           # pruebas de las tablas, sin root
```

| Variable | Por defecto | Qué hace |
| --- | --- | --- |
| `BOND_CASES` | todos | casos a correr |
| `BOND_VARIANTS` | `bonded solo` | variantes (arriba); dadas, `solo` corre también en `reorder50` y `reorder150` |
| `BOND_SOLO_CASES` | `equal het` | casos en los que corre `solo` |
| `BOND_SMALL` | 0 | pps de un flujo de datagramas pequeños por el túnel durante la corrida (0: ninguno) |
| `BOND_SMALL_SIZE` | 160 | bytes de esos datagramas (un datagrama de WireGuard de 224) |
| `BOND_NETEM_INFLIGHT` | 0 | 1: el límite de netem cuenta también lo que cabe en el retardo (arriba); cambia la línea base del paso 0 |
| `BOND_WARM` | 0 | segundos de la misma transferencia antes de cada corrida, para que arranque con los enlaces medidos (el arranque rápido, historia 013); solo con `BOND_REST` > 0 y sin `quic` |
| `BOND_REST` | 0 | segundos sin tráfico entre ese calentamiento y la corrida: arranca desde el reposo (0: ninguno) |
| `BOND_KEEP` | vacío | directorio donde guardar el JSON de iperf3 de cada corrida, con intervalos de 0,5 s (la rampa de los primeros segundos) |
| `BOND_S` | 10 | segundos por corrida (`starlink15`, al menos 31) |
| `BOND_N` | 1 | repeticiones; la tabla da la mediana y el rango |
| `BOND_CC` | `cubic bbr quic` | controles de congestión; `quic` solo si importa aioquic, y si no se avisa |
| `BOND_QUIC_CC` | `cubic` | el control de congestión del emisor QUIC (`cubic` o `reno`) |
| `BOND_QUIC_MTU` | 1350 | tamaño del datagrama QUIC (el túnel tiene MTU 1380) |
| `BOND_DIRS` | `up down` | sentidos (`down` es iperf3 `-R`) |
| `BOND_OUT` | vacío | archivo al que añadir las líneas JSON |
| `WG_GO` | vacío | wireguard-go, si el kernel no tiene WireGuard |
| `BOND_NEED_NETEM` | 0 | 1: sin netem, falla en vez de avisar |
| `BOND_NEED_QUIC` | 0 | 1: sin aioquic, falla en vez de avisar |

No entra en `ci`: lo corre su propio workflow, `bond.yml`, en cada cambio de
`engine/` o `bench/`, con todos los casos y las variantes por defecto y una
repetición (~21 min, con `BOND_NEED_NETEM=1` y `BOND_NEED_QUIC=1`). La tabla
queda en el resumen del job y el JSON en un artefacto. A mano
(`workflow_dispatch`) se pueden pedir más repeticiones, otros segundos, otros
casos y otras variantes (`variants`). En un runner compartido las cifras de una
sola repetición varían: para comparar dos versiones, usa varias repeticiones en
la misma máquina. El primer paso del job es `bond_plan`, que cuenta los pares
de caso y variante, y rechaza lo que pasaría de 180 min (el límite del job es
de 200). Con 10 repeticiones y todos los casos con las variantes por defecto
son unos 175 min; para menos tiempo, elige casos: `equal reorder150
starlink15` con 10 repeticiones son unos 85 min, y las variantes `split solo
splitnoro` en `equal reorder50 reorder150`, unos 140. Cada corrida suma ahora
la espera de 1,1 s por el estado del motor y un ping más, y la estimación
cuenta 4 s de arranque y cierre por corrida (eran 3).

### `interferer`: alguien más carga la celda de un enlace (`lab.d/interferer.sh`, `interferer.py`)

El experimento de campo del 2026-10-16 (historia 013): un teléfono cargando
la misma celda que uno de los enlaces no hace perder paquetes, los encola;
la parte del enlace cae a la mitad o un tercio, su cola crece 35–40 ms por
segundo mientras dura la carga y se vacía en menos de 0,2 s al terminar.
Aquí: el túnel nativo (como `native`) sobre 2 enlaces con netem, l1 de
`IF_RATE` (40) Mbit/s, 20 ms y 2 s de cola (`IF_QUEUE_MS`), y l2 de
`IF_RATE2` (20) Mbit/s, 30 ms y 200 ms de cola; un TCP de subida a ritmo
constante (`iperf3 -b`, `IF_LOAD` = 24M, como un emisor RTMP), un ping por
el túnel cada 100 ms, y la subida de l1 cortada a `IF_RATE / IF_DIV` (un
tercio) solo del lado del cliente (en el campo solo dañaba la carga en el
mismo sentido); `IF_JITTER_MS` hace oscilar el retardo de l1 (el `jitter`
de netem, que además desordena algunos de sus paquetes). Casos
(`IF_CASES`):

- `long`: un corte de `IF_S` (10) s;
- `burst`: `IF_BURSTS` (3) cortes de 2 s, cada 8 s;
- `drain`: 10 s cortado, 1 s a pleno (la cola se vacía al ritmo de la celda)
  y 10 s a la mitad (la celda sigue compartida): lo que la capacidad toma
  del vaciado.

La base del motor es `split = on`, `capacity_weights = yes` y `split_eta =
yes`; `IF_EXTRA` suma ajustes a los dos extremos (p. ej. `split_predict =
yes`) y `CENGARDE_BIN` compara binarios. `interferer.py sample` lee los
contadores de netem de los dos enlaces cada 100 ms (lo ofrecido a cada uno
y su cola) y el estado del cliente cada segundo; `measure` da por corte
`off_s` (desde el corte hasta el primer segundo en que el reparto le ofrece
a l1 no más que su nuevo ritmo), `back_s` (desde el fin del corte hasta que
le vuelve a ofrecer el 80 % de lo de antes), `cap_s` (lo mismo para su
`capacity_mbit`), `cap_over` (en `drain`, la mayor capacidad mientras la
celda da la mitad, sobre esa mitad: más de 1 es el rebote del vaciado),
`q_ms` y `q100_s` (la mayor cola de l1 y cuánto pasó de 100 ms), y por
corrida el RTT máximo de TCP y las retransmisiones (iperf3) y el p99 del
ping. Una línea JSON por corrida va a `IF_OUT`; `interferer.py table
IF_OUT` da la mediana por variante y caso. No está en `ci` (cada corrida
dura ~45 s). Ejemplo, antes y después:

```sh
sudo CENGARDE_BIN=/tmp/cengarde-master IF_LABEL=master IF_N=3 IF_OUT=/tmp/if.jsonl bench/lab.sh interferer
sudo IF_LABEL=predict IF_EXTRA="split_predict = yes" IF_N=3 IF_OUT=/tmp/if.jsonl bench/lab.sh interferer
```

Las medidas del 2026-10-16 están en la historia 013.

### `reorder`: el reordenador del receptor y el reparto (`lab.d/reorder.sh`, `robench`)

Para el paso 1 de la Fase 5 (historia 012). `lab.d/reorder.sh` se escribió
contra los ajustes y el estado del plan (`split`, `reorder_*`, el objeto
`reorder` del estado): con un motor que no lo tiene dice
`reorder: skip: engine without reorder` y sale con 0; sin netem,
`skip: no netem here` (con `REORDER_NEED_NETEM=1`, un fallo). Va con
`LAB_CI=0` hasta que entre la integración. Los dos escenarios corren con
`split = force` en los dos extremos y el silenciado apagado.

- **`reorder_imix`: el desborde.** Paquetes IMIX (`udpgen -s imix`: 7 de 64, 4
  de 576 y 1 de 1400 bytes; WireGuard falso) por dos enlaces con 150 ms de
  diferencia (20 y 170 ms, con una cola de 60 000 paquetes, porque netem cuenta
  contra su límite lo que va en el retardo), arriba y abajo, a dos tasas. La
  ventana es de 4096 secuencias: a 20 kpps (205 ms) cabe la diferencia y a 30
  kpps (136 ms) no, así que hay desborde. En todas: cada paquete llega una vez
  (`uniq` = `sent`), `too_old` y `wireguard_drops` en 0 y `pool_full` en 0. Bajo
  la ventana, sin desborde (`lost.overflow` = 0) y con los paquetes grandes en
  orden (`IMIX_BELOW_ORD_MAX_PERMILLE`, 5 por mil); sobre ella, `lost.overflow`
  > 0, y se informa qué parte de los grandes llegó detrás de uno posterior
  (`bigord` de `udpgen`). Esa parte no tiene por qué ser casi 0: la copia tardía
  de un hueco forzado sale desordenada, y a 30 kpps con 150 ms es la de todos
  los paquetes del enlace lento; `IMIX_ORD_MAX_PCT` la acota si se quiere.
  `IMIX_S` (8) son los segundos de cada tramo.
- **`reorder_hol`: la cabeza de línea.** Lo real, el túnel con WireGuard (el
  escenario `bond`) sobre `HOL_CASE` (`reorder50`), con iperf3 y QUIC
  (`HOL_CC`) y, a la vez, `HOL_PPS` (50) datagramas de `HOL_SIZE` (160) bytes
  por el túnel, con `reorder_bypass` 0, 128 y 256 (`HOL_BYPASS`). Cuánto espera
  el flujo pequeño detrás de los grandes retenidos (p50 y p99 de su retardo) y
  qué ven las transferencias (retransmisiones y pérdidas espurias) sale en la
  tabla de `bond`. Falla solo si algo se tira por viejo o en el socket de
  WireGuard.

**`robench`** (`bench/robench.c`, lo compila `build`): cuesta `engine/src/reorder.h`
sola, en ns por paquete, con flujos hechos a mano y el llamador que usará el
motor (un `cg_ro_next` tras cada paquete); cada escenario, solo la lógica y con
la copia del cuerpo al búfer del grupo. En orden, 1 de cada 100 adelantado con
`reorder_dup_wait_ms` 0, 50/50 por dos enlaces con 50 ms de diferencia, IMIX con
bypass y el desborde (IMIX a 30 kpps con 150 ms). Cada paquete tiene que salir
exactamente una vez, o falla. Tarda menos de un segundo con un millón de
paquetes por escenario (el primer argumento lo cambia). En este contenedor x86
compartido (gcc -O2, cuatro corridas) [medido]: en orden 10–14 ns por paquete;
con diferencia entre enlaces, 17–26 ns la lógica sola y 19–47 ns con la copia
del cuerpo (la mitad de los paquetes se retiene en `spread 50`, el 21 % en
IMIX). Ruidoso, y sirve para comparar versiones, no como coste en la Pi.

### Transmisión simulada (`bench/stream/`)

RTMP (TCP) y SRT (UDP) por el túnel sin una transmisión real: `send.sh` en el
PC de la LAN de la Pi (imagen de prueba con reloj al milisegundo, NVENC o
libx264, y lo que ve el emisor: `ss -tin` cada 100 ms, el progreso de ffmpeg,
las estadísticas de SRT), `recv.sh` en un servidor propio (graba y anota la
llegada de cada paquete de video) y `report.py`, que junta las dos puntas con
la exportación de `fieldrec.py` y dice qué congelamiento o caída de la ventana
TCP coincidió con un corte o un silenciado de un enlace. `lab.sh stream` lo
corre de punta a punta en el laboratorio de `bond` (a mano). Comandos,
claves, codificadores por máquina y lo que se probó: [stream/README.md](stream/README.md).

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
  enlace;
- si el router repartió la subida, qué hizo cada enlace en el reparto
  mientras repartía (`split_share`: su parte, fuera por su retardo con
  `split_spread_ms`, con copias o sin ellas según `split_spread_copy`,
  esperando una medida de retardo...),
  cuántas veces la guarda lo sacó y cuánto iba detrás del más rápido.

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

#### Radio: cruzar con `cell-monitor` (`--rf`)

`--rf ARCHIVO...` suma al informe una sección **Radio** con lo que
`cell-monitor` (SmoothStream, `Cell monitor/`) manda a InfluxDB desde cada CPE
5G: RSRP, RSRQ, SINR, banda, RAT y celda. Sin `--rf` el informe no cambia.
Pon `--rf` después de las grabaciones, porque toma todos los nombres que le
siguen:

```sh
influx query --raw 'from(bucket: "Cellular")
  |> range(start: 2026-10-14T00:00:00Z, stop: 2026-10-21T00:00:00Z)
  |> filter(fn: (r) => r._measurement =~ /^cell_(nwinfo|serving_lte|serving_nr|rxpath|ca|monitor)$/)
  |> filter(fn: (r) => r._field =~ /^(sinr|rsrp|rsrq|cellid|pci|earfcn|arfcn|up|plmn|(rsrp|rsrq|sinr)[1-4])$/)' > radio.csv
bench/fieldrec.py /ruta/a/cengarde-rec --utc-offset -3 --rf radio.csv --json campo.json
```

- **Formatos** (se reconocen por el contenido; planos o con gzip): CSV
  anotado de InfluxDB (`#group`, `#datatype`, `#default`), de `influx query
  --raw` o de la descarga de la interfaz, con una fila por campo o con
  `pivot()` y con varias tablas en un archivo; y *line protocol* con
  timestamp (s, ms, µs o ns, por el tamaño; `--rf-precision` lo fija). Las
  demás mediciones y campos se ignoran. La salida de `cell-monitor -n` no
  sirve: no lleva timestamp, y se avisa.
- **Asignación a un enlace:** el tag `leg` contra `links[].label` y, si no,
  `links[].name`, sin distinguir mayúsculas y con la limpieza de
  `cell-monitor` (minúsculas, espacios a `_`, solo `a-z 0-9 _ . -`). Sin
  `leg`, el `carrier` contenido en la etiqueta. Lo que no se asigna sale como
  `NOT MAPPED` con el motivo, y también los enlaces sin radio; si la etiqueta
  de un enlace nombra a otro operador que el `carrier` de su serie, avisa de
  un posible cruce. `--rf-map LEG=ENLACE` asigna a mano.
- **El reloj:** `cell-monitor` no manda timestamp, así que cada punto lleva la
  hora del servidor de InfluxDB al recibir el POST, en segundos. Esa hora y la
  de la Pi (`time_ms` del motor) tienen que coincidir; la del CPE no cuenta.
  `--rf-offset S` suma S segundos a las horas de radio. Todo se calcula solo
  donde hay grabación y radio a la vez, y el informe da ese solape.
- **Qué trae:** por enlace, el tiempo en cada RAT y banda, p10/p50/p90 de SINR,
  RSRQ y RSRP, y los cambios de RAT, de banda y de celda por hora. Luego, para
  cada tipo de evento del enlace (caída, silenciado de subida, silenciado de
  bajada), cuántos inicios tuvieron un cambio a menos de N s (N = 2 intervalos
  de radio, mínimo 20 s; `--rf-window` lo fija) frente a lo esperado al azar,
  que es la parte del tiempo cubierta por esas ventanas, y cuántos fueron
  antes o después. Ese azar se calcula dos veces: *global* (sobre todo el
  tiempo) y *local* (para cada episodio, sobre el tiempo a menos de L = 30 min
  de él; `--rf-local` lo fija y 0 lo apaga). El local evita que las horas en
  que caídas y cambios de radio abundan a la vez (un auto en movimiento, una
  tarde con mucha gente, lluvia) se lean como si uno anunciara al otro: las
  caídas esperadas son la suma de esas partes y el p sale de la cola
  Poisson-binomial (exacta; con más de unos 3 millones de celdas, una
  aproximación normal). La lectura usa los *episodios* (eventos separados por
  60 s o 2N, porque los de una ráfaga comparten ventana), pide 10 y se hace
  con el azar local si lo hay; la tabla dice cuál usó. Después, el SINR del
  minuto anterior a cada episodio frente al mismo promedio en todo momento del
  enlace (global) y frente a los de las muestras a menos de L de él, sin
  [e−60 s, e+N] (local, que es el que se lee), y su tendencia frente a los 4
  minutos previos. Al final, una tabla por hora del día. Con muchas filas es
  normal que alguna salga por azar: pesan más los conteos que la palabra.
- **Antes de la semana:** pon `INTERVAL=10` en `/etc/cell-monitor.conf` (por
  defecto son 30 s), usa un `CPE_ID` y un `MODEMn_ID` distintos en cada CPE,
  pon `MODEMn_LEG` igual a la etiqueta del enlace en cengarde (o a su
  nombre), y sincroniza por NTP la Pi y el servidor de InfluxDB. Comprueba
  con `cell-monitor -n` que el SINR sale en dB (de −20 a 30): algunos módems
  lo dan en 1/5 de dB, de 0 a 250, y el informe lo avisa si pasa de 50.

#### Radio cada segundo desde un PC (`radiorec.py`)

`cell-monitor` mide cada 5 s, y una cola de módem se llena en menos de uno.
`radiorec.py` corre en un PC que llega por ssh a los CPEs (claves cargadas)
y pregunta cada segundo a cada módem Quectel, por un puerto AT que nadie
más use (`cell-monitor` y `cell-watchdog` tienen los suyos), solo comandos
de consulta: `AT+QENG="servingcell"` (celda servidora LTE, y la pata NR en
NSA o la celda en SA), `AT+QCAINFO` (cada portadora agregada) y
`AT+QNWINFO`. Escribe en el mismo line protocol que `cell-monitor`, con su
hora de pared (NTP, como la Pi), así que `fieldrec.py --rf` lo cruza sin
cambios; la etiqueta `leg` es la del enlace de cengarde. También guarda
cada respuesta cruda (`.raw.jsonl`), para reinterpretar lo que el parser
haya entendido mal. La potencia de transmisión (`tx_power`) viene en
décimas de dBm; en reposo (`NOCONN`) algunos módems no la dan.

```sh
# el banco: Entel y WOM en el CPE de dos módems, Movistar y Claro
python3 bench/radiorec.py -o ~/campo/radio \
    -m cpe1:/dev/ttyUSB2:ENTEL -m cpe1:/dev/ttyUSB6:WOM \
    -m cpe2:/dev/ttyUSB2:MOVISTAR -m cpe3:/dev/ttyUSB2:CLARO
bench/fieldrec.py /ruta/a/cengarde-rec --rf ~/campo/radio/radio-*.lp
```

`radiorec_test.py` (en `lab.d/fieldrec.sh`) lo prueba con las respuestas
reales de los cuatro módems del banco (2026-10-14: LTE solo, EN-DC con su
línea NR5G-NSA, tres portadoras LTE agregadas, una NR secundaria, una
portadora configurada y sin activar), más la forma SA del manual, y que lo
que escribe vuelva a leerse por `fieldrec.py`.

#### Exportar para R o pandas (`--export`)

`--export DIR` escribe, mientras analiza, CSV planos en formato largo (una
fila de cabecera, hora en UTC ISO y en segundos desde 1970, el enlace por
nombre y etiqueta, celda vacía donde falta el dato). `--export-gzip` los
escribe como `.csv.gz`. Nada se calcula aparte: son los mismos números del
informe, fila a fila.

```sh
bench/fieldrec.py /ruta/a/cengarde-rec --rf radio.csv --export campo/ --export-gzip
```

| Archivo | Una fila por |
| --- | --- |
| `samples.csv` | enlace y muestra de estado: estado, caído, silenciado, RTT y su desvío, retraso, deltas de paquetes, pérdida de bajada, y el id de la corrida (`segment`) |
| `events.csv` | caída, silenciado de subida o de bajada, hueco o reinicio: inicio, fin, duración, y cómo terminó |
| `joint.csv` | intervalo, plazo y conjunto de enlaces (máscara y nombres): paquetes esperados y tardíos |
| `tunnel.csv` | intervalo: paquetes entregados, perdidos en todos los enlaces y tamaños de WireGuard (una columna por borde) |
| `rf.csv`, `rf_changes.csv` | muestra de radio de cada serie asignada a un enlace, y cada cambio de RAT, banda o celda (con `--rf`) |
| `README.txt` | lista cada archivo con sus columnas, unidades y filas, y dice cómo se relacionan los relojes (el del motor, el de InfluxDB y el `--rf-offset`) |

```r
s <- readr::read_csv("campo/samples.csv.gz"); e <- readr::read_csv("campo/events.csv.gz")   # data.table::fread lee .csv; para .gz pide R.utils
aggregate(cbind(rtt_ms, out) ~ link, subset(s, counted == 1), mean)
aggregate(duration_s ~ link, subset(e, kind == "outage"), function(x) c(n = length(x), mediana = median(x)))
```
```python
import pandas as pd
s = pd.read_csv("campo/samples.csv.gz", parse_dates=["time_utc"]); e = pd.read_csv("campo/events.csv.gz")
s[s.counted == 1].groupby("link").agg(rtt_p50=("rtt_ms", "median"), caida=("out", "mean"))
e[e.kind == "outage"].groupby("link").duration_s.agg(["count", "median"])
```

Las cabeceras están fijadas en `fieldrec_test.py`: un nombre cambiado en
silencio hace fallar la prueba. Las horas de `samples`, `events`, `joint` y
`tunnel` son el `time_ms` del motor (el reloj de la Pi); antes de que NTP lo
corrija (`clock_synced` en 0) no valen. Las de `rf` y `rf_changes` son las de
InfluxDB al recibir el POST más `--rf-offset`; `rf_time_epoch` conserva la
original.

Tamaño, tiempo y memoria, medidos con una semana sintética a 1 Hz con 5
enlaces (604 800 muestras de estado completas, 168 archivos por hora): salen
3 024 000 filas de `samples.csv` (517 MB), 5,1 millones de `joint.csv` (458 MB)
y 604 800 de `tunnel.csv` (225 MB), 1,2 GB en total, o 160 MB con
`--export-gzip`. El análisis tarda 196 s sin exportar, 263 s con `--export` y
340 s con `--export-gzip` y 4 módems en `--rf` (2,7 millones de líneas). El
pico de memoria es de unos 8 GB en los tres casos: lo ocupan las muestras, que
se guardan todas (unos 11 KB cada una); las filas salen a medida que se
calculan, así que exportar casi no suma (3 MB; con `--rf`, 350 MB). Antes de
guardar solo las claves que se leen y de compartir las cadenas repetidas eran
12,7 GB y 275 s: una semana a 1 Hz pide una máquina de 16 GB.

`lab.d/fieldrec.sh` (en `ci`) lo prueba de dos maneras:

- `fieldrec_test.py`, sobre una grabación inventada con respuestas
  conocidas: a y b caen juntos, c cae solo y sin que el motor lo marque
  todavía, y Starlink corta en los segundos 12/27/42/57. La grabación trae
  además un reinicio del motor, un hueco, el reloj antes de NTP, un archivo
  de un motor sin `time_ms` y otro cortado por un apagón. Se comprobó que la
  prueba falla si se rompe el pliegue, la correlación, el corte entre
  corridas, la detección por silencio o el inicio de la caída. Otra
  grabación inventada, de dos horas, es para `--rf`: caídas y silenciados a
  horas conocidas, y datos de radio con la forma de la salida de
  `cell-monitor`, en *line protocol* y en CSV anotado (una fila por campo y
  con `pivot()`), con un enlace cuyos cambios de RAT preceden a sus caídas y
  otros dos que no, series sin enlace, un hueco de datos, un módem caído y
  radio que empieza después de la grabación y la rebasa. El conteo de aciertos
  y el azar se comparan con un cálculo por fuerza bruta, y las cuatro formas de
  leer los datos tienen que dar lo mismo. Otra grabación, de seis horas, tiene
  caídas y cambios de RAT que abundan en la misma hora pero son
  independientes dentro de ella: con el azar global se lee «más que el azar» y
  con el local «igual que el azar», y el azar local de cada episodio se compara
  con un conteo en rejilla y la cola con la convolución completa. Las
  exportaciones se comprueban contra el informe (caídas y silenciados por
  enlace, sumas de `joint.csv` contra P(S tarde), sumas de `tunnel.csv`). También fija el formato real: son 17
  líneas que escribió `cell-monitor.sh` contra `modem-sim.py`. Se comprobó que
  la prueba falla si se rompe `--rf-offset`, la ventana del azar, el corte en
  los huecos de datos, la asignación por etiqueta, nombre o carrier, los
  episodios, el solape con la grabación, la fuente del SINR, su dirección, las
  horas, el inicio de los silenciados o la lectura de ns y de `#default`.
- De punta a punta: el motor y `cengarde-rec` reales (bajo `sh`), a 4
  muestras por segundo, mientras s2 cae 3 s. Tienen que quedar solo
  archivos comprimidos, cada muestra una vez, una caída de 2 a 5 s en l2 y
  ninguna en l1 ni l3.

### `fieldperf`: cuánto lleva cada enlace, con iperf3 (`fieldperf.sh`, `fieldperf.py`, `lab.d/fieldperf.sh`)

El registrador dice qué hizo el motor; esto mide qué pueden llevar los
enlaces y el túnel, en el router de verdad (historia 012). `fieldperf.sh`
corre en el router (le basta el busybox de OpenWrt, más iperf3:
`apk add iperf3` en la 25.12, `opkg install iperf3` en la 24.10) y prueba,
con TCP y 4 flujos, subida y bajada:

- `solo`: cada enlace solo, fuera del túnel (`iperf3 --bind-dev`, como el
  motor ata sus sockets), contra la dirección del VPS que usa ese enlace;
- `all`: todos los enlaces a la vez, fuera del túnel (un puerto del VPS por
  enlace). Si la suma no llega a la de `solo`, el cuello está del lado del
  router: el trunk, la Pi o un CPE que comparte puerto;
- `tunnel`: a través del túnel, a 10.79.0.1, tal como está configurado el
  motor (copia o reparto).

Mientras corre cada prueba hace ping por cada enlace que carga (y por el
túnel), y antes y después guarda los contadores que dicen dónde se pierde
lo que el router no pudo mandar: `SndbufErrors` de UDP (`/proc/net/snmp`),
`tx_dropped` de cada interfaz y de la que tiene debajo (la del trunk), la
cola (`tc -s qdisc`, si está `tc-tiny`) y los `tx_drops` del motor.
Antes de todo, 5 pings en reposo por enlace. Las marcas van a
`marcas.txt`, como las de los perfiles, para cruzarlas con `cengarde-rec`.

Gasta datos móviles a propósito: `plan` imprime las pruebas y una
estimación de los MB (con 40 Mbit/s de subida y 100 de bajada por enlace,
`-e` para otras; en copia, el túnel cuesta lo mismo en cada enlace), y
`run` pregunta antes de empezar. Con 4 enlaces, 10 s por prueba y las tres
pruebas son unos 2,1 GB (*estimación*). No se corre mientras se transmite.

```sh
# en el VPS (una vez; vps off al terminar): servidores iperf3 en 5201-5204,
# el puerto abierto en ufw y reservado en cengarde-nat (con IP pass, todo
# puerto desde 1024 que el VPS no escucha va al router)
sudo sh fieldperf.sh vps on
# en el router
sh /tmp/fieldperf.sh plan
sh /tmp/fieldperf.sh run              # o -m "solo all" sin el túnel, -t 5...
tar czf /tmp/fieldperf.tgz -C /tmp fieldperf-FECHA
# en el PC
python3 bench/fieldperf.py fieldperf-FECHA --csv fieldperf.csv
```

`fieldperf.py` da una tabla por prueba y enlace (la fila `*`, la suma de la
prueba): Mbit/s recibidos, retransmisiones, el RTT de TCP (en subida), el
ping bajo carga contra el de reposo, lo que el motor mandó por cada enlace
(en `tunnel`, cómo cayó la carga en cada uno) y las pérdidas de cada
contador; `--csv` y `--json` dan las mismas filas. Los archivos tienen las
direcciones del VPS y de los módems: no van al repositorio.

`lab.d/fieldperf.sh` (en `ci`) lo prueba con `fieldperf_test.py`, sobre
una corrida inventada con respuestas conocidas (pings de busybox y de
iputils, uno sin respuestas, un iperf3 caído, los contadores de UDP en IPv4
e IPv6, la cola raíz y no las hijas), y de punta a punta con 2 enlaces del
laboratorio: servidores iperf3 en `srv`, `fieldperf.sh` (bajo `sh`) en
`cli` tomando los enlaces y sus direcciones del estado del motor, `solo` y
`all` (el laboratorio no tiene WireGuard, así que no `tunnel`). `plan` no
puede correr nada y tiene que dar 210 MB.

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
- `big` y `bigord` (al final de la línea de `udpgen`): los paquetes únicos de
  256 bytes o más y cuántos de ellos llegaron detrás de uno posterior. Con
  `-s imix` (7 de 64, 4 de 576 y 1 de 1400 bytes, 347 de media; `SIZE=imix`
  en el laboratorio) separa el desorden de lo que retiene un reordenador del
  de los pequeños, que lo adelantan.
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
