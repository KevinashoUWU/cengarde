# 002 — WireGuard: lo que cengarde necesita saber

- **Fecha:** 2026-10-03
- **Estado:** vigente
- **Fuentes:** wireguard-go `golang.zx2c4.com/wireguard@v0.0.0-20260522210424-ecfc5a8d5446`
  (verificado en `device/noise-protocol.go`, `device/constants.go`,
  `device/send.go`, `device/receive.go`, `device/cookie.go` y
  `replay/replay.go`). Lo del kernel Linux está marcado como no verificado.

## TL;DR

- engarde ve datagramas WireGuard opacos, pero su cabecera va en claro:
  - tipo (1 byte + 3 a cero);
  - índice receptor (offset 4);
  - contador (offset 8, u64 little-endian);
  - en los de datos, el tag Poly1305 son los últimos 16 B.
- Clave de deduplicación segura: (índice receptor, contador, tag), es decir,
  solo duplicados exactos.
- WireGuard descifra antes de comprobar el anti-replay, así que hoy descifra
  las N copias de cada paquete.
- Sesiones: la iniciación lleva el índice del cliente, la respuesta lleva los
  dos, y cada mensaje de datos lleva el índice de quien lo recibe.
- Tamaños fijos (148 / 92 / 64 / ≥32 B): validar el formato cuesta muy poco.
- Mensaje de datos = paquete interior rellenado a múltiplo de 16 (sin pasar
  del MTU) + 32 B.
- **El reloj del router importa:** quien responde un handshake ignora una
  iniciación cuyo timestamp no sea más nuevo que el último que aceptó de ese
  peer. Una Pi (sin reloj con batería) que se reinicia vuelve con la hora
  atrasada, y el VPS la ignora; con todo por el túnel, el NTP no puede
  corregirla. Desde la 0.4.4, el servidor nota las iniciaciones sin
  respuesta y hace que el WireGuard del VPS inicie él (`wgwatch.h`).

## Hallazgos

### Formato de los mensajes (verificado)

| Mensaje | Tipo | Tamaño | Campos (offset en bytes) |
| --- | ---: | ---: | --- |
| Iniciación | 1 | 148 | sender (4), efímera (8, 32 B), estática cifrada (40, 48 B), timestamp cifrado (88, 28 B), mac1 (116), mac2 (132) |
| Respuesta | 2 | 92 | sender (4), receiver (8), efímera (12, 32 B), vacío cifrado (44, 16 B), mac1 (60), mac2 (76) |
| Cookie reply | 3 | 64 | receiver (4), nonce (8, 24 B), cookie cifrada (32, 32 B) |
| Datos | 4 | 16 + n + 16 | receiver (4), contador (8, u64 LE), contenido cifrado (16), tag (últimos 16 B) |

El tipo se escribe como un u32 little-endian: el primer byte vale 1–4 y los
tres siguientes son 0. Un keepalive es un mensaje de datos sin carga: 32 B
(`MessageKeepaliveSize`).

### Constantes (verificado, `device/constants.go`)

| Constante | Valor | Constante | Valor |
| --- | --- | --- | --- |
| RekeyAfterTime | 120 s | RejectAfterTime | 180 s |
| RekeyAttemptTime | 90 s | RekeyTimeout | 5 s |
| KeepaliveTimeout | 10 s | CookieRefreshTime | 120 s |
| HandshakeInitationRate | 1/50 s | PaddingMultiple | 16 |
| RekeyAfterMessages | 2^60 | RejectAfterMessages | 2^64 − 2^13 − 1 |

La ventana anti-replay (`replay/replay.go`) es de (128 − 1) × 64 = 8.128
contadores.

### Comportamientos que importan (verificado)

- **Descifrar antes del anti-replay:** `receive.go:255` (`Open`) va antes que
  `receive.go:453` (`ValidateCounter`), así que cada duplicado se descifra y
  autentica entero antes de descartarse.
- **Roaming:** el endpoint del peer pasa a ser el origen del último paquete
  autenticado y no repetido (`receive.go:459`). Detrás de engarde, ese origen
  es el socket local de engarde, que es estable.
- **Relleno:** `calculatePaddingSize` (`send.go:419-431`) redondea a múltiplo
  de 16 sin pasar del MTU. Un mensaje de datos ocupa como mucho MTU + 32, así
  que con el buffer de 1500 B de engarde Go se trunca si el MTU de WireGuard
  pasa de 1468.
