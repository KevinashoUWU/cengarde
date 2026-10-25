# 025 — El protocontrolador: TCP partido por el propio motor (variantes A y B, protocolo GRIT)

- **Fecha:** 2026-10-25
- **Estado:** implementado (A y B) y medido en el laboratorio; el
  microcorte de B y la RLC que rehacía basura, corregidos (2026-10-26); falta
  la Pi, el VPS real y el terreno
- **Fuentes:** historia 024 (TCP partido con shadowsocks, las cifras a
  batir), 023 (rescate, PARQ, `harq.h`, reglas de TCP), 020/021
  (`redund_spare`, `rs`/`rlc`), 014 (túnel nativo), 013/019
  (`capacity.h`); tarea #71. Datos crudos: `~/campo/2026-10-25/pepb/`
  (`plan*.json`, `out-plan*/all.jsonl`, `tables*.md`); las correcciones del
  26-10, `~/campo/2026-10-26/rlcfix/` (`planF.json` y `planY.json` el lote
  final, `tablesF.md`, `cmp.py`, `tables.py`; `plan1`–`plan14` los
  intentos; `tr*.log`/`ct*.log` las trazas; `planSoak.json`,
  `planAsan.json` la resistencia).

## TL;DR

- **Qué es:** el router termina el TCP de la LAN (a ~1 ms del emisor) y el
  VPS abre un TCP nuevo al destino; lo que la RFC 3135 llama un PEP, hecho
  por el motor mismo, sin proxies ni cifrado doble. Una perilla:
  `protocontroller = no | a | b` (solo con `tunnel = native`; `no` por
  omisión). El VPS acepta las dos variantes con cualquiera.
- **A:** el tramo router → VPS es un TCP simple del kernel por el túnel, a
  la dirección del VPS en el túnel, con BBR en el medio
  (`protocontroller_mid_cc`) y `auto` hacia el destino (cubic, BBR en
  cuanto pierde). Iguala al mejor A de la 024 (`Ab`, shadowsocks) en caudal
  (`protect` 53,9 contra 53,8 Mbit/s; `beyond` 53,0 contra 53,4;
  `protectcut` 36,2 contra 36,0) con menos memoria (2,1 contra 3,4 MB en el
  router) y mejor vivo cercano (`protect`, demora p99 44 contra 101 ms).
- **B, el protocolo GRIT** (*Guaranteed Redundant IP Transport*): el tramo
  del medio es un flujo fiable propio dentro del túnel (protocolo 10), con ARQ que nunca se
  rinde, ritmo por el retardo de ida y la capacidad del motor, y las copias,
  la FEC y el rescate debajo. Mismo caudal que A (54,6 y 52,8 Mbit/s en
  `protect` y `beyond`) con menos CPU (12,8 contra 17,6 % de un núcleo, los
  dos motores), menos memoria en el router (1,3 MB) y menos cola (RTT p99 36
  contra 43 ms; vivo cercano 38–40 contra 44–46 ms, p < 0,01).
- **El microcorte de GRIT, corregido (2026-10-26, n = 8):** TCP de 30,4 a
  **40,2 Mbit/s** (p < 0,01; A 36,1, p 0,02) y el vivo de 19,5 % perdido y
  2,5 cortes a **0,0 % y 0 cortes** (p < 0,01; A 18,4 %), demora p99 lejana
  de 4,9 a 0,65 s. El RTT p99 de los pings (224 ms) queda como el de A
  (198, p 0,64), no bajo los 145 de la meta. Lo que lo hundía: la ventana
  de RACK inflada a 100 ms por las reparaciones tardías del túnel, muestras
  de ritmo de 300–900 Mbit/s tras cada hueco, el flujo mandado al celular
  cuando el primer enlace se veía detenido o «ahogado», y las copias de
  cada reenvío llenando el celular.
- **La RLC que rehacía basura** (falla del motor, de la 021): al permutar
  filas en la eliminación también se permutaba la tabla de qué ecuación es
  cada fila, y lo rehecho sumaba las ecuaciones equivocadas. Corregida (en
  el lote final, de 16 344 `bad` y 115 `not_ip` en 144 corridas a 0), con
  una guarda que revisa todo lo rehecho antes de entregarlo (`rebuilt.h`:
  58 212 bien, 0 malos).
- **Frente al túnel transparente (`no`):** las dos variantes llevan el TCP
  de 29 a 54 Mbit/s en `protect`, de 1,2 a 53 en `beyond` (1 % perdido más
  allá del VPS) y de 8,6 a 31–36 en `protectcut`; el vivo más allá del VPS
  pasa de 80 % perdido a 0 %.
- **Qué usar:** A para todos (estable, sin formato nuevo: su TCP es un
  paquete más de la TUN); B (GRIT) experimental, mejor en latencia y CPU y
  ahora también en el microcorte (más TCP, el vivo sin pérdida), con el RTT
  de los pings en el corte como el de A.

## Contexto

La historia 024 midió el TCP partido con piezas comunes (shadowsocks,
kcptun): lo que vale es elegir el control de congestión del tramo lejano
(BBR en el VPS), y lo que cuesta es un TCP ciego a los enlaces en el medio
(cubic es el techo con copias; BBR o KCP llenan la cola del celular hasta
0,7–1,9 s), búferes sin tope (1,7–7,8 MB por conexión, el vivo atrasado
10–20 s) y dos demonios con cifrado doble. El protocontrolador hace el mismo
corte dentro del motor: A con un TCP del kernel en el medio (la 024 sin
proxies), B con el protocolo propio. Nombre: «protocontrolador» en los
documentos, `protocontroller` en el código, la configuración, UCI, LuCI y el
estado; nunca «pep» en un nombre visible (se confunde con el producto
comercial). El protocolo de B se llama **GRIT** (*Guaranteed Redundant IP
Transport*, decidido el 2026-10-26): así en los documentos, LuCI («B
(GRIT)») y el estado (`protocol`); en el código sigue `stream.h`,
`streamcc.h` y `protoctl.*`, y la perilla `no | a | b`.

## Diseño

### Común a las dos variantes

- **Dónde vive:** dentro del motor, en los dos extremos: `protoctl.c`
  (sockets, epoll, el destino original, el servicio de cada pasada del
  lazo), `protoctl.h` (formato, destinos aceptados, prefijo del túnel,
  tabla), `stream.h` y `streamcc.h` (B). La lógica de decisión es pura y
  tiene sus pruebas (`test_protoctl.c`, `test_stream.c`, `test_streamcc.c`,
  con un extremo a extremo en memoria que pierde y reordena).
