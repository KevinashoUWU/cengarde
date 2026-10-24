# Motor cengarde (C)

Reescritura en C de engarde: duplica cada datagrama de WireGuard por todos los
enlaces del cliente hacia el servidor, y en el otro extremo entrega a
WireGuard solo la primera copia. Cliente y servidor son el mismo binario; el
modo lo decide la configuración. El diseño y sus razones están en
[`docs/historias/005-motor-c-v1.md`](../docs/historias/005-motor-c-v1.md) y
la salud de los enlaces en
[`docs/historias/006-salud-de-enlaces.md`](../docs/historias/006-salud-de-enlaces.md).

**No es compatible en el cable con engarde Go:** usa su propia cabecera
autenticada (protocolo v5), así que los dos extremos tienen que ser cengarde
de la misma versión de protocolo. `cengarde version` la muestra al final, por
ejemplo `cengarde 0.5.0-r1-g<commit> (protocol 5)` en OpenWrt. El formato
está en [`src/proto.h`](src/proto.h) y lo que cambió el v4 y el v5 en las
secciones [Protocolo v4](#protocolo-v4) y [Protocolo v5](#protocolo-v5).

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
  Con dos enlaces, entonces, nada se silencia.
- **`mute_queue` (`no`; `yes` o `no`), experimental, en los dos extremos:**
  el silenciado compara, en vez de solo lo que un enlace va detrás del más
  rápido, el mayor entre eso, su cola (su retardo sobre su mínimo de 60 s)
  menos la del más rápido y, en el cliente, cuánto lleva sin respuesta su
  sonda más vieja menos el RTT del enlace más rápido. Un enlace atrasado
  cuyos informes llegan a ráfagas ya no reinicia su espera en cada hueco.
  En la celda virtual (un LTE cayendo al acantilado del SNR junto a un
  fijo, con `min_active_links = 1`) se silencia a los 3,3 s en vez de
  3,8–7,9 s o nunca, pero el TCP de la redundancia no mejora y la vuelta de
  la señal cuesta 6–9 s sin el LTE por la espera exponencial; en el
  laboratorio no cambia nada (*medido*, historia 016). El estado dice
  `upload_late_ms` (cliente) y `download_late_ms` (servidor).
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
- **`reorder_by_link` (`no`; `yes` o `no`), experimental:** cada paquete
  de datos dice por qué enlace salió la secuencia anterior, cuando salió
  por uno solo (protocolo v5, `CG_F_PREV`). Con `yes`, el reordenador
  espera un hueco solo lo que tarda ese enlace (su retraso más cuatro
  desvíos, más `reorder_margin_ms`), en vez del enlace más lento, y lo da
  por perdido apenas ese enlace trae una secuencia posterior (los enlaces
  entregan en orden). Sin el dato, como siempre. Medido, no mejoró el
  rendimiento (16 flujos de bajada, 3 rondas: 151,2 Mbit/s sin la opción y
  141,5 con ella), aunque suelta antes, como se esperaba (historia 013).
- **`reorder_adaptive` (`no`; `yes` o `no`), experimental:** el margen de
  cada enlace aprende de sus tardíos, como la ventana de reordenamiento de
  RACK (RFC 8985): cuando un paquete llega por un enlace después de que se
  abandonó su hueco, ese enlace espera desde entonces ese atraso y un cuarto
  más (dentro de `reorder_max_wait_ms`), y lo aprendido baja a la mitad tras
  5 s sin tardíos. El estado lo muestra en `reorder.adapt_ms`, por enlace.
  Con un margen fijo de 10 ms, MPEG-TS crudo por el túnel llegaba con
  errores (los paquetes de un módem con la cola en un salto llegaban
  100–500 ms tarde); con uno fijo de 400, limpio, pero cada hueco espera más.
  El adaptativo perdió contra el fijo de 400 (56 y 116 errores contra 3 y 6):
  aprende después de equivocarse y olvida más rápido de lo que vuelven los
  saltos de cola de un módem cargado (historia 013).
- **El rescate (experimental, protocolo v5; `rescue.h`, historia 013):**
  un ARQ que rescata un paquete antes de que TCP note que se perdió.
  Cuando el reordenador del cliente va a abandonar un hueco, lo pide al
  servidor (`CG_T_NACK`, por el enlace de menor RTT), y el servidor lo
  reenvía por su ruta más rápida. Se pide apenas se sabe perdido (el enlace
  por el que iba trajo algo posterior) o al vencer su plazo, y se espera
  `rescue_wait_ms` más, nunca más allá del tope de siempre. Su lugar es el
  tráfico de tasa fija (radio, video, llamadas): con TCP saturando, tapar la
  pérdida deja que TCP llene el búfer del enlace, y en la red de pruebas
  (16 flujos de bajada) rindió algo menos con él (86,4 contra 91,0 Mbit/s);
  con MPEG-TS crudo, un margen de 10 ms más el rescate dio 0 errores en dos
  rondas de 60 s (historia 013). Apagado por omisión.
  - **`rescue_kb` (0, apagado; hasta 262144):** el anillo de lo enviado
    hace poco, en entradas de 2 KiB (32768: unos 16 mil paquetes, ~0,6 s a
    300 Mbit/s); con él, las sondas y sus respuestas lo anuncian
    (`CG_BOND_RESCUE`). En el servidor rescata la bajada; en el cliente, la
    subida. Solo cambia al reiniciar.
  - **`rescue_pct` (5; 1–50):** el presupuesto, en % de lo que se envía en
    ese sentido: lo que se reenvía sale de ahí.
  - **`rescue_wait_ms` (el que recibe; 0, no pide; hasta 1000):** cuánto
    espera un hueco pedido; el cliente para la bajada, el servidor para la
    subida (el otro extremo necesita `rescue_kb`). Con un intento, ~1,5 RTT
    del enlace más rápido; con `rescue_tries` intentos, al menos ese número
    de RTT (60 en la red de pruebas, RTT ~35 ms).
  - **`rescue_tries` (2; 1–4):** cuántas veces se pide un hueco dentro de
    `rescue_wait_ms`, repartidas (como el mínimo y el máximo de reintentos de
    librist). Cada NACK sale por los dos enlaces más rápidos, en rangos (una
    secuencia y una máscara de 64 bits); el que reenvía manda una vez por los
    dos, y un reintento (`CG_F_RETRY`) lo manda por dos caminos.
  - **No se rescata la congestión:** lo que se pierde en una ruta llena, o
    con el presupuesto agotado, no se reenvía, y el cliente ni lo pide
    (`CG_BOND_FULL`): esa pérdida es la que hace frenar a TCP. En el
    laboratorio, con 1 % de pérdida al azar por enlace, la bajada pasa de
    10 a 50 Mbit/s; donde la pérdida es de un enlace lleno, cuesta ~15 %.
    En redundancia (`split` apagado) solo pide con `reorder_dup_wait_ms`
    mayor que 0.
  - **El gobernador del rescate:** cada enlace tiene un peso de 0 a 1
    (`rescue_weight` en el estado; `cg_cap_rescue_w`, `capacity.h`) según
    dónde estará su cola dentro de 1 s: la cola de ahora (sobre el mínimo
    de su retardo en 60 s), más su pendiente y la mitad de la pendiente de
    la pendiente (`queue_slope_ms_s` es la pendiente). Hasta `governor_target_ms`, el peso es 1; desde 4 veces
    `governor_target_ms`, 0 (lleno: lo que pierde es congestión, y no se
    rescata); entre medio, una recta. Sin cola medida, 1.
  - **Lo que no vendrá (`CG_F_GONE`):** el extremo al que se pide avisa qué
    secuencias no volverá a enviar (ya no las guarda, se perdieron en una
    ruta llena o el presupuesto no alcanza), y el reordenador las da por
    perdidas al instante en vez de esperar `rescue_wait_ms`.
  - Estado: `rescue` en el cliente (`on`, `server_keeps`, `nacks_sent`; de
    la subida: `keeps`, `nacks_rx`, `sent`, `missing`, `over_budget`,
    `congested`) y en cada sesión del servidor (`on`, `nacks`, `sent`,
    `missing`, `over_budget`, `congested`; de la subida: `client_keeps`,
    `asks`, `nacks_sent`); en los dos, `gone_sent` y `gone_received`.
    `reorder.rescue` (`asked`, `rescued`, `lost` y `refused`, los perdidos
    que el otro extremo avisó que no vendrían).
- **`fec` (no; protocolo 7):** la paridad espacial de la subida repartida
  (`fec.h`). El cliente junta en grupos los paquetes que fueron cada uno por
  un solo enlace, como mucho uno por enlace, y manda el XOR del grupo por un
  enlace de fuera (`CG_T_FEC`). El servidor guarda lo recibido hace poco y
  reconstruye un paquete que falte en un grupo antes de que el rescate lo
  pida (con `fec`, un hueco espera al menos 2 × `fec_wait_ms` + 20 ms antes
  del NACK). Un grupo de uno no manda paridad (sería una copia). Solo
  cambia al reiniciar (el anillo del que recibe). Va en los dos extremos.
  - **`fec_wait_ms` (10; 1–200):** el plazo máximo de un grupo. El plazo
    real sale de la tasa: dos veces lo que tardan en salir k paquetes al
    intervalo medio, entre 1 ms y `fec_wait_ms`.
  - **`fec_share_max` (0, sin tope; hasta 100):** con `fec`, ningún
    enlace lleva más de este % de lo repartido reciente; el que lo pasa se
    salta el paquete siguiente, sin dejar nunca menos de dos enlaces. Una
    paridad por grupo cuesta al menos 1/(1 − s) cuando un enlace lleva una
    parte s (historia 012). En la red de pruebas (RTMP de 8 Mbit/s,
    2026-10-17) el tope de 50 % empeoró todo: más paridad (27–36 % de los
    bytes frente a 12–21 %) y de 3 a 12 veces las retransmisiones de sin
    tope, porque manda tráfico a los enlaces lentos. Apagado por omisión.
  - Estado: `fec` en el cliente (`parities`, `bytes`, `no_link`, `capped`,
    `deadline_us`, `sizes` y por qué cerró cada grupo) y en cada sesión
    del servidor (lo reconstruido y lo que no).
  - En redundancia `fec` no hace nada: solo protege lo repartido (cada
    paquete va por todos los enlaces y ninguno queda solo en uno; medido,
    0 paridades, historia 020). Para eso está `redund_spare`.
  - Las paridades ya no cuentan en `tx_packets`/`tx_bytes` del enlace que
    las lleva (van en `tx_fec_packets`/`tx_fec_bytes`): el servidor cuenta
    solo DATA por camino, y una paridad contada como enviada parecía
    perdida a la capacidad (`short` y la pérdida de la muestra).
- **`redund_spare` (no; `copies`, `parity` o `mix`; solo el cliente,
  experimental):** en redundancia, protege al primer enlace (el de menor
  retardo sin una cola parada; un Starlink que pierde) con lo que les sobra
  a los demás (un celular ahogado sobre su codo), sin llenarles la cola
  (`spare.h`, historia 020). El que como primero junta una cola de más de
  4 × `redund_spare_ms` queda vetado 60 s; en un microcorte suyo el primero
  no cambia. Cada enlace que no es el primero tiene una asignación que sigue
  a su cola sobre el mínimo de 60 s: baja un cuarto si pasa de
  `redund_spare_ms`, sube si está bajo la mitad y se usa. `copies` manda en
  ella copias parciales (los paquetes chicos y, con `tcp_aware`, los ACK y
  las retransmisiones de TCP primero); `parity`, un XOR de k paquetes del
  primero (k de 2 a 15, el menor cuya paridad cabe; el servidor necesita
  `fec = yes`, el mismo `CG_T_FEC`, sin protocolo nuevo); `mix`, copias en
  un microcorte del primero (pierde más de 5 %; un XOR no rehace nada ahí)
  y paridad si no. En los tres, copias cuando la asignación lleva todo.
  *Medido* en la celda virtual (WOM ahogado con 7 Mbit/s, el fijo perdiendo
  0,7 % y un microcorte de 10 s al 60 %; n = 3 contra `redund_pace_ms =
  250`): `mix` bajó lo perdido fuera del corte de 0,239 a 0,066 % con 0,42
  bytes en el celular por byte (0,68) y su RTT p95 de 411 a 192 ms, y subió
  el TCP de 10,1 a 17,5 Mbit/s, igual en el corte; con Entel B2, de 0,584 a
  0,061 %. En el laboratorio (`protect`, n = 8) el flujo de 6 Mbit/s pierde
  0,011 % contra 0,280 % y el TCP sube de 6,9 a 16,8 Mbit/s. `parity` pierde
  0,010 % fuera del corte pero en él no ayuda (50 % perdido). En el corte,
  nada de lo que cabe iguala a las copias completas detrás de segundos de
  cola (UDP: 26 % con `mix` frente a 19 % con 250). Por eso, apagada por
  omisión.
  - **`redund_spare_ms` (50; 1–2000):** la cola a la que sostiene cada
    enlace lo que lleva. 150 no mejoró nada medible (historia 020).
  - **`redund_spare_first` (vacío; el nombre de un `[link NAME]`):** fija el
    primer enlace (p. ej. un celular a la vista de su torre) mientras esté
    vivo, medido y sin ahogarse; si se ahoga queda vetado 60 s y manda la
    elección automática; vuelve cuando se despeja. Un nombre sin su sección
    `[link]` es un error de configuración.
  - Ahogado es una cola sobre el mínimo de 60 s, o un retardo sin cola sobre
    el del más rápido, de más de 4 × `redund_spare_ms`: con una carga larga
    el mínimo sube con la cola, y la base lo delata. Un primero ahogado se
    veta aunque esté perdiendo (antes su pérdida lo retenía como en un
    microcorte).
  - Estado: `upload.spare` del cliente (`mode`, `now`, `k`, `first`,
    `pinned`, `data_mbit`, `copies`, `copies_skipped`, `fed`, `parities`,
    `parities_skipped`, `parities_no_link`) y `spare` por enlace (`mbit`,
    `cuts`, `taken`, `skipped`); lo rehecho, en `fec` de la sesión del
    servidor.
- **`reorder_pool_kb` (8192; 256–131070):** memoria para los paquetes
  retenidos, en bloques de 2 KiB, reservada al arrancar; solo ocupa RAM lo
  que llega a usarse. En el servidor la comparten todas las sesiones (cada
  una suma además unos 322 KiB de ventana); una sesión que no encuentra
  búfer entrega al instante y lo cuenta (`pool_full`). Es lo único de esta
  sección que reinicia el proceso al cambiarlo; el resto se aplica en
  caliente, y apagar `reorder` entrega en orden lo que estaba retenido.
- **Sin bloquear:** el plazo del hueco más antiguo es el tiempo de espera
  del bucle (no el tic de 100 ms), y una pasada entrega como mucho 512
  retenidos (la siguiente viene enseguida), para no desbordar la cola de
  descifrado de WireGuard (1024 por par). Un desborde de la ventana (16384
  secuencias, `CG_RO_SLOTS`) se entrega entero.
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
- **`capacity_sat_ms` y `rescue_target_ms` (`0`: lo que diga
  `governor_target_ms`; de 0 a 1000), en los dos extremos (auditoría C12):**
  `governor_target_ms` movía cuatro lazos a la vez. `capacity_sat_ms` es la
  cola que dice que un enlace está lleno para la capacidad (`capacity.h`:
  los cortes, la sonda bajo la mitad, `cake`, el ETA); `rescue_target_ms`,
  donde empieza a bajar el peso del rescate (lleno a 4 veces). Sin ellas,
  todo sigue como antes; sirven para afinar cada lazo por separado (la
  búsqueda de presets).
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
  mientras su cola está bajo la mitad de `governor_target_ms` (el doble si
  está bajo la mitad de su mejor capacidad de los últimos 5 min: el arranque
  rápido, abajo), para que muestre más si puede (el sondeo de BBR). Un enlace aún sin medida lleva
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
- **`split_eta` (`no`; `yes` o `no`), en los dos extremos, para lo que cada
  uno envía (experimental):** cada paquete repartido va por el enlace donde
  llegaría antes (`eta.h`, historia 013, y el eje 5 del estudio de la
  historia 012): su retardo sin cola (el mínimo del suyo sobre el menor de
  los enlaces), más lo que tiene encolado y el paquete sobre su capacidad
  (`capacity_mbit`). Lo encolado es una cola virtual que se vacía a la
  capacidad entre paquete y paquete y se vuelve a fijar cada 100 ms con la
  cola que mide el enlace. Así los paquetes enviados en orden llegan casi en
  orden, y un enlace más lento recibe tráfico solo cuando la cola de los
  rápidos haría llegar un paquete más tarde que por él. Mientras un enlace
  no tiene capacidad medida, reparte por los pesos. El porqué: por pesos,
  los paquetes salían por enlaces con decenas de ms de diferencia, el
  reordenador soltaba huecos y el 72 % de las retransmisiones de TCP de la
  bajada eran espurias (DSACK). Apaga las copias por omisión. El estado
  muestra `split_eta` en `upload` (cliente) y en cada sesión (servidor).
  - **`split_tickle_ms` (0, nunca; hasta 60000) y `split_stale_max` (1,
    apagado; 1–64), experimentales:** un enlace que lleva 3 s sin cargarse
    (sin recibir 3/4 de su capacidad) no muestra si da más, y el reparto
    por llegada no le manda nada mientras los otros tengan cola corta.
    Con `split_tickle_ms`, cada ese tiempo uno de esos enlaces recibe los
    paquetes repartidos durante 600 ms a la tasa de sondeo (5/4 de su
    capacidad, `tickle.h`); si su capacidad no creció, espera el doble la
    vez siguiente. Con `split_stale_max`,
    su capacidad cuenta para el reparto el doble cada 3 s sin carga, hasta
    ese múltiplo y la del mayor enlace (`cg_cap_stale`). El estado muestra
    `tickle` y `split_by.tickle`.
- **`pacing` (`no`; `yes` o `no`), en el cliente (experimental):** el
  socket de cada enlace se dosifica (`SO_MAX_PACING_RATE`, `pace.h`,
  historia 013) al doble de su capacidad y nunca bajo 20 Mbit/s: rompe las
  ráfagas a la velocidad de la red local sin frenar el enlace (a 5/4 de la
  capacidad se hundía: tras un reinicio la capacidad es casi nada, las
  sondas van por el mismo socket y la capacidad medida tras la dosificación
  se quedaba ahí). Sin capacidad medida, sin límite. El
  motor no guarda colas: las guarda el kernel, por socket, en el qdisc `fq`,
  que tiene que estar por donde salen los enlaces (en OpenWrt, el
  dispositivo bajo las subidas: `tc qdisc replace dev eth0 root fq`); sin
  `fq` el ritmo se fija y nada dosifica. El porqué: un lote sale por un
  enlace de una vez, a la velocidad de la red local, espera en la cola del
  módem y el retardo de ese enlace salta decenas de ms mientras se vacía;
  los demás enlaces llegan entonces fuera de orden. El estado muestra
  `pace_mbit` por enlace y `pacing` en `upload`.
- **`weight` en `[link X]` (cliente; 1, de 0 a 1000):** la parte del reparto
  que lleva ese enlace con `split = on`, en la subida y en la bajada (el
  cliente se los manda al servidor en las sondas; `force` no los mira). Con
  0, el enlace lleva copia de todos los paquetes, repartidos o no. Una
  interfaz sin sección `[link]` pesa 1.
- **`type` en `[link X]` (cliente; `auto`; `auto`, `cellular`, `starlink`,
  `fiber`, `ethernet` o `radio`):** qué clase de enlace es (`linkmodel.h`,
  historia 015). Cada tipo tiene un preset de lo que los lazos de capacidad
  y de reparto hacen con ese enlace, en la subida; el cliente manda el tipo
  de cada enlace al servidor en las sondas (bits 20–22 de la cola de
  bonding, reservados hasta ahora: sin versión nueva de protocolo), y el
  servidor aplica el mismo preset a la bajada de ese camino. `auto` es el
  motor de antes. Hoy solo cambia `starlink`, y solo con `split_eta`: mientras
  pierde más del 0,3 % de lo que se le manda (`lossy`), no se sondea a 5/4
  y se retiene en la cola más alta que mostró en 10 s; lo que pase de ahí
  va por otro enlace mientras llegue por él a menos de 40 ms de lo que
  tardaría por Starlink (y si no, por Starlink, donde TCP ve la pérdida).
  En el laboratorio (Starlink de cola corta y dos celulares más lentos,
  n = 8; *medido en laboratorio*), el reparto pasó de 41,6 a 56,8 Mbit/s de
  bajada y de 40,3 a 51,1 de subida, sin costo con retardos iguales ni con
  cola profunda. En redundancia, un enlace `starlink` hace que los demás
  enlaces de su sesión lleven una copia solo si llegaría a menos de 60 ms
  de la primera (`pace_peers_ms`): con los celulares de cola de 300 ms, el
  p99 del ping de 1200 bytes en `starsat` bajó de ~360 a ~95 ms, con el
  caudal a ±5 % en todos los casos con Starlink (*medido en laboratorio*,
  historia 015); una sesión sin Starlink no cambia. En bonding, un enlace
  `starlink` que entrega al menos 2,5 veces lo que pueden juntos los
  enlaces más lentos que él (`dominant.h`; 2 ms o más de retardo base por
  encima) hace que el reparto se haga a un lado: cada paquete va como en
  redundancia, con las copias a tiempo de arriba, hasta que entrega menos
  de 2 veces eso. Con Starlink a 400 Mbit/s y dos celulares de 25 y 18, el
  bonding pasó de 135 a 241 Mbit/s de bajada (n = 16, p < 0,001; Starlink
  solo, 294; redundancia, 275; *medido en laboratorio*, caso `dominant`,
  historia 015); a 200, de 101 a 155; a 100 no entra. `cellular`, `fiber`,
  `ethernet` y `radio` se comportan
  como `auto` hasta que una medida diga otra cosa. El
  estado muestra `type` en cada enlace (cliente) y camino (servidor),
  `lossy` y `lossy_noprobe` en su `cap_diag`, y `dominant` (el enlace, su
  mejor entrega en 10 s y los enlaces que domina) en `upload` del cliente y
  en cada sesión del servidor. Se aplica en caliente.
- **`eta_buffer` (`no`; `yes` o `no`) y `capacity_loss_pct` (0; 0–50), en
  los dos extremos, para lo que cada uno envía (experimentales):** las
  perillas globales de lo que hacen los presets, para todos los enlaces a
  la vez, encima del tipo de cada uno. `eta_buffer`: un enlace que perdió
  más del 0,3 % de lo enviado en los últimos 5 s (`lossy`) no se sondea a
  5/4 y, con `split_eta`, se retiene en la cola más alta que mostró en 10 s
  (al menos 5 ms): lo que pase de ahí va por otro enlace. `capacity_loss_pct`:
  un enlace cargado que pierde más de ese porcentaje en 3 muestras seguidas
  está lleno, aunque su cola no lo diga. Aplicado a todos, `eta_buffer`
  cuesta en enlaces sin Starlink (en `het`, −15 % de subida, n = 4; los
  celulares también quedan `lossy`): para un solo enlace, mejor su `type`.
- **Las correcciones mixtas de la auditoría de relojería (experimentales, en
  los dos extremos, todas 0 = apagadas):** `split_qpred_ms` y
  `split_qpred_mode` (#33), `split_dwell_ms` y `split_dwell_oq_ms` (C3),
  `cap_g4_episode` (C5), `eta_keep` y `eta_keep_ms` (C9),
  `rescue_full_hyst` (C11) y `redund_pace_ms` (una copia va por un enlace
  solo si llegaría a menos de eso de la primera). `split_dominant_pct` (0:
  el del preset, 250 en `starlink`) mueve el umbral del enlace dominante
  solo donde el preset lo tiene; un valor grande (65535) lo apaga. Cada una es un campo del
  modelo de cada enlace (`linkmodel.h`); la perilla global lo fija en todos
  los enlaces, encima del preset de su tipo. `rescue_by_owd` (C10, solo el
  cliente) sigue global. Ninguna entró en un preset salvo lo dicho de
  `starlink` (historia 015, tabla por tipo).
- **Los engranes de Starlink (historia 015, sección 9; los dos extremos):**
  `capacity_seq_lag` (`yes`) cuenta la pérdida de cada muestra contra lo
  enviado antes del paquete más nuevo que el otro extremo dice haber recibido
  (su `rx_top`), atrasado lo que el enlace va detrás del más rápido, y no
  contra lo enviado 250 ms antes del reporte. `starlink_slot_clock` (0: el
  preset, encendido en `starlink`; 1 sí, 2 no) lleva el reloj de las ranuras
  de Starlink (`slot.h`): a los :12, :27, :42 y :57 de cada minuto
  (`starlink_edge_ms`, 12000) cambia de satélite, y lo que se envía de
  `starlink_edge_pre_ms` (100) antes a `starlink_edge_post_ms` (50) después
  no cuenta como pérdida del enlace. Usa la hora del sistema: hace falta NTP
  en el router y el VPS. `starlink_edge_copies` (`no`): en redundancia, fuera
  de esa ventana un paquete grande va solo por Starlink, como dato repartido
  que el otro extremo pide de nuevo si se pierde (`rescue_kb`); dentro, o con
  Starlink sin reportes o perdiendo, va por todos (≈ −65 % de datos en los
  celulares con un stream de 8 Mbit/s, sin pérdida, y el p99 del ping
  grande hasta +30 %). `split_dominant_hold` (0: apagado; 1 sí) mantiene el
  modo dominante mientras Starlink no pierde y lo presume en reposo si su
  historia lo muestra dominante; no ganó en el laboratorio y costó en
  `shallowq`, por eso queda como perilla. El estado dice `edge_samples` y
  `lost_age_ms` en `cap_diag`, y `presumed` y `why` en `dominant`.
- **`capacity_expire_ms` (0, apagado; 0–600000), experimental, en los dos
  extremos:** un enlace fuera del envío (silenciado, mudo o fuera del
  reparto) ese tiempo sin una muestra cargada ve su capacidad a la mitad, y
  otra vez cada tanto, hasta 1/16 de su mejor; cada mitad cuenta como
  muestra llena para el arranque rápido, y al volver lo sube el sondeo al
  doble. Sin ella, la capacidad de un enlace fuera queda congelada en la de
  antes. En la celda virtual el LTE volvió con 0,3–1,3 Mbit/s en vez de
  3,6–21, pero eso costó cuando la señal volvió entera (bonding 20,8 frente
  a 23,0 Mbit/s en los 10 s siguientes) y no ganó cuando volvió débil (los
  cortes de siempre bajan la vieja en 1,5–2,5 s); en el laboratorio, igual
  (*medido*, historia 016). El estado cuenta las mitades en `cap_diag`
  (`expired`) y en las decisiones (`cap_expire`).
- **`capacity_loss_queue_ms` (0, apagado; 0–1000; sugerido 10) y
  `capacity_stand` (0, apagado; 0–2), experimentales, en los dos extremos
  (historia 019):** `capacity_loss_queue_ms` toma como lleno, a cualquier
  carga, un enlace que en dos muestras seguidas pierde al menos el 1 % de
  lo enviado, recibe más de lo que entrega y tiene una cola sobre su mínimo
  de 60 s de al menos ese valor: su capacidad pasa a lo que entrega. Es el
  búfer corto de un operador delante de un TCP: la cola no pasa de
  `capacity_sat_ms` y el enlace nunca está «cargado» frente a una capacidad
  vieja. Con el preset de Starlink (que descarta sin cola y se retiene por
  eso) solo cuenta cargado. `capacity_stand = 1` frena el arranque rápido
  mientras la cola sobre el mínimo de 60 s pasa `capacity_sat_ms` (4
  muestras): una cola que dura más que `governor_window_ms` se vuelve base y
  el arranque rápido devolvía la capacidad a la mitad de la mejor (choque C5
  de la auditoría). `capacity_stand = 2` además deja ir el arranque rápido
  antes de los 10 s si el enlace entrega 9/8 de su capacidad, sin pérdida ni
  cola, dos muestras. En las trazas de ns-3 (LTE, 10 KB de RLC, emisor AIMD)
  la capacidad pasó de +275–510 % de error a −2 % en 1,0–1,4 s, sin cortes
  en pérdida aleatoria sin cola, y la cola permanente de +523 % a +3 %; en
  la celda calibrada la capacidad se corrige antes, pero ni ahí ni en el
  laboratorio cambia el TCP, por eso quedan apagadas (*medido*, historia
  019). El estado cuenta `cut_lossq` y `restore_room` en `cap_diag`.
- **`exp_link_types` (vacío; nombres de tipo):** las perillas globales de
  arriba (y `eta_buffer` y `capacity_loss_pct`) solo para los enlaces de
  esos tipos, para probar una antes de ponerla en un preset; vacío: todos.
  `exp_link_types = cellular` con `redund_pace_ms = 60` en una sesión con
  un Starlink es lo que hace el preset de `starlink`.

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

## Túnel nativo (`tunnel = native`, experimental)

Por omisión (`tunnel = wireguard`) cengarde lleva datagramas de WireGuard:
como va por fuera de WireGuard, no ve los flujos de dentro. Con `tunnel =
native` cada extremo abre un dispositivo TUN propio y lleva paquetes IP tal
cual, con la misma autenticación de siempre (MAC SipHash y anti-replay, con
las claves del secreto de emparejamiento). **No cifra:** quien mire los
enlaces ve el tráfico; solo impide que alguien inyecte o repita paquetes. El
cifrado llegará después, como opción. La razón de esta fase es ver los
paquetes: el reordenador por flujo y la liberación según TCP vendrán
encima (`src/flow.h` ya da la clave de flujo de un paquete y si es TCP, con
pruebas, aunque aún no la usa nadie).

Todo lo demás no cambia: el reparto, la duplicación, el rescate y el
reordenador tratan cada paquete IP como un paquete de datos de WireGuard
(los pequeños, hasta `split_small` y `reorder_bypass`, se duplican y no
esperan, igual que antes). Lo que solo tiene sentido con WireGuard no
corre: la detección de su formato, la vigilancia de handshakes,
`wireguard_poke` y `wireguard_port_file` (este último se rechaza).

Ajustes, en los dos extremos (cambiarlos reinicia el motor):

- `tun`: el nombre del TUN, `cg0` por omisión.
- `tun_addr`: hasta 4 direcciones con su prefijo que cengarde le pone
  (`10.79.0.2/16 fd00:79::2/64`); sin ella, las pone el sistema.
- `tun_mtu`: 1420 por omisión, lo que cabe en un enlace de 1500 por IPv6
  (1500 − 40 − 8 − 24 de cabecera de cengarde = 1428). Si un enlace tiene
  menos, el cliente avisa con la cifra que cabe.
- `tun_txqueuelen`: la cola de transmisión del TUN, en paquetes; `0` (por
  omisión) deja la del kernel (500). El PC de la testnet usa 5000, que
  antes ponía a mano con `ip link set cgpc txqueuelen 5000` después de cada
  arranque. No se recarga en caliente (como `tun_mtu`).

Las rutas por el TUN son del sistema (`ip route add … dev cg0`), como con
cualquier interfaz. El servidor tiene **un solo TUN para todos los
clientes**: un paquete que el kernel mete en él va a la sesión más nueva
del cliente cuyo `tun_route` contiene su destino (el prefijo más largo
gana, como `AllowedIPs` de WireGuard), y el paquete de un cliente entra al
TUN solo si su origen es de ese cliente (si no, cuenta como `spoofed`). Con
un solo cliente, `tun_route` es `0.0.0.0/0 ::/0` por omisión; con
`[client NAME]`, cada sección lleva la suya (obligatoria) y dos clientes no
pueden tener el mismo prefijo. Las rutas se aplican en caliente con una
recarga.

```ini
# router
mode = client
key = …
server = vps.example.com:59402
tunnel = native
tun_addr = 10.79.12.34/16
# y en el sistema, p. ej.: ip route add default dev cg0 table 100

# VPS
mode = server
tunnel = native
tun_addr = 10.79.0.1/16
[client alpha]
key = …
tun_route = 10.79.12.34/32 192.168.1.0/24
```

Requiere `CAP_NET_ADMIN` y `/dev/net/tun`. Estado: `tunnel` y `tun` en los
dos extremos; en el cliente, `download.not_ip` (lo que no era IP y no se
escribió); en el servidor, `tun` (`rx`, `not_ip`, `no_route`,
`no_session`) y por sesión `not_ip` y `spoofed`. En el laboratorio
(`sudo bench/lab.sh native`): ping por IPv4 e IPv6 sin pérdidas, iperf3 TCP
por 2 enlaces de 40 Mbit/s con `split = on` a 73,5 Mbit/s de subida y 73,5
de bajada (más de lo que da un enlace), y dos routers en un servidor, cada
uno con su ruta, sin poder usar la dirección del otro. Sin límite en los
veth, iperf3 pasa unos 2,5 Gbit/s en el PC del laboratorio (sin cifrar).

**`tcp_aware` (`no`; `yes` o `no`), experimental, en el extremo que envía:**
con el túnel nativo, el emisor marca `CG_F_FREE` (protocolo v6) los
segmentos de TCP sin datos (ACK, SYN, FIN, RST) y las retransmisiones de
TCP (`src/tcpfree.h`; en el estado, `tcp_free`: `control` y
`retransmissions`). El reordenador del otro extremo entrega al instante
los segmentos sin datos. Una retransmisión (marcada y con datos) **no** se
adelanta: ocupa su lugar en la secuencia, como cualquier dato. Adelantada
a lo retenido detrás de un hueco, el RACK de TCP daba por perdido todo lo
enviado antes y lo reenviaba: en la red de pruebas, 135 veces las
retransmisiones rápidas y 95 veces los DSACK de `tcp_aware = no`; en el
laboratorio (`sudo bench/lab.sh tcpaware`), retenerla en su lugar las deja
como sin `tcp_aware` (historia 014).

- **`reorder_retx_release_ms` (0, apagado; 0–2000), experimental, en el
  que recibe:** una retransmisión de TCP abandona los huecos anteriores a
  ella cuyo paquete retenido se envió al menos ese tiempo antes que ella
  (`lost.retx`), y sale en orden justo detrás. En el laboratorio no
  conviene: donde el hueco es un paquete atrasado en un enlace lento (una
  cola que salta), el original llega después como duplicado y el
  rendimiento bajó de 45 a 34 Mbit/s (historia 014).

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
  2^20 más adelante) y va unas 32800 secuencias por detrás (la ventana
  anti-replay, `CG_REPLAY_BITS`, es de 32768).
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
  al instante), `late.*`, `far` (un salto de 32768 secuencias o más
  mientras retenía, como el de un servidor que retoma la sesión),
  `resyncs` y `pool_full` (sin búfer libre: al instante); `split_rx` cuenta
  los repartidos, y `retx` las retransmisiones de TCP marcadas por el otro
  extremo (`tcp_aware`), sea cual sea su suerte;