- **Contador:** u64 little-endian (`send.go:457`).
- **mac1:** BLAKE2s-128 con clave = BLAKE2s-256(`"mac1----"` ‖ clave pública
  del que responde) (`cookie.go:50-54`, `noise-protocol.go:52`). Un
  engarde-server que conozca la clave pública del WireGuard del servidor (un
  dato público) puede verificar el `mac1` de las iniciaciones sin ningún
  secreto.

### El timestamp de la iniciación y el reloj del router (medido en QEMU, 2026-10-05)

- **La regla:** la iniciación lleva cifrado el reloj de pared de quien la
  envía (TAI64N). Quien la recibe la descarta si no es más nueva que la
  última que aceptó de ese peer (`noise-protocol.go:407`, verificado). El
  kernel hace lo mismo: el WireGuard del «VPS» de la prueba en QEMU
  (OpenWrt 25.12.5) lo cumple. Ese último timestamp dura
  lo que dure el peer en la interfaz: no lo borra el fin de la sesión, solo
  quitar el peer o recrear la interfaz.
- **El reloj de una Pi:** sin batería, OpenWrt arranca con la fecha del
  archivo más nuevo de `/etc` (`sysfixtime`). Recién flasheada, es la de la
  imagen (`SOURCE_DATE_EPOCH`: 2026-06-29 12:59 UTC en la 25.12.5 r33051).
  Después de un reinicio, suele ser la del último cambio de configuración.
- **Lo que vio el usuario en su Pi:** «sin handshake desde 2344 h». Es el
  handshake del primer arranque, hecho con la hora de la imagen. Luego el
  NTP (por el túnel) puso la fecha real y la página de estado restó: 2344 h
  después del 29 de junio a las 12:59 es el 5 de octubre entre las 04:59 y
  las 05:59 UTC, justo cuando probaba. Se corrige en el siguiente handshake.
- **El bloqueo:**
  - **Prueba:** reloj del router 30 min o 1 h atrás y su WireGuard
    reiniciado, como tras un reinicio.
  - **Resultado:** el VPS ignoró 14 iniciaciones en 60 s, 43 en 80 s y
    52 en 4,5 min, sin handshake. La ruta por defecto del router va por el
    túnel (`0.0.0.0/1` y `128.0.0.0/1`), así que el NTP no puede corregir la
    hora. El túnel queda caído hasta que el reloj del router alcanza el
    último timestamp que vio el VPS: horas, o meses si se flasheó.
- **Lo que lo destraba:** que el WireGuard del VPS inicie él. El router
  acepta su iniciación sea cual sea su propia hora. El VPS lo hace cuando
  tiene algo que mandar y no tiene sesión, o cuando lo que mandó lleva 15 s
  sin respuesta (`timers.go:138-141`, KeepaliveTimeout + RekeyTimeout,
  verificado). En la primera prueba, el VPS acababa de contestar pings y
  el túnel volvió solo a los ~17 s; sin tráfico hacia el router no vuelve.
  Con IP pass, cualquier conexión desde Internet hacia el router produce
  ese tráfico.

### Sin verificar (recuerdo; comprobar antes de depender de ello)

- WireGuard del kernel Linux: ventana anti-replay de ~8.128 en 64 bits
  (`COUNTER_BITS_TOTAL` 8192) y también descifra antes de validar el contador.
- El kernel solo copia ECN, no DSCP, al paquete exterior. Si es así, engarde
  no puede priorizar por DSCP, solo por tamaño.
- El kernel recibe con `encap_rcv` sobre su socket UDP, sin buffer de socket,
  y encola el descifrado en workers por CPU.

## Qué hacemos con esto

- **Validación barata** de tipo y tamaño para descartar basura y para admitir
  caminos en el servidor, más `mac1` opcional si se configura la clave
  pública.
- **Deduplicación:** anillo indexado por (índice receptor, contador); en cada
  hueco se guardan 8 B del tag. Mismo contador con distinto tag = paquete
  distinto (posible falsificación): se reenvía y que WireGuard decida.
- **Sesiones en el servidor, sin cambiar el protocolo:**
  - la iniciación lleva el índice del cliente;
  - la respuesta lleva el índice del servidor y el del cliente;
  - los datos de subida llevan el índice del servidor y los de bajada el del
    cliente.

  Hay rekey cada ≤120 s, y los índices viejos se mantienen hasta
  RejectAfterTime (180 s).