- **Solo con el túnel nativo:** con WireGuard el motor no ve paquetes IP
  propios donde meter los mensajes de B, ni un TCP del medio que no vaya
  cifrado dos veces.
- **Router:** escucha en `protocontroller_port` (7801, IPv4 e IPv6) lo que
  el firewall le redirige; el destino sale de `SO_ORIGINAL_DST` /
  `IP6T_SO_ORIGINAL_DST`, o de `getsockname` con TPROXY. El socket que
  escucha tiene un `SO_RCVBUF` de 128 KB (las conexiones lo heredan): el
  emisor siente pronto la ventana llena. Rehúsa (RST) lo que va al propio
  router o a su puerto, y con los topes llenos.
- **VPS:** abre el TCP al destino desde su propia dirección (sin NAT; el MSS
  de cada tramo es el de su interfaz), no bloqueante, con `TCP_NODELAY`, un
  `TCP_NOTSENT_LOWAT` dinámico y un `SO_RCVBUF` del tamaño del búfer de la
  conexión (no los 6 MB del autoajuste). No conecta a loopback, sin
  especificar, multicast ni enlace local (169.254.169.254). El tramo lejano
  usa `protocontroller_cc`: `auto` (por omisión) es el del kernel (cubic) y
  BBR en cuanto ese tramo reenvía 3 segmentos; BBR no toma la pérdida al
  azar por congestión (`beyond`), pero su ProbeRTT (4 paquetes por ida y
  vuelta durante 200 ms cada 10 s) retenía un vivo de 8 Mbit/s 50–140 ms.
- **Memoria:** trozos de 16 KB de un fondo común (`protocontroller_mem_mb`,
  64; las páginas que no se tocan no ocupan), hasta
  `protocontroller_buf_kb` (1024) por conexión y sentido;
  `protocontroller_max_flows` (256) conexiones.
- **Interceptar (OpenWrt):** `cengarde-setup` escribe
  `/etc/cengarde/protoctl.nft`, un include de firewall4 con una cadena
  propia (`cengarde_protoctl`, `type nat hook prerouting priority dstnat -
  5`) que redirige el TCP que entra por los dispositivos de la zona `lan`
  (la `dstnat_lan` de fw4 existe solo con alguna regla DNAT de la zona), a
  `protocontroller_port` (UCI: vacío, todos los puertos) y
  `protocontroller_dest` (vacío: todo lo que no es privado ni local). LuCI:
  pestaña *Túnel*, lista *Protocontrolador* (No, A, B).
- **VPS (despliegue):** `cengarde-vps-setup protocontroller on|off`
  (necesita `tunnel native`): `protocontroller = b` en el motor (acepta las
  dos), `PROTOCONTROLLER="yes"` en `nat.conf` (y `cengarde-nat` acepta el
  TCP 7801 de `cg0` a 10.79.0.1, para A), `tcp_bbr` al arrancar.

### Variante A: un TCP simple en el medio

- El router abre, por cada conexión de la LAN, un TCP no bloqueante a la
  dirección del VPS en el túnel (`protocontroller_vps`; vacío, la primera
  del prefijo de `tun_addr`: 10.79.0.1) y a `protocontroller_port`, con
  `protocontroller_mid_cc`, `TCP_NODELAY` y `TCP_NOTSENT_LOWAT`. Al
  conectar escribe una cabecera: `0xA5`, familia, puerto y dirección (8 o
  20 bytes). Ese TCP es un paquete más de la TUN: las copias, la FEC y el
  rescate del túnel le aplican como a cualquier otro.
- El VPS escucha en su dirección del túnel (`IP_FREEBIND`: la TUN puede no
  estar arriba todavía), acepta solo direcciones del prefijo del túnel
  (`cg_pctl_in_pfx`), lee la cabecera (30 s de plazo) y abre el destino.
- El motor copia bytes entre los dos sockets por el fondo común (hasta
  `protocontroller_buf_kb` por sentido): leer se detiene con el búfer lleno
  y el emisor siente la ventana; el FIN de un lado se vuelve `shutdown` del
  otro cuando todo pasó; un error o un RST cierra los dos con RST.
- **El medio con BBR** (`protocontroller_mid_cc`, en cada extremo para lo
  que envía): con cubic, la pérdida que el túnel no repara a tiempo frena
  ese tramo (`protect` 42 contra 54 Mbit/s; la bajada, 7 contra 53). El
  paquete de OpenWrt trae `kmod-tcp-bbr`.

### Variante B: el flujo fiable propio

#### Formato (protocolo 10)

Carga de un DATA del túnel (detrás de la cabecera de 24 B y bajo su MAC):

```
0     0xF0 | tipo        (1 OPEN, 2 DATA, 3 ACK, 4 RST)
1     banderas           (por tipo)
2..5  id del flujo       (lo elige el router: generación << 16 | ranura)
...   cuerpo
-4    etiqueta           (SipHash-2-4 de clave fija, 4 bytes)
```

- `OPEN` (router → VPS): familia, puerto y dirección de destino (4 o 16 B).
  Se repite con el temporizador hasta el primer ACK del VPS; el VPS
  contesta con un ACK aunque el `connect` siga en curso, y con `RST`
  (motivo: rehusado, inalcanzable, plazo, negado, lleno, apagado) si falla.
  Los DATA pueden ir detrás del OPEN sin esperar (0-RTT); el VPS los guarda
  dentro de su ventana mientras conecta.
- `DATA`: desplazamiento (32 bits bajos del de 64 del flujo, que el receptor
  rehace contra lo que espera), el reloj del emisor y los bytes. Bandera
  `FIN`: el flujo termina ahí; el FIN ocupa un byte de la secuencia (como
  en TCP). Bandera `EST`: el que envía ya recibió un ACK de este flujo (un
  VPS que no lo conoce contesta `RST`; sin `EST`, calla: el OPEN pudo
  perderse).
- `ACK`: su secuencia, el acumulado, el borde derecho de la ventana
  (absoluto; puede retroceder), el menor retardo de ida desde el ACK
  anterior y hasta 8 rangos SACK, el del DATA más nuevo primero. Cada ACK
  lleva el estado completo. Bandera `DUP`: llegó un duplicado.
- `RST`: motivo. Se contesta `RST` (con tope de ritmo) a un DATA con `EST` o
  a un ACK de un flujo desconocido: un extremo reiniciado limpia al otro.
