# 020 — Copias parciales y paridad por el enlace ahogado

- **Fecha:** 2026-10-23
- **Estado:** vigente
- **Fuentes:** `engine/src/spare.h` (`cg_spare_tick`, `cg_spare_take`,
  `cg_spare_k`, `cg_spare_how`, `cg_spare_first`),
  `engine/tests/test_spare.c`, `engine/src/client.c` (`spare_masks`,
  `spare_feed`, `spare_close`, `spare_tick`); celda virtual
  (`bench/celda/proteger.py`, `cbr.py`, `proteger_an.py`; historia 016) con
  los resultados en `~/campo/2026-10-23/spare/cell*/` del PC del banco;
  laboratorio de bonding (`bench/lab.d/bond.sh`, casos `protect`,
  `protectcut`, `protectslow` y `protectpin`;
  `~/campo/2026-10-23/spare/lab*/`); terreno (historia 017).

## TL;DR

- Con el enlace de respaldo **ahogado** (un celular sobre su codo, cola
  honda) y el principal **perdiendo sin cola** (Starlink), lo que cabe en lo
  que le sobra es **paridad**, no copias: un XOR de k paquetes cuesta 1/k y
  rehace cualquier pérdida suelta; una copia parcial solo cubre lo que copia.
- Perilla nueva del cliente, `redund_spare` (`no`, `copies`, `parity`,
  `mix`; `spare.h`), apagada por omisión: cada enlace que no es el primero
  lleva copias o paridad (`CG_T_FEC` de siempre, el servidor con `fec =
  yes`, **sin protocolo nuevo**) en una asignación que sostiene su cola en
  `redund_spare_ms` (50). `mix`: copias si cabe todo o en un microcorte,
  paridad si no.
- **Celda virtual** (WOM ahogado a 7 Mbit/s, el fijo perdiendo 0,7 % y un
  microcorte de 10 s al 60 %; n = 3 contra `redund_pace_ms = 250`, lo del
  terreno): `mix` pierde fuera del corte **0,066 % contra 0,239 %** con 0,42
  bytes en el celular por byte (0,68) y su RTT p95 en 192 ms (411); TCP
  **17,5 contra 10,1 Mbit/s**, igual en el corte. Con Entel B2 (1,3 Mbit/s):
  **0,061 % contra 0,584 %** al mismo costo. `parity` llega a 0,010 % fuera
  del corte pero en él no rehace nada (50–59 % perdido).
- **Laboratorio** (`protect`, `protectcut`, n = 8, control nulo): el flujo de
  6 Mbit/s pierde 0,011 % con `mix` y 0,000 % con `parity` contra 0,280 %
  (p < 0,001); TCP 16,8 contra 6,9 Mbit/s (2,4×) y 12,1 contra 6,1 con el
  microcorte.
- El costo: **en el microcorte** ninguna protección que cabe iguala a las
  copias completas detrás de segundos de cola (UDP en el corte: 26 % con
  `mix` contra 19 % con 250 y 17 % con todas; laboratorio 7,6 contra 6,5 %).
  Por eso queda apagada; se propone `mix` para probar en terreno.
- La FEC v7 (`fec = yes`) en redundancia no hace nada (0 paridades): solo
  protege lo repartido. Y una paridad contaba como DATA perdido en la
  capacidad del enlace que la llevaba: corregido (`tx_fec_*` aparte).

## Contexto

En Nostálgica (historia 017) la subida va en redundancia por Starlink, Entel
y WOM. Starlink pierde sin hacer cola (0,3–1,3 %, 0,62 % en el día; 0,4 %
aun con 1–2 Mbit/s) y tiene microcortes de 20–40 s con 50–77 % perdido.
WOM tiene su codo cerca de 5 Mbit/s y le mandábamos 6–9: las copias completas
le armaban 3–8 s de cola, llegaban tarde o se caían por la cola llena.
Cortarlas por su llegada esperada (`pace_peers_ms` 60, el preset de Starlink)
dejaba 40–64 % de la subida solo en Starlink: 0,19–0,29 % perdido en todos
los enlaces (20 y 21-10). Con 250 (22-10, ~90 % de copias en WOM y Entel ya
en B28) bajó a 0,0017 %, pero el episodio de las 10:30 (218 paquetes) fue
con WOM 8 s atrás. La pregunta: **cuando el enlace de respaldo está
ahogado, ¿qué protección cabe en lo que le sobra?** (cola de análisis del
20-10, ítem 9).

