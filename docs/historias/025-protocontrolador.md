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
  firewall (UCI `pep_port`, `pep_dest`); VPS: `pep = yes`, `pep_cc = bbr`
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
- `ACK`: su secuencia, el acumulado (lo siguiente que espera), el borde
  derecho de la ventana (hasta dónde acepta, absoluto; puede retroceder: el
  receptor igual toma lo que llegue dentro de su búfer), el menor retardo de
  ida desde el ACK anterior y hasta 8 rangos SACK. Cada ACK lleva el estado
  completo: cualquiera sirve de actualización de ventana; uno adelantado por
  otro posterior no mueve el borde (la secuencia). Bandera `DUP`: llegó un
  duplicado.
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
  reenvíos, estado, si había cola, y la foto de lo entregado para la muestra
  de ritmo). Un reenvío reusa su segmento: el receptor nunca ve dos cortes de
  los mismos bytes.
- **Pérdida:** RACK (un segmento sin confirmar enviado más de una ventana
  antes que uno ya confirmado está perdido; la ventana, `min_rtt / 4` más
  `pep_reo_ms`, 10 ms, que cada duplicado avisado por el receptor agranda
  4 ms hasta 100 y vuelve sola, como la de RACK con D-SACK: la reparación del
  túnel, la RLC o el rescate, llega a veces después) y un temporizador desde
  el envío más viejo de lo que está en vuelo (`srtt + 4 rttvar`, al menos dos
  `min_rtt` más la ventana de RACK, retroceso doble hasta 2 s). Nunca se
  rinde.
- **Receptor:** reensambla por desplazamiento (hasta 256 huecos; con más,
  descarta lo que abriría otro y el emisor lo reenvía: con 16, una ráfaga de
  pérdidas al arrancar se volvía una cascada de descartes), entrega en orden al
  socket. El ACK lleva primero el rango del DATA más nuevo (RACK conoce la
  entrega más reciente aunque haya más huecos de los que caben) y hasta 8.

### Ritmo (`pepcc.h`)

- Uno por extremo y par (el router tiene uno; el VPS uno por sesión), para
  todos los flujos; entre flujos, turnos por igual (DRR).
- **Lo que el camino consume:** muestras por ACK al estilo de BBR (bytes
  entregados entre el envío de un segmento y su ACK, sobre ese tiempo), pero
  contando también lo perdido de los segmentos enviados mientras no había
  cola (hasta 3 perdidos por 1 entregado): la pérdida al azar no baja el
  ritmo. La pérdida por desborde (segmentos enviados con cola) no cuenta.
- **La cola es el retardo de ida:** cada DATA lleva el reloj del emisor y
  cada ACK devuelve el menor retardo de ida de lo que llegó desde el ACK
  anterior (el reloj del receptor menos el del emisor, el desfase incluido:
  solo cuenta su cambio sobre el mínimo de 10 s). El veredicto es el menor
  de cada ventana de una ida y vuelta: una cola sube todas las muestras, una
  copia tardía (un paquete perdido en el primer enlace que llegó por el
  segundo) solo algunas. La ida y vuelta no sirve para esto: en un microcorte
  lleva las pérdidas y las copias lentas del ACK de vuelta, que se leían como
  cola y hundían el ritmo.
- **Ritmo** = estimador × ganancia: arranque 3/2 (el 2,89 de BBR, y 2,
  desbordaban una cola corta de una vez) que termina con la primera muestra
  con cola; luego el ciclo 5/4, 3/4, 1 × 6; y 3/4 mientras la ventana muestre
  cola sobre `pep_queue_ms` (5 ms): con el estimador en lo entregado, una cola
  ya formada no se vacía sola. Una ventana entera con cola baja el estimador a
  lo entregado. El estimador es el mayor de las muestras de 1–2 s (más corto,
  caía con el ruido de las pérdidas de un microcorte).
- **En vuelo:** dos veces `bw × (min_rtt + pep_queue_ms)`, más lo que RACK
  todavía no declaró perdido (`bw ×` su ventana: con 60 % perdido y 100 ms
  de ventana, lo perdido sin declarar llenaba la ventana, los envíos paraban y
  el estimador los seguía hacia abajo).
