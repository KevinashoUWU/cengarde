# 025 — PEP modo B, el «protocontrolador»: TCP partido con el protocolo de cengarde en el medio

- **Fecha:** 2026-10-25
- **Estado:** en curso (diseño; implementación y medidas abajo cuando estén)
- **Fuentes:** historia 024 (PEP A, las cifras a batir), 023 (rescate,
  PARQ, `harq.h`, reglas de TCP), 020/021 (`redund_spare`, `rs`/`rlc`), 014
  (túnel nativo), 013/019 (`capacity.h`); tarea #71.

## TL;DR

- **Qué es:** el router termina el TCP de la LAN (~1 ms del emisor) y el VPS
  abre un TCP nuevo al destino, como el A; pero el tramo router ↔ VPS no es
  TCP: cada conexión es un **flujo de bytes fiable dentro del túnel**, con
  ARQ completo que nunca se rinde, las copias, la FEC (`rs`/`rlc`) y el
  rescate del motor debajo. El resto del tráfico (UDP, lo no interceptado)
  sigue como hoy, con plazos.
- **El ritmo del medio no sale de la pérdida:** un solo dosificador por
  extremo (`pepcc.h`) reparte entre los flujos lo que el camino consume
  (entregado más perdido mientras la cola no crece), arranca de la capacidad
  que el motor ya estima (`capacity.h`), la toma de techo cuando el motor ve
  sus enlaces llenos, y limita lo que vuela a `min_rtt + pep_queue_ms`. La
  pérdida al azar (Starlink, el microcorte) no frena: la repara el ARQ.
- **Contrapresión de punta a punta:** el VPS anuncia en cada ACK hasta dónde
  acepta (lo que el destino no toma lo detiene); el router deja de leer el
  socket de la LAN cuando lo que tiene sin enviar pasa `pep_backlog_ms` a su
  ritmo, y el TCP del emisor se frena por su ventana.
- **Protocolo 10:** los mensajes del flujo (`OPEN`, `DATA`, `ACK`, `RST`)
  van como carga de un DATA del túnel nativo cuyo primer nibble es `0xF`
  (nunca IPv4 ni IPv6): el MAC, el anti-replay, las copias, la FEC y el
  rescate les aplican sin cambios; el receptor los entrega al instante (el
  reordenador no los retiene, tienen su propio orden).
- **Configuración:** apagado por omisión. Router: `pep = yes`, `pep_port`
  (el `REDIRECT` del firewall llega ahí) y qué se intercepta lo dice el
  firewall (UCI `pep_ports`, `pep_dest`); VPS: `pep = yes`, `pep_cc = bbr`
  para el tramo lejano. Memoria acotada: `pep_buf_kb` por flujo y sentido,
  `pep_mem_mb` en total, `pep_max_flows`.

## Contexto

La historia 024 midió el TCP partido con piezas comunes (shadowsocks,
kcptun): lo que vale es elegir el control de congestión del tramo lejano
(BBR en el VPS), y lo que cuesta es un TCP ciego a los enlaces en el medio
(cubic es el techo con copias; BBR o KCP llenan la cola del celular hasta
0,7–1,9 s), búferes sin tope (1,7–7,8 MB por conexión, el vivo atrasado
10–20 s) y dos demonios con cifrado doble. El B es el mismo corte con el
protocolo propio en el medio. Las cifras a batir están al final de la 024
(el mejor A con `rlc`).

## Diseño

### Dónde vive

- Dentro del motor, en los dos extremos: `pep.c` (sockets, epoll, el
  destino original, el servicio de cada pasada del lazo), `stream.h` (el
  flujo fiable: búfer por trozos, marcador de segmentos, reensamblado, ACK,
  pérdida), `pepcc.h` (el dosificador) y `pep.h` (el formato de los
  mensajes, la tabla de flujos, qué destinos se aceptan). La lógica de
  decisión es pura y tiene sus pruebas (`test_stream.c`, `test_pepcc.c`,
  `test_pep.c`, con un extremo a extremo en memoria que pierde y reordena).