## Diseño

### Lo que había

- **Copias a tiempo** (`eta.h`, `redund_pace_ms`, o `pace_peers_ms` del
  preset de Starlink): una copia va por un enlace solo si llegaría a menos
  de la ventana de la primera. Con la capacidad bien medida eso ya es «lo
  que cabe»: la cola del enlace lento se queda cerca de la ventana. Copia en
  orden de llegada, sin prioridad, y depende de la capacidad estimada.
- **FEC v7** (`fec.h`, `fec = yes`): paridad XOR «espacial» sobre DATA
  **repartido**, como mucho uno por enlace. En redundancia cada paquete va
  por todos y `fec_feed` ni se llama (`client.c`: solo con `split`):
  **no hace nada**. Medido: 0 paridades (tabla de abajo, brazo `fec`).
  El receptor, en cambio, no mira enlaces: rehace cualquier grupo de hasta
  15 secuencias en 512 con una paridad (`cg_fec_rx_*`). La paridad en
  redundancia sale sin protocolo nuevo.

### `redund_spare` (cliente, `spare.h`)

- **Primer enlace** (`cg_spare_first`): el protegido. El de menor retardo
  sin cola (mínimo de 10 s, relativo al menor) entre los que no tienen una
  cola parada (sobre el mínimo de 60 s, como mucho 4 × `redund_spare_ms`);
  se queda con 5 ms de histéresis. El que como primero junta esa cola queda
  vetado 60 s. Durante un microcorte suyo (pérdida > 5 % en una muestra) y
  3 s después se queda, aunque la salud lo saque un momento. La capacidad no
  decide (ver «Tropiezos»).
- **Asignación** de cada otro enlace: una tasa que sigue a su cola sobre el
  mínimo de 60 s (`ql_us` de `capacity.h`): −1/4 si pasa de
  `redund_spare_ms` (50; como mucho cada 500 ms), +1/16 + 64 kbit/s por tick
  bajo la mitad si se usa; entre 128 kbit/s y 3/2 de lo que sube. Un balde a
  esa tasa (profundidad 20 ms) decide cada paquete. Un paquete chico va
  siempre (se cuenta); uno prioritario (`CG_F_FREE` de `tcp_aware`: ACK y
  retransmisiones) puede dejar el balde en negativo hasta una ráfaga.
- **Modos:** `copies` (copias parciales del resto), `parity` (sin copias
  salvo las prioritarias; XOR de k paquetes que fueron solo por el primero, k
  el menor cuya paridad cabe en 7/8 de la asignación, 2–15, plazo
  `fec_wait_ms`; la paridad va por el enlace con más balde) y `mix` (copias
  en un microcorte del primero, donde un XOR no rehace nada; paridad si no).
  En los tres, copias cuando la asignación lleva toda la subida.
- **Plazo:** lo que deja pasar el balde espera como mucho ~`redund_spare_ms`
  en la cola de ese enlace; la ventana de `redund_pace_ms` sigue encima si
  está puesta.
- El servidor no cambia: rehace con `fec = yes` (necesario) y entrega lo
  rehecho fuera de orden (en redundancia el reordenador no espera).

### Correcciones que salieron del camino

- **Paridades en los contadores del enlace:** `fec_send` las sumaba a
  `tx_packets`/`tx_bytes`; el servidor cuenta por camino solo DATA, así que
  la capacidad (`capacity.h`) las tomaba por perdidas: un enlace que solo
  llevaba paridad caía a la mitad cada muestra (`short`). Ahora van en
  `tx_fec_packets`/`tx_fec_bytes`. Vale también para la FEC del reparto.
- `capacity.h` guarda la pérdida de la última muestra (`loss_pm`) para la
  mezcla; `eta.h` da el primer enlace de cada copia (`cg_eta_copies_first`) y
  descuenta una copia que no salió (`cg_eta_unadd`).

## Montaje