- **Del motor** (`capacity.h`): el estimador arranca de la capacidad de los
  enlaces que llevan los datos (en redundancia con `redund_spare`, la del
  primero; repartido, la suma), al menos 16 Mbit/s; y cuando el motor halla
  lleno ese enlace (`CG_CAP_CUT_FULL`: su capacidad medida con una cola
  sostenida), es el techo, salvo que esté por debajo de la mitad del
  estimador: la capacidad de un enlace ocioso es lo que entregan sus sondas,
  «lleno» de ellas, y como techo retuvo un flujo nuevo en pocos kbit/s.
- Tras un segundo sin nada en vuelo, el arranque vuelve (con el estimador que
  tenía).

### Copias y FEC para el flujo fiable

- Un DATA del flujo es un DATA del túnel: el modo decide (en `protect`, con
  `redund_spare = rlc`, va por el primer enlace y la RLC lleva sus
  reparaciones por lo que le sobra al segundo).
- **En las copias de un microcorte** (`redund_spare` pasa a copias cuando el
  primer enlace pierde mucho), los DATA del flujo no se copian: su ARQ
  reenvía lo perdido, y copiado, un flujo que mantiene su ritmo durante el
  corte llenó la cola del celular hasta 2 s.
- **Un segmento reenviado por segunda vez** va como copia prioritaria (dentro
  del presupuesto de lo que sobra), y no a un enlace con cola sobre
  `redund_spare_ms`: llegaría después del próximo reenvío.
- **ACK:** cada 4 DATA o 2 ms; al instante con un hueco nuevo, uno que se
  llena o el FIN. Un ACK por cada DATA fuera de orden (60 % perdido) copiado
  por todos los enlaces llenó la bajada del celular.

### El tramo lejano (`pep_cc`)

- `auto` (por omisión): el control de congestión del kernel (cubic) y BBR en
  cuanto ese tramo reenvía 3 segmentos. BBR no toma la pérdida al azar por
  congestión (el tramo lejano de `beyond`, historia 024), pero su ProbeRTT
  (4 paquetes por ida y vuelta durante 200 ms cada 10 s) retenía un vivo de
  8 Mbit/s 50–140 ms. Un nombre (`bbr`, `cubic`) lo fija.

### Búferes y contrapresión

- **VPS:** escribe en orden al socket del destino solo hasta
  `TCP_NOTSENT_LOWAT` sin enviar (el kernel no esconde más); lo que no
  entra queda en su búfer y el borde de la ventana deja de avanzar cuando lo
  que guarda pasa `pep_backlog_ms` al ritmo con que el destino lo toma (y
  nunca pasa `pep_buf_kb`): el router se detiene.
- **Router:** lee del socket de la LAN mientras lo que tiene sin enviar no
  pase `pep_backlog_ms` a su ritmo (al menos 256 KB: con 64, un flujo que
  arrancaba lento dejaba de leer, el búfer chico del socket descartaba un
  segmento y el emisor esperaba su temporizador de 200 ms) y lo no confirmado
  quepa en `pep_buf_kb`; el socket de la LAN tiene un `SO_RCVBUF` de 128 KB
  (puesto en el socket que escucha), así que el emisor siente la ventana
  cerrada en ~`pep_backlog_ms` más lo que viaja.
- **Memoria por conexión y extremo:** `pep_buf_kb` (1024) por sentido como
  tope, más los búferes del kernel acotados (el `SO_RCVBUF` de la LAN, el
  `NOTSENT_LOWAT` del destino); `pep_mem_mb` (64) para todos.

### Interceptar

- **Router:** el firewall elige: `REDIRECT` del TCP que entra por la LAN al
  `pep_port` (fw4: una cadena propia, `cengarde_pep`, enganchada justo antes
  de la `dstnat` de fw4 para lo que entra por los dispositivos de la zona
  `lan`, que escribe `cengarde-setup` desde UCI `pep_port` y `pep_dest`; sin
  puertos, todo el TCP; sin destinos, todo lo que no es privado ni local; la
  `dstnat_lan` de fw4 existe solo con alguna regla DNAT de la zona). IPv4 e IPv6 (dos
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
