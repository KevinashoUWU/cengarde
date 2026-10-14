# Motor cengarde (C)

Reescritura en C de engarde: duplica cada datagrama de WireGuard por todos los
enlaces del cliente hacia el servidor, y en el otro extremo entrega a
WireGuard solo la primera copia. Cliente y servidor son el mismo binario; el
modo lo decide la configuración. El diseño y sus razones están en
[`docs/historias/005-motor-c-v1.md`](../docs/historias/005-motor-c-v1.md) y
la salud de los enlaces en
[`docs/historias/006-salud-de-enlaces.md`](../docs/historias/006-salud-de-enlaces.md).

**No es compatible en el cable con engarde Go:** usa su propia cabecera
autenticada (protocolo v4), así que los dos extremos tienen que ser cengarde
de la misma versión de protocolo. `cengarde version` la muestra al final, por
ejemplo `cengarde 0.5.0-r1-g<commit> (protocol 4)` en OpenWrt. El formato
está en [`src/proto.h`](src/proto.h) y lo que cambió el v4 en la sección
[Protocolo v4](#protocolo-v4).

## Compilar

```sh
make -C engine            # binario engine/cengarde
make -C engine test       # tests unitarios
make -C engine SANITIZE=1 test   # con ASan y UBSan
make -C engine SANITIZE=thread test   # con ThreadSanitizer (compilación aparte; los hilos: tests/test_threads.c)
```

`test_threads` pasa 10^7 entradas por el anillo de los hilos;
`CG_TEST_RING_N` cambia la cifra (p. ej. 10^8 en un ARM de verdad).

Solo necesita un compilador C11 y las cabeceras de Linux, sin más
dependencias. Respeta `CC`, `CFLAGS` y `LDFLAGS` para compilar en cruzado (por
ejemplo `make CC=aarch64-linux-gnu-gcc`) y dentro del SDK de OpenWrt.

## Puesta en marcha

En OpenWrt todo se configura desde LuCI, y el VPS se instala con el
cloud-config que esta entrega: ver [`openwrt/README.md`](../openwrt/README.md).
A mano, en cualquier Linux:

1. **Clave compartida** (la misma en los dos extremos):

   ```sh
   cengarde genkey
   ```

2. **Servidor (VPS):** copia `examples/server.conf`, pon la clave y la
   dirección de escucha de WireGuard (`wireguard = 127.0.0.1:51820`).
   Compruébala con `cengarde -t -c server.conf` y arranca con
   `cengarde -c server.conf`. Abre en el cortafuegos el puerto UDP de `listen`
   (59402 por defecto).

3. **Cliente:** copia `examples/client.conf`. Pon la clave, `server = <VPS>:59402`
   (o varias direcciones del VPS, ver [más abajo](#varias-direcciones-del-servidor))
   y los enlaces: patrones en `interfaces` o secciones `[link NOMBRE]`.

4. **WireGuard del cliente:** el `Endpoint` del peer pasa a ser la dirección
   `listen` del cliente (`127.0.0.1:59401`). El servidor WireGuard no cambia.

5. **MTU de WireGuard:** cengarde añade 24 bytes por paquete. Con una ruta de
   1500 bytes el máximo es 1416 sobre IPv4 y 1396 sobre IPv6
   (1500 − IP − 8 UDP − 24 cengarde − 32 WireGuard). En redes móviles conviene
   ir más bajo, por ejemplo `MTU = 1380`, o menos si algún operador tiene un
   MTU de camino menor. El cliente lee cada 5 s el MTU de camino de cada
   enlace (`path_mtu` en el estado) y avisa en el log, como mucho cada 10
   minutos por enlace, si los datagramas de WireGuard más grandes no caben,
   con el MTU que sí cabría.

## Emparejar con un solo secreto

En lugar de repartir cuatro claves, los dos extremos pueden derivarlas de un
secreto de 32 bytes (la salida de `cengarde genkey`):

```sh
cengarde keys < secreto
# CG_LINK_KEY='…'                 la clave de cengarde ("key"), igual en los dos extremos
# CG_WG_SERVER_KEY='…'            la clave privada de WireGuard del VPS
# CG_WG_CLIENT_KEY='…'            la del router
# CG_WG_PSK='…'                   la PSK de WireGuard
# CG_TUNNEL_ADDR='10.79.x.y'      la IPv4 del router dentro del túnel
# CG_TUNNEL_ULA='fdxx:xxxx:xxxx'  su prefijo ULA, un /48 (el túnel usa su ::/64)
# CG_CLIENT_HINT='N'              la pista de cliente, de 0 a 255
```

- **Derivación:** cada clave es BLAKE2s-256 con el secreto como clave y
  `"cengarde pairing v1: " + etiqueta` como mensaje. Las privadas de
  WireGuard salen recortadas, como las de `wg genkey`.
- **Direcciones del túnel:** salen de la misma fórmula, para que nadie tenga
  que elegir números cuando varios routers comparten un VPS:
  - `CG_TUNNEL_ADDR`: con `d` la salida de la etiqueta `tunnel` y
    `u = d[0] · 256 + d[1]` (big-endian, igual en cualquier CPU),
    `v = 2 + u mod 65533` y la dirección es `10.79.(v >> 8).(v & 255)`.
    Nunca es 10.79.0.0, ni 10.79.0.1 (la del VPS), ni 10.79.255.255.
  - `CG_TUNNEL_ULA`: `fd` seguido de los 5 primeros bytes de la etiqueta
    `tunnel-ula`, con cuatro cifras por grupo para que `${CG_TUNNEL_ULA}::2`
    sea una dirección.
- **Pista de cliente:** `CG_CLIENT_HINT` es el primer byte de BLAKE2s-256
  con la clave de cengarde (`CG_LINK_KEY`, no el secreto) como clave y
  `"cengarde v4 client hint"` como mensaje. El protocolo v4 la llevará en el
  byte 2 de la cabecera para que el servidor elija la clave de cada router
  sin probarlas todas; hoy ningún paquete la lleva.
- **Variables nuevas al final:** las cuatro primeras líneas no cambian, así
  que los scripts que hacen `eval` de la salida siguen igual. Desde 0.5.2
  el router usa en el túnel la dirección derivada (`CG_TUNNEL_ADDR`, una
  /32), así que varios routers caben en un VPS sin repartir direcciones a
  mano; los instalados antes conservan 10.79.0.2.
- **Entrada estándar:** el secreto se lee de ahí para que no aparezca en la
  lista de procesos.
- **Salida:** asignaciones de shell para `eval`. Las públicas se sacan con
  `wg pubkey`.
- **Quién lo usa:** `cengarde-setup`, en OpenWrt, y
  [`contrib/vps/cengarde-vps-setup`](../contrib/vps/cengarde-vps-setup).

Detalle y razones en la
[historia 008](../docs/historias/008-luci-uci-y-emparejamiento.md).

## Qué hace cada extremo

- **Cliente:**
  - abre un socket por enlace (`SO_BINDTODEVICE` + bind a la IP del enlace),
    hacia una de las direcciones del servidor (ver la sección siguiente);
  - sigue los cambios de interfaces y direcciones por netlink;
  - manda cada paquete por todos los enlaces activos, sin bloquear: un enlace
    lleno pierde solo sus copias;
  - entrega a WireGuard la primera copia de cada paquete del servidor;
  - envía una sonda por enlace cada 100 ms mientras hay tráfico y cada
    segundo en reposo: mide el RTT y el retraso de ida de cada enlace y
    mantiene abierto el NAT.
- **Servidor:**
  - crea una sesión, aprende un camino (sesión + enlace) o lo cambia de
    dirección (un NAT que cambió) solo con una sonda autenticada que trae
    la cookie de un HELLO enviado a esa dirección (ver
    [Protocolo v4](#protocolo-v4));
  - cada sesión tiene su propio socket hacia WireGuard, así que varios
    clientes comparten un puerto;
  - sirve a uno o a varios routers, cada uno con su clave (ver
    [Varios routers](#varios-routers-por-servidor-client-name));
  - responde por cada camino desde la dirección a la que envía el cliente
    (con un `listen` comodín, `*` o `0.0.0.0`): sirve cualquier dirección
    del VPS, también una añadida en marcha, como una IP reservada o IPv6;
  - escucha con un socket por enlace del cliente (*lanes*, ver la sección
    [Colas del servidor](#colas-del-servidor-lanes));
  - reparte la bajada por todos los caminos activos.

## Varias direcciones del servidor

`server` es una lista ordenada de hasta 8 entradas, global o por
`[link NOMBRE]`: IPv4, `[IPv6]` o nombres. El orden es la preferencia. Un
nombre vale por sus direcciones (como mucho 4 de cada familia), que el
cliente busca por su cuenta y sigue cuando cambian (ver
[Nombres del servidor](#nombres-del-servidor-dns-dinámico)).

```ini
server = 203.0.113.10:59402 [2001:db8::4]:59402
#server_failover_ms = 10000
```

- **Una familia por enlace:** cada enlace usa una sola dirección a la vez,
  la primera de una familia de la que tenga dirección propia; nunca IPv4 e
  IPv6 a la vez por el mismo módem, que duplicaría el tráfico por la misma
  radio sin ganar diversidad. Las entradas de otra familia no cuentan para
  ese enlace: con solo IPv4, una IPv6 primera en la lista se salta.
- **Failover:** si el enlace pasa `server_failover_ms` (10 s; 0 lo apaga,
  como mínimo 3 × `probe_idle_ms`) sin una respuesta verificada, prueba la
  siguiente, y al final vuelve a empezar. Si no puede ni abrir el socket
  hacia una, pasa a la siguiente al momento (solo con el failover
  encendido). En la práctica pasa con una IPv6 sin ruta; en IPv4 el socket
  atado a la interfaz se abre aunque no haya ruta, así que una IPv4 muerta
  solo cae por el plazo.
- **Pegajoso:** se queda donde le contestan. Vuelve a la primera solo:
  - si cambia o pierde su dirección local: se cae, se queda sin dirección
    (o sin ninguna de la familia del servidor) o desaparece, aunque vuelva
    con la misma (un módem USB tras perder la señal);
  - si gana o pierde una familia con entradas en su lista: la IPv6 de
    Starlink llega después de la IPv4, así que con `server = [IPv6] IPv4`
    el enlace empieza en IPv4 y pasa a la IPv6 cuando aparece; con una
    lista solo IPv4, la IPv6 que va y viene no lo mueve;
  - si una recarga cambia su lista. Cuentan las entradas de su familia:
    añadir, quitar, editar o mover una lo devuelve a la primera; una
    entrada de otra familia, o un nombre que al resolverse otra vez da las
    mismas direcciones en otro orden, no lo mueven;
  - con la primera respuesta después de una ronda entera sin respuesta en
    ninguna: entonces el caído era el enlace, no la dirección (un corte de
    la red móvil en el que el módem conserva su dirección), y vuelve a
    mandar el orden de la lista.

  No reintenta la preferida por su cuenta: cada intento costaría 10 s sin
  ese enlace mientras siga rota.
- **Dirección de origen IPv6:** la elige como RFC 6724: nunca una tentativa,
  fallida o de enlace local; una ULA (fc00::/7) hacia un destino que no es
  ULA va última (sirve si el módem hace NAT66), luego las obsoletas, luego
  las temporales y primero las estables. Entre iguales, la más nueva, como
  el kernel: si llega un prefijo nuevo mientras el viejo sigue preferido
  (una renumeración brusca, RFC 8978), pasa al nuevo, igual en marcha que
  al arrancar. En IPv4, la principal.
- **Log:** `link X: no reply from A for 10 s, trying B` en cada cambio de la
  primera ronda; si ninguna contesta, una vez por minuto. Tras una ronda
  entera sin respuesta, la primera respuesta se registra una vez:
  `link X: the server answers again after a round of its addresses without
  replies`, con `back to A` si vuelve a la primera o `at A` si ya estaba
  en ella.
- **Estado:** cada enlace muestra su familia, en qué dirección de la lista
  está y cuántos failovers lleva (ver [Estado](#estado)). Un enlace con
  sección `[link]` y sin dirección de la familia de ninguna entrada aparece
  caído con el motivo "no address of the server's family".

En el laboratorio (`sudo bench/lab.sh fallback`, con 4 s de failover): con
la primera dirección muerta el enlace vive en la segunda a los 4 s y se
queda; una IPv6 primera en un enlace solo IPv4 no retrasa nada; tras 25 s
sin ninguna dirección del servidor, vuelve a la primera; y también vuelve
al momento si pierde su propia dirección y recupera la misma.

## Nombres del servidor (DNS dinámico)

Un nombre en `server` sirve para un servidor con IP dinámica (un servidor en
casa con DNS dinámico) o simplemente para no escribir la IP. El cliente lo
busca él mismo, por los enlaces, y no con el resolvedor del sistema: con todo
el tráfico por el túnel, las consultas del sistema van también por el túnel,
que está caído justo cuando la IP del servidor cambió.

```ini
server = casa.example.net:65500 203.0.113.10:65500
# Las de todos los enlaces, después de las propias (estas son las de fábrica).
resolver = 1.1.1.1 9.9.9.9 [2606:4700:4700::1111] [2620:fe::fe]

[link eth1.10]
# Los DNS de la operadora: este enlace pregunta primero aquí.
resolver = 192.168.8.1
```

- **Por los enlaces:** cada consulta sale por un enlace, atada a su
  dirección como sus sockets, y pregunta primero a los `resolver` de su
  `[link]` (en OpenWrt, los DNS que da la operadora a ese enlace) y después a
  los globales (de fábrica, Cloudflare y Quad9; `resolver =` vacío, ninguno).
  Un intento dura como mucho 2 s; los siguientes van por el siguiente enlace
  (el primer DNS de cada enlace, luego el segundo...), hasta 6 por búsqueda.
  Primero los enlaces vivos.
- **Cuándo pregunta:** al arrancar (en cuanto hay un enlace por el que
  preguntar); después, según el TTL de la respuesta, entre 30 s y 15 min;
  y al momento si los enlaces que apuntan a sus direcciones llevan 2 s sin
  estar vivos (el servidor cambió de IP o se cayó), y otra vez a los 15 s,
  30 s y luego cada 60 s mientras siga así, sin importar el TTL. Un nombre
  al que ya no apunta ningún enlace (pasaron a otra entrada de la lista)
  espera a su TTL.
- **La última respuesta buena se queda:** si una búsqueda falla (sin
  respuesta, SERVFAIL, REFUSED), el nombre conserva sus direcciones y vuelve
  a preguntar (cada 5 s, doblando hasta 60 s; antes de la primera respuesta,
  desde 1 s hasta 15 s). Una respuesta vacía (NXDOMAIN, o sin registros de
  la familia) solo quita las direcciones de esa familia si el servidor ya
  no contesta en ellas: un DNS que falla nunca corta un túnel que funciona.
  Un nombre que se queda sin ninguna dirección vuelve a preguntarse cada
  15 s, no a los 5 min de una respuesta vacía. Las direcciones que ningún
  servidor tiene (0.0.0.0, `::`, loopback, multicast, una IPv4 dentro de un
  AAAA; un DNS que bloquea un nombre suele dar 0.0.0.0) se descartan.
- **Cuando cambian las direcciones:** un enlace vuelve a la primera
  dirección de su lista solo si la que usa desapareció o si cambió una
  entrada anterior a la suya (el servidor preferido tiene otra IP); un
  cambio después de la suya no lo mueve, y si sigue en la misma dirección
  conserva su socket. Las direcciones de un nombre van ordenadas (IPv6
  primero), así que uno con más de 4 da siempre las mismas.
- **Sin dirección todavía:** el enlace queda con el motivo `server name not
  resolved` y sube en cuanto llega la respuesta. Un nombre que no resuelve
  ya no impide arrancar.
- **`/etc/hosts` primero:** un nombre que está ahí toma esas direcciones al
  leer el archivo y nunca se consulta (`localhost`, o un nombre fijado a
  mano).
- **El corte cuando cambia la IP:** la detección de los enlaces, más 2 s,
  más una consulta; y antes, lo que tarden el DNS dinámico en publicar la IP
  nueva y los DNS de la operadora en olvidar la vieja (su TTL: conviene uno
  corto, 60 s). En el laboratorio, sin cachés y a 1000 pps
  (`sudo bench/lab.sh names`): todos los enlaces vivos en la dirección nueva
  3,3 s después de borrar la vieja (1,1 s de detección), dos veces
  seguidas; 40 s con el DNS caído no pierden ni un paquete; con TTL 0
  pregunta cada 30 s; un NXDOMAIN con el servidor vivo no cambia nada; y un
  nombre que se quedó sin dirección vuelve 16 s después de que vuelve su
  registro.
- **Seguridad:** sin DNSSEC. Cada intento usa un socket nuevo, conectado al
  DNS, con puerto e id al azar; una respuesta falsa solo puede mandar el
  túnel a una dirección que no tiene la clave, que no contesta, y el nombre
  vuelve a buscarse a los 2 s.
- **Log:** `server name X: A B (was C; TTL 60 s, from eth1 192.168.8.1:53)`
  cuando cambian; una vez por minuto si las búsquedas fallan.
- **Estado:** `names[]`, ver [Estado](#estado).

## Colas del servidor (`lanes`)

El servidor escucha en su puerto con varios sockets a la vez (un grupo
`SO_REUSEPORT`): `lanes` sockets, 8 por defecto, y uno más para la basura.
Un programa BPF clásico (`src/steer.h`) mete cada datagrama en el socket de
su número de enlace (el enlace 0 en el 0, el 1 en el 1…), así que las
copias de un mismo paquete esperan en colas distintas.

- **Por qué:** con un solo socket, un parón del VPS (una VM se para 5–38 ms
  varias veces cada 10 s en el laboratorio) desbordaba la única cola y se
  llevaba las tres copias de cada paquete: el 73–98 % de las copias
  perdidas en el servidor eran paquetes enteros, y la redundancia no
  servía de nada. Con colas separadas desbordan en momentos distintos.
- **Medido** (`bench/mt.py s1`: subida de 40 a 110 kpps con 3 enlaces, 5 s
  por punto, 4 rondas con `lanes = 1` y 8 intercaladas, búferes del
  servidor por defecto y los del router a 32 MiB; tres sesiones): paquetes
  perdidos con todas sus copias en el servidor, de 18.011 a unos 108.000
  con un socket y de 0 a unos 12.600 con 8 colas (11,8× menos sumando las
  tres; estimados por resta donde el router también tiró), con la misma
  CPU por paquete del servidor (de −4,4 % a +4,9 %). A 110 kpps, 3, 2 y 2
  rondas de 4 con pérdida ≤ 0,1 % con 8 colas (1, 0 y 0 con un socket): la
  puerta S1 pide 4 de 4, y tal como está escrita no se cumple.
- **Lo que las colas no arreglan:** absorben un parón más corto que el
  búfer de una cola (unos 33 ms a 110 kpps con el valor por defecto
  *(cálculo)*). Si el único hilo del servidor se queda atrás más tiempo,
  desbordan todas a la vez y vuelven a perderse paquetes enteros; así
  fallaron, con 8 colas, las rondas de 110 kpps que no perdieron en el
  propio router. Eso queda para los hilos del servidor.
- **La bajada sale por la cola de su enlace:** el camino `p` envía desde el
  socket `p & (lanes − 1)`, el mismo al que llega su enlace (están todos
  atados a la misma dirección y puerto, y la respuesta sale desde la
  dirección de llegada, como antes). Así cada camino tiene su propio búfer
  de envío y su propio lote de respuestas a sondas: un camino con una cola
  local larga ya no llena el búfer de los demás. En el laboratorio
  (`sudo bench/lab.sh deepq`: un camino a 5 Mbit/s con 20 MB de cola,
  20.000 pps de bajada) el túnel pierde un 10,48 % con un solo socket y
  nada con las colas.
- **El primer camino rota:** el servidor envía cada lote camino a camino, y
  el que sale primero llega primero; antes era siempre el 0, que ganaba el
  90–100 % de las primeras llegadas (`rx_first`) en enlaces idénticos.
  Ahora el primero cambia de lote a lote: 33 % por enlace en el
  laboratorio a 2.000 y 40.000 pps (`sudo bench/lab.sh skew`).
- **Basura:** lo que no es protocolo 4 (datagramas más cortos que una
  cabecera, o de otra versión, como un router de otra versión) va al socket
  de basura, nunca a una cola real: se cuenta (`rx.junk`, `rx.short`,
  `rx.bad_version`) y se registra de vez en cuando ("protocol vN packet
  from …: update the router or the VPS"). Tiene un búfer fijo de 256 KiB,
  así que una avalancha de basura solo llena ese.
- **Otro proceso en el mismo puerto:** antes de montar el grupo, el
  servidor ata un socket sin `SO_REUSEPORT` y lo cierra; si el puerto está
  ocupado, no arranca ("Address already in use") en vez de colarse en el
  grupo de otro cengarde.
- **`lanes = 1`** es un solo socket, como antes, sin socket de basura: el
  interruptor para volver atrás. `lanes` (`auto` = 8, o 1, 2, 4, 8, 16)
  solo cambia reiniciando el proceso. Un kernel anterior a 4.5, que no
  admite el programa, también se queda con un socket (y lo dice en
  `steering_error`).
- **Memoria:** todos los sockets UDP de la máquina comparten
  `net.ipv4.udp_mem`. Pasado su primer valor, el kernel solo deja un
  datagrama en la cola de cada socket UDP, y pasado el último, ninguno;
  ocho colas llenas podrían llevarse por delante cualquier otro socket UDP
  con algo de cola (en el laboratorio, uno que lee cada 250 ms recibió 163
  de 599 datagramas). Por eso los sockets que reciben (las colas, el
  socket de WireGuard de la sesión y el de basura) se reparten la mitad del
  umbral de presión, y cada uno recibe `min(rcvbuf, presupuesto / (2 ×
  sockets))` (el kernel dobla el valor): en un VPS de 1 GB, unos 3,2 MiB
  por socket en vez de 4 MiB *(cálculo)*. Se recalcula al arrancar y en
  cada recarga; `cengarde -t` y el estado (`rcvbuf`, `rcvbuf_capped`)
  dicen cuánto queda. En un namespace de red propio (un contenedor) el
  sysctl no se ve y se estima desde la RAM, como lo calcula el kernel al
  arrancar (`budget_from: "RAM"`).

Detalle, medidas y razones en la
[historia 011](../docs/historias/011-hilos.md).

## Salud de los enlaces

Cada extremo decide por qué enlaces envía, con el retraso de ida que le
reporta el otro extremo en las sondas (el cliente decide la subida y el
servidor la bajada):

- **Mudo:** un enlace que deja de funcionar en cualquiera de los dos
  sentidos deja de llevar datos hasta que vuelve a funcionar. El cliente lo
  nota cuando 3 sondas seguidas no tienen respuesta durante 2 RTT + 1 s; el
  servidor, cuando las sondas dejan de llegar, o dejan de decir que llegan
  sus respuestas, durante 3 intervalos de sondeo + 1 s.
- **Silenciado:** un enlace que va más de `mute_behind_ms` (150) por detrás
  del más rápido durante `mute_settle_ms` (2 s) deja de llevar datos, pero
  conserva su socket y sus sondas. Vuelve cuando se mantiene a menos de
  `unmute_behind_ms` (120) durante el doble de ese tiempo, y la espera se
  duplica cada vez que vuelve y no aguanta (hasta ×8).
- **Mínimo:** al menos `min_active_links` (2) enlaces vivos llevan siempre
  todo. Si uno se queda mudo, el silenciado más rápido vuelve al instante.
- `mute_behind_ms = 0` desactiva el silenciado y deja redundancia pura,
  también en una recarga: los enlaces que estaban silenciados vuelven al
  instante (`sudo bench/lab.sh muteoff`).
- Todas estas perillas se cambian en caliente (SIGHUP o `cengarde ctl
  reload`), sin cortar el túnel ni perder la sesión.

En el laboratorio, un enlace con 500 ms de cola se silencia en 3–3,6 s sin
perder un paquete del túnel y vuelve, sin recaer, cuando se le quita
(`sudo bench/lab.sh health`).

## Reordenamiento y reparto (`reorder`, `split`)

Las perillas del bonding (historia
[012](../docs/historias/012-bonding.md)). **Cada extremo reordena lo que
recibe** (el cliente la bajada; el servidor la subida de cada sesión) y
reparte lo que envía según `split`: `force` siempre (el laboratorio); `on`,
en el cliente, cuando el servidor dice que reordena; `client`, en el
servidor, cuando el cliente lo pide y reordena. Con redundancia pura y los
valores por omisión no retiene nada: el orden de entrega es el de siempre.

- **`reorder` (`yes`), en los dos extremos:** el receptor pone en secuencia
  los datos verificados del otro extremo antes de entregarlos a WireGuard.
  Un paquete que llega por delante de uno que falta espera como mucho
  `reorder_max_wait_ms` (200; 0–2000), y menos si el que falta ya habría
  llegado por el enlace más lento (su retraso más `reorder_margin_ms`: 10;
  0–1000). Donde no hay reparto, es decir, con redundancia pura, la espera
  es `reorder_dup_wait_ms` (0; de 0 a `reorder_max_wait_ms`): con 0 no se
  retiene nada y el orden de entrega es el de siempre.
- **`reorder_bypass` (128; 0–2048 bytes):** los datagramas de datos de
  WireGuard de hasta ese tamaño (keepalives, ACK de TCP vacíos) y los
  handshakes se entregan al instante, sin esperar. Con 0, solo los handshakes.
- **`reorder_pool_kb` (8192; 256–131070):** memoria para los paquetes
  retenidos, en bloques de 2 KiB, reservada al arrancar; solo ocupa RAM lo
  que llega a usarse. En el servidor la comparten todas las sesiones (cada
  una suma además unos 81 KiB de ventana); una sesión que no encuentra
  búfer entrega al instante y lo cuenta (`pool_full`). Es lo único de esta
  sección que reinicia el proceso al cambiarlo; el resto se aplica en
  caliente, y apagar `reorder` entrega en orden lo que estaba retenido.
- **Sin bloquear:** el plazo del hueco más antiguo es el tiempo de espera
  del bucle (no el tic de 100 ms), y una pasada entrega como mucho 512
  retenidos (la siguiente viene enseguida), para no desbordar la cola de
  descifrado de WireGuard (1024 por par). Un desborde de la ventana (4096
  secuencias) se entrega entero.
- **`split`:** reparte la subida (cliente) o la bajada (servidor) entre los
  enlaces en vez de copiar cada paquete por todos: un paquete grande viaja
  por un solo enlace, elegido según los pesos. El cliente admite `off` (por
  omisión: todo por todos los enlaces, como siempre), `on` y `force`; el
  servidor, `off`, `client` (por omisión) y `force`. `on` es el modo de campo:
  el cliente reparte la subida cuando el servidor reordena y le pide repartir
  la bajada con los pesos de cada enlace. `client` hace que el servidor
  reparta la bajada solo si el cliente lo pide y reordena. `force` reparte
  a partes iguales, sin pesos y sin consultar al otro extremo: es para el
  laboratorio. Los extremos se lo dicen en las sondas (ver [Protocolo
  v4](#protocolo-v4)): cada sonda del cliente lleva el peso del enlace, si
  el cliente reordena y, con `on`, si pide que le repartan la bajada; cada
  respuesta del servidor, si el servidor reordena. Así, la subida del
  cliente con `on` depende solo de que el servidor reordene, y la bajada
  depende del `split` del servidor. Con un extremo de una versión anterior,
  que no lo dice, no se reparte nada (`waiting for the peer`). Solo reciben
  parte los enlaces vivos y activos (ni silenciados ni en espera) cuyo otro
  extremo respondió en los últimos tres intervalos de sondeo y que no van
  más de `split_spread_ms` detrás del más rápido; con menos de dos, todo va
  por todos (con `split_spread_copy = no`, si queda uno solo, ese lleva todo
  lo que se reparte).
- **Recarga:** apagar `split` vuelve a duplicar desde el lote siguiente en
  ese extremo. El cliente manda una sonda por cada enlace al recargar, así
  que el servidor deja de repartir la bajada (o empieza) en un viaje de ida
  y vuelta; si es el servidor el que apaga `reorder`, el cliente deja de
  repartir la subida con la respuesta siguiente, dentro de un intervalo de
  sondeo (100 ms con tráfico).
- **`split_small` (256; 0–2048 bytes):** los datagramas de hasta ese tamaño,
  los handshakes y todo lo que se envía sin ningún enlace apto van siempre
  por todos los enlaces. Con un solo enlace apto, todo va por todos, o con
  `split_spread_copy = no` ese lleva todo lo que se reparte (guarda G2).
- **`split_spread_ms` (30; 0–2000), en los dos extremos, para lo que cada
  uno envía:** solo reciben parte los enlaces que van, en ese sentido, a lo
  sumo esa diferencia de retraso detrás del más rápido de los que podrían
  recibirla; los demás quedan fuera del reparto y llevan copia de todos los
  paquetes, o solo lo que va por todos, según `split_spread_copy`. El
  retraso es el mismo que
  usa el silenciado (el que informa el otro extremo, filtrado). Un enlace
  sale en cuanto pasa la diferencia y vuelve cuando se mantiene dentro de
  3/4 de ella durante `mute_settle_ms` (2000), para que no oscile en el
  borde; uno que todavía no tiene medida de retraso queda fuera hasta
  tenerla. Con 0 no hay guarda: es el reparto sin filtro, para medirlo en el
  laboratorio. Se aplica en caliente. El porqué: el receptor entrega en
  orden, así que un reparto va al paso de su enlace más lento. En el
  laboratorio (`bond.yml` con netem, `split = force`, 3 repeticiones;
  *medido en laboratorio*, no en campo), dos enlaces iguales dieron 1,98× un
  enlace solo con Cubic y con BBR; con 50 ms de diferencia (`reorder50`),
  Cubic unas 1,15×, BBR 1,47× y QUIC cayó a 1–4 Mbit/s; con 150 ms
  (`reorder150`) cayeron todos (BBR ~1 Mbit/s, Cubic 7–59 Mbit/s), con los
  contadores del reordenador limpios (nada soltado antes de tiempo, nada
  tardío). Con el tubo casi vacío, los paquetes del enlace rápido no esperan
  y el flujo de adentro mide el RTT rápido; con carga, todo espera al lento:
  BBR dimensiona su ventana por el RTT mínimo y se hunde, y HyStart saca a
  Cubic del arranque lento antes de tiempo (*razonado*). La solución de fondo
  es el planificador por hora de llegada del paso 3; hasta entonces, esta
  guarda.
- **`split_spread_copy` (`yes`; `yes` o `no`), en los dos extremos, para lo
  que cada uno envía:** qué llevan los enlaces que la guarda de retraso deja
  fuera, y qué pasa cuando queda uno solo en el reparto. Con `yes`, como
  hasta la 0.6.2: los que quedan fuera llevan copia de todos los paquetes,
  como uno de peso 0, para tapar los huecos de los demás, y con menos de dos
  en el reparto todo va por todos. Con `no`: llevan solo lo que va por todos
  (handshakes y paquetes chicos), y si queda uno solo, ese lleva todo lo que
  se reparte (guarda G2). El porqué de `no`: un enlace se atrasa casi
  siempre porque su cola está llena, y el flujo entero encima de su parte lo
  empeora. En la primera prueba en la Pi (historia 012, 2026-10-13, cuatro
  enlaces 5G con colas profundas; *medido en campo*, dos tandas), con
  `split_spread_ms` 30 el enlace que salía seguía atrasándose, hasta 2,6 s,
  el router perdía de 4000 a 53000 envíos por prueba y la subida pasaba del
  15 al 30 % del tiempo duplicando; sin la guarda no perdía ninguno. El de
  `yes`: en el laboratorio (`bond`, caso `bloat`, colas de netem de 1000 a
  2000 paquetes, 3 repeticiones; *medido en laboratorio*), sacar las copias
  subió BBR con `split_spread_ms` 30 de 26 a 30–31 Mbit/s, pero lo bajó a
  21 con 250 y hundió QUIC de 26 a 6 Mbit/s, que declara perdido por desorden
  lo que llega tarde por un solo enlace; y el laboratorio no reproduce los
  envíos perdidos del campo (historia 012, «`split_spread_copy` en el
  laboratorio»). Queda `yes` hasta que una prueba
  de campo A/B diga otra cosa. Se aplica en caliente.
- **`governor` (`no`; `yes` o `no`), en los dos extremos, para lo que cada
  uno envía (experimental):** los pesos del reparto salen de la cola de cada
  enlace en vez de `weight` (`govern.h`). La cola es el retraso de envío del
  enlace (el que usa el silenciado) menos su propio mínimo de los últimos
  `governor_window_ms` (10000; 1000–120000): los relojes no están en hora,
  pero el desfase es constante, así que la diferencia es lo que espera en la
  cola del módem. Cada 100 ms, mientras ese extremo reparte y hay tráfico,
  el peso de un enlace baja 1/8 si su cola pasa de `governor_target_ms` (20;
  1–1000) y sube 20 (de 1000) si queda bajo la mitad; entre medio se
  mantiene, y nunca baja de 10, para que el enlace siga diciendo cuándo se
  vació su cola. Así cada enlace termina llevando lo que puede sin hacer
  cola. Un peso 0 sigue siendo una copia de todo. El cliente gobierna la
  subida y el servidor la bajada, cada uno con su propia medida: no cambia
  el protocolo. El porqué: con pesos iguales, en el banco (historia 012,
  2026-10-14) cada enlace llevaba el 23–27 % de la subida aunque sus
  capacidades eran distintas, y los más débiles acumulaban 120–150 ms de
  cola. El estado muestra, por enlace, `gov_weight` y `gov_queue_ms`, y
  `governor` en `upload` (cliente) y en cada sesión (servidor). Se aplica en
  caliente.
- **`capacity_weights` (`no`; `yes` o `no`), en los dos extremos, para lo
  que cada uno envía (experimental):** los pesos del reparto salen de la
  capacidad medida de cada enlace (`capacity_mbit`, abajo; `capacity.h`,
  historia 013): la parte de cada enlace es su capacidad, y 5/4 de ella
  mientras su cola está bajo la mitad de `governor_target_ms`, para que
  muestre más si puede (el sondeo de BBR). Un enlace aún sin medida lleva
  la parte mayor, para medirlo; uno que no entrega nada mientras otros sí,
  el piso (10 de 1000), que le deja un hilo y con él su medida. Con
  `governor` además, el peso del gobernador escala cada parte. Pesa más que
  `governor` y que `weight`, salvo los pesos 0. Como `governor`, apaga por
  omisión las copias a los enlaces fuera del reparto (`split_spread_copy`).
  El porqué: las bajadas de los enlaces van de 7 a 230 Mbit/s según el
  operador y la hora, y con pesos iguales los lentos se atoran y sus
  paquetes llegan tarde al reordenador (historia 013). El estado muestra
  `cap_weight` por enlace y `capacity_weights` en `upload` (cliente) y en
  cada sesión (servidor). Se aplica en caliente.
- **`weight` en `[link X]` (cliente; 1, de 0 a 1000):** la parte del reparto
  que lleva ese enlace con `split = on`, en la subida y en la bajada (el
  cliente se los manda al servidor en las sondas; `force` no los mira). Con
  0, el enlace lleva copia de todos los paquetes, repartidos o no. Una
  interfaz sin sección `[link]` pesa 1.

**El reparto es experimental.** Sin los pasos que siguen en el plan (detección
de pérdidas por enlace, control de las colas de los módems) reparte peor que
la redundancia, y por eso no es el valor por omisión:

- Un enlace que muere conserva su parte hasta que se detecta: del orden de
  1,3–1,5 s (estimación). Uno que pasa más de tres intervalos de sondeo sin
  ningún paquete verificado del otro extremo deja de recibir parte, aunque
  sigue llevando los paquetes que van por todos.
- El enlace más lento fija la latencia: con la guarda, hasta
  `split_spread_ms` (30) más el margen; sin ella (0), hasta `mute_behind_ms`
  (150).
- Los pesos son fijos. Uno por encima de lo que aguanta el enlace llena la
  cola del módem y lo hace oscilar; no hay pacing ni control de colas.

## Baja latencia

- `busy_poll_us`: tras cada paquete sigue sondeando sin dormir durante ese
  tiempo, así que el siguiente se atiende sin despertar al proceso. Con un
  enlace a 10.000 pps en el laboratorio, la mediana baja de 107–142 µs a
  57–74 µs con `busy_poll_us = 200`, pero la CPU sube de ~12 % a ~75 % por
  extremo; con 50 queda en 69–113 µs y ~27 %. Sin tráfico no gasta nada.
  Interesa en un VPS o en un x86, no en una Pi justa de CPU.
- `cpu = N`: fija el proceso a una CPU (mejor una que no atienda las
  interrupciones de la tarjeta de red).
- `rt_priority = 1..99`: planificación `SCHED_FIFO` (necesita root o
  `CAP_SYS_NICE`). Con prioridad de tiempo real y espera activa, el motor
  ignora `busy_poll_us` si hay una sola CPU o si se fija con `cpu`, para no
  dejar sin CPU a los hilos del kernel que le entregan los paquetes.

## Hilos por enlace (`link_threads`, cliente)

Cómo lee el cliente los sockets de sus enlaces (historia
[011](../docs/historias/011-hilos.md)):

- **`legacy`, por omisión:** el bucle de siempre (0.4), que lee todos los
  enlaces. Sigue siendo el valor por omisión hasta medir los hilos en la Pi
  (puerta P), y es la vía de escape: un cambio en LuCI vuelve a él.
- **`on`:** un hilo por enlace (como mucho 8; a partir del noveno, comparten
  el hilo que menos enlaces tenga) que lee su socket a un anillo; el hilo
  principal (el *hub*) toma lo de cada hilo por turnos y decide todo como
  antes (secuencia, MAC, ventana anti-replay, salud, sondas). Un enlace o un
  hilo que no recibe CPU llena solo su propio socket: los demás siguen. Un
  atasco del hilo principal, en cambio, frena todos los enlaces a la vez
  (cada socket lo amortigua con su buffer). Cuesta más CPU con poco
  tráfico: un despertar por lote en cada hilo (medido: historia 011).
- **`off`:** la misma estructura nueva en un solo hilo, sin hilos ni
  timbres; sirve para distinguir un fallo de los hilos de uno del código
  nuevo. No es el código de antes: eso es `legacy`.
- **`auto`:** `on` solo para la clase de máquina medida (aarch64 o x86-64,
  4 CPU o más, sin `cpu`) y solo en una versión que lo permita; en esta
  versión, `legacy` siempre. `cengarde -t` dice en qué queda.
- **Perillas:** `io_queue` (64–1024, potencia de dos; 256) es la cola de
  órdenes de cada hilo; `cpu` en una sección `[link]` fija el hilo de ese
  enlace; `cpu` global fija el principal; `rt_priority` va a todos los hilos
  de datos y `busy_poll_us` también, mientras quepan en las CPU con una de
  sobra (si no, ningún hilo de enlace sondea, tampoco los que ya lo hacían:
  solo el principal, con un aviso).
- **Todo se fija al arrancar:** cambiar cualquiera de estas reinicia el
  proceso en el lugar.

## Cambiar la configuración en marcha

`SIGHUP` (o `cengarde ctl reload`) vuelve a leer el archivo sin cortar el
túnel:

- **En el lugar, con la misma sesión:** enlaces (`interfaces`, `exclude`,
  `[link]`), direcciones del servidor y `server_failover_ms`, etiquetas,
  salud de los enlaces, reordenamiento y reparto (salvo `reorder_pool_kb`),
  sondas, buffers, `log_level`, `status_file`, `description`, IP pass y, en
  el servidor, los tiempos de espera. Un enlace
  empieza otra vez por la primera dirección del servidor solo si cambiaron
  sus propias entradas (las de su familia: añadidas, quitadas, editadas o
  movidas); una entrada de otra familia, o un nombre que vuelve a
  resolverse con las mismas direcciones en otro orden, no mueve un enlace
  que funciona. En el laboratorio, dos recargas y la pausa de un enlace a
  2000 pps no perdieron ningún paquete (`sudo bench/lab.sh control`).
- **En el servidor, también en el lugar:** las secciones `[client NAME]`
  (agregar, quitar, desactivar o cambiar un router no toca a los demás) y
  `max_sessions` (la tabla crece; con un límite menor, solo se rechazan
  sesiones nuevas).
- **Reiniciando el proceso en el lugar** (mismo PID, sesión nueva): `mode`,
  `key`, `listen`, `control_socket`, `busy_poll_us`, `cpu`, `rt_priority`,
  `reorder_pool_kb`, en el cliente `link_threads`, `io_queue` y el `cpu` de
  un `[link]`, y en el servidor `wireguard` (con un solo router), `lanes` y
  pasar de un router a varios o al revés (`[client] sections`).
- **Si el archivo tiene un error,** sigue con la configuración anterior,
  lo registra y lo publica en el estado (`config_error`).
- **Sin bloquear el túnel:** la lectura va en un hilo aparte. Los nombres
  ya no se resuelven al leer el archivo (salvo los de `/etc/hosts`): uno que
  sigue en la configuración conserva sus direcciones, y uno nuevo se busca
  al momento.
- **En OpenWrt,** procd manda `SIGHUP` en vez de reiniciar cuando cambia la
  configuración generada.

## Socket de control: `cengarde ctl`

Con `control_socket = /var/run/cengarde/cengarde.sock` (un socket Unix, solo
para root), `cengarde ctl` habla con el motor en marcha:

```sh
cengarde ctl links              # cada interfaz, por qué lleva el túnel o no, y hacia dónde
cengarde ctl link eth1.30 off   # pausar un enlace (on: forzarlo; auto: lo que diga la config)
cengarde ctl reset              # todos los enlaces otra vez como dice la config
cengarde ctl reload             # como SIGHUP, pero responde si se aplicó
cengarde ctl status             # el JSON de estado
cengarde ctl threads            # cada hilo: TID, última CPU, ms de CPU y % de CPU en los últimos 5 s
```

- **Socket:** el de arriba por omisión; otro con `-s RUTA`, o el de una
  configuración con `-c ARCHIVO`.
- **Pausas:** se mantienen en las recargas y se pierden al reiniciar, como
  las exclusiones temporales del gestor web de engarde Go
  (`include`/`exclude`/`swap`/`reset`).
- **`links` en el cliente:** estado, RTT, MTU de camino (`MTU`), failovers y
  las direcciones al final: la local, tan ancha como la más larga (IPv6), y
  la del servidor, con `(2/3)` cuando la lista del enlace tiene más de una
  (la segunda de tres).
- **`links` en el servidor:** muestra las sesiones y sus enlaces, con la
  dirección del cliente (`ADDRESS`) y la del servidor a la que envía
  (`LOCAL`); con varios routers, la columna `CLIENT` va primero (con uno
  solo, la salida no cambia).
- **`threads`:** el hilo principal (`cg-hub` en el cliente, `cg-main` en el
  servidor), los de cada enlace (`cg-<interfaz>`, con `link_threads = on`) y
  los que escriben el estado (`cg-status`, y `cg-pass` o `cg-forward` en el
  servidor), con
  el mismo nombre que muestran `ps -L` o `top -H` (el principal se sigue
  llamando `cengarde`). La CPU sale del reloj de CPU de cada hilo
  (`pthread_getcpuclockid`): los kernels de OpenWrt no tienen `schedstat`.
  Los scripts del laboratorio buscan ahí los hilos por nombre.

## IP pass pedido por el cliente

El IP pass, los puertos del VPS reenviados al sitio del cliente, se gobierna
desde el cliente:

- **Cliente:** `passthrough = yes` o `no` lo pide en cada sonda (bandera
  autenticada). Sin la opción no pide nada.
- **Servidor:** con `passthrough_file = RUTA`, escribe ahí `on` u `off` según
  lo pida la sesión más nueva, desde un hilo aparte. Mantiene el valor cuando
  no queda ninguna sesión.
- **Confirmación:** el servidor devuelve en sus respuestas lo que entregó, y
  el cliente lo muestra en `passthrough.server`.
- **Quién aplica:** cengarde no toca el cortafuegos. En el VPS,
  [`contrib/vps`](../contrib/vps/) vigila ese archivo con una unidad
  `.path` de systemd y ejecuta `cengarde-nat sync`.
- **Con varios routers,** el IP pass va a la tabla de reenvío
  (`forward_file`, sección siguiente).

## Varios routers por servidor (`[client NAME]`)

Desde la 0.5.1 (historia 010, PR 3d2b), sin cambiar el protocolo. Un
servidor sin secciones `[client]` sirve a un router con la `key` global,
como siempre; con ellas, a uno por sección:

```ini
mode = server
listen = *:65500
forward_file = /var/lib/cengarde/forward

[client alpha]
key = <la clave de alpha>
wireguard = 127.0.0.1:65501
wireguard_poke = 10.79.12.34:9
forward = tcp:9000=22 udp:5000-5010

[client bravo]
key = <la de bravo>
wireguard = 127.0.0.1:65502
enabled = no
```

- **Nombres:** de 1 a 12 minúsculas, dígitos o `-`, sin `-` al principio
  y sin llamarse `default`: los que acepta `cengarde-nat` (su interfaz
  `cg-NOMBRE` tiene que caber en un nombre de interfaz).
- **Por sección:** `key` (obligatoria y distinta en cada una), `wireguard`
  (la global si no tiene), `wireguard_poke` (sin valor por omisión: la
  dirección de ese router en el túnel), `label`, `enabled` (con `no`, sus
  sondas reciben un HELLO de rechazo y el router muestra `vps_refusing`),
  `passthrough` (`no`: nunca recibe el IP pass) y `forward`. Con secciones,
  la `key`, el `wireguard_poke` y el `passthrough_file` globales son un
  error.
- **Qué clave:** cada paquete trae la pista de su router (protocolo v4). La
  sesión se busca solo entre los routers de esa pista; una sonda sin sesión
  prueba sus claves, normalmente una y rara vez dos o tres
  (`src/clients.h`). Una pista que no es de nadie se descarta sin MAC
  (`rx.other_hint`). Si dos routers de una misma pista tienen una sesión
  con el mismo id (los ids viajan en claro: uno podría copiar el del
  otro), decide la clave que verifica el paquete.
- **Cuánto cuesta** (`bench/macbench`, un núcleo de un Xeon de 2,8 GHz,
  mínimos de 5 corridas): buscar la sesión cuesta 3 ns con un router y 10 ns
  con 64 (4 sesiones cada uno); el MAC de un DATA de 1400 B, 720 ns. Buscar
  y verificar un DATA no se distingue de antes; una sonda, unos 7 ns más
  (de 59–60 a 66–67 ns). Admitir una sesión cuesta un MAC por clave
  probada, unos 57 ns cada uno.
- **Sesiones:** cada router tiene las suyas, cada una con su socket hacia
  su WireGuard, y como mucho 4 a la vez (`CG_CLIENT_SESSIONS`), también
  con un solo router: una quinta reemplaza a la que hace más tiempo que no
  se oye (un router que se reinicia en bucle no llena la tabla). `max_sessions` vale por omisión 64,
  o 4 por router si son más de 16. La sesión más nueva de cada router
  decide su IP pass, y el puerto que WireGuard conoce y el empujón de
  `wireguard_poke` son de cada router.
- **Cada router su presupuesto de HELLO** (50 por segundo): las sondas
  reenviadas de uno no frenan a los demás.
- **Reenvío de puertos** (`forward`): `PROTO:PUERTO[-PUERTO][=DESTINO]`,
  con `tcp`, `udp` o `both`, separadas por espacios o comas; `DESTINO` es
  el puerto del router (por omisión, el mismo). Un puerto pedido dos veces,
  en el mismo router o en dos, es un error de la configuración.
- **La tabla de reenvío** (`forward_file`, `src/fwdtable.h`): el motor
  escribe ahí, cuando cambia y desde un hilo aparte, las reglas `forward`
  de los routers activos y, en `pass NOMBRE`, el router que tiene el IP pass
  (el rango entero). [`cengarde-nat`](../contrib/vps/cengarde-nat) la
  encuentra por el `forward_file` de la configuración (o por `FORWARD_FILE`
  en `nat.conf`) y la aplica; deja fuera los puertos reservados y lo que se
  pise. En un VPS, [`cengarde-vps-setup`](../contrib/vps/cengarde-vps-setup)
  escribe una sección por router y sus reglas
  (`cengarde-vps-setup forward add NOMBRE tcp:8080=80`). Formato:

  ```
  # cengarde forward table 1, written by the engine: data only
  pass bravo
  rule alpha tcp 9000 9000 22
  rule alpha udp 5000 5010 5000
  ```

- **Quién tiene el IP pass:** el primero que lo pide, y lo conserva mientras
  no lo suelte (lo pida `off`), aunque otro lo pida después o el motor se
  reinicie (lo lee de la tabla al arrancar); al soltarlo pasa al que lo pidió
  antes de los que esperan. También lo pierde un router sin sesión desde
  hace más de `session_timeout_ms` (apagado o mudado): si no, bloquearía a
  los demás para siempre. Un router que lo pide y espera lo ve en el
  registro ("asks for IP pass on, which alpha holds"), en sus respuestas
  (`passthrough.server`: `off`) y en LuCI ("el VPS no lo concede").
- **Recargas:** agregar un router, cambiar su etiqueta, sus reglas o su
  `wireguard_poke` no toca ninguna sesión; quitarlo, desactivarlo o cambiar
  su clave o su `wireguard` cierra solo las suyas (su router vuelve con el
  HELLO, o queda rechazado). En el laboratorio
  (`sudo bench/lab.sh multiclient`), tres routers, dos con la misma pista,
  pasan 1000 pps cada uno en los dos sentidos sin perder ni cruzar nada, y
  las recargas no le quitan un paquete al que sigue.

## Router con la hora atrasada (`wireguard_poke`)

WireGuard ignora una iniciación de handshake que no sea más nueva que la
última que aceptó de ese peer. Una Raspberry Pi no tiene reloj con batería:
al reiniciarse vuelve con la hora atrasada (la de la imagen, o la del último
cambio en `/etc`), y con todo por el túnel el NTP no puede corregirla. El
túnel quedaba caído hasta que el reloj alcanzaba esa hora (historia 002).
Desde la 0.4.4, el servidor lo destraba solo, sin cambiar el protocolo:

- **Detecta:** cuenta las iniciaciones del cliente que WireGuard no contesta
  (`wgwatch.h`).
- **Empuja:** tras 3 sin respuesta, manda un datagrama a `wireguard_poke`,
  la dirección del router dentro del túnel (por omisión `10.79.0.2:9`; `none`
  lo apaga). WireGuard inicia entonces el handshake él mismo, y el router lo
  acepta tenga la hora que tenga. Como mucho uno cada 15 s, y solo mientras
  el cliente siga llamando.
- **Redirige:** WireGuard manda su iniciación a la sesión que conoce, que
  tras un reinicio del router es la vieja; va por la sesión nueva mientras
  esta llame en vano y la vieja haya callado antes.
- **Hereda el puerto:** WireGuard conoce al cliente por el puerto del socket
  de su sesión. Ese puerto pasa a la sesión nueva cuando la vieja se cierra
  mientras la nueva llama en vano, o cuando la nueva empieza sin otras
  vivas. Así se destraba también un corte más largo que
  `session_timeout_ms`, o un WireGuard que contesta tarde.
- **Guarda el puerto (0.6.2):** con `wireguard_port_file` el servidor
  escribe el puerto de cada cliente en ese archivo, desde un hilo
  (`cg-wgport`), y lo lee al arrancar: la primera sesión de cada cliente
  vuelve a tomarlo. Antes vivía solo en memoria, y un reinicio del motor
  (una actualización) con el router atrasado a la vez dejaba el túnel caído
  hasta reiniciar WireGuard en el VPS. El formato (`src/wgport.h`) es una
  cabecera con versión y una línea `PUERTO NOMBRE` por cliente (`PUERTO` a
  secas con un solo cliente). `cengarde-vps-setup` lo pone en
  `/var/lib/cengarde/wg/ports`, un directorio aparte porque
  `cengarde-passthrough.path` vigila `/var/lib/cengarde`, y la primera vez lo
  siembra desde `wg show`.
- **Estado:** `wireguard.pokes` y `wireguard.redirected_handshakes`, y por
  sesión `wireguard_unanswered`.

## Estado

`status_file` escribe cada segundo (`status_interval_ms`), desde un hilo
aparte para que un disco lento no frene el túnel, un JSON con contadores
globales, `uptime_ms` (el reloj monótono del proceso), `time_ms` (la hora de
pared en ms, que salta cuando NTP la corrige en un router sin RTC) y, por
enlace:
- estado (`live`, `waiting` antes de la primera respuesta, `stalled`, `down`
  o `paused`), por qué no lleva el túnel (`reason`, vacío si lo lleva), si
  está pausado o forzado a mano (`override`) y RTT;
- en el cliente, hacia dónde: `local`, `remote`, `family` (`ipv4` o `ipv6`),
  `candidate` (su lugar entre las direcciones del servidor de su familia: 0
  es la primera; más, que hizo failover), `candidates`, `detour` (`true`
  cuando el socket no abrió donde tocaba, por falta de ruta por el enlace,
  y está en otra dirección del servidor o sale desde otra dirección suya; lo
  reintenta cada 5 s), `failovers` y `path_mtu` (el MTU de camino de su
  socket, 0 si no se sabe);
- salud en el sentido que decide este extremo (`upload` en el cliente,
  `download` en el servidor): `active` o `muted`, cuánto va por detrás del
  más rápido (`*_behind_ms`), cuántas veces se silenció y desde cuándo;
- si el otro extremo lo tiene silenciado en su sentido (`download_muted` en
  el cliente, `upload_muted` en el servidor);
- paquetes y bytes enviados, descartados y errores;
- `rx_first`: cuántas veces llegó primero;
- `rx_lag_ms`: retraso medio frente a la primera copia;
- `rx_missed`: paquetes que no trajo;
- lo mismo visto desde el otro extremo (`server_view` / `client_view`).

En el cliente, `names[]` tiene cada nombre de las listas de servidor:
`name`, `state` (`ok`; `hosts` si sale de `/etc/hosts`; `waiting` antes de
la primera respuesta; `failed` si las búsquedas fallan y no tiene
direcciones; `none` si el DNS respondió sin direcciones), `addresses` (IPv6
primero), `answered_ms_ago`, `next_lookup_ms`, `ttl` (el que dio el DNS, en
s), `silent` (ningún enlace vivo en sus direcciones), `error` (`timeout`,
`servfail`, `refused`, `nxdomain`, `nodata`, `malformed` o `no link`),
`via` (enlace y DNS de la última respuesta), `lookups` y `failures`.

Además, `config_error` (por qué no se aplicó la última recarga) y el IP pass:
`passthrough.requested` y `passthrough.server` en el cliente, y
`passthrough` en el servidor (con un router) o `ip_pass_holder` (con
varios: el nombre del que lo tiene).

En el servidor, `clients[]` tiene cada router: `name` (`null` con uno
solo), `label`, `enabled`, `hint`, `wireguard`, `sessions`, `newest` (el id
de su sesión más nueva), `passthrough` (lo que pide), `passthrough_allowed`,
`ip_pass` (si lo tiene) y `refused` (sondas rechazadas: desactivado, o sin
lugar). Cada sesión dice su `client`, y `max_sessions` el límite;
`rx.id_clashes` cuenta las sesiones rechazadas porque su clave en la tabla
chocó con la de otra (una en 2^32).

En el servidor, `listen` es la dirección que abrió (`*` queda en
`0.0.0.0` en un kernel sin IPv6) y `reply_from_arrival` dice si responde
desde la dirección de llegada. Por enlace:
- `local`: la dirección del servidor a la que envía el cliente (`null` si
  no se sabe, p. ej. con un `listen` concreto);
- `moves`: cuántas veces cambió la dirección de cualquiera de los dos
  extremos (un NAT nuevo, otra dirección del VPS);
- `local_errors`: paquetes que no pudieron salir porque esa dirección ya no
  existe o no hay ruta; el camino sigue, y lo registra como "address removed
  or no route".

`rx.ctrunc` cuenta los paquetes cuya dirección de llegada no cupo.

Las colas del servidor (ver [Colas del servidor](#colas-del-servidor-lanes)):
- `lanes[]`: por socket, `index`, `group` (0 hasta el protocolo 4), `rx`
  (datagramas leídos), `drops` (los que el kernel tiró por cola llena;
  `null` si el kernel no lo dice) y `links` (los enlaces cuyos paquetes
  verificados llegaron por él: uno por cola);
- `junk`: el socket de basura (`index`, `rx`, `drops`), o `null` con
  `lanes = 1`; `rx.junk`, `rx.short` y `rx.bad_version` lo desglosan;
- `rcvbuf`: `configured`, `budget` (bytes para todos los sockets que
  reciben), `budget_from` (`net.ipv4.udp_mem` o `RAM`), `sockets`,
  `per_socket` (lo que se pide) y `effective` (lo que el kernel da a cada
  cola, el doble); `rcvbuf_capped` dice si `per_socket` quedó por debajo de
  `rcvbuf`;
- `steering_error`: por qué hay un solo socket aunque `lanes` pida más
  (vacío si no pasa).

En el cliente, `threads` dice el modo pedido (`setting`), el que corre
(`mode`), cuántos hilos de enlace hay (`pumps`), la CPU del principal
(`hub_cpu_pct`) y los avisos del plan de hilos, los mismos que van al
registro (`warnings`: `rt_priority_every_cpu`, tiempo real en tantos hilos
de datos como CPU; `busy_poll_capped`, los hilos de enlace no sondean
porque no caben en las CPU con una de sobra; `pumps_share_cpu`, dos hilos
de enlace fijados a la misma CPU); por enlace:
- `pump`: su hilo (`null` en `legacy`);
- `socket_drops`: datagramas que el kernel tiró en su socket (`SO_MEMINFO`;
  `null` si no lo dice), en cualquier modo;
- `io_stalled_ms`: cuánto lleva el trabajo (órdenes, o datagramas en su
  socket con sitio en su anillo) esperando a su hilo sin que dé una vuelta,
  contado desde la primera comprobación (una por segundo) que lo vio
  esperando: puede quedarse corto hasta 1 s, y nunca cuenta lo que el hilo
  durmió sin nada que hacer (0 por debajo de 1 s; a los 5 s lo registra);
- `pump_cpu_pct`: la CPU de su hilo en los últimos 5 s;
- `hop_us.down`: p50 y p99, en los últimos 5 s, de lo que espera un lote
  entre el hilo que lo leyó y el principal que lo toma (`up`, con el PR 3c);
- `rx_paused`: veces que su hilo dejó de leer porque el principal no daba
  abasto (el kernel guarda lo que llega, y si se llena descarta solo de ese
  enlace); `open_failed`: sockets que su hilo no pudo vigilar.
`download.stale` cuenta lo que llegó por un socket ya cerrado o sustituido
(se descarta antes de mirarlo).

En el cliente, `upload.largest` es el datagrama de WireGuard más grande de
los últimos 5 s, el que se compara con cada `path_mtu`, y `download.hellos`
cuenta los HELLO que tomó (un servidor que se reinició, una sesión que
expiró, un enlace nuevo o un NAT que cambió). Por enlace, `cookie` dice si
el servidor le dio una y `refused`, si lo rechaza (desactivado en el servidor, o sin lugar para
otra sesión); `vps_refusing` resume eso para todo el router. En el servidor, `hellos`
(`sent`, `refused` y `over_budget`) y, en `rx`, `other_hint` (paquetes de
otro router, por su pista) y `no_session` (DATA de una sesión que todavía
no tiene: espera a la sonda).

Para el bonding (historia 012) hay tres medidas que solo se muestran y
nunca deciden nada; las toma el registrador de campo (`cengarde-rec`,
`bench/fieldrec.py`). `lost_all` y `joint` miden la bajada en el cliente
(en la raíz del JSON) y la subida de cada sesión en el servidor
(`sessions[]`); `sizes` está en la raíz en los dos extremos.
- `lost_all`: secuencias de DATA que salieron de la ventana anti-replay sin
  llegar por ningún enlace, es decir, lo que el túnel perdió en todos. Se
  cuenta desde la primera secuencia que vio la ventana (o desde donde siguió
  un salto mayor que la ventana, como el de un servidor que retoma la sesión
  2^20 más adelante) y va unas 8200 secuencias por detrás.
- `joint`: la tardanza conjunta. Cuando su casilla se reutiliza (4096
  secuencias después), cada paquete suma uno en `expect`, por la máscara de
  enlaces que debían traer una copia, y uno en cada objeto de `late`, por la
  máscara de los que no la trajeron dentro del plazo de `deadlines_ms`
  correspondiente (50, 100 y 150 ms desde la primera copia; no llegar nunca
  también es tarde). La clave es la máscara en decimal: el bit i es el enlace
  cuyo `links[].id` es i; solo aparecen los cubos distintos de cero.
  `packets` son los paquetes evaluados y `other`, los que quedan fuera: un
  enlace esperado con id 8 o más, o una casilla reutilizada antes de 150 ms
  (unos 27 kpps o más). La probabilidad de que todos los enlaces de un
  conjunto S lleguen tarde a un plazo es la suma de los cubos de ese plazo
  cuya máscara contiene a S dividida por la de los cubos de `expect` que lo
  contienen: el número que decide k-de-N. Las copias tardías no pasan por el
  MAC, así que esto solo sirve para medir.
- `sizes`: los datagramas de WireGuard por tipo (`init`, `response`,
  `cookie` y `other`) y los de datos por tamaño (`data` y `data_bytes`, 12
  cubos: hasta cada borde de `edges`, incluido, y el último por encima de
  1440). `up` es lo que el cliente lee de WireGuard y lo que el
  servidor le entrega (primeras copias de todos los routers); `down`, al
  revés.

Cuesta poca memoria: cada casilla de llegadas pasa de 16 a 20 bytes y los
histogramas suman 8 KiB, unos 24 KiB más por sesión (el cliente tiene una; el
servidor, una por sesión: ~1,5 MiB con las 64 de `max_sessions` de fábrica).

El reordenador (ver [Reordenamiento y
reparto](#reordenamiento-y-reparto-reorder-split)) está en `reorder`: en la
raíz del cliente para la bajada, y en cada sesión del servidor para su
subida:
- `enabled`, `held` (retenidos ahora), `held_max` y, en el cliente,
  `pool_buffers` y `pool_free` (en el servidor, `reorder_pool` en la raíz:
  `buffers`, `free`, `sessions_holding` y `full`, las veces que no hubo
  búfer);
- cada paquete verificado cuenta en uno solo de `in_order`, `held_total`
  (retenido hasta que llegó lo anterior), `bypassed` (pequeño o handshake:
  al instante), `late.*`, `far` (un salto de 8192 secuencias o más
  mientras retenía, como el de un servidor que retoma la sesión),
  `resyncs` y `pool_full` (sin búfer libre: al instante); `split_rx` cuenta
  los repartidos;
- `lost`: huecos que dejó de esperar, por qué: `rack` (todos los enlaces
  trajeron uno posterior), `cap` (el plazo), `overflow` (la ventana se
  llenó) y `flush` (al apagarlo);
- `late`: los que llegaron después de eso, por la regla que soltó su hueco
  (`after_rack`, `after_cap`, `after_overflow`; `old`, si su casilla ya era
  de otro). `after_rack` > 0 dice que un enlace no entrega en orden o que se
  esperaba a uno equivocado; `after_cap`, que el plazo es corto;
- `wait` (cuánto esperó cada retenido) y `late_by` (cuánto después de
  soltar su hueco llegó cada tardío): histogramas con los bordes de
  `edges_ms`, incluidos, y un último cubo por encima de 500 ms;
  `max_burst`, la mayor tanda de retenidos entregada de una vez: lo que
  soltó una sola pasada del bucle, que entrega a lo sumo 512 (el resto va
  en las pasadas siguientes, cada una con su tanda), salvo cuando la
  ventana se desborda y sale todo junto.
Por enlace, `rx_owd_spread_ms` es cuánto va su retraso de llegada (bajada
en el cliente, subida en el servidor) por detrás del más rápido de los
enlaces que espera el reordenador (`null` sin medida), y
`rx_owd_swing_ms` cuánto espera el reordenador por encima de su retraso
medio: cuatro veces la desviación de ese retraso (la media de lo que
cada medida se aparta de él, como el RTTVAR de TCP). El plazo de un hueco
es el peor retraso más esa espera, más `reorder_margin_ms`, entre los
enlaces que espera: un enlace celular cargado trae paquetes decenas de ms
por encima de su media (historia 013). `rx_reorder_late` cuenta los
paquetes que el enlace trajo después de que su hueco se diera por perdido
(cualquier motivo): dice qué enlace llega tarde. Su ventana ocupa
81 KiB (en el servidor, por sesión: ~5 MiB con 64, *cálculo*), y el pool,
solo lo que llega a retener.

La capacidad de cada enlace en el sentido en que envía este extremo (la
subida en el cliente, la bajada por camino en el servidor; `capacity.h`,
historia 013): `delivery_mbit`, lo que el otro extremo dice haber
recibido por él entre dos de sus informes (los paquetes de `rx`, por el
tamaño medio de lo enviado, en al menos 500 ms), suavizado (un cuarto de
cada muestra nueva); `capacity_mbit`, el
mayor `delivery_mbit` visto, salvo que su cola (`queue_ms`, el retardo
de ida sobre su mínimo de `governor_window_ms`) pase de
`governor_target_ms`: entonces lo que entrega ahora. Un enlace al que se
le manda menos de lo que lleva entrega lo que se le manda, y eso no dice
nada de lo que lleva: su capacidad se mantiene, y solo una cola la baja
(con `capacity_weights`, que bajara sola dejaba enlaces al mínimo:
historia 013). Con `capacity_weights`, de
ella salen los pesos del reparto (arriba); sin, solo se informa.
`shape_mbit` (en `upload` en el cliente, por sesión en el servidor) es el
ritmo para un `cake` delante del túnel que no se fija a mano
(`bench/shaper.sh`, experimental; historia 013), como `cake-autorate`
pero con las medidas del motor: `null` hasta que la cola de un enlace
pasa de `governor_target_ms` (sin cola no hace falta frenar); entonces
9/10 de lo que entregan los enlaces, para que la cola se forme en `cake`
y no en los módems; sube (ticks de 100 ms) mientras entregan
al menos 4/5 de ese ritmo sin cola (el freno podría ser `cake` mismo), un 1/32 por tick;
se mantiene en reposo; solo baja cuando los enlaces con cola llevan la mayor parte de lo entregado y eso pasa de 2 Mbit/s (la cola de un solo enlace lento es para los pesos). Solo vale repartiendo: copiando, un enlace
atrasado no cuenta lo que recibe.

Los paquetes repartidos: en el cliente, `upload.split` y, por enlace,
`tx_split` (los que salieron por él: su parte, o todos con peso 0) y
`weight`; en el servidor, `download_split` por sesión y `tx_split` por
enlace. En los dos, por enlace, `split_share` dice qué hace en el reparto
(`null` si ese extremo no reparte ahora): `yes` (recibe su parte), `spread`
(está más de `split_spread_ms` detrás: fuera del reparto, lleva copia de
todo con `split_spread_copy` y solo lo que va por todos sin ella), `waiting`
(sin medida de retraso todavía: igual), `weight 0`
(lleva copias por su peso) o `no` (no es apto: silenciado, en espera o sin
respuesta reciente), y
`split_behind_ms`, cuánto va detrás del más rápido según la guarda (`null`
sin medida); los valores de `split_spread_ms` y `split_spread_copy` están
en `upload` (cliente) y en cada sesión (servidor). `upload.split_state` dice qué hace el cliente con
la subida: `off`, `splitting`, `one link` (uno solo apto con
`split_spread_copy = no`: lleva todo lo que se reparte), `duplicating`
(ninguno apto, o uno solo con `split_spread_copy`; hasta la 0.6.2,
`duplicating with < 2 links`) o `waiting for the peer` (`split = on`, hasta que el servidor
diga que reordena); `upload.server_reorders`, lo que dijo la respuesta más
nueva (`null` sin cola: un servidor anterior). En cada sesión del servidor,
`split_mode` dice qué hace con la bajada: `off`, `force`, `client` (reparte como pide el
cliente) o `duplicating` (`split = client`, pero el cliente no lo pide, no
reordena o no lo dice); `client_reorders` y `client_asks_split`, lo que dijo
su sonda más nueva (`null` sin cola: un cliente anterior), y por enlace
`client_weight`, el peso que manda el cliente. «Más nueva» es por su número
de secuencia de control, no por la hora de llegada: tras una recarga, las
sondas o respuestas que salieron antes por un enlace lento llegan después de
las nuevas por uno rápido, y no deshacen lo que estas dijeron.

## Protocolo v4

Desde la 0.5.0 (historia 010, PR 3d2). La cabecera sigue midiendo 24 bytes:

- **Byte 2, la pista de cliente:** el primer byte de un BLAKE2s de la clave
  (`pair.h`). El servidor descarta sin calcular el MAC lo que trae una
  pista que no es de ninguno de sus routers, y con varios elige con ella
  qué claves probar.
- **HELLO y cookies** (`cookie.h`): el servidor contesta con un HELLO la
  sonda que todavía no puede aceptar (no tiene su sesión, es un enlace
  nuevo o viene de otra dirección), con una cookie atada al cliente, la
  sesión, el enlace y esa dirección, válida de 30 a 60 s. Solo la sonda que
  la trae crea la sesión, aprende el camino o lo cambia de dirección. Una
  sonda capturada y reenviada después, desde otro lado o tras un reinicio
  del servidor, no crea nada, no desvía un camino y no cambia el IP pass. El
  HELLO va solo a sondas autenticadas, nunca es más grande que la sonda y
  sale de un balde de 50 por segundo.
- **El servidor retoma la sesión donde está el cliente:** la sonda dice la
  última secuencia que el cliente recibió en cada clase y la siguiente que
  enviará. El servidor sigue 2^20 por delante de lo recibido, así que el
  cliente nunca rehace su ventana, y sus ventanas de subida nacen con todo
  lo anterior marcado. Tras un reinicio del servidor, el túnel vuelve en el
  viaje del HELLO (en el laboratorio, `sudo bench/lab.sh restart`).
- **Secuencia de control aparte:** sondas y respuestas cuentan en su propia
  secuencia, con su propia ventana. La de DATA ya no tiene huecos que no
  sean pérdidas (lo que necesita el reordenador del bonding), y un enlace
  muy atrasado sigue entregando sus respuestas.
- **Para el bonding** (historia 012): en DATA, los bits de ECN (0x03,
  reservados: se envían en 0 y se ignoran) y la marca de reparto (0x04: este
  paquete no va por todos los enlaces que llevan datos, aunque otro pueda
  llevar una copia); la carga de las sondas puede crecer (se leen los campos
  conocidos y se ignora el resto), para el informe por enlace y las sondas
  rellenadas.
- **La cola de bonding de las sondas:** 4 bytes más tras los 40 de
  siempre, una palabra cuyos bits 0–9 son el peso del enlace (0–1000), el
  16 dice "reordeno lo que recibo" y el 17 "repárteme lo que me envías"; el
  resto va en 0 y se ignora. El cliente la manda en cada sonda; el servidor
  la devuelve, solo con el bit 16, únicamente a una sonda que la traía, así
  que una respuesta nunca es más grande que su sonda. Sigue siendo la
  versión 4: un motor anterior lee los 40 bytes e ignora la cola, y uno nuevo
  no lee cola de una sonda o respuesta de 40.

`sudo bench/lab.sh replay` lo prueba con `bench/cgprobe`, que fabrica sondas
con la clave del laboratorio.

## Limitaciones conocidas

- **Puerto de WireGuard en el cliente:** el cliente lo aprende del primer
  paquete que WireGuard le envía, como engarde.
- **Enlaces asimétricos:** un enlace que sube pero no baja (o al revés) se da
  por mudo en los dos sentidos, aunque uno de ellos funcione.
- **Aún no** baja privilegios y el servidor es de un solo hilo; en el
  cliente, con `link_threads = on`, la subida la sigue enviando el hilo
  principal (los hilos de enlace la enviarán con el PR 3c).