- **Celda virtual** (`bench/celda/proteger.py`): cliente en `ue1` con `lte`
  (la celda, perfil `wom` o `entel_b2` de la historia 016: capacidad
  limitada y cola honda) y `fijo` (veth con netem: `delay 5ms rate 30mbit
  loss 0.7% limit 150`, el Starlink). La subida de la celda tarda 10–20 ms
  (sus 35 ms de camino van en la bajada): el fijo tiene que ser el más
  rápido, como Starlink. Túnel nativo. Por corrida, 40 s: o un flujo UDP de
  7 Mbit/s y 1200 B (`cbr.py`, como la subida de la radio: secuencia y hora
  de envío, pérdida por segundo, retardo de ida) o TCP cubic (iperf3); pings
  en el túnel; a los 15 s, un microcorte de 10 s (el fijo pierde 60 % de la
  subida). «En el corte» cuenta del segundo 14 al 27 de envío (lo perdido
  por la cola al volver cae ahí) sobre lo enviado en los 10 s; «fuera del
  corte», el resto. Costo: bytes por `lte` (DATA y paridad) por byte del
  fijo, que lleva todo una vez.
- **Brazos:** `all` (redundancia sin ventana: todas las copias), `full`
  (`redund_pace_ms = 250`, lo del terreno desde el 22-10), `cut` (60, el
  preset), `fec` (`fec = yes` en los dos extremos), `pcopy`, `parity` y
  `mix` (`redund_spare`, servidor con `fec = yes`), y `pcopy150`/`mix150`
  (`redund_spare_ms = 150`). El fijo queda `auto`: con `type = starlink` el
  preset pone 60 a los demás y `redund_pace_ms` no puede subirlo (la
  perilla `pace_peers_ms` de la rama `claude/pace-peers-knob` no está en
  esta base); la ventana es la misma.
- n = 3 por brazo en la celda (los brazos rotan en cada repetición). Con
  n = 3 contra 3, el p de Mann-Whitney de dos colas más chico posible es
  0,1: «p 0,1» es «los tres de un lado».

## Hallazgos

### Brazos de base en la celda (WOM, primera tanda)

UDP de 7 Mbit/s, n = 3, una misma sesión de la celda (`cell/wom.jsonl`):

| Brazo | Perdido % | Fuera del corte % | En el corte % | OWD p99 ms | RTT LTE p95 ms | Bytes LTE/fijo |
|---|---:|---:|---:|---:|---:|---:|
| `full` (250) | 5,19 | 0,183 | 20,1 | 535 | 399 | 0,68 |
| `all` (todas) | 4,39 (p 0,1) | 0,157 (p 0,4) | 17,1 (p 0,1) | 3034 | 3056 | 1,00 |
| `cut` (60) | 5,37 (p 0,2) | 0,244 (p 0,38) | 20,8 (p 0,7) | 196 | 161 | 0,66 |
| `fec` | 4,46 (p 0,1) | 0,137 (p 0,1) | 17,4 (p 0,1) | 3024 | 3053 | 1,01 |

TCP cubic, n = 3 (`full` / `all` / `cut`): 10,7 / 9,8 / 11,1 Mbit/s;
retransmisiones 778 / 551 / 735; ping p99 286 / 2861 / 83 ms; RTT LTE p95
536 / 3048 / 217 ms; bytes LTE/fijo 0,42 / 1,00 / 0,39.

- `fec = yes` es `all`: 0 paridades en las tres corridas.
- Todas las copias: la cola del celular llega a ~3 s, y aun así pierde 0,16 %
  fuera del corte (la cola llena tira copias). 250 la deja en ~400 ms y 60 en
  ~160 ms, perdiendo algo más (0,18 y 0,24 %): las copias que cabían.

### Brazos finales en la celda (motor final, `cell6/`)

WOM, UDP de 7 Mbit/s, n = 3 contra `full` (mediana, entre paréntesis el
rango o el p):

