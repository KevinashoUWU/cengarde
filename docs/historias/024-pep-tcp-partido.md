# 024 — PEP modo A: TCP partido en el router y en el VPS, contra el túnel transparente

- **Fecha:** 2026-10-25
- **Estado:** vigente
- **Fuentes:** `bench/lab.d/bond.sh` (`BOND_PEP`, `bond_hosts_up`,
  `bond_pep_start`, `bond_pep_traffic`), `bench/livesim.py` (las
  transmisiones en vivo), `bench/pepstat.py` (tramos, búferes, CPU),
  `bench/peptab.py` (tablas), `bench/bondtab.py` (`--pep`, `cc live`);
  shadowsocks-libev 3.3.5+ds-16 y kcptun 20210922+ds-1+b18 de Debian 13.
  Corridas con `bench/labq.py -j 6 --lab-offset 8` en el PC del banco (16
  hilos): motor `aa39d14` (master con el #108, protocolo 9) para los 12
  brazos y `e4813f9` (con el #109, `tcp_ack_rule`) para la comprobación;
  planes, salidas y tablas en `~/campo/2026-10-25/pepa/` (`plan.json`,
  `plan01.json`, `plan109.json`; `out/`, `out01/`, `out109/`;
  `tables*.md`, `compact.md`). Historias 014 (túnel nativo), 017 (RTMP a
  Facebook en Miami desde el terreno), 023 (rescate, TCP de adentro, caso
  `beyond`).

## TL;DR

- **Lo que gana el TCP partido es BBR en el tramo largo.** Con 1 % de
  pérdida más allá del VPS (host lejano a 100 ms, caso `beyond`), el TCP
  de hoy (transparente, cubic en el emisor) da **1,1 Mbit/s** y una
  transmisión en vivo de 8 Mbit/s pierde el **80 %** de sus cuadros;
  partido con **BBR del VPS al host lejano** (`Af`): **43,7 Mbit/s** con
  `rlc` (15,0 con copias) y **0 %** de cuadros perdidos, demora p99 410 ms
  (n = 6, p 0,005 y 0,003). Con 0,1 % más allá: 5,0 → 44,5 Mbit/s y 22 → 0 %.
  Partido pero con cubic en el tramo lejano (`A`), apenas el doble (2,3–2,8)
  y la transmisión se atrasa **10–20 s** dentro de los proxies.
- **El control:** el mismo emisor con BBR de punta a punta, sin partir
  (`Tb`), llega casi a lo mismo (1 %: 38,5 contra 43,7, p 0,005; 0,1 %: 44,9
  contra 44,5, p 0,81; en vivo, 0 % en los dos, con el peor segundo más
  estable partido: p5 7,86 contra 6,91 Mbit/s, p 0,008). El TCP partido vale
  porque **el TCP del codificador no es nuestro** (OBS, Windows: cubic): el
  router no puede cambiarlo y el VPS sí elige el suyo. Y sin partir, las
  retransmisiones de lo perdido más allá cruzan la subida celular (1700–3900
  por corrida en `Tb`); partido, se quedan en el VPS.
- **Sin pérdida más allá** (`protect`: Starlink con 0,7 % y un celular
  ahogado), partir duplica o triplica el TCP con copias (5,5 → 15,1 Mbit/s,
  p 0,005) y salva la transmisión en vivo con copias (25 → 0 % perdido); con
  `rlc` el túnel transparente ya la entrega entera (0 %) y partir solo suma
  caudal (26,5 → 44,9). BBR también en el medio (`Ab`) llega a 51–54, pero con
  copias llena la cola del celular (RTT p99 755 ms).
- **En el microcorte** (`protectcut`, 60 % perdido en Starlink un tercio de
  la corrida, 16 Mbit/s pedidos sobre un celular de 5) nadie sostiene el
  vivo: partido se pierde el 13–19 % (transparente, 39–44 %, p 0,005), con la
  demora p99 en 4,7–7,4 s. KCP en el medio (ARQ sin control de congestión) no
  pierde nada, a cambio de 3,4–3,7 s de demora y el RTT del túnel en
  1,1–1,9 s.
- **Costos:** se rompe la semántica de los ACK de punta a punta (el emisor
  cree entregado lo que está en el router); **1,7–7,8 MB de búfer del kernel
  por conexión sin tope en cada extremo** (0,5–7,8 MB; hasta 17 MB con
  kcptun), que esconden al
  codificador un tramo lejano lento (los 10–20 s de atraso); CPU de los
  proxies 1–2,7 % de un núcleo del PC a 15–54 Mbit/s (kcptun 7–9 %); dos
  demonios, `REDIRECT`, NAT y el MSS por fijar, solo TCP.
- **Para el PEP B** (protocolo propio con ARQ completo en el medio, tarea
  #71, «protocontrolador»): las cifras a batir son las del mejor A con `rlc`
  (tabla al final): 53,6 / 53,4 / 35,7 Mbit/s en `protect` / `beyond` /
  `protectcut`, 0 % perdido en vivo con demora p99 ≤ 264–410 ms, y en el
  microcorte el 0 % de KCP **sin** su RTT de segundos.

## Contexto

Idea del usuario (2026-10-24): terminar el TCP de la LAN en el router (el
emisor queda a ~1 ms) y en el VPS (un TCP nuevo hacia el destino, con BBR si
se quiere), con un TCP simple entre los dos que viaja por el túnel de
cengarde. Las pérdidas más allá del servidor (Vultr → ingesta de Facebook en
Miami, 100–130 ms) las retransmite el VPS, y las de los enlaces celulares las
ve un TCP de RTT corto cubierto por las copias, la FEC y el rescate. La
historia 023 había medido que el túnel no puede hacer nada por lo perdido
más allá (`tcp_e2e.beyond`): TCP lo repara de punta a punta, y con 1 % y
100 ms de RTT cubic no pasa de ~1,1–1,4 Mbit/s (Mathis: MSS / RTT × 1,22 / √p).
La pregunta: **¿cuánto gana partir el TCP con piezas comunes, qué cuesta, y
qué tendría que superar un PEP propio (B)?**

## Montaje

Laboratorio de bonding (`bench/lab.d/bond.sh`) con el túnel nativo y tres
`netns` más (`BOND_PEP`, `bench/README.md`):

```
peplan  h0 192.168.79.2 ── lan0 .1  cli ══ túnel (l1, l2) ══ srv  fv 172.30.1.1 ── f0 172.30.1.2  pepfar
                              (NAT a 10.79.0.2 hacia cg0)          nv 172.30.2.1 ── n0 172.30.2.2  pepnear
```

- **Enlaces:** los de `protect` (historia 020): `l1` como Starlink, 60 Mbit/s,
  10 ms por sentido, 15 ms de cola, 0,7 % perdido al azar en cada sentido;
  `l2` como WOM pasado de su rodilla, 5 Mbit/s, 25 ms, 2 s de cola.
  `protectcut`: `l1` pierde 60 % el tercio central. `beyond`: los enlaces de
  `protect` y el tramo VPS ↔ host lejano perdiendo 1 % (o 0,1 %) por sentido.
  El tramo lejano tiene 50 ms por sentido (100 ms de RTT), el cercano 5 ms;
  sin tope de caudal. `BOND_NETEM_INFLIGHT = 1`; el arreglo de netem del #109
  (el límite que sobrevive a un salto de retardo) no toca estos casos: ninguno
  cambia el retardo de un enlace.
- **Brazos** (motor igual en todos; `-copies`: `redund_pace_ms = 250`,
  `tcp_aware`, rescate `rescue_wait_ms = 60`, `rescue_kb = 32768` en los dos
  extremos, como en la historia 023; `-rlc`: `redund_spare = rlc` en el
  cliente, `fec = yes` en el servidor, `tcp_aware` y el mismo rescate):
  - `T`: transparente (el router reenvía y enmascara): lo de hoy.
  - `Tb`: transparente con el emisor en BBR (el control: ¿es partir o es BBR?).
  - `A`: `ss-redir` en el router (iptables `REDIRECT` del TCP que entra por
    la LAN) → TCP por el túnel → `ss-server` en `10.79.0.1`, que abre un TCP
    nuevo al destino; cubic en los tres tramos.
  - `Af`: `A` con BBR del VPS al host lejano.
  - `Ab`: `A` con BBR también en el tramo del medio (el router manda la
    subida).
  - `K`: `ss-redir` → kcptun (KCP: ARQ sobre UDP, modo `fast`, sin FEC
    propia, sin cifrado, ventanas de 1024) → `ss-server`; cubic lejos.
  shadowsocks-libev no tiene modo sin cifrar: va con
  `chacha20-ietf-poly1305` y `--no-delay` (sin Nagle, como el transparente).
- **Medidas** (subida, 30 s, n = 6 por brazo y caso, repeticiones rotadas
  entre brazos, p de Mann-Whitney contra `T` de la misma familia):
  - **TCP sin tope:** iperf3 del host de la LAN al lejano; el caudal es lo
    que recibió el lejano.
  - **En vivo** (`cc live`, `livesim.py`): dos transmisiones como RTMP desde
    OBS, 8 Mbit/s cada una, al host lejano (Facebook) y al cercano
    (YouTube). El emisor arma registros de 1250 B cada 10 ms, guarda lo que
    el socket no toma y **descarta** lo que esperó más de 700 ms (el umbral
    de OBS); el receptor mide el caudal de cada segundo (percentil 5: el
    peor segundo), los cortes de más de 500 ms y la demora de cada registro
    (llegada menos fabricación: la cola del emisor, el búfer del socket, los
    proxies y la red). Los sockets usan los búferes del kernel por omisión.
  - **Por tramo** (`pepstat.py`): cada `netns` tiene sus contadores TCP, así
    que `RetransSegs` se separa en LAN (el emisor: todo el camino si es
    transparente), medio (el router: el TCP de `ss-redir` al VPS) y lejano (el
    VPS: el de `ss-server` al host). La memoria es `rmem_alloc +
    wmem_queued` de los sockets del router y del VPS (`ss -tm` cada 0,5 s,
    el pico); también RSS y CPU de los proxies.
  - **RTT p99:** el ping de 84 B del router al VPS por el túnel durante la
    corrida.

## Hallazgos

Medianas de n = 6; «perdido» en vivo es lo que el emisor descartó; «peor
segundo» es el percentil 5 del caudal por segundo del flujo lejano; «retx» son
segmentos retransmitidos por corrida en cada tramo (LAN / medio / lejano).

### `protect`: el primer enlace pierde, el segundo está ahogado

| Brazo | TCP Mbit/s | p | retx LAN / medio / lejano | RTT p99 ms | Vivo: perdido % | peor segundo | demora p99 ms | cercano demora p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `T-copies` | 5,5 | – | 164 / 0 / 0 | 35 | 24,6 | 3,31 | 1981 | 492 |
| `Tb-copies` | 44,0 | 0,005 | 3774 / 0 / 0 | 632 | 0,0 (p 0,010) | 6,09 | 403 | 128 |
| `A-copies` | 15,1 | 0,005 | 29 / 118 / 0 | 29 | 0,0 (p 0,010) | 7,29 | 292 | 351 |
| `Af-copies` | 16,1 | 0,005 | 25 / 88 / 0 | 34 | 0,0 | 7,27 | 397 | 965 |
| `Ab-copies` | 50,9 | 0,005 | 16 / 6651 / 0 | 755 | 0,0 | 7,79 | 358 | 153 |
| `K-copies` | 26,5 | 0,005 | 9 / 64 / 1 | 1432 | 0,0 | 7,78 | 273 | 69 |
| `T-rlc` | 26,5 | – | 519 / 0 / 0 | 35 | 0,0 | 7,91 | 258 | 37 |
| `Tb-rlc` | 45,5 | 0,005 | 3914 / 0 / 0 | 38 | 0,0 | 7,76 | 347 | 116 |
| `A-rlc` | 44,9 | 0,005 | 58 / 384 / 0 | 38 | 0,0 | 7,92 | 264 | 40 |
| `Af-rlc` | 44,3 | 0,005 | 64 / 414 / 0 | 37 | 0,0 | 7,90 | 374 | 48 |
| `Ab-rlc` | 53,6 | 0,005 | 30 / 1480 / 0 | 43 | 0,0 | 7,33 | 361 | 106 |
| `K-rlc` | 26,8 | 1,0 | 3 / 59 / 2 | 116 | 0,0 | 7,85 | 276 | 40 |

- Con copias, el TCP transparente ve la pérdida de Starlink a 120 ms de RTT
  (las copias del celular llegan tarde): cubic no pasa de 5,5 Mbit/s y el
  flujo lejano en vivo pierde un cuarto. Partido, esa pérdida la ve un TCP
  de ~20 ms de RTT (`mid_rtt_ms` 20,2): 15 Mbit/s, y el vivo entero.
- Con `rlc` la pérdida de Starlink ya no llega a TCP: el vivo transparente
  sale entero y partir solo suma caudal sin tope (26,5 → 44,9).
- El tramo del medio con cubic es el techo con copias (`A`/`Af` 15–16);
  con BBR en el medio (`Ab-copies`) sube a 51, pero BBR no ve la pérdida y
  llena la cola de 2 s del celular (6651 retransmisiones en el medio, RTT p99
  755 ms). `Tb-copies` hace lo mismo de punta a punta (632 ms).
- KCP (sin control de congestión) también llena las colas (RTT p99 1,4 s
  con copias) y retransmite 130 000–150 000 segmentos KCP por corrida.

### `beyond`: 1 % perdido más allá del VPS

| Brazo | TCP Mbit/s | p | retx LAN / medio / lejano | RTT p99 ms | Vivo: perdido % | peor segundo | demora p50 / p99 ms | cercano demora p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `T-copies` | 1,1 | – | 54 / 0 / 0 | 35 | 83,2 | 0,57 | 3324 / 4858 | 1041 |
| `Tb-copies` | 35,9 | 0,005 | 1686 / 0 / 0 | 49 | 0,0 (p 0,003) | 6,29 | 244 / 580 | 143 |
| `A-copies` | 2,3 | 0,020 | 6 / 58 / 56 | 32 | 59,0 | 1,24 | 10574 / 19404 | 474 |
| `Af-copies` | 15,0 | 0,005 | 40 / 96 / 354 | 34 | 0,0 (p 0,003) | 7,19 | 73 / 468 | 285 |
| `Ab-copies` | 51,0 | 0,005 | 12 / 7514 / 1286 | 1041 | 0,0 | 7,23 | 70 / 459 | 139 |
| `K-copies` | 2,2 | 0,066 | 2 / 14 / 73 | 663 | 3,7 | 1,04 | 11292 / 25167 | 14415 |
| `T-rlc` | 1,1 | – | 35 / 0 / 0 | 21 | 79,6 | 0,64 | 3274 / 5860 | 35 |
| `Tb-rlc` | 38,5 | 0,005 | 3900 / 0 / 0 | 38 | 0,0 (p 0,003) | 6,57 | 162 / 529 | 109 |
| `A-rlc` | 2,8 | 0,005 | 10 / 34 / 72 | 35 | 40,3 | 1,08 | 9663 / 22875 | 43 |
| `Af-rlc` | **43,7** | 0,005 | 44 / 441 / 1206 | 37 | **0,0** (p 0,003) | 7,18 | 63 / 410 | 47 |
| `Ab-rlc` | 53,4 | 0,005 | 20 / 1527 / 1404 | 39 | 0,0 | 7,21 | 63 / 376 | 104 |
| `K-rlc` | 2,2 | 0,005 | 2 / 12 / 76 | 40 | 8,6 | 1,05 | 11487 / 25038 | 14519 |

- El techo es el tramo lejano: con cubic, 1 % y 100 ms de RTT no da más de
  ~1,1–1,4 Mbit/s, esté partido o no (`T`, `A`, `K`: 1,1–2,8). Lo que cambia todo
  es **BBR del VPS al host** (`Af`), que no toma la pérdida al azar por
  congestión: 15 (copias, el medio cubic es el techo) a 43,7 Mbit/s.
- Partido con cubic lejos es **peor para el vivo que no partir**: el codificador
  no ve el tramo lento (los proxies tragan 1,5–5,7 MB por conexión) y la
  transmisión se atrasa 10–20 s antes de que el emisor empiece a descartar;
  transparente descarta antes, con 3–5 s de demora.
- Contra el control `Tb-rlc`: `Af-rlc` +13 % en TCP (p 0,005), el mismo 0 %
  en vivo, demora p50 63 contra 162 ms y p99 410 contra 529 (p 0,005), peor
  segundo 7,18 contra 6,57 (p 0,005). `Ab-rlc` 53,4 (p 0,005 contra `Tb-rlc`).
- Las 354–1404 retransmisiones del tramo lejano de `Af`/`Ab` no pasan por
  la radio; las 1686–3900 de `Tb` sí (y con copias, `Tb` sube el RTT a 49 ms).

### `beyond` con 0,1 % (más realista para Vultr → Miami)

| Brazo | TCP Mbit/s | p | retx LAN / medio / lejano | Vivo: perdido % | p | peor segundo | demora p50 / p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `T-copies` | 4,6 | – | 100 / 0 / 0 | 57,6 | – | 1,54 | 1744 / 3512 |
| `Tb-copies` | 42,7 | 0,005 | 2990 / 0 / 0 | 0,0 | 0,004 | 6,11 | 75 / 628 |
| `A-copies` | 11,2 | 0,005 | 28 / 165 / 28 | 0,0 | 0,004 | 5,67 | 158 / 1537 |
| `Af-copies` | 13,8 | 0,005 | 26 / 96 / 30 | 0,0 | 0,003 | 7,27 | 64 / 468 |
| `T-rlc` | 5,0 | – | 221 / 0 / 0 | 22,0 | – | 3,62 | 406 / 2229 |
| `Tb-rlc` | 44,9 | 0,005 | 3706 / 0 / 0 | 0,0 | 0,003 | 6,91 | 62 / 353 |
| `A-rlc` | 20,5 | 0,005 | 33 / 124 / 48 | 0,0 | 0,003 | 5,45 | 175 / 3483 |
| `Af-rlc` | 44,5 | 0,005 | 40 / 388 / 99 | 0,0 | 0,003 | 7,86 | 62 / 359 |

- Hasta con 0,1 %, el vivo transparente pierde 22–58 % y el TCP no pasa de
  5 Mbit/s. Partido con cubic lejos ya alcanza (0 %), pero con 1,5–3,5 s de
  demora p99; con BBR lejos, 359–468 ms.
- `Af-rlc` contra `Tb-rlc`: TCP igual (44,5 contra 44,9, p 0,81), peor
  segundo 7,86 contra 6,91 (p 0,008), demora p99 igual (p 0,47).

### `protectcut`: el microcorte

| Brazo | TCP Mbit/s | p | RTT p99 ms | Vivo: perdido % | p | cortes > 500 ms | demora p50 / p99 ms | cercano perdido % | RTT p99 en vivo |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `T-copies` | 6,1 | – | 296 | 43,9 | – | 1,5 | 1379 / 3077 | 26,1 | 306 |
| `Tb-copies` | 20,9 | 0,005 | 742 | 11,3 | 0,013 | 4,5 | 546 / 6084 | 23,0 | 1457 |
| `A-copies` | 12,4 | 0,013 | 152 | 18,6 | 0,005 | 0 | 83 / 7009 | 17,0 | 161 |
| `Af-copies` | 11,2 | 0,005 | 146 | 18,4 | 0,005 | 1,0 | 80 / 7095 | 18,4 | 270 |
| `Ab-copies` | 34,2 | 0,005 | 688 | 13,3 | 0,005 | 0 | 68 / 7421 | 13,6 | 366 |
| `K-copies` | 20,9 | 0,005 | 1858 | **0,0** | 0,003 | 2,0 | 160 / 3738 | 0,0 | 1856 |
| `T-rlc` | 8,4 | – | 57 | 38,7 | – | 0,5 | 240 / 6132 | 27,2 | 120 |
| `Tb-rlc` | 24,5 | 0,005 | 1329 | 22,7 | 0,005 | 1,0 | 75 / 3864 | 23,9 | 228 |
| `A-rlc` | 19,8 | 0,005 | 113 | 18,3 | 0,005 | 0 | 75 / 4697 | 19,9 | 122 |
| `Af-rlc` | 20,2 | 0,005 | 110 | 18,4 | 0,005 | 0 | 64 / 6585 | 16,6 | 119 |
| `Ab-rlc` | 35,7 | 0,005 | 145 | 17,5 | 0,005 | 0 | 64 / 6242 | 19,4 | 160 |
| `K-rlc` | 21,8 | 0,005 | 1823 | **0,0** | 0,003 | 2,5 | 667 / 3442 | 0,0 | 1136 |

- 10 s con 60 % perdido en el enlace grande y 16 Mbit/s pedidos sobre los
  5 del celular: no hay capacidad, y los dos flujos se reparten la falta.
  Partido, cada TCP corto se recupera más rápido: la mitad de lo perdido de
  `T` y ningún corte de más de 500 ms, pero lo que no cabe espera en los
  proxies (demora p99 4,7–7,4 s).
- KCP sigue mandando a través del 60 % sin frenar: ningún cuadro perdido,
  2–2,5 cortes y 3,4–3,7 s de demora p99, a costa de llenar las colas (RTT
  del túnel 1,1–1,9 s) y de ~40 000–60 000 retransmisiones KCP por corrida.

### Comprobación con el #109 (`e4813f9`, `tcp_ack_rule`)

Los brazos `T` y `Af` (copias y `rlc`) otra vez con el motor que trae el #109
(n = 6, mismos casos): ninguna diferencia significativa con `aa39d14` en el
TCP ni en lo perdido en vivo (p ≥ 0,066) salvo `Af-rlc` en `protect`, 44,3 →
42,5 Mbit/s (p 0,008). La ventaja de partir se mantiene: `protect`, `T-rlc`
30,8 contra `Af-rlc` 42,5 (p 0,013); `beyond`, 1,1 contra 42,3 (p 0,005), vivo
79 → 0 %. Tablas en `~/campo/2026-10-25/pepa/tables109.md`.

### Costos

- **La semántica de punta a punta:** el emisor recibe el ACK del router; lo
  que el router o el VPS tenían en sus búferes se pierde si un proxy se
  reinicia, y el codificador no se entera a tiempo de que el tramo lejano no
  da abasto: en `beyond` con cubic lejos la demora llega a 10–20 s (los
  búferes de los proxies se llenan antes de que la contrapresión llegue al
  emisor; es la lectura de la demora, no se registró la línea de tiempo). Las opciones de TCP (SACK, ventana, ECN) se negocian por
  tramo.
- **Memoria:** el pico de `rmem_alloc + wmem_queued` es de 1,7–5,4 MB en el
  router y 0,5–7,8 MB en el VPS por conexión sin tope (el autoajuste del
  kernel: `tcp_rmem` máx. 6 MB, `tcp_wmem` 4 MB), 7–8 MB en el router con
  kcptun; en vivo, 0,1–5,3 MB en el router y 0,3–5,7 MB en el VPS (hasta
  17 MB con kcptun). RSS: `ss-redir` 3,6 MB, `ss-server`
  4,4 MB, cada kcptun 27–37 MB. Cien conexiones así serían cientos de MB en
  la Pi: hay que acotar los búferes por socket.
- **CPU** (los dos proxies juntos, % de un núcleo del PC): 0,8–1,2 % a
  11–16 Mbit/s y 2,5–2,7 % a 44–54 Mbit/s (shadowsocks-libev, con ChaCha20);
  kcptun 7–9 %. En la Pi no se midió (estimación: varias veces más).
- **Montaje:** un demonio en cada extremo, `REDIRECT` (o TPROXY), NAT, el
  MSS fijado al MTU del túnel, ip6tables aparte para IPv6, una clave más (y
  un cifrado encima del MAC de cengarde, porque shadowsocks no tiene modo
  sin cifrar). Solo TCP: QUIC, SRT y WebRTC siguen transparentes.
- **Lo que no cambia en el motor:** el tramo del medio es TCP visible por
  el túnel nativo, así que `tcp_aware`, el presupuesto de TCP y
  `tcp_retx_one` le aplican igual (con un RTT corto: el presupuesto baja a
  ~25 ms).

## Qué hacemos con esto

- **El valor de partir es elegir el control de congestión del tramo largo.**
  Con pérdida más allá del VPS, BBR en el VPS multiplica el caudal y salva
  el vivo; sin pérdida más allá, `rlc` en el túnel ya entrega el vivo y
  partir suma caudal sin tope. Un PEP sin BBR lejos no vale el costo para
  vivo (atrasa en vez de descartar).
- **Para terreno no se usará el A tal cual** (dos demonios de terceros,
  cifrado doble, búferes sin tope); queda en el laboratorio como la línea
  base del B.
- **El PEP B** (tarea #71, «protocontrolador»: el mismo corte, pero con el
  protocolo de cengarde y ARQ completo en el medio, y el TCP del VPS al
  destino con BBR) tiene que batir al mejor A con `rlc`:

| Caso | Medida | A batir | Brazo | Además |
| --- | --- | ---: | --- | --- |
| `protect` | TCP sin tope | 53,6 Mbit/s | `Ab-rlc` | RTT p99 ≤ 43 ms (`Ab-copies`: 755) |
| `protect` | vivo | 0 % perdido, peor segundo 7,92, demora p99 264 ms | `A-rlc` | cercano p99 ≤ 40 ms |
| `beyond` 1 % | TCP sin tope | 53,4 Mbit/s | `Ab-rlc` | `Af-rlc` 43,7; `Tb-rlc` 38,5 |
| `beyond` 1 % | vivo | 0 %, peor segundo 7,21, demora p99 376 ms | `Ab-rlc` | cercano p99 ≤ 47 ms (`Af-rlc`) |
| `beyond` 0,1 % | TCP sin tope / vivo | 44,5 Mbit/s; 0 %, peor segundo 7,86, p99 359 ms | `Af-rlc` | |
| `protectcut` | TCP sin tope | 35,7 Mbit/s | `Ab-rlc` | RTT p99 ≤ 145 ms |
| `protectcut` | vivo | 17,5 % perdido, 0 cortes | `Ab-rlc` | KCP: 0 % con 3,4 s de demora y RTT 1,1–1,9 s |
| todos | memoria por conexión | ≤ 2–4 MB por extremo, con contrapresión al emisor en ~1 s | `A` | sin esconder 10–20 s de atraso |
| todos | CPU | ≤ 2,7 % de un núcleo del PC a ~50 Mbit/s | `A` | |

  Lo que dicen los brazos sobre cómo: el tramo del medio con cubic es el
  techo con copias (15 contra 51 con BBR), y BBR o KCP en el medio sin saber
  de los enlaces llenan la cola del celular (RTT p99 0,7–1,9 s). El ritmo del
  medio tiene que salir de la capacidad que el motor ya estima por enlace
  (`capacity.h`), no de la pérdida, y el ARQ del medio debería acercarse al 0 %
  de KCP en el microcorte con el RTT de `A-rlc` (~120 ms).

## Pendiente

- **Bajada** (el VPS manda al router): no medida; el emisor del medio sería
  `ss-server` y el lejano el host.
- **La Pi:** CPU y memoria de los proxies en la Pi 4 (estimación arriba).
- **Búferes acotados:** `A` con `TCP_NOTSENT_LOWAT` o `SO_RCVBUF` chicos en
  los proxies, para ver cuánto del atraso en vivo se convierte en
  descartes visibles al codificador (lo que B tendrá que hacer).
- **Terreno:** el RTMP real hacia Facebook en Miami con BBR en el VPS
  (hoy el VPS solo reenvía); la pérdida real Vultr → Miami está por medir
  (aquí se supuso 1 % y 0,1 %).
- kcptun con su propio control de congestión (`--nc 0`, modo `manual`) y con
  su FEC, como otra referencia de B: no se probó.
- Un proxy de UDP fiable tipo hysteria no está en Debian: no se probó.

## Cambios

- 2026-10-25: creada.