- **La etiqueta:** la RLC del túnel reconstruía mal a veces (corregido el
  26-10, ver Hallazgos); un DATA así escribió un reloj falso en el retardo
  de ida (el ritmo se detuvo 2–10 s en `beyond`) y podía meter bytes falsos
  en un flujo. Un mensaje cuya etiqueta no cuadra se descarta (`corrupt`).
  El MAC del túnel cubre lo que viaja; la etiqueta, lo que la FEC rehace;
  ahora el servidor también revisa la etiqueta de lo rehecho antes de
  entregarlo (`rebuilt.h`).
- Una versión 10 es necesaria porque un servidor 9 escribiría los mensajes a
  la TUN (los descarta como no IP, y la conexión quedaría colgada sin aviso).
  A no cambia nada del formato.

#### Estados

- Router: aceptado → `OPEN` enviado → `EST` (primer ACK) → medio cerrado
  por cada lado → cerrado cuando los dos FIN están confirmados y
  entregados. Un `RST` del VPS (o del emisor: `ECONNRESET`) cierra con `RST`
  del otro lado (`SO_LINGER` 0).
- VPS: `OPEN` → `connect` → `EST`; un `connect` que falla o tarda más de
  30 s → `RST`. Se borran todos los flujos de una sesión que se cierra.
- **El túnel caído mucho tiempo:** el ARQ no se rinde, pero un flujo con
  datos sin confirmar y sin avance en `protocontroller_dead_s` (60) se
  cierra con `RST` en los dos lados.

#### Fiabilidad (`stream.h`)

- **Emisor:** un búfer circular por trozos con lo no confirmado y lo no
  enviado, hasta `protocontroller_buf_kb`; un anillo de segmentos
  (desplazamiento, largo, hora de envío, reenvíos, estado, si había cola, si
  la aplicación limitaba, y la foto de lo entregado para la muestra de
  ritmo). Un reenvío reusa su segmento.
- **Pérdida:** RACK (la ventana, `min_rtt / 4` más
  `protocontroller_reo_ms`, 10 ms, que cada duplicado avisado agranda 4 ms
  hasta 100 y vuelve sola: la reparación del túnel llega a veces después) y
  un temporizador desde el envío más viejo (`srtt + 4 rttvar`, al menos dos
  `min_rtt` más la ventana de RACK, retroceso doble hasta 2 s). Nunca se
  rinde. **Con pérdida al azar pesada** (`cg_scc_lossw`: en 100 ms, más de
  1 byte de cada 5 declarado perdido con la cola corta), la ventana y el
  temporizador vuelven a `protocontroller_reo_ms` y los duplicados no la
  agrandan: las reparaciones del túnel no alcanzan, y con 100 ms el hueco
  más viejo tardaba varias vueltas y llenaba el búfer (26-10).
- **Receptor:** reensambla por desplazamiento (hasta 256 huecos; con 16, una
  ráfaga de pérdidas al arrancar se volvía una cascada de descartes),
  entrega en orden al socket; ACK cada 4 DATA o 2 ms, al instante con un
  hueco nuevo, uno que se llena o el FIN.

#### Ritmo (`streamcc.h`)

- Uno por extremo y par para todos los flujos (turnos por igual).
- **Lo que el camino consume:** muestras por ACK al estilo de BBR, contando
  también lo perdido de lo que salió sin cola (hasta 3 perdidos por 1
  entregado, solo al sondear y nunca más de lo que salió entretanto): la
  pérdida al azar no baja el ritmo.
- **La cola es el retardo de ida:** cada DATA lleva el reloj del emisor y
  cada ACK devuelve el menor retardo de ida desde el anterior; cuenta su
  cambio sobre el mínimo de 10 s, el menor de cada ventana de una ida y
  vuelta. La ida y vuelta no sirve: en un microcorte lleva las pérdidas y
  las copias lentas del ACK de vuelta.
- **Ritmo** = estimador × ganancia: arranque 3/2 que termina con la primera
  muestra con cola; ciclo 5/4, 3/4, 1 × 6; 3/4 mientras haya cola sobre
  `protocontroller_queue_ms` (5 ms); una ida y vuelta entera con cola baja
  el estimador a lo entregado (desde el 26-10: el primer corte de una racha
  a lo más entregado en esa ventana, los siguientes a lo entregado). El
  estimador es el mayor de 1–2 s. Cada muestra es sobre lo más largo entre
  el intervalo de los ACK y el vuelo del segmento (26-10: tras un hueco,
  una ráfaga de SACK sobre un intervalo corto leía 300–900 Mbit/s).
- **En vuelo:** dos veces `bw × (min_rtt + protocontroller_queue_ms)` más
  `bw ×` la ventana de RACK; los reenvíos solo esperan el ritmo.
- **Del motor** (`capacity.h`): el comienzo (al menos 16 Mbit/s) y el techo
  cuando el motor halla lleno el enlace de los datos, si no queda bajo la
  mitad del estimador.
- Tras un segundo sin nada en vuelo, el arranque vuelve.

#### Copias y FEC

- Un DATA del flujo es un DATA del túnel: el modo decide (en `protect` con
  `rlc`, va por el primer enlace y la RLC repara por lo que sobra al
  segundo). El reordenador del receptor no lo retiene.
- Con `redund_spare`, los DATA del flujo van por **un** enlace
  (`pnc_first`, 26-10): el de menor retardo sin cola entre los portadores y
  los enlaces con las sondas detenidas que trajeron algo en el último
  segundo (`pnc_links`), sin mirar su pérdida ni si parece ahogado; en las
  copias de un microcorte, solo ese (el ARQ reenvía; copiado, un flujo llenó
  la cola del celular hasta 2 s). Un segmento reenviado por segunda vez,
  entre los 8 más viejos (`cg_st_tx_copy`), es además una copia por lo que
  sobra, dentro de la asignación, que no va a un enlace con cola sobre
  `redund_spare_ms` (`prc`).

#### Contrapresión

- **VPS:** escribe al destino solo hasta `TCP_NOTSENT_LOWAT` (100 ms a su
  ritmo, 32–512 KB); lo que no entra queda en su búfer y el borde de la
  ventana deja de avanzar cuando pasa `protocontroller_backlog_ms` (200) a
  ese ritmo.
- **Router:** lee de la LAN mientras lo que tiene sin enviar no pase
  `protocontroller_backlog_ms` a su ritmo (al menos 64 KB; 256 KB los
  primeros 2 s: con menos, un flujo que arrancaba lento dejaba de leer, el
  búfer chico del socket descartaba un segmento y el emisor esperaba 200 ms)
  y lo no confirmado quepa en el búfer.

### Lo que no cambia