| Brazo | Perdido % | Fuera del corte % | En el corte % | OWD p99 ms | RTT LTE p95 ms | Bytes LTE/fijo | Rehechos |
|---|---:|---:|---:|---:|---:|---:|---:|
| `full` | 4,96 (4,87–5,51) | 0,239 (0,203–0,254) | 19,2 (18,9–21,3) | 509 | 411 | 0,68 | 0 |
| `pcopy` | 6,44 (p 0,1) | 0,264 (p 0,2) | 25,0 (p 0,08) | 248 (p 0,1) | 262 (p 0,1) | 0,57 | 0 |
| `parity` | 12,56 (p 0,1) | **0,010** (0,000–0,041; p 0,1) | 50,2 (p 0,1) | 21 (p 0,1) | 60 (p 0,1) | 0,31 | 1399 |
| `mix` | 6,60 (p 0,1) | **0,066** (0,036–0,071; p 0,1) | 26,2 (p 0,1) | 295 (p 0,1) | 192 (p 0,1) | 0,42 | 728 |

WOM, TCP cubic, n = 3 contra `full`:

| Brazo | TCP Mbit/s | En el corte | Retrans. | Ping p99 ms | RTT LTE p95 ms | Bytes LTE/fijo |
|---|---:|---:|---:|---:|---:|---:|
| `full` | 10,09 (10,09–11,50) | 3,98 | 717 | 68 | 571 | 0,45 |
| `pcopy` | 11,66 (p 0,18) | 4,09 (p 0,7) | 640 (p 0,1) | 70 | 243 (p 0,1) | 0,33 |
| `parity` | **17,61** (p 0,08) | 3,98 (p 0,8) | 642 (p 0,1) | 63 | 163 (p 0,1) | 0,23 |
| `mix` | **17,48** (p 0,08) | 4,28 (p 0,4) | 705 (p 0,1) | 54 | 204 (p 0,1) | 0,24 |

Entel B2 (~1,3 Mbit/s, cola de 220 KB), UDP de 7 Mbit/s, n = 3 contra `full`:

| Brazo | Perdido % | Fuera del corte % | En el corte % | OWD p99 ms | RTT LTE p95 ms | Bytes LTE/fijo | Rehechos |
|---|---:|---:|---:|---:|---:|---:|---:|
| `full` | 13,56 | 0,584 (0,574–0,635) | 52,7 | 666 | 567 | 0,17 | 0 |
| `pcopy` | 13,25 (p 1) | 0,655 (p 0,2) | 51,3 (p 1) | 223 (p 0,1) | 343 (p 0,1) | 0,16 | 0 |
| `parity` | 14,80 (p 0,1) | **0,081** (p 0,1) | 59,1 (p 0,1) | 13 (p 0,1) | 251 (p 0,1) | 0,17 | 204 |
| `mix` | 12,95 (p 0,2) | **0,061** (p 0,1) | 51,7 (p 0,7) | 194 (p 0,1) | 242 (p 0,1) | 0,17 | 128 |

- **Paridad contra copias, al mismo costo:** con 0,7 % de pérdida suelta,
  copiar una fracción f de los paquetes deja (1 − f) de la pérdida (`pcopy`,
  0,26 % con 57 % copiado); una paridad cada k rehace todo salvo dos
  pérdidas en un mismo grupo, ~(k·p)² / 2 por grupo (estimación: con k = 3–8,
  unos milésimos de lo perdido). Entel, que lleva 1/6 de la subida, alcanza
  para k ≈ 7.
- **En el microcorte** (60 %), un XOR no rehace (dos o más perdidos por
  grupo) y las copias que caben (5 de 7 Mbit/s en WOM, con su cola en
  ~50 ms) cubren menos que 250 ms o 3 s de cola: la mezcla vuelve a copias,
  pero pierde 26 % contra 19 %. Con Entel nadie protege el corte (1,3 de 7).
- **TCP:** lo perdido suelto en el fijo cuesta retransmisiones y ventana;
  rehecho a ~50 ms (la cola del celular) el TCP sube 70 %. En el corte el TCP
  baja a menos de lo que sube el celular, la asignación lo lleva todo y
  `parity` también pasa a copias: igual que `mix`.
- `redund_spare_ms = 150` (`pcopy150`, `mix150`, primera tanda, con la
  primera regla del primer enlace) no mejoró nada medible (en el corte 23 y
  21 % contra 24 y 24 %, p 0,7) y subió el RTT del celular.

### Laboratorio de bonding (n = 8, motor final, `lab/stream2`, `lab/tcp2`)

