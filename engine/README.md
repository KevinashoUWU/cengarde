# Motor cengarde (C)

Reescritura en C de engarde: duplica cada datagrama de WireGuard por todos los
enlaces del cliente hacia el servidor, y en el otro extremo entrega a
WireGuard solo la primera copia. Cliente y servidor son el mismo binario; el
modo lo decide la configuración. El diseño y sus razones están en
[`docs/historias/005-motor-c-v1.md`](../docs/historias/005-motor-c-v1.md) y
la salud de los enlaces en
[`docs/historias/006-salud-de-enlaces.md`](../docs/historias/006-salud-de-enlaces.md).

**No es compatible en el cable con engarde Go:** usa su propia cabecera
autenticada (protocolo v3), así que los dos extremos tienen que ser cengarde
de la misma versión de protocolo. `cengarde version` la muestra al final, por
ejemplo `cengarde 0.4.1-r1-g<commit> (protocol 3)` en OpenWrt.

## Compilar

```sh
make -C engine            # binario engine/cengarde
make -C engine test       # tests unitarios
make -C engine SANITIZE=1 test   # con ASan y UBSan
```

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
  que los scripts que hacen `eval` de la salida siguen igual. Hoy el túnel
  sigue usando 10.79.0.2/30; las direcciones derivadas son para varios
  routers por VPS.
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
  - crea una sesión por cliente solo a partir de un paquete autenticado;
  - cada sesión tiene su propio socket hacia WireGuard, así que varios
    clientes comparten un puerto;
  - aprende los caminos (sesión + enlace) solo de paquetes autenticados,
    incluidos los cambios de NAT;
  - responde por cada camino desde la dirección a la que envía el cliente
    (con un `listen` comodín, `*` o `0.0.0.0`): sirve cualquier dirección
    del VPS, también una añadida en marcha, como una IP reservada o IPv6;
  - reparte la bajada por todos los caminos activos.

## Varias direcciones del servidor

`server` es una lista ordenada de hasta 8 entradas, global o por
`[link NOMBRE]`: IPv4, `[IPv6]` o nombres. El orden es la preferencia. Un
nombre vale por sus direcciones (como mucho 4 de cada familia) y se resuelve
al leer el archivo.

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
- `mute_behind_ms = 0` desactiva el silenciado y deja redundancia pura.

En el laboratorio, un enlace con 500 ms de cola se silencia en 3–3,6 s sin
perder un paquete del túnel y vuelve, sin recaer, cuando se le quita
(`sudo bench/lab.sh health`).

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

## Cambiar la configuración en marcha

`SIGHUP` (o `cengarde ctl reload`) vuelve a leer el archivo sin cortar el
túnel:

- **En el lugar, con la misma sesión:** enlaces (`interfaces`, `exclude`,
  `[link]`), direcciones del servidor y `server_failover_ms`, etiquetas,
  salud de los enlaces, sondas, buffers, `log_level`, `status_file`,
  `description`, IP pass y, en el servidor, los tiempos de espera. Un enlace
  empieza otra vez por la primera dirección del servidor solo si cambiaron
  sus propias entradas (las de su familia: añadidas, quitadas, editadas o
  movidas); una entrada de otra familia, o un nombre que vuelve a
  resolverse con las mismas direcciones en otro orden, no mueve un enlace
  que funciona. En el laboratorio, dos recargas y la pausa de un enlace a
  2000 pps no perdieron ningún paquete (`sudo bench/lab.sh control`).
- **Reiniciando el proceso en el lugar** (mismo PID, sesión nueva): `mode`,
  `key`, `listen`, `control_socket`, `busy_poll_us`, `cpu`, `rt_priority` y,
  en el servidor, `wireguard` y `max_sessions`.
- **Si el archivo tiene un error,** sigue con la configuración anterior,
  lo registra y lo publica en el estado (`config_error`).
- **Sin bloquear el túnel:** la lectura va en un hilo aparte, porque un
  servidor dado por nombre espera al DNS.
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
  (`LOCAL`).

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

## Estado

`status_file` escribe cada segundo, desde un hilo aparte para que un disco
lento no frene el túnel, un JSON con contadores globales y, por enlace:
- estado (`live`, `waiting` antes de la primera respuesta, `stalled`, `down`
  o `paused`), por qué no lleva el túnel (`reason`, vacío si lo lleva), si
  está pausado o forzado a mano (`override`) y RTT;
- en el cliente, hacia dónde: `local`, `remote`, `family` (`ipv4` o `ipv6`),
  `candidate` (su lugar entre las direcciones del servidor de su familia: 0
  es la primera; más, que hizo failover), `candidates`, `failovers` y
  `path_mtu` (el MTU de camino de su socket, 0 si no se sabe);
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

Además, `config_error` (por qué no se aplicó la última recarga) y el IP pass:
`passthrough.requested` y `passthrough.server` en el cliente, y
`passthrough` en el servidor.

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

En el cliente, `upload.largest` es el datagrama de WireGuard más grande de
los últimos 5 s, el que se compara con cada `path_mtu`, y
`download.window_resets` cuenta las veces que el servidor
empezó de cero (se reinició con una secuencia por detrás de la ventana
anti-replay) y el cliente rehízo su ventana para seguir recibiendo; cada vez
lo registra como "server started over".

## Limitaciones conocidas

- **Puerto de WireGuard en el cliente:** el cliente lo aprende del primer
  paquete que WireGuard le envía, como engarde.
- **Enlaces asimétricos:** un enlace que sube pero no baja (o al revés) se da
  por mudo en los dos sentidos, aunque uno de ellos funcione.
- **Aún no** baja privilegios y el servidor es de un solo hilo.