- El tráfico no interceptado va por la TUN como hoy.
- Con `protocontroller = no` (la omisión), ni un socket ni un byte de más:
  la versión del protocolo es lo único distinto. Sin regresión del túnel
  transparente: `T2` (con todo el código de B) contra `T` del mismo día,
  p 0,81–1,0 en `protect`, `beyond` y `protectcut`.

## Resultados (laboratorio, 2026-10-25)

`bench/lab.sh bond` con los hosts de la 024 (`BOND_PEP=transparent`, `cga`,
`cgb`; `tcp` para shadowsocks), redundancia con `redund_spare = rlc`,
`tcp_aware`, rescate a 60 ms; 30 s por corrida, n = 6, subida salvo donde se
dice; mediana y p de Mann-Whitney a dos colas. Lote 5a (`T5`, `A5`) con `B4`
(el mismo código de B que el final; `B5`, con el corte del ritmo a no menos
de la mitad, se revirtió: `protectcut` 35,6 contra 31,1 Mbit/s, p 0,13, con
RTT p99 737 contra 209 ms, p 0,03). Abajo de cada tabla, del lote 4a de la
misma jornada: A con cubic en el medio y los A de la 024 (shadowsocks). CPU:
los dos motores (y los proxies), en % de un núcleo del i9-9900KF; memoria:
los sockets del kernel en cada netns más los búferes del motor, al pico.

### `protect` (Starlink con 0,7 % al azar protegido por un celular ahogado)

| Brazo | n | Mbit/s | RTT p99 ms | memoria router MB | memoria VPS MB | CPU % |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 29,4 | 37 | 0,0 | 0,0 | 10,3 |
| a | 6 | 53,9 | 43 | 2,1 | 2,1 | 17,6 |
| b | 6 | 54,6 | 36 | 1,3 | 2,3 | 12,8 |
| a, cubic en el medio | 6 | 42,1 | 38 | 1,7 | 1,2 | 15,8 |
| 024 `Af` (shadowsocks) | 6 | 42,6 | 37 | 2,2 | 1,3 | 15,8 |
| 024 `Ab` (shadowsocks) | 6 | 53,8 | 38 | 3,4 | 2,9 | 17,4 |

- p a/no: caudal < 0,01, RTT 0,05; b/no: caudal < 0,01, RTT 0,13; b/a:
  caudal < 0,01, RTT 0,02, memoria del router < 0,01, CPU < 0,01.

| Brazo (vivo) | n | peor segundo lejano | demora p99 lejano ms | perdido lejano % | demora p99 cercano ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 7,91 | 243 | 0,0 | 38 |
| a | 6 | 7,90 | 272 | 0,0 | 44 |
| b | 6 | 7,92 | 243 | 0,0 | 38 |
| a, cubic en el medio | 6 | 7,88 | 262 | 0,0 | 36 |
| 024 `Af` | 6 | 7,86 | 365 | 0,0 | 43 |
| 024 `Ab` | 6 | 7,89 | 367 | 0,0 | 101 |

- b/a: cercano < 0,01, lejano 0,07; a/no y b/no sin diferencia (p ≥ 0,07).

### `beyond` (1 % perdido en cada sentido más allá del VPS)

| Brazo | n | Mbit/s | RTT p99 ms | memoria router MB | memoria VPS MB | CPU % |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 1,2 | 21 | 0,0 | 0,0 | 0,9 |
| a | 6 | 53,0 | 38 | 2,1 | 3,4 | 17,4 |
| b | 6 | 52,8 | 36 | 1,3 | 2,9 | 12,4 |
| a, cubic en el medio | 6 | 42,0 | 37 | 1,7 | 3,1 | 15,8 |
| 024 `Af` | 6 | 42,5 | 37 | 2,4 | 2,4 | 16,1 |
| 024 `Ab` | 6 | 53,4 | 43 | 3,1 | 3,2 | 17,0 |

- b/a: caudal 0,58, RTT < 0,01, CPU < 0,01.

| Brazo (vivo) | n | peor segundo lejano | demora p99 lejano ms | perdido lejano % | demora p99 cercano ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 0,57 | 4904 | 79,9 | 35 |
| a | 6 | 7,19 | 350 | 0,0 | 44 |
| b | 6 | 7,26 | 343 | 0,0 | 40 |
| a, cubic en el medio | 6 | 7,19 | 372 | 0,0 | 38 |
| 024 `Af` | 6 | 7,46 | 378 | 0,0 | 58 |
| 024 `Ab` | 6 | 7,25 | 398 | 0,0 | 111 |

- a/no y b/no: lejano < 0,01 en todo; b/a: cercano < 0,01, lo demás ≥ 0,58.

Con 0,1 % más allá del VPS (lote 4b; `a` es el de BBR en el medio): no 5,4
Mbit/s, a 53,6, b 53,8 (RTT p99 38 y 35, p 0,03); vivo, peor segundo 2,94 /
7,30 / 7,70 (b/a p 0,05), demora p99 lejana 2228 / 343 / 314 (p < 0,01),
perdido 39,9 / 0 / 0 %, cercano 35 / 46 / 40 ms (p < 0,01).

### `protectcut` (60 % perdido en Starlink el tercio central)

| Brazo | n | Mbit/s | RTT p99 ms | memoria router MB | memoria VPS MB | CPU % |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 8,6 | 85 | 0,0 | 0,0 | 4,2 |
| a | 6 | 36,2 | 256 | 2,1 | 2,1 | 12,4 |
| b | 6 | 31,1 | 209 | 1,3 | 2,3 | 8,6 |
| a, cubic en el medio | 6 | 19,9 | 109 | 1,8 | 1,2 | 8,4 |
| 024 `Af` | 6 | 21,1 | 113 | 2,4 | 1,1 | 8,8 |
| 024 `Ab` | 6 | 36,0 | 218 | 3,4 | 1,8 | 11,9 |

- b/a: caudal 0,07, RTT 0,81.

| Brazo (vivo) | n | peor segundo lejano | demora p99 lejano ms | perdido lejano % | cortes | demora p99 cercano ms | RTT p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| no | 6 | 1,38 | 5709 | 36,1 | 0,0 | 2406 | 157 |
| a | 6 | 1,59 | 6499 | 17,7 | 0,0 | 6959 | 156 |
| b | 6 | 1,90 | 3006 | 8,8 | 2,0 | 3289 | 1081 |
| a, cubic en el medio | 6 | 1,69 | 6383 | 16,1 | 0,0 | 7052 | 116 |
| 024 `Af` | 6 | 1,38 | 5682 | 20,7 | 0,0 | 5550 | 120 |
| 024 `Ab` | 6 | 1,50 | 6194 | 17,0 | 0,0 | 7440 | 172 |