`protect`: enlace 1 como Starlink (60 Mbit/s, 10 ms, 15 ms de cola, 0,7 %
de pérdida en cada sentido), enlace 2 como WOM ahogado (5 Mbit/s, 25 ms, 2 s
de cola); `protectcut`: además el enlace 1 pierde 60 % el tercio del medio.
WireGuard (sin `tcp_aware`), subida, cubic, 30 s. `null` es otra vez `all`
(el control nulo). Flujo: `BOND_SMALL=625` de 1200 B (6 Mbit/s, `udpgen`)
y un TCP de 1 Mbit/s (`BOND_RATE=1M`). p de Mann-Whitney contra `full`.

| Caso | Brazo | Flujo perdido % | p99 del flujo ms | Ping 1200 B p99 ms | Copias/paquete |
|---|---|---:|---:|---:|---:|
| `protect` | `full` | 0,280 | 150 | 189 | 1,59 |
| | `all` / `null` | 0,179 / 0,176 (p 0,001) | 12 | 23 | 2,00 |
| | `cut` | 0,307 (p 0,40) | 130 | 143 | 1,57 |
| | `pcopy` | 0,317 (p 0,07) | 12 | 23 | 1,52 |
| | `parity` | **0,000** (p < 0,001) | 13 | 38 | 1,07 |
| | `mix` | **0,011** (p < 0,001) | 13 | 38 | 1,07 |
| `protectcut` | `full` | 6,52 | 200+ | 592 | 1,57 |
| | `all` / `null` | 5,86 / 5,53 (p 0,003) | 200+ | 1947 | 1,95 |
| | `cut` | 7,21 (p 0,009) | 200+ | 424 | 1,53 |
| | `pcopy` | 7,58 (p 0,13) | 200+ | 483 | 1,50 |
| | `parity` | 12,73 (p < 0,001) | 90 | 128 | 1,04 |
| | `mix` | 7,63 (p 0,010) | 200+ | 375 | 1,20 |

TCP cubic sin tope (Mbit/s; retransmisiones), contra `full`:

| Caso | `full` | `all` / `null` | `cut` | `pcopy` | `parity` | `mix` |
|---|---:|---:|---:|---:|---:|---:|
| `protect` | 6,9; 206 | 6,9 / 6,8; 140 | 7,5 (p 0,002); 258 | 7,8 (p < 0,001); 146 | **16,5** (p < 0,001); 222 | **16,8** (p < 0,001); 245 |
| `protectcut` | 6,1; 442 | 5,3 / 6,2; 307 | 6,4 (p 0,16); 402 | 6,9 (p 0,001); 268 | **12,5** (p < 0,001); 332 | **12,1** (p < 0,001); 323 |

- El control nulo (`all` contra `null`) se mueve hasta 15 % en el caudal de
  `protectcut` (5,3 contra 6,2): por debajo de eso no hay señal.
- El p99 del flujo satura en 200 ms (`udpgen`).

### Tropiezos (para no repetirlos)

1. **El primer enlace por la llegada esperada de cada paquete** (`eta.h`): con
   el fijo medido bajo lo que llevaba, el ETA se daba vuelta al celular unos
   paquetes por tick; los grupos de paridad mezclaban enlaces y no tenían por
   dónde salir (419 sin enlace contra 9 enviadas).
2. **La celda con el fijo a 20 ms:** la subida de la celda tarda 10–20 ms (sus
   35 ms de camino van en la bajada) y el celular era el más rápido. Montaje
   final: el fijo a 5 ms, como Starlink frente a los celulares.
3. **El primero por capacidad:** en un microcorte la capacidad del fijo cae a
   la mitad por muestra (`short`) y pasaba a primero el celular, que llenaba
   3 s de cola; ya primero, sus propias caídas por cola llena lo retenían.
   Peor: con paridad, al arrancar los dos medían unos kbit/s, el ahogado
   quedaba primero y el otro, que solo llevaba paridad, nunca podía medir más
   (laboratorio: 0 Mbit/s de TCP). La regla final no mira la capacidad: el de
   menor retardo sin cola parada, el que junta cola como primero vetado 60 s,
   y quieto durante su propio microcorte.