- **Solo con el túnel nativo** (`tunnel = native`): con WireGuard el motor
  no ve paquetes IP propios donde meter los mensajes.

### Formato (protocolo 10)

Carga de un DATA del túnel (detrás de la cabecera de 24 B y bajo su MAC):

```
0     0xF0 | tipo        (1 OPEN, 2 DATA, 3 ACK, 4 RST)
1     banderas           (por tipo)
2..5  id del flujo       (lo elige el router: generación << 16 | ranura)
```

- `OPEN` (router → VPS): familia, puerto y dirección de destino (4 o 16 B).
  Se repite con el temporizador hasta el primer ACK del VPS; el VPS
  contesta con un ACK aunque el `connect` siga en curso, y con `RST`
  (motivo: rehusado, inalcanzable, plazo, negado, lleno, apagado) si falla.
  Los DATA pueden ir detrás del OPEN sin esperar (0-RTT); el VPS los guarda
  dentro de su ventana mientras conecta.
- `DATA`: desplazamiento (32 bits bajos del desplazamiento de 64 del flujo,
  que el receptor rehace contra lo que espera) y los bytes. Bandera `FIN`:
  el flujo termina ahí; el FIN ocupa un byte de la secuencia (como en TCP),
  así que el ACK que lo cubre lo confirma. Bandera `EST`: el que envía ya
  recibió un ACK de este flujo (un VPS que no lo conoce contesta `RST`; sin
  `EST`, calla: el OPEN pudo perderse).
- `ACK`: el acumulado (lo siguiente que espera), el borde derecho de la
  ventana (hasta dónde acepta, absoluto) y hasta 4 rangos SACK. Cada ACK
  lleva el estado completo: cualquiera sirve de actualización de ventana.
- `RST`: motivo. Se contesta `RST` (con tope de ritmo) a un DATA con `EST`
  o a un ACK de un flujo desconocido: así un extremo reiniciado limpia al
  otro.
- Lo demás del túnel no cambia. Una versión 10 es necesaria porque un
  servidor 9 escribiría los mensajes a la TUN (los descarta como no IP, pero
  la conexión quedaría colgada sin aviso).

### Estados

- Router: `ACCEPT` (llega un `accept` del puerto del `REDIRECT`; destino
  con `SO_ORIGINAL_DST` / `IP6T_SO_ORIGINAL_DST`, y si no hay conntrack,
  `getsockname`, que es lo que da TPROXY) → `OPEN_SENT` → `EST` (primer ACK)
  → medio cerrado por cada lado → cerrado cuando los dos FIN están
  confirmados y entregados. Un `RST` del VPS (o del emisor: `ECONNRESET`)
  cierra con `RST` del otro lado (`SO_LINGER` 0).
- VPS: `OPEN` → `connect` no bloqueante (con `pep_cc`, `TCP_NODELAY`,
  `TCP_NOTSENT_LOWAT`) → `EST`; un `connect` que falla o tarda más de 30 s →
  `RST`. Se borran todos los flujos de una sesión que se cierra.
- **El túnel caído mucho tiempo:** el ARQ no se rinde, pero un flujo con
  datos sin confirmar y sin avance en `pep_dead_s` (60) se cierra con `RST`
  en los dos lados; lo que esperaba está acotado por los búferes.

### Fiabilidad (`stream.h`)

- **Emisor:** un búfer circular por trozos de 16 KB (de un fondo común con
  tope `pep_mem_mb`) con lo no confirmado y lo no enviado, hasta
  `pep_buf_kb`; un anillo de segmentos (desplazamiento, largo, hora de envío,
  reenvíos, estado, y la foto de lo entregado para la muestra de ritmo).
- **Pérdida:** RACK (un segmento sin confirmar enviado más de una ventana
  de reorden antes que uno ya confirmado está perdido; la ventana,
  `min_rtt / 4` más el tiempo que tarda la reparación del túnel) y un
  temporizador (`srtt + 4 rttvar`, con retroceso doble hasta 2 s) para la
  cola. Nunca se rinde: un segmento se reenvía hasta que llega.