- b/a: perdido 0,07, cortes < 0,01, RTT < 0,01; a/no y b/no: perdido < 0,01.

### Bajada (el host lejano manda; lote 7, el código final)

| Caso | Brazo | n | Mbit/s | RTT p99 ms | memoria router / VPS MB | CPU % |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `protect` | no | 6 | 4,3 | 35 | – | 2,2 |
| `protect` | a | 6 | 52,6 | 38 | 1,2 / 3,7 | 14,8 |
| `protect` | b | 6 | 49,8 | 31 | 1,4 / 3,0 | 9,6 |
| `protectcut` | no | 6 | 4,1 | 596 | – | 2,3 |
| `protectcut` | a | 6 | 35,4 | 2056 | 0,8 / 3,5 | 10,6 |
| `protectcut` | b | 6 | 29,5 | 2058 | 1,1 / 3,0 | 6,5 |
| `beyond` | no | 6 | 1,7 | 35 | – | 1,1 |
| `beyond` | a | 6 | 2,2 | 35 | 0,1 / 0,2 | 1,6 |
| `beyond` | b | 6 | 3,0 | 29 | 0,2 / 0,5 | 1,7 |

- `protect` y `protectcut`: las dos frente a `no`, p < 0,01; b/a, caudal
  p 0,01 (A algo más), RTT p 0,03 (`protect`, B menos). En `protectcut` las
  dos llenan la cola de bajada del celular (RTT p99 2 s) igual (p 1,0).
- `beyond`: el emisor es el host lejano con su propio cubic sobre 1 %
  perdido; el corte no lo cambia (2–3 Mbit/s).
- Con cubic en el medio de A (el VPS manda la bajada), 7 Mbit/s en
  `protect` (lote 4c): de ahí BBR por omisión.

### Varios celulares (`cell4`: 20, 15, 10 y 5 Mbit/s con 0,3–2 s de cola; `msl2c`: dos Starlink y dos celulares; lote 5c)

| Caso | Brazo | n | Mbit/s | RTT p99 ms | vivo: demora p99 lejano / cercano ms |
| --- | --- | ---: | ---: | ---: | ---: |
| `cell4` | no | 6 | 14,5 | 56 | 551 / 420 |
| `cell4` | a | 6 | 16,5 | 152 | 373 / 266 |
| `cell4` | b | 6 | 16,2 | 56 | 255 / 112 |
| `msl2c` | no | 6 | 15,7 | 39 | 574 / 333 |
| `msl2c` | a | 6 | 26,8 | 66 | 742 / 255 |
| `msl2c` | b | 6 | 22,8 | 33 | 1076 / 1057 |

- El caudal sube con las dos (p ≤ 0,02 frente a `no`); A llena las colas de
  los celulares (RTT p99 152 y 66 ms, p < 0,01), B no (56 y 33). El vivo
  no se distingue (p ≥ 0,17): en los tres brazos, de 6 corridas, 1 a 3
  tienen 1–2,7 s de demora (el reparto del motor entre colas hondas, no
  el protocontrolador); ningún brazo pierde.

### CPU y memoria

- **CPU** (los dos motores, y los proxies donde los hay, en % de un núcleo
  del PC): b 12,4–12,8 % a 53–55 Mbit/s (~0,23 % por Mbit/s), a 17,4–17,6 %
  (~0,33), `Ab` de la 024 17,0–17,4 % (2,5 de ellos los proxies), `no`
  10,3 % a 29 Mbit/s (~0,35): el motor entero, túnel incluido; partido, cada
  Mbit/s cuesta menos que transparente (menos retransmisiones). En la Pi 4,
  estimación (no medido): 4–6 veces el PC, 25–40 % de un núcleo en el router
  con b a 50 Mbit/s (todo el motor).
- **Memoria por conexión:** router b 1,3 MB (el búfer de 1 MB y el kernel),
  a 2,1–2,3 MB; VPS 2,1–3,5 MB las dos (el `SO_RCVBUF` del destino); la 024
  con shadowsocks, 2,2–3,4 MB en el router.

### Resistencia

- **30 min, compilación normal** (lote 6, `protect`, `cc soak`: los dos
  vivos y 16 conexiones cortas a la vez de `churn.py`, 5 % cortadas con RST
  a mitad): B, 37 707 conexiones completas y 2 037 cortadas a propósito, 0
  fallidas ni cortas, 39 746 abiertas y 39 746 cerradas en cada extremo, sin
  `nomem`; los vivos 0 % perdidos, sin cortes, demora p99 200 / 130 ms. A,
  35 955 completas, 0 fallidas, abiertas igual a cerradas. RSS del motor:
  sube en los primeros 10 s (los búferes del rescate, 32 MB, y el fondo) y
  queda plano (router 44,8 MB y VPS 75,3 MB en una corrida de 5 min: +20 y
  +136 KB del segundo 10 al 300).
- **10 min con ASan y UBSan** (`make SANITIZE=1`), B y luego A en el mismo
  caso: ningún aviso de los sanitizadores; 12 384 y 11 908 conexiones
  completas, 0 fallidas.
- La etiqueta de B descartó 151 mensajes (`corrupt`) en el VPS en los 30
  min: la RLC rehace mal (ver Hallazgos).

## Las correcciones del 2026-10-26: la RLC y el microcorte de GRIT

### La RLC que rehacía basura

- **La causa** (`rlc.h`, `cg_rlc_rx_solve`): la eliminación lleva, junto a
  los coeficientes, la matriz `t` (cuánto de cada fila *como se armó* tiene
  cada fila ahora) y `row_eq` (qué ecuación es cada fila como se armó). Al
  buscar un pivote fuera de su fila se permutaban las filas de los
  coeficientes y de `t`, **y también `row_eq`**: las columnas de `t` nombran
  las filas originales, así que lo rehecho sumaba los símbolos de las
  ecuaciones que después ocuparon esas filas. Hace falta una permutación,
  que aparece con varias pérdidas en las ventanas de ecuaciones distintas:
  la PARQ (combinaciones de solo las que faltan) y los microcortes. El
  prefijo de longitud de 16 bits atajaba casi todo (`bad` por miles) y ~1
  de cada 40 basuras pasaba (`not_ip`, y la etiqueta de GRIT, `corrupt`);
  lo rehecho mal se restaba además de las otras ecuaciones (más basura) y
  quedaba marcado en el anti-replay (el paquete verdadero, o su rescate,
  ya no entraba).