4. **Las paridades contadas como DATA** en el enlace que las lleva (ver
   Diseño).
5. En `parity`, copias cuando cabe todo (si no, el enlace que solo lleva
   paridad no mide y no puede volver a ser primero).
6. labq: un brazo llamado `all` escribe también en `all.jsonl` (sus filas
   salen dos veces): se cuentan una vez por (brazo, caso, repetición).
7. Las tandas intermedias de la celda (`cell2/` a `cell5/`) son de reglas
   del primer enlace descartadas; solo cuentan `cell/` (brazos de base) y
   `cell6/` (motor final).

## Decisión

- `redund_spare` queda **apagada por omisión**: gana claro con la pérdida
  suelta (3,6–10 veces menos perdido, 70 % más de TCP, la mitad de bytes y
  de cola en el celular) pero pierde contra las copias completas **dentro**
  de un microcorte (26 contra 19 % en la celda, 7,6 contra 6,5 % en el
  laboratorio), y en terreno los microcortes de Starlink son la parte grande
  de lo perdido en todos los enlaces (historia 017).
- Para terreno se propone **`redund_spare = mix`** en el cliente y `fec =
  yes` en el servidor (`fec` reinicia el servidor; el modo del cliente se
  recarga), con el registrador mirando `upload.spare`, el `fec.recovered`
  del VPS y la pérdida en todos los enlaces durante los microcortes.
- `parity` sola no: el TCP aguanta (vuelve a copias cuando cabe todo), pero
  en un corte un flujo fijo pierde el doble que con `full`.
- `copies` sola no aporta sobre `redund_pace_ms`: las copias parciales
  cubren lo mismo que la ventana con su capacidad bien medida.

## Pendiente

- **Con `type = starlink`** el preset pone 60 ms de ventana a las copias de
  los demás (`pace_peers_ms`), y `redund_spare` recorta **después** de esa
  máscara: las copias de `mix` quedarían en lo que ya dejaba la ventana (la
  paridad no pasa por ella). Probar en la celda con el fijo `starlink` y la
  perilla `pace_peers_ms` (rama `claude/pace-peers-knob`) alta, o que
  `redund_spare` ignore la ventana.
- La **bajada** (el VPS decide sus copias): el mismo `spare.h` en
  `down_prepare`/`down_send`, y el cliente con la memoria de `fec` para
  rehacer (hoy solo el servidor rehace).