- `lost`: huecos que dejó de esperar, por qué: `rack` (todos los enlaces
  trajeron uno posterior), `cap` (el plazo), `overflow` (la ventana se
  llenó), `flush` (al apagarlo) y `retx` (una retransmisión de TCP, con
  `reorder_retx_release_ms`);
- `late`: los que llegaron después de eso, por la regla que soltó su hueco
  (`after_rack`, `after_cap`, `after_overflow`, `after_retx`; `old`, si su
  casilla ya era de otro). `after_rack` > 0 dice que un enlace no entrega en orden o que se
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
322 KiB (en el servidor, por sesión: ~20 MiB con 64, *cálculo*), y el pool,
solo lo que llega a retener. La ventana del reordenador (16384) y la
anti-replay (32768) cubren ~600 ms a ~27.000 paquetes por segundo; con las
anteriores (4096 y 8192) un paquete 300 ms tarde ya se rechazaba como viejo
y el desborde soltaba huecos a los ~150 ms (historia 013, PR #46).

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
(o entregar menos de la mitad de lo enviado durante dos muestras seguidas:
un enlace atorado deja de responder a tiempo y su retardo medido queda
viejo)
(con `capacity_weights`, que bajara sola dejaba enlaces al mínimo:
historia 013). Una cola cuenta como enlace lleno con 2 muestras seguidas
si se le enviaba al menos 3/4 de su capacidad (`CG_CAP_SAT_N`); con menos
carga, solo con 4 seguidas (`CG_CAP_SAT_LONG`): un enlace celular se
detiene 100–600 ms de vez en cuando, con cualquier carga, y esa cola es un
tropiezo de la radio, no el límite del enlace (historia 013, PR #57).
**Arranque rápido** (PR #56): cada enlace recuerda su mejor capacidad de
los últimos 5 min; tras 10 s sin llenarse vuelve al menos a la mitad de
ella, y mientras está bajo esa mitad el reparto le ofrece el doble de su
capacidad, en vez de 5/4 (el arranque de BBR). Así un enlace que quedó en
reposo con una cifra baja no tarda segundos en volver a recibir su parte;
si de verdad lleva menos, su cola lo dice al instante. Con
`capacity_weights`, de
ella salen los pesos del reparto (arriba); sin, solo se informa. El estado
trae además, por enlace, `queue_slope_ms_s` (la pendiente de su cola, `null`
sin medida) y `rescue_weight` (arriba).
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
`client_weight`, el peso que manda el cliente, y `type`, el tipo que dice
para ese camino (`auto` sin él; en el cliente, `type` de cada enlace es el
de su `[link]`). «Más nueva» es por su número
de secuencia de control, no por la hora de llegada: tras una recarga, las
sondas o respuestas que salieron antes por un enlace lento llegan después de
las nuevas por uno rápido, y no deshacen lo que estas dijeron.

**La relojería** (auditoría del 2026-10-17; `decision.h`, `decrec.h`): en
`upload` (cliente) y en cada sesión (servidor), `decisions` son las últimas
64 decisiones del sentido en que envía ese extremo, la más nueva primero:
`t_ms` (reloj monótono del motor), `age_ms`, `link` (`null`: el sentido
entero), `mech`, `from` y `to` (el estado o valor antes y después) y un
motivo con su unidad como clave (`behind_us`, `queue_us`, `silent_ms`,
`credit_bytes`, `queue_leaves`, `recoveries`). Los mecanismos: G1 (`mute`,
`unmute`, `unmute_forced`; estados 0 activo, 1 silenciado), G3 (`stale`,
`fresh`), G4 (`leave_queue`, `leave_stall`, `leave_base`: salió sin haber
estado dentro; `rejoin_settle`, `rejoin_recover`, `rejoin_empty`,
`rejoin_off`; estados como `g4.state`, en número: 0 `out`, 1 `waiting`,
2 `in`, 3 `left`, 4 `holding`, 5 `qleft`), `stress_on/off` y
`recover_on/off`, `copies_dry/ok` (el presupuesto de copias de `cg_sdup` se
agotó bajo estrés; también `copies_dry`, el estado actual), la capacidad
(`cut_full`, `cut_short`, `restore_rest`, el arranque rápido; en bps), el
gobernador (`gov_down`, `gov_up`: su peso cruza la mitad del máximo) y el
rescate (`rescue_full_on/off`: su presupuesto bajo, `CG_BOND_FULL`). Solo se
anota cada transición, en los ticks, nunca por paquete; `decision_totals`
cuenta cada mecanismo desde el arranque. Junto a ellas: `weights_used`
(`eta` si el reparto por llegada tiene ritmo para todos los enlaces del
reparto, y entonces `gov_weight` y `cap_weight` no se usan; si no,
`capacity`, `governor` o `links`; `null` sin reparto), `lead` (el enlace
más rápido dentro de G4) y `gov_qmin_ms` (la cola más corta del
gobernador). Por enlace: `g4` (`state`: `in`, `left`, `holding`, `qleft`,
`waiting` u `out`; `last`, la última transición, y `age_ms`; y los
contadores `leave_queue`, `leave_stall`, `leave_base`, `rejoin_settle`,
`rejoin_recover`, `rejoin_empty`, `rejoin_off`), `owd_age_ms` (desde el
último informe de retardo) y `owd_reports_per_s`, `gov_ticks` (`down`,
`up`, `hold`; `null` sin gobernador) y `cap_diag` (`sat_n`, `sat_load_n`,
`short_n`, `lossy` y `lossy_noprobe` (perdió lo bastante hace poco, y si
por eso no se sondea; solo con un tipo o `eta_buffer` que lo siga), `cut`,
`full_age_ms`, `best_mbit`, `ql_ms`, la cola sobre el
mínimo de un minuto, y los contadores `cut_full`, `cut_short`, `cut_loss`,
`restore_rest`).

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

## Protocolo v5

Historia 013, PR #47 (el enlace anterior) y #48 (el NACK). La cabecera sigue midiendo 24
bytes, y `cengarde-vps-setup` escribe las mismas configuraciones que con el
v4. Cliente y servidor tienen que ser v5: un motor descarta lo que trae otra
versión.

- **El enlace anterior en DATA (`CG_F_PREV`, 0x08):** cuando la secuencia
  anterior salió por un solo enlace, el paquete dice cuál (su id, en los 4
  bits altos de las banderas). Es lo que usa `reorder_by_link`; se envía
  siempre, y bajo el MAC como toda bandera.
- **NACK (`CG_T_NACK`, tipo 5):** en cualquier sentido, por un enlace; pide
  que se envíen otra vez secuencias de DATA (el rescate). Cuenta en la
  secuencia de control. La carga es un número k de rangos (1 a 16) y k
  rangos de 12 bytes: una secuencia base y una máscara de 64 bits cuyo bit i
  pide la base + 1 + i; con 16 rangos, un NACK pide hasta 1040 secuencias.
  Banderas: `CG_F_RETRY` (0x01) dice que ya se pidieron y no llegaron
  (quien reenvía las manda por dos caminos) y `CG_F_GONE` (0x02) es la
  respuesta de quien no las enviará (ver el rescate).
- **Las sondas anuncian el rescate:** en la cola de bonding, el bit 18
  (`CG_BOND_RESCUE`: guardo lo que envío y lo reenvío si me lo piden) y el
  19 (`CG_BOND_FULL`: no lo de esta ruta ahora, porque está llena o el
  presupuesto se agotó). Los demás bits siguen reservados.
- **Ventanas:** el reordenador y la anti-replay son más grandes (16384 y
  32768 secuencias; ver el estado del reordenador), y no son parte del
  formato.

## Limitaciones conocidas

- **Puerto de WireGuard en el cliente:** el cliente lo aprende del primer
  paquete que WireGuard le envía, como engarde.
- **Enlaces asimétricos:** un enlace que sube pero no baja (o al revés) se da
  por mudo en los dos sentidos, aunque uno de ellos funcione.
- **Aún no** baja privilegios y el servidor es de un solo hilo; en el
  cliente, con `link_threads = on`, la subida la sigue enviando el hilo
  principal (los hilos de enlace la enviarán con el PR 3c).