- **Reproducido** en `test_rlc.c`: `test_rlc_swap` (cuatro pérdidas,
  ecuaciones sobre {12, 14}, {20, 22}, {14, 20} y {22}: la tercera columna
  pide un pivote dos filas abajo; antes rehacía 1 de 4 y 3 `bad`) y
  `test_rlc_random` (200 000 paquetes, pérdidas en ráfaga de 3 %,
  reparaciones perdidas, PARQ cada 7, a través del borde de 32 bits: antes
  3 331 rehechos mal y 247 342 `bad`; ahora 0 y 0). `row_eq` ya no se
  permuta.
- **RS y XOR:** `test_rs_random` (bloques de 4 a 30, m de 1 a 6, índices
  pedidos de nuevo, el borde de 32 bits) no halla nada; el XOR de `fec.h`
  rehace uno por grupo sin eliminación. Además, lo rehecho nunca pisa en el
  anillo un DATA más nuevo de la misma ranura (una ecuación vieja resuelta
  tarde: `cg_rep_put_sym` devuelve −2, se cuenta en `dropped`).
- **La guarda** (`rebuilt.h`, `test_rebuilt.c`): todo lo rehecho (XOR, RS,
  RLC) se revisa antes de entregarlo, en `fec_deliver`: con el túnel
  nativo, la forma IP (`ippkt.h`) y la suma de TCP, UDP (con suma), ICMP o
  ICMPv6 sobre todo el paquete, o la de la cabecera IPv4 sin ellas (un
  fragmento, otro protocolo); IPv6 sin una suma conocida no se puede
  confirmar (`unsure`, tampoco se entrega); un mensaje de GRIT, su etiqueta;
  con WireGuard, el tipo y el largo (su MAC revisa el resto). Lo que no
  pasa no se marca en el anti-replay, el anillo lo olvida y las ecuaciones
  de la RLC se sueltan. Cuesta una pasada por paquete rehecho (~0,2 µs por
  1 400 B, estimación), no por DATA. Estado por sesión: `rebuilt` (`ok`,
  `bad`, `unsure`). Sin cambio de formato: no sube la versión.

### El microcorte de GRIT

Trazas del ritmo por ACK y de los envíos por enlace cada 10 ms (un binario
de laboratorio, no en la rama: `tr*.log`, `ct*.log`) mostraron, en orden:

1. **La ventana de RACK en 100 ms** antes del corte: las reparaciones de la
   RLC iban detrás de la cola del celular (100–200 ms, la asignación de lo
   que sobra por encima de sus 5 Mbit/s), GRIT reenviaba antes, el receptor
   avisaba duplicados y cada uno sumaba 4 ms (y bajaba 2 ms por segundo).
   En el corte, cada vuelta de un hueco tardaba 120 ms y varias; el búfer de
   1 MB (140 ms a 57 Mbit/s) se llenaba detrás del hueco más viejo y no
   entraba nada nuevo: se mandaban 21–27 Mbit/s de un ritmo de 41–57.
   → con pérdida pesada, la ventana y el temporizador vuelven a
   `protocontroller_reo_ms`.
2. **Muestras de 300–900 Mbit/s** tras cada hueco lleno: el intervalo era el
   de los ACK (una ráfaga de SACK), no el del envío; el estimador las
   seguía, los envíos desbordaban el enlace y la cola cortaba a casi nada.
   → la muestra es sobre lo más largo de los dos intervalos (como BBR).
3. **El flujo al celular:** cuando las sondas de `l1` quedaban sin respuesta
   un segundo (0,84 por sonda con 60 % en cada sentido: ~17 % por ventana)
   o el primer enlace de las copias cambiaba (la pérdida lo veta, o parece
   ahogado por la cola del propio flujo), o no se conocía (todo copiado), el
   flujo iba a `l2` a su ritmo de 60 Mbit/s: hasta 1 MB en la cola del
   celular, su retardo de ida leído como cola, el ritmo al piso
   (0,5 Mbit/s) y 5–8 s sin avanzar. → un enlace con las sondas detenidas
   que sigue trayendo paquetes cuenta como vivo para lo que sube
   (`cg_link_rx_alive`; para el DATA de GRIT, `cg_link_rx_alive_arq`, un
   segundo), y el DATA de GRIT va por el enlace de menor retardo sin cola
   (`pnc_first`). La PARQ ya no va a un enlace ahogado (1 500 reparaciones
   por segundo lo llenaban).
4. **Las copias de cada reenvío** (la segunda vez y más) como toma
   prioritaria del balde de lo que sobra: con la ventana de RACK corta
   son muchas. → solo las de los 8 segmentos más viejos, dentro de la
   asignación.
5. El primer corte de una racha de cola va a lo más entregado en la ventana
   (no a lo de ese instante, detrás de un hueco).

Probados y descartados (`plan2`–`plan14`, n = 6, `protectcut`): solo el
mudo con el retardo de 2 RTT (`G`, sin efecto: el ritmo ya había caído y no
volvían paquetes), sin copias de reenvíos (`H3`/`Q2`: RTT p99 282 ms pero
el vivo 5,6 % y 2 cortes), copias como toma no prioritaria sin límite de
cabeza (`H2`), el DATA de GRIT detrás del primero de las copias si no está
ahogado (`P2`/`U`: el veto por pérdida lo mandaba al celular), mantener el
enlace mientras el flujo entrega (`W`: sin mejora) y reiniciar el ritmo con
la capacidad del enlace nuevo al cambiar (`X`: igual que sin él). Un techo
en la asignación de lo que sobra (crece un sesentaicuatroavo cerca de donde
la cola se pasó la última vez) bajó el RTT de A en `protect` (48 → 38 ms,
p 0,02) pero subió la demora p99 cercana de su vivo en `beyond` (43 → 99 ms,
p < 0,01): se quitó.

### El lote final (n = 8)

`bench/lab.sh bond` como arriba (redundancia, `redund_spare = rlc`,
`tcp_aware`, rescate a 60 ms, túnel nativo, 30 s, subida), `origin/master`
(`2107a89`) contra la rama (un binario de laboratorio, `091e90d`: el código
final salvo el campo `protocol` del estado), los tres brazos; mediana y p de Mann-Whitney a dos
colas de la rama contra `origin/master`. «RLC `bad`» y «rehecho bien /
malo», del servidor (la guarda no existía en `origin/master`).

#### `protect`, TCP