- **Reordenación** (bonding, Fase 5): el contador es secuencial por par de
  claves, así que se puede reordenar por (índice receptor, contador) sin
  cabecera propia.
- **MTU:** buffers de 64 KiB, y avisar cuando el MTU de camino de un enlace
  sea menor que el MTU de WireGuard + 32 + IP/UDP.
- **Reloj del router (0.4.4, solo el servidor, sin cambio de protocolo):**
  - **Detección (`wgwatch.h`):** el servidor cuenta las iniciaciones del
    cliente que WireGuard no contesta.
  - **Empujón:** tras 3 sin respuesta (unos 10 s de reintentos), manda un
    datagrama a la dirección del router dentro del túnel (`wireguard_poke`,
    `10.79.0.2:9` por omisión) para que WireGuard inicie el handshake él
    mismo. Como mucho uno cada 15 s, y solo mientras el cliente siga
    llamando.
  - **Redirección:** WireGuard manda su iniciación al endpoint que conoce,
    que tras un reinicio del router es la sesión vieja; esa iniciación va
    por la sesión nueva mientras la nueva llame en vano y la vieja haya
    callado antes.
  - **Puerto:** WireGuard conoce al cliente por el puerto del socket de su
    sesión. Ese puerto pasa a la sesión nueva cuando la vieja se cierra
    mientras la nueva llama en vano, o cuando la nueva empieza sin otras
    vivas. Así se destraba también un router que tarda en volver más que
    `session_timeout_ms`, o uno al que WireGuard contesta tarde (con su
    sesión aún válida, espera 15 s).
  - **Router:** la página de estado ya no muestra una edad absurda cuando
    el handshake es anterior al ajuste de la hora.
  - **Laboratorio (`bench/lab.sh wgpoke`, con `bench/fakewg.py`):** la
    iniciación del VPS llega al router tras 3 iniciaciones ignoradas:
    - por la sesión nueva (redirección);
    - por el puerto heredado si WireGuard contesta 7 s tarde, con la sesión
      vieja ya expirada (`session_timeout_ms` 5 s);
    - tras un corte de 8 s, por el puerto retomado.

    El motor anterior falla todo eso; sin el traspaso del puerto, falla el
    caso tardío.
  - **QEMU, con WireGuard de verdad** (el motor de la 0.4.4 en las dos VMs,
    reloj del router 1 h atrás, 30 s sin tráfico antes de cada reinicio):

    | Corte | Camino | Túnel de vuelta |
    | --- | --- | --- |
    | 2 s, `session_timeout_ms` por omisión | empujón a los 11 s; WireGuard inicia 15 s después hacia la sesión vieja y va por la nueva | 36 s |
    | 2 s, `session_timeout_ms` 15 s | la sesión vieja cierra mientras la nueva llama: traspaso del puerto, y empujón | 38 s |
    | 25 s, `session_timeout_ms` 15 s | la sesión nueva empieza sola con el puerto retomado, y empujón | 35 s |

    Siempre con un handshake nuevo en `wgcg` y 10 de 10 pings por él. Con
    tráfico justo antes del reinicio, WireGuard inicia solo y basta la
    redirección o el puerto (13 s y 11 s).
  - **Ojo al medir:** `10.79.0.1` también contesta por un enlace, sin
    túnel, mientras `wgcg` no está arriba. Un ping que responde no prueba el
    túnel: hace falta un handshake nuevo en `wgcg` y un ping con `-I wgcg`.

## Pendiente

- Verificar los tres puntos del kernel antes de diseñar en función de ellos.
- **Reloj del router, lo que el arreglo no cubre:**
  - **Reinicio del motor del VPS** mientras el router está caído: el
    servidor olvida el puerto que WireGuard conoce y el empujón no llega.
    Reiniciar la interfaz WireGuard del VPS lo destraba.
  - **Protocolo v4 (PR 3d2):** la sesión activa por cliente, con un socket
    por cliente hacia WireGuard, hace innecesarias la redirección y el
    reuso del puerto. El empujón sigue sirviendo.

## Cambios

- 2026-10-03: creada; datos de wireguard-go verificados en el código.
- 2026-10-05: el timestamp de la iniciación frente al reloj de un router sin
  batería (el «2344 h» de la Pi y el bloqueo, medidos en QEMU), y el arreglo
  del servidor en la 0.4.4.