- El microcorte: subir la cola permitida mientras dura (la mezcla en copias
  con `redund_spare_ms` × 4) acercaría el corte a 250 ms; la FEC de ráfagas
  (#57, varias paridades por grupo) es la herramienta para eso.
- Varios respaldos (Entel y WOM a la vez): la paridad va por el que tiene más
  balde; repartirla entre los dos, o copias por uno y paridad por otro, sin
  medir.
- n = 3 en la celda (p mínimo 0,1): repetir los brazos finales con n = 8 si
  se va a decidir un preset.

### El primero ahogado que se quedaba (NUC, 2026-10-23)

En el NUC (cuatro celulares, `mix`), Claro en B5 (5 MHz, ~5–8 Mbit/s) tenía
el menor retardo sin carga y quedó de primero cargando toda la subida: RTT
900 ms, 670 ms atrás. Dos cosas lo dejaban ahí: su pérdida por cola llena
pasaba por un microcorte (más de 5 % en una muestra) y renovaba la
retención en cada tick, y la retención saltaba el veto; además, con la
carga larga su mínimo de 60 s subió con la cola y la cola sobre ese mínimo
se leía ~0. Arreglo (`spare.h`): `cg_spare_choked` (cola o base sobre 4 ×
`redund_spare_ms`), `cg_spare_keep` (un primero ahogado se veta aunque
pierda, y se borra la retención; un microcorte sigue reteniendo) y
`cg_spare_pinned` con la perilla nueva `redund_spare_first` (fijar, p. ej.,
Entel a 70 m de su torre; si se ahoga, la elección automática, y vuelve al
despejarse y terminar su veto).

Laboratorio, caso nuevo `protectslow` (enlace 1 a 10 ms pero 5 Mbit/s, 2 s
de cola y 8 % de pérdida; enlace 2 a 25 ms y 20 Mbit/s), `mix`, antes
(`fbb2565`) y después:

| Caso | Flujo perdido % (n) | Ping 1200 B p99 ms | Enlace 1 primero | TCP Mbit/s (n = 4) |
|---|---|---:|---:|---:|
| `protectslow` antes | 0,590 (8) | 945 | 100 % del tiempo | 14,5 |
| `protectslow` después | **0,212** (8; todas bajo todas las de antes) | **362** | 3 % | 15,4 |
| `protect` antes / después | 0,007 / 0,013 (16; A/A con `fbb2565` dinámico: 0,011) | 38 / 38 | 100 % | 15,9 / 15,7 |
| `protectcut` antes / después | 7,65 / 7,86 (4) | 371 / 409 | 99 % | 13,7 / 13,8 |

En `protect` y `protectcut` la decisión es la misma (el enlace 1 no se
ahoga): las diferencias son ruido. Datos en `~/campo/2026-10-23/spare/lab2/`.

### El fijado que nunca quedaba primero (NUC, 2026-10-23, noche)

Con `36b22f6`, `mix` y `redund_spare_first = ENTEL`, con el túnel casi
quieto (0,0–0,4 Mbit/s) el primero (`upload.spare.first`) saltaba entre
Movistar y Claro y nunca era Entel, vivo a 43 ms de RTT; Claro llegó a ser
primero con un RTT de 8,9 s (su cola sobre el mínimo de 60 s, ~3 ms).

**Causa** (reproducida en el laboratorio; el estado de `36b22f6` no
mostraba vetos ni ahogos, así que en terreno queda por confirmar con los
campos nuevos):

1. **Un tick ahogado vetaba un minuto.** `cg_spare_keep` vetaba 60 s al
   primero en el primer tick (100 ms) en que se leía ahogado, y
   `cg_spare_pinned` respetaba el veto. La cola de un celular sobre su
   mínimo de 60 s salta cientos de ms por un momento (la promoción de la
   radio desde reposo, un traspaso) aunque el túnel esté quieto: cada
   salto sacaba al fijado un minuto, y los automáticos caían por turno (el
   ir y venir entre Movistar y Claro). El veto además sobrevivía a la
   pausa y vuelta del enlace: el socket nuevo heredaba el del viejo.
2. **Medidas viejas contaban como medidas.** `spare_tick` tomaba por
   medido a todo enlace con socket y mínimo, con su reporte de retardo
   fresco o no. Uno cuyas respuestas se cortan o llegan segundos tarde
   guarda la cola y el mínimo de su último reporte (la capacidad solo los
   mueve con un reporte fresco): se leía despejado y podía ser primero
   (Claro con 8,9 s), y su mínimo viejo hacía de referencia de las bases.
   El RTT no contaba.

Descartado, con pruebas (laboratorio, cuatro enlaces de 10, 15, 20 y
22 ms, `l4` fijado, n = 1 por binario): la pausa en sí (`ctl link l4 off`
y la interfaz abajo 15 s, arriba y `auto`: el fijado vuelve a primero en
menos de 1 s con los dos binarios) y el nombre (las ranuras se reusan por
nombre y `used` no se borra nunca). Los portadores del último lote en
reposo tampoco lo dejaban afuera (el fijado era primero con el túnel
quieto), aunque se cambiaron por los del tick.

**Arreglo** (`spare.h`, `spare_tick`):

- Un primero ahogado se veta al momento si los demás no copian toda la
  subida (ahí su ahogo es suyo), y mientras la copian entera (con el
  túnel quieto: ser el primero no le suma carga) solo tras 2 s seguidos
  (`CG_SPARE_CHOKE_MS`, `CG_SPARE_KEEP_WAIT`, sigue de primero
  entretanto). Esperar también con carga le costaba a `protectslow` 0,60 %
  de su flujo contra 0,21 % (n = 8). Al fijado se lo veta 15 s
  (`CG_SPARE_PIN_BAN_MS`), no 60.
- Cuentan solo los enlaces vivos con un reporte en los últimos 3 s
  (`cg_spare_fresh`): con la frescura de la salud (500 ms con tráfico) el
  primero se iba en el primer segundo de un microcorte, antes de que su
  pérdida lo retuviera (`protectcut`: 98 % primero).
- El **atraso** por el RTT (`cg_spare_lag`: el suavizado, o el último si
  es mayor, sobre el menor suavizado) ahoga si pasa del umbral y de 1 s
  (`CG_SPARE_LAG_US`: es ida y vuelta, con la cola de bajada). Con el
  suavizado solo, las primeras respuestas atrasadas lo movían 1/8 y el
  enlace lento volvía 2 s; con las sondas sin respuesta en la cuenta, un
  microcorte (60 % perdido) vetaba al enlace que la retención cuida
  (`protectcut`: ping de 1200 B p99 1,8 s contra 0,37). Entre ahogados, el
  menos atrasado (`cg_spare_late`), no la menor cola.
- El conjunto es el de los portadores del tick; un socket nuevo borra su
  veto y reinicia su asignación (`cg_spare_restart`).
- Estado: `upload.spare.pin` (`first`, `down`, `unmeasured`, `banned`,
  `choked`, `absent`, `none`) y por enlace `spare.fresh`, `base_ms`,
  `lag_ms`, `choked`, `banned`.

Laboratorio, `36b22f6` contra el arreglo:

| Prueba | n | `36b22f6` | Arreglo |
|---|---:|---|---|
| `l4` fijado, salto de 300 ms 1 s cada 20 s, túnel quieto | 1 | primero 31 de 90 s | **90 de 90** |
| bajada de `l1` (el de menor base) a 4,5 s, sin fijar | 1 | `l1` primero 23 de 44 s tras el cambio | **0** (1 s de transición) |

Caso nuevo `protectpin` (`bond.sh`: tres enlaces de 20 Mbit/s a 10, 15 y
22 ms, `l3` fijado con saltos de 300 ms 1 s cada 10 s y en pausa el tercio
del medio, por el socket de control), y los de siempre, `mix`, 30 s
(`~/campo/2026-10-23/spare/lab3/`, brazo final `fix4`):

| Caso | Flujo 6 Mbit/s perdido %, mediana (n = 8) | Ping 1200 B p99 ms, media | Primero | TCP Mbit/s (n = 4) |
|---|---|---:|---|---:|
| `protect` antes / después | 0,011 / 0,000 | 29 / 34 | `l1` 100 / 100 % | 16,3 / 16,6 |
| `protectcut` antes / después | 8,20 / 8,06 | 373 / 414 (medianas 386 / 383) | `l1` 100 / 100 % | 12,8 / 12,8 |
| `protectslow` antes / después | 0,213 / 0,200 | 380 / 398 | `l1` 3 / 3 % | 15,2 / 15,2 |
| `protectpin` antes / después | 0 / 0 | 34 / 37 | `l3` fuera de la pausa: **30 / 100 %** (TCP: 30 / 95 %) | 17,1 / 17,1 |

En `protect`, `protectcut` y `protectslow` el arreglo no cambia nada
medible; en `protectpin` el fijado es el primero todo el tiempo fuera de
la pausa con el flujo (con TCP sin tope, que los demás no copian entero,
un salto lo veta al momento, 15 s, y queda 95 %). Con `36b22f6` el primer
salto lo vetaba y no volvía en el resto de la corrida, ni tras la pausa.

## Cambios

- 2026-10-23: creada.
- 2026-10-23: el primero ahogado que se quedaba (NUC), `cg_spare_choked`,
  `cg_spare_keep`, `redund_spare_first` y el caso `protectslow`.
- 2026-10-23: el fijado que nunca quedaba primero (NUC): veto tras 2 s de
  ahogo con el túnel copiado entero, 15 s al fijado, solo medidas de los
  últimos 3 s, atraso por el RTT, `upload.spare.pin` y el caso
  `protectpin`.
- 2026-10-24: con el rescate encendido, `mix` (la paridad XOR) ya no pide
  un hueco mientras una paridad pueda rehacerlo (el sello, historia 023); la
  pista `CG_F_PREV` dice ahora si un paquete quedó solo en el primer enlace
  después de `spare_masks`. Con TCP visible, los huecos se sueltan para TCP:
  `mix` igual que antes (18,0 contra 18,1 Mbit/s en `protect`).