| Brazo | n | Mbit/s | RTT p99 ms | RLC `bad` | rehecho bien / malo | p Mbit/s (Y/M) | p RTT |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 28,6 | 38 | 0 | – |  |  |
| rama `no` | 8 | 24,5 | 38 | 0 | 441 / 0 | 0,49 | 0,96 |
| origin/master `a` | 8 | 53,9 | 48 | 34 | – |  |  |
| rama `a` | 8 | 53,8 | 42 | 0 | 818 / 0 | 0,32 | 0,07 |
| origin/master `b` | 8 | 53,9 | 36 | 0 | – |  |  |
| rama `b` | 8 | 54,2 | 36 | 0 | 782 / 0 | 0,96 | 0,79 |

#### `protect`, vivo

| Brazo | n | perdido lejano % | cortes | demora p99 lejano ms | demora p99 cercano ms | RTT p99 ms | p perdido | p cortes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 0,0 | 0,0 | 245 | 37 | 35 |  |  |
| rama `no` | 8 | 0,0 | 0,0 | 230 | 37 | 35 | 0,38 | 1 |
| origin/master `a` | 8 | 0,0 | 0,0 | 269 | 47 | 35 |  |  |
| rama `a` | 8 | 0,0 | 0,0 | 292 | 45 | 35 | 1 | 1 |
| origin/master `b` | 8 | 0,0 | 0,0 | 248 | 39 | 35 |  |  |
| rama `b` | 8 | 0,0 | 0,0 | 260 | 38 | 28 | 1 | 1 |

#### `beyond`, TCP

| Brazo | n | Mbit/s | RTT p99 ms | RLC `bad` | rehecho bien / malo | p Mbit/s (Y/M) | p RTT |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 1,0 | 35 | 0 | – |  |  |
| rama `no` | 8 | 1,1 | 21 | 0 | 4 / 0 | 1,00 | 0,34 |
| origin/master `a` | 8 | 52,9 | 44 | 118 | – |  |  |
| rama `a` | 8 | 53,2 | 38 | 0 | 891 / 0 | 0,23 | 0,11 |
| origin/master `b` | 8 | 53,6 | 36 | 0 | – |  |  |
| rama `b` | 8 | 53,3 | 34 | 0 | 788 / 0 | 0,37 | 0,87 |

#### `beyond`, vivo

| Brazo | n | perdido lejano % | cortes | demora p99 lejano ms | demora p99 cercano ms | RTT p99 ms | p perdido | p cortes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 81,5 | 0,0 | 5680 | 35 | 28 |  |  |
| rama `no` | 8 | 80,2 | 0,0 | 4242 | 35 | 35 | 0,64 | 1 |
| origin/master `a` | 8 | 0,0 | 0,0 | 366 | 43 | 35 |  |  |
| rama `a` | 8 | 0,0 | 0,0 | 344 | 43 | 34 | 1 | 1 |
| origin/master `b` | 8 | 0,0 | 0,0 | 349 | 42 | 35 |  |  |
| rama `b` | 8 | 0,0 | 0,0 | 366 | 40 | 35 | 1 | 1 |

#### `protectcut`, TCP

| Brazo | n | Mbit/s | RTT p99 ms | RLC `bad` | rehecho bien / malo | p Mbit/s (Y/M) | p RTT |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 6,7 | 53 | 35 | – |  |  |
| rama `no` | 8 | 7,0 | 57 | 0 | 114 / 0 | 0,60 | 0,13 |
| origin/master `a` | 8 | 35,8 | 176 | 227 | – |  |  |
| rama `a` | 8 | 36,1 | 198 | 0 | 506 / 0 | 0,49 | 0,92 |
| origin/master `b` | 8 | 30,4 | 330 | 0 | – |  |  |
| rama `b` | 8 | 40,2 | 224 | 0 | 450 / 0 | < 0,01 | 0,71 |

#### `protectcut`, vivo

| Brazo | n | perdido lejano % | cortes | demora p99 lejano ms | demora p99 cercano ms | RTT p99 ms | p perdido | p cortes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| origin/master `no` | 8 | 43,9 | 0,0 | 5270 | 1744 | 120 |  |  |
| rama `no` | 8 | 45,1 | 0,0 | 5114 | 1915 | 118 | 0,43 | 0,38 |
| origin/master `a` | 8 | 16,6 | 1,0 | 6071 | 6997 | 156 |  |  |
| rama `a` | 8 | 18,4 | 0,0 | 7195 | 5905 | 167 | 0,19 | 0,05 |
| origin/master `b` | 8 | 19,5 | 2,5 | 4941 | 4861 | 594 |  |  |
| rama `b` | 8 | 0,0 | 0,0 | 650 | 602 | 450 | < 0,01 | < 0,01 |

- **`protectcut`, GRIT:** TCP 30,4 → 40,2 Mbit/s (p < 0,01; contra A de la
  rama, 36,1, p 0,02); vivo 19,5 → 0,0 % perdido y 2,5 → 0 cortes (p < 0,01;
  A 18,4 %), demora p99 lejana 4 941 → 650 ms. El RTT p99 de los pings (en
  el corte van por el celular cuando `l1` pierde su copia) queda en 224 ms
  (p 0,71 contra `origin/master`, 0,64 contra A): la meta de 145 no se
  alcanza; en las corridas por separado va de 60 a 2 000 ms según cuánto
  llene el celular en el corte (lo que sobra y las reparaciones que siguen
  al corte, también para A).
- **Sin regresión** de `no` y A en ningún caso (p ≥ 0,05 en caudal,
  pérdida, cortes y demoras; A en `protect`, RTT 48 → 42 ms, p 0,07), ni de
  GRIT en `protect` y `beyond`.
- **La RLC sin el protocontrolador** (`no`): el mismo caudal (`protect`
  28,6 / 24,5 Mbit/s, p 0,49; `protectcut` 6,7 / 7,0, p 0,60): la corrección
  no cuesta.
- **En total** (144 corridas por lado): `origin/master` 16 344 `bad` de la
  RLC, 115 `not_ip` y 15 `corrupt`; la rama 0, 0 y 0, con 58 212 paquetes
  rehechos que pasaron la guarda y ninguno rechazado.

### La celda virtual (la RLC sin el protocontrolador)

`bench/celda/proteger.py`, perfil `wom`, brazo `rlc`, el fijo con 0,7 % y un
microcorte de 60 % en 15–25 s, 40 s por corrida, n = 6 por tipo,
`origin/master` contra la rama (`~/campo/2026-10-26/rlcfix/cell/`): TCP
17,6 / 17,2 Mbit/s (p 0,20), en el corte 4,2 / 4,1 (p 0,75); UDP de 7 Mbit/s
perdido fuera del corte 0,010 / 0,015 % (p 0,42) y en el corte 27,5 / 26,5 %
(p 0,30); ping p99 con TCP 61 / 54 ms (p 0,15). La RLC de `origin/master`
contó 1 329 `bad` en esas 12 corridas; la rama, 0. La corrección no cuesta
caudal ni pérdida.

