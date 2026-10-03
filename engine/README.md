# Motor cengarde (C)

Reescritura en C de engarde: duplica cada datagrama de WireGuard por todos los
enlaces del cliente hacia el servidor, y en el otro extremo entrega a
WireGuard solo la primera copia. Cliente y servidor son el mismo binario; el
modo lo decide la configuración. El diseño y sus razones están en
[`docs/historias/005-motor-c-v1.md`](../docs/historias/005-motor-c-v1.md) y
la salud de los enlaces en
[`docs/historias/006-salud-de-enlaces.md`](../docs/historias/006-salud-de-enlaces.md).

**No es compatible en el cable con engarde Go:** usa su propia cabecera
autenticada (protocolo v2), así que los dos extremos tienen que ser cengarde
de la misma versión de protocolo.

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

En OpenWrt hay paquete y una guía completa del router y del VPS (enlaces,
WireGuard, reenvío de puertos): [`openwrt/README.md`](../openwrt/README.md).
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
   y los enlaces: patrones en `interfaces` o secciones `[link NOMBRE]`.

4. **WireGuard del cliente:** el `Endpoint` del peer pasa a ser la dirección
   `listen` del cliente (`127.0.0.1:59401`). El servidor WireGuard no cambia.

5. **MTU de WireGuard:** cengarde añade 24 bytes por paquete. Con una ruta de
   1500 bytes el máximo es 1416 sobre IPv4 y 1396 sobre IPv6
   (1500 − IP − 8 UDP − 24 cengarde − 32 WireGuard). En redes móviles conviene
   ir más bajo, por ejemplo `MTU = 1380`, o menos si algún operador tiene un
   MTU de camino menor.

## Qué hace cada extremo

- **Cliente:**
  - abre un socket por enlace (`SO_BINDTODEVICE` + bind a la IP del enlace);
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
  - reparte la bajada por todos los caminos activos.

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

## Estado

`status_file` escribe cada segundo, desde un hilo aparte para que un disco
lento no frene el túnel, un JSON con contadores globales y, por enlace:
- estado (`live`, `stalled` o `down`) y RTT;
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

## Limitaciones conocidas

- **Servidor con varias IPs públicas:** responde desde la IP que elija el
  kernel. Usa `listen = <IP pública>:59402`.
- **Puerto de WireGuard en el cliente:** el cliente lo aprende del primer
  paquete que WireGuard le envía, como engarde.
- **Enlaces asimétricos:** un enlace que sube pero no baja (o al revés) se da
  por mudo en los dos sentidos, aunque uno de ellos funcione.
- **Aún no** baja privilegios, no recarga la configuración y el servidor es de
  un solo hilo.