- **Copias por regla de cengarde:** el primer reenvío va como un DATA
  cualquiera (las copias y la FEC del modo); desde el segundo, copiado en
  todos los enlaces que llevan (como la regla de la evidencia de la 023 para
  lo que el túnel perdió).
- **Receptor:** reensambla por desplazamiento en su búfer (hasta 16 rangos
  fuera de orden; más, descarta y lo reenviará el emisor), entrega en orden
  al socket y contesta: al instante con un hueco, el FIN o una ventana que
  se abre; si no, cada 2 DATA o 2 ms. Los duplicados se cuentan.

### Ritmo (`pepcc.h`)

- Uno por extremo y par (el router tiene uno; el VPS uno por sesión), para
  todos los flujos; entre flujos, turnos por igual (DRR).
- **Lo que el camino consume:** muestras por ACK al estilo de BBR (bytes
  entregados entre el envío de un segmento y su ACK, sobre ese tiempo), pero
  contando también lo perdido mientras el RTT de la muestra no pasa de
  `min_rtt + pep_queue_ms`: la pérdida al azar no baja el ritmo. Con la cola
  arriba, la muestra cuenta solo lo entregado y el estimador baja a ella (la
  pérdida por desborde no se confunde con capacidad).
- **Ritmo** = estimador × ganancia (arranque 2, luego un ciclo 5/4, 3/4,
  1, …); **en vuelo** ≤ estimador × (`min_rtt` + `pep_queue_ms`) más unos
  pocos paquetes: la cola que el medio arma está acotada por diseño.
- **Del motor** (`capacity.h`): el estimador arranca de la capacidad de los
  enlaces que llevan los datos (en redundancia, la del primero; repartido,
  la suma), sin arranque lento; y cuando el motor ve su cola sobre
  `capacity_sat_ms`, la capacidad es el techo.

### Búferes y contrapresión

- **VPS:** escribe en orden al socket del destino solo hasta
  `TCP_NOTSENT_LOWAT` sin enviar (el kernel no esconde más); lo que no
  entra queda en su búfer y el borde de la ventana deja de avanzar cuando lo
  que guarda pasa `pep_backlog_ms` al ritmo con que el destino lo toma (y
  nunca pasa `pep_buf_kb`): el router se detiene.
- **Router:** lee del socket de la LAN mientras lo que tiene sin enviar no
  pase `pep_backlog_ms` a su ritmo y lo no confirmado quepa en `pep_buf_kb`;
  el socket de la LAN tiene un `SO_RCVBUF` chico (puesto en el socket que
  escucha), así que el emisor siente la ventana cerrada en ~`pep_backlog_ms`
  más lo que viaja.
- **Memoria por conexión y extremo:** `pep_buf_kb` (1024) por sentido como
  tope, más los búferes del kernel acotados (el `SO_RCVBUF` de la LAN, el
  `NOTSENT_LOWAT` del destino); `pep_mem_mb` (64) para todos.

### Interceptar

- **Router:** el firewall elige: `REDIRECT` del TCP que entra por la LAN al
  `pep_port` (fw4: una regla `dstnat` que escribe `cengarde-setup` desde UCI
  `pep_ports` y `pep_dest`; sin puertos, todo el TCP). IPv4 e IPv6 (dos
  sockets que escuchan). El motor no decide qué se intercepta, solo si lo
  acepta: rehúsa (RST) lo que va al propio router o a su puerto, y con los
  topes llenos.
- **VPS:** abre el TCP desde su propia dirección (no hace falta NAT); no
  conecta a loopback, sin especificar ni multicast. El MSS de cada tramo es
  el de su interfaz: no hay que fijarlo.

### Lo que no cambia

- El tráfico no interceptado va por la TUN como hoy, con todo lo de antes.
- Con `pep = no` (la omisión), ni un socket ni un byte de más: la versión
  del protocolo es lo único distinto.

## Hallazgos

(Pendiente: medidas en `~/campo/2026-10-25/pepb/`.)

## Qué hacemos con esto

## Pendiente

## Cambios

- 2026-10-25: creada (diseño).