### Resistencia

- **30 min** (`protect`, `cc soak`: los dos vivos y 16 conexiones cortas a
  la vez de `churn.py`, 5 % cortadas con RST a mitad; las tres a la vez,
  `planSoak.json`):
  - **GRIT, la rama:** 37 725 conexiones completas y 2 038 cortadas a
    propósito, 0 fallidas ni cortas; 39 765 abiertas y cerradas en el router
    y 39 764 en el VPS (la última, abierta al cortar la foto); sin `nomem`
    ni `corrupt`; los vivos 0,0 % perdidos, sin cortes, demora p99 188 /
    129 ms. El servidor: 42 956 paquetes rehechos, todos pasaron la guarda;
    RLC `bad` 0, `not_ip` 0. RSS del motor al final: router 45,9 MB, VPS
    75,1 MB (como el 25-10).
  - **GRIT, `origin/master`:** 37 709 completas, 0 fallidas, pero 98 576
    `bad` de la RLC, 818 `not_ip` y 154 mensajes descartados por su etiqueta
    (`corrupt`; el 25-10 fueron 151).
  - **La RLC sin el protocontrolador, la rama** (`no`): 26 165 conexiones
    completas, 0 fallidas; 41 643 rehechos, 0 rechazados, `bad` 0,
    `not_ip` 0.
- **11 min con ASan y UBSan** (`make SANITIZE=1`, GRIT en `protect`, `cc
  soak`, `planAsan.json`): ningún aviso de los sanitizadores en los dos
  motores (`asan/client.log`, `asan/server.log`); 13 670 conexiones
  completas, 715 cortadas a propósito, 0 fallidas, 14 387 abiertas y
  cerradas en cada extremo; 15 610 rehechos, 0 rechazados.

## Hallazgos

- **A iguala a la 024 sin proxies:** el caudal es el de `Ab` en los tres
  casos y la memoria baja; el vivo cercano de `Ab` (101–111 ms) baja a 44
  porque el motor no esconde MB en búferes de usuario (su copia se detiene
  con 1 MB por sentido y el emisor siente la ventana).
- **El medio de A necesita BBR:** con cubic, 42 Mbit/s en subida y 7 en
  bajada (`protect`): la pérdida que la RLC no repara a tiempo frena a
  cubic. Por eso `protocontroller_mid_cc = bbr` por omisión en los dos
  extremos.
- **B rinde lo mismo con menos:** el mismo caudal que A con 30 % menos CPU,
  menos memoria en el router y sin cola en los enlaces (RTT p99 de los demás
  paquetes 33–36 ms contra 38–152 de A): su ritmo sale del retardo de ida,
  no de llenar colas.
- **El microcorte era el pendiente de B** (corregido el 26-10, arriba): con
  60 % perdido en el enlace grande, B perdía la mitad del vivo que A pero
  tenía 2 cortes y la cola del celular llena (RTT p99 1,1 s). Visto por segundo: el estimador sube con lo
  perdido sin cola (60 → 69 Mbit/s), la cola aparece, el corte lo baja a lo
  entregado detrás de un hueco (0,5 Mbit/s) y los reenvíos esperan el ritmo;
  además, durante el corte el motor marca `l1` como detenido a ratos y las
  reparaciones de la RLC llenan el celular (727–1036 ms de cola). Probados y
  descartados: no retroceder el temporizador mientras algo se entrega (más
  expiraciones, los mismos cortes) y cortar a no menos de la mitad
  (`B5`, arriba).
- **La RLC reconstruía mal a veces** (falla del motor, de la 021; corregida
  el 26-10, arriba): al permutar filas en la eliminación se permutaba
  también `row_eq`, y lo rehecho sumaba las ecuaciones equivocadas; `bad`
  por miles y `not_ip` 8–49 en las corridas con PARQ.
- **Sin regresión del túnel transparente** con el código nuevo y
  `protocontroller = no`.

## Qué hacemos con esto

- `protocontroller = a` es la opción para el usuario: el mismo resultado que
  el mejor A de la 024 sin instalar ni mantener proxies, en LuCI y en
  `cengarde-vps-setup`.
- B (GRIT) queda experimental: mejor en latencia, CPU y memoria, y desde el
  26-10 también en el microcorte; falta la Pi y el terreno.

## Pendiente

- **El RTT de los pings en el microcorte** (224 ms con GRIT, 198 con A,
  meta 145): lo pone la cola del celular cuando `l1` pierde la copia de un
  ping; la asignación de lo que sobra pasa de la capacidad del celular (un
  techo bajaba el RTT pero empeoraba el vivo de A en `beyond`), y las
  reparaciones que siguen al corte (la pérdida media aún alta pide m de
  8–10 por 64).
- **El estimador de GRIT sube con lo perdido** en una pérdida antes del
  cuello de botella (netem): manda hasta 150 Mbit/s a un enlace de 60 que
  pierde 60 % y, al terminar el corte, desborda una vez. En un enlace real
  que pierde después del cuello, la cola lo frena antes. Sin cambiar.
- **La Pi y el VPS reales:** CPU y memoria en la Pi 4; `e2e.sh` en QEMU
  cubre b y a con una página del VPS; falta el terreno con el RTMP real.
- **La demora del vivo con varios celulares** (los tres brazos): es del
  reparto del motor.

## Cambios

- 2026-10-25: creada (diseño de B); implementación de B, medidas contra la
  024; el nombre (protocontrolador, sin «pep» en nada visible); la variante
  A en el motor (`protocontroller = no | a | b`), medidas de A y B contra el
  túnel transparente, OpenWrt y VPS.
- 2026-10-26: B se llama GRIT (documentos, LuCI, `protocol` en el estado).
  La RLC que rehacía basura (`row_eq` permutado), corregida, con la guarda
  de lo rehecho (`rebuilt.h`); el microcorte de GRIT: RACK corto con
  pérdida pesada, muestras sobre el vuelo, el primer corte a lo más
  entregado, el DATA de GRIT por un enlace (`pnc_first`, `pnc_links`), el
  mudo que sigue trayendo vivo para lo que sube, la PARQ nunca a un enlace
  ahogado, copias de los 8 reenvíos más viejos dentro de la asignación;
  lote final n = 8 y resistencia.
