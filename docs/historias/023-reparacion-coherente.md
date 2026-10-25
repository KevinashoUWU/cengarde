# 023 — Reparación coherente: rescate, paridad pedida (PARQ) y TCP

- **Fecha:** 2026-10-24
- **Estado:** vigente
- **Fuentes:** `engine/src/harq.h` (nuevo), `reorder.h` (`cg_ro_skip`,
  `cg_ro_ask_left`, `ask_repaired`), `client.c` (`seal_batch`,
  `retx_masks`, `on_parq`, `tcp_budget_us`), `server.c` (`down_seal`,
  `down_retx_masks`, `send_nacks`, `harq_one`, `harq_service`, `rep_seal`),
  `rlc.h` (`cg_rlc_rx_need`, `cg_rlc_combine`), `rs.h`/`rs.c`
  (`cg_rs_mhdr`, `cg_rs_rx_need`, `cg_parq_rs_pick`), `tcpfree.h`
  (`cg_tcpf_class`, `struct cg_tcpe`), `flow.h` (`cg_flow_of_dir`),
  `proto.h` (protocolo 9, `CG_T_PARQ`); tests `test_harq.c` y las partes
  nuevas de `test_reorder.c`, `test_rlc.c`, `test_rs.c`, `test_tcpfree.c`,
  `test_proto.c`; laboratorio `bench/lab.d/bond.sh` (`BOND_TUNNEL=native`,
  caso `beyond`) con `bench/labq.py`, resultados en
  `~/campo/2026-10-24/harq/` del PC del banco (`an.py`); terreno, historia
  017. La retransmisión según la evidencia (2026-10-25): `tcpack.h` (nuevo),
  `tcpfree.h` (`cg_tcpe_verdict`, `struct cg_tcpe_view`), `arrival.h`
  (`cg_arr_top`), `proto.h` (`CG_F_TUNHOLE`, `CG_BOND_TCPMARK`), tests
  `test_tcpack.c`; casos `spurious` y `lossall` de `bond.sh`; resultados en
  `~/campo/2026-10-24/tcpack/` (`an.py`, tandas `m1`–`m6`).

## TL;DR

- **Por qué el rescate estaba mudo** (112 pedidos en un día de ~25 000
  pérdidas, historia 017): la pista `CG_F_PREV` («el anterior fue solo por
  tal enlace») se escribía **antes** de que las copias a tiempo y
  `redund_spare` quitaran copias, así que decía «por varios»; y en
  redundancia (`reorder_dup_wait_ms` 0) nada se retenía, y un hueco pedido al
  vencer su tope no esperaba nada. Arreglado: la cabecera se sella después
  de decidir las copias, un hueco perdido en todos los enlaces retiene lo que
  sigue, y lo pedido espera más allá del tope.
- **Laboratorio** (`protect`, copias de 250 ms y rescate como en terreno,
  túnel nativo, n = 6, contra `origin/master`): flujo de 6 Mbit/s perdido
  **0,275 → 0,005 %** (p 0,005), 55 rescatados por corrida (antes 0); TCP
  **7,6 → 14,4 Mbit/s** (p 0,002), y en el microcorte 6,2 → 10,9 (p 0,005).
- **Orden de las reparaciones** (`harq.h`): primero el código (no se pide
  mientras puedan llegar reparaciones que lo cubran: hasta el sello o
  `rescue_code_ms`); sellado y con déficit, la **paridad pedida** (PARQ,
  `CG_T_PARQ`, **protocolo 9**: el déficit de rango de la RLC en
  combinaciones nuevas de lo que falta, o índices nuevos del bloque RS, que
  ahora declara una geometría mayor); sin código, el NACK. Funciona (tests de
  punta a punta en `test_rlc.c`/`test_rs.c`, pedidos y reparaciones contados
  en el laboratorio), pero **no se midió ganancia** frente al NACK: en
  `protect` el código lo rehace todo y en el microcorte las reparaciones
  pedidas se pierden con el enlace (7,9–8,2 % contra 7,6–7,9 %, p ≥ 0,39).
- **TCP de adentro:** nada se pide ni se retiene más allá del presupuesto de
  TCP (RTT de sus flujos más un cuarto, medido del SYN al SYN-ACK); con TCP,
  esperar al código **no retiene** (retener hasta el sello bajó un TCP con el
  host a 100 ms de 23,6 a 16,9 Mbit/s, p 0,002; sin retener, 25,2, p 0,39);
  la primera retransmisión de un segmento va por **un solo enlace**
  (copiadas por corrida: 188 → 2,5, caudal igual); las pérdidas **más allá
  del túnel** se cuentan aparte (`tcp_e2e.beyond`: 125 de 205 huecos SACK
  con 1 % perdido después del túnel) y no tocan los estimadores.
- **La retransmisión según la evidencia** (2026-10-25, `tcp_ack_rule`, sí
  por omisión): el extremo que envía lee los ACK del flujo que vuelven por
  el túnel, y el otro le marca en cada ACK con SACK si el túnel perdió el
  hueco en todos los enlaces (`CG_F_TUNHOLE`, el bit reservado de DATA; sin
  versión nueva). Ya ACK o SACK: un enlace aunque se repita; perdida por el
  túnel: copiada desde la primera; lo demás, como `tcp_retx_one`. En el
  laboratorio (n = 8) el caudal de TCP queda como la conjetura en todos los
  casos (p ≥ 0,17); copia 130 retransmisiones por corrida donde el túnel
  pierde en los dos enlaces (`lossall`; la conjetura, 4) y ninguna en el
  caso de retransmisiones espurias, donde copiarlas todas (`off`) hace que
  TCP retransmita 742 contra 572 (p 0,03). Marcar lo que todavía venía por
  un enlace detenido, o lo que el túnel no podía asegurar, duplicaba las
  retransmisiones de TCP (1104 contra 484, p 0,003).
- Costo: el bloque RS con la geometría mayor cuesta ~40 % más al codificar
  (0,14 → 0,20 µs por paquete, k 21 m 3), decodificar igual. Perillas nuevas
  `rescue_code_ms` (100), `parq`, `rescue_tcp_race`, `tcp_retx_one` (sí);
  el rescate sigue apagado por omisión (`rescue_wait_ms` 0).

## Contexto

En Nostálgica (historia 017) el rescate pidió 112 reenvíos en un día con
~25 000 pérdidas en todos los enlaces de la subida: el reordenador casi no
retenía huecos para pedirlos. Mientras, la FEC v2 (historia 021) protegía lo
que iba solo por el primer enlace, y el TCP de adentro reparaba lo suyo: tres
mecanismos que no se miraban. La pregunta: **¿por qué el rescate casi no
actúa, y cómo se reparten el trabajo el código, el rescate y TCP para no
hacer cada uno lo del otro?**

## Por qué el rescate estaba mudo

Reproducido en el laboratorio (`protect`, subida en redundancia, el primer
enlace pierde 0,7 % sin cola y el segundo, de 5 Mbit/s, tiene 2 s de cola;
copias a 250 ms y rescate como en terreno: `rescue_wait_ms = 60`,
`rescue_kb = 32768`; `origin/master`, n = 3): el flujo perdía **0,256 %**, el
reordenador del servidor **no retuvo nada** (`held_total` 0) y **no pidió
nada** (`asked` 0). Tres causas, en cadena:

1. **La pista se escribía antes de las copias.** El paquete siguiente a un
   hueco dice por qué enlace fue el anterior cuando fue solo por uno
   (`CG_F_PREV`). Pero el emisor la escribía al armar la cabecera, antes de
   que las copias a tiempo (`eta.h`, `redund_pace_ms`/`pace_peers_ms`) y
   `redund_spare` quitaran copias. En redundancia, un paquete que terminaba
   yendo solo por Starlink decía «por varios»: el receptor nunca sabía que
   se había perdido en todos los enlaces. El anillo del rescate tampoco
   guardaba su enlace (la congestión no se distinguía).
2. **En redundancia nada espera.** `reorder_dup_wait_ms` es 0: el hueco se
   abandona apenas llega el paquete siguiente (`cg_ro_skip`), y el rescate
   solo pide lo que el reordenador está por abandonar mientras retiene algo.
   RACK sobre todos los enlaces tampoco llega a tiempo: el segundo enlace va
   segundos atrás.
3. **Pedido al tope, sin espera.** Un hueco pedido esperaba `rescue_us`
   «nunca más allá del tope del paquete que lo sigue». Si el plazo del hueco
   era justamente ese tope (el enlace más lento muy atrás, o
   `reorder_dup_wait_ms` en redundancia), el pedido salía al vencer y el
   hueco se abandonaba en el mismo instante: el reenvío llegaba tarde
   (`late`), contado como pedido pero no rescatado.

**El arreglo:**

- El emisor sella la cabecera (`seal_batch` en el router, `down_seal` en el
  VPS) **después** de decidir las copias: la pista es la de
  `cg_harq_only` sobre los enlaces por los que de verdad va el paquete, el
  MAC se calcula una sola vez, y el anillo del rescate guarda su enlace.
- Un hueco que la pista dio por perdido en todos los enlaces (su enlace ya
  trajo algo posterior: `cg_ro_pend_take`) se pide al instante **y retiene lo
  que sigue** aunque el tope sea 0 (`cg_ro_skip`); los demás huecos de la
  redundancia siguen sin esperar.
- Un hueco pedido espera `rescue_us` desde el pedido, acotado por el tope
  **más** `rescue_us` (`cg_ro_ask_left`), no por el tope solo.

Laboratorio, mismo binario de antes contra el nuevo (n = 3): `protect` con
copias de 250 ms pasa de 0,256 % a **0,000 %** perdido, con 60 huecos
pedidos y 60 rescatados por corrida (antes 0 y 0), sin cambiar el ping
grande (p99 185 → 169 ms). En reparto (`lossy`, tres enlaces con 1 %) no
cambia (ya pedía: 444 → 391 pedidos, 0,027 % en los dos).

## La paridad pedida (PARQ) y el orden de las reparaciones

Antes, la única regla entre el rescate y la FEC era la de la paridad XOR
(`cg_rescue_wait_ms`: con `fec`, un hueco espera al menos 2 × `fec_wait_ms`
+ 20); los códigos `rs` y `rlc` no tenían ninguna, y el NACK salía apenas se
pedía el hueco, duplicando lo que el código iba a rehacer. Ahora decide
`harq.h` (`cg_harq_route`, puro), en el servidor, que es quien rehace la
subida:

1. **El código primero.** Mientras puedan llegar reparaciones o paridades que
   cubran el hueco, se espera. El hueco queda **sellado** cuando llega una
   cuya base lo pasó (las ventanas de la RLC y los bloques de RS avanzan en
   orden: ninguna posterior lo cubrirá), o al cumplirse `rescue_code_ms`
   (100). El servidor anota la base más nueva en cada reparación y cada
   paridad (`rep_seal`) y repasa los huecos en espera en cada una
   (`harq_service`, también en el temporizador).
2. **Sellado y faltando, con déficit:** se pide la **paridad** de nuevo
   (`CG_T_PARQ`, servidor → cliente, protocolo 9), tantas reparaciones como
   el **déficit de rango**:
   - **RLC** (`cg_rlc_rx_need`): las incógnitas de las ecuaciones que echan
     de menos el hueco, y las que comparten incógnitas con ellas (dentro de
     64 secuencias), menos el rango de esas ecuaciones. El cliente manda
     combinaciones nuevas de **exactamente esas incógnitas** (semillas de la
     mitad alta, `cg_rlc_combine`): con las ecuaciones que el servidor ya
     tiene completan el rango (HARQ de tipo II, redundancia incremental: una
     combinación cualquiera arregla cualquier hueco), y cuesta el déficit, no
     todos los paquetes que faltan.
   - **RS** (`cg_rs_rx_need`, `cg_parq_rs_pick`): índices nuevos del mismo
     bloque. Leopard no deja agregar índices a un bloque codificado con otra
     m (su FFT cambia), así que las reparaciones declaran desde el principio
     una geometría mayor (`cg_rs_mhdr`: el doble de m redondeado a potencia
     de dos, dentro de k y de 32) y el plan manda las m primeras; la PARQ
     pide de las de arriba las que el servidor no tiene, y el cliente vuelve
     a codificar el bloque desde su anillo de rescate (los mismos símbolos:
     probado en `test_rs_parq`).
3. **Sin código que lo cubra** (ningún código encendido, o ninguna de sus
   reparaciones llegó): el NACK de siempre, el paquete.
4. **Nunca lo decodificable:** un hueco que el código rehízo se cuenta en
   `not_asked`; uno cuyo bloque o ecuaciones alcanzan no tiene déficit.

El cliente rehace lo pedido desde lo que guarda para el rescate
(`rescue_kb`), dentro del crédito del rescate, y lo manda por un enlace
**con lugar** (`parq_link`: vivo, no ahogado según `redund_spare` ni lleno
según la capacidad, el de menor RTT; desde el 26-10, si no hay ninguno no
sale, ver «Cambios»). El reordenador cuenta por separado
`rescued` (llegó el paquete) y `repaired` (lo rehizo un código); el servidor,
`repair.parq` y los motivos de `rescue`.

**Formato** (`harq.h`): un recuento n (1–8) y n pedidos de 20 bytes: tipo
(RS o RLC), m del bloque (RS), d (las reparaciones nuevas, 1–64), base,
mapa de 64 bits (RS: los miembros del bloque; RLC: los que faltan), y las
reparaciones que el servidor tiene (RS). Las respuestas son `CG_T_REPAIR`
normales. La versión sube a 9 (también porque la pista cambió de
significado).

## TCP de adentro

Con el túnel nativo el motor ve el TCP que lleva (`tcpfree.h`):

- **No competirle a TCP.** TCP da por perdido un hueco su RTT más un cuarto
  de su RTT mínimo (el `reo_wnd` de RACK, RFC 8985) después de que llegan
  los paquetes que lo siguen; una reparación del túnel que no llega antes
  solo duplica la suya. El extremo por donde TCP **sale** del túnel mide el
  RTT de sus flujos: el tramo lejano, del SYN que escribe al SYN-ACK que lee
  (`struct cg_tcpe`), más el RTT del túnel (en el servidor, subida más
  bajada del camino más rápido: el desfase de los relojes se cancela). Con
  TCP visible (datos de TCP en los últimos 10 s), el presupuesto acota la
  espera de un hueco pedido, y un pedido que no llegaría a tiempo no sale:
  el hueco se suelta para TCP (`left_to_tcp`).
- **Esperar al código sin retener.** Con TCP, un hueco que espera al código
  ya **no retiene lo que sigue**: se suelta enseguida y se sigue fuera de
  orden (`released`); lo que rehaga el código, o una PARQ que todavía llegue
  dentro del presupuesto, entra como paquete tardío, que la ventana de RACK
  absorbe. La primera versión retenía hasta el sello y el TCP lo pagó (ver
  las medidas, «lejano»). Sin TCP (UDP, o túnel WireGuard) se retiene: el
  flujo sale en orden, a cambio de demora.
- **Una retransmisión, un enlace** (`tcp_retx_one`). En redundancia, la
  primera retransmisión de unos bytes (`CG_TCPF_RETX1`) va por un solo
  enlace, el más rápido de los que la llevarían (`retx_masks`,
  `down_retx_masks`), no copiada por todos: lo más probable es que el túnel
  ya hubiera entregado el original (se perdió más allá, o su ACK tardó), y
  en terreno las copias de cada retransmisión alimentaban los espirales
  (RTMP de Facebook hacia Miami). Si TCP manda otra vez los mismos bytes
  (`CG_TCPF_RETXN`), esa sí va copiada: la única copia no alcanzó.
- **Las pérdidas más allá del túnel no son de un enlace.** En el extremo por
  donde TCP sale, un bloque SACK del host lejano por encima del ACK
  acumulado dice que le faltan los bytes de en medio. Si el túnel los había
  entregado y no eran un hueco propio (lo último que escribió pasó por
  encima de ellos), se perdieron más allá: `tcp_e2e.beyond`. No alimentan
  ningún estimador de enlace (esos solo ven la secuencia del túnel); quedan
  a la vista en el estado (`sack_holes`, `beyond`, `tunnel`, `far_rtt_ms`).
  Es aproximado a propósito: una entrada por cubo de la tabla de flujos y
  el primer bloque SACK.

## Medidas

Laboratorio de bonding (`bench/lab.d/bond.sh`, `labq.py -j 4 --lab-offset
16`), subida, 30 s, cubic, **túnel nativo** (`BOND_TUNNEL=native`, nuevo) con
`tcp_aware = yes` en todos los brazos, servidor con `fec = yes` en los de
código; «rescate» es `rescue_wait_ms = 60` y `rescue_kb = 32768` en los dos
extremos (lo de terreno). Brazos: `copies` (`redund_pace_ms = 250` y rescate),
`mix`, `rlc`, `rs` (con rescate); el sufijo 0 es `origin/master` (`55c826e`);
`rlc` sin sufijo, el binario nuevo **sin** rescate (el código solo); `rlcparq`
y `rsparq`, el nuevo con rescate (PARQ). Los de código con rescate son del
binario final (`cengarde-new3`; las corridas con el binario intermedio, que
retenía hasta el sello también con TCP, quedan abajo como «retenido»).
Mediana de n = 6, p de Mann-Whitney. «Flujo» es el de 6 Mbit/s (`udpgen`,
625 pps de 1200 B) junto a un TCP de 1 Mbit/s; «TCP» es cubic sin tope.
Resultados y planes en `~/campo/2026-10-24/harq/` (`an.py`).

**Flujo de 6 Mbit/s** (perdido %, p99 del flujo en ms, pedidos /
rescatados por corrida):

| Caso | `copies0` | `copies` | `mix0` / `mix` | `rlc0` / `rlc` / `rlcparq` | `rs0` / `rsparq` |
|---|---:|---:|---:|---:|---:|
| `protect` perdido | 0,275 | **0,005** (p 0,005) | 0,005 / 0,013 | 0,000 / 0,000 / 0,000 | 0,000 / 0,000 |
| p99 del flujo | 11,0 | 30,4 | 12,6 / 12,5 | 12,7 / 12,8 / 12,8 | 12,7 / 12,7 |
| pedidos / rescatados | 0 / 0 | 113 / 55 | 0 / 0 | 0 / 0 / 0 (144 sueltos a TCP) | 0 / 0 |
| `protectcut` perdido | 5,85 | **4,94** (p 0,004) | 7,89 / 6,71 | 8,37 / 8,65 / 7,38 | 7,94 / 6,88 |
| `protectslow` perdido | 0,133 | 0,037 (p 0,15) | 0,179 / 0,213 | 0,181 / 0,253 / 0,243 | 0,173 / 0,200 |

- Las copias con rescate dejan de perder en `protect`: el p99 del flujo sube
  de 11 a 30 ms (lo retenido espera el reenvío, una vuelta de 20 ms).
- Con código, lo que pierde el primer enlace lo rehace el código; con el
  TCP de 1 Mbit/s al lado (presupuesto ~25 ms, el RTT del túnel), los huecos
  no se retienen ni se piden (`left_to_tcp`): el p99 queda como sin rescate
  (12,8 ms; la versión que retenía 5 ms lo subía a 16,6).

**TCP sin tope** (Mbit/s):

| Caso | `copies0` | `copies` | `mix0` / `mix` | `rlc0` / `rlc` / `rlcparq` | `rs0` / `rsparq` |
|---|---:|---:|---:|---:|---:|
| `protect` | 7,6 | **14,4** (p 0,002) | 18,1 / 18,0 | 43,2 / 44,6 / 45,2 (p 0,18) | 34,2 / **38,5** (p 0,03) |
| `protectcut` | 6,2 | **10,9** (p 0,005) | 13,5 / 12,8 | 21,0 / 20,7 / 21,5 | 16,9 / 18,1 (p 0,26) |
| `protectslow` | 17,1 | 17,4 | 15,8 / 15,6 | 14,7 / 14,5 / 14,5 | 15,6 / 15,4 |

- El rescate en redundancia ahora le sirve a TCP: con copias, el doble.
- En `protect`, `copies` manda menos copias (1,32 contra 1,57 envíos por
  paquete): las retransmisiones de TCP van por un enlace (53 por corrida).

**El host lejano a 100 ms** (`BOND_FAR_MS = 100`, nuevo: el presupuesto de
TCP pasa de ~25 a ~150 ms), `protect`, TCP (Mbit/s, ping grande p99):

| Brazo | Mbit/s | p99 ms |
|---|---:|---:|
| `rlc0` | 22,1 | 138 |
| `rlc` (sin rescate) | 23,6 | 137 |
| `rlcparq` **retenido** (binario intermedio) | **16,9** (p 0,002) | **260** |
| `rlcparq` (final: suelto y seguido) | 25,2 (p 0,39; contra `rlc0` p 0,04) | 138 |
| `rsparq` retenido / final | 15,6 / 25,8 | 258 / 138 |
| `copies0` / `copies` | 5,0 / **6,4** (p 0,02) | 134 / 158 |

- Retener hasta que el código sella (decenas de ms) para el reloj de ACK de
  TCP aunque su presupuesto lo permita; por eso, con TCP, esperar al código
  ya no retiene. Retener una vuelta por un NACK (copias) sí paga.
- El flujo de 6 Mbit/s con el host lejano: `rlcparq` retenido 0,000 % con
  p99 31 ms (todo rehecho en orden), final 0,000 % con 15 ms, como `rlc`.

**PARQ contra NACK** (host a 100 ms, flujo de 6 Mbit/s, n = 6; `*nack` es
`parq = no`): en el microcorte, `rlcparq` pidió 40 PARQ por corrida (372
reparaciones) y reenvió 264 paquetes, contra 635 reenvíos de `rlcnack`;
perdido 8,23 contra 7,92 % (p 0,39); `rsparq` 7,88 contra `rsnack` 7,57
(p 0,94). En `protectslow`, 1–2 PARQ por corrida, sin diferencia (p ≥ 0,38).
Mismos bytes, misma pérdida: en el corte las reparaciones viajan por el
enlace con lugar, que es el que pierde 60 %.

**Pérdida más allá del túnel** (`beyond`, nuevo: protect con 1 % de TCP
perdido al salir del túnel, iptables en cada host; copias con rescate):

| | `copies` (`tcp_retx_one`) | `copiesN` (sin) | `copies0` |
|---|---:|---:|---:|
| bajada Mbit/s | 4,54 | 4,69 (p 0,33) | 4,4 |
| retransmisiones por un enlace / copiadas | 182 / 2,5 | 0 / 188 | – |
| envíos por enlace (DATA) | 25 352 | 26 134 (p 0,31) | – |
| huecos SACK / más allá del túnel (`tcp_e2e`) | 205 / 125 | 209 / 129 | – |
| subida Mbit/s | 4,58 | 4,49 (p 0,69) | 4,5 |

- El caudal lo pone la pérdida de afuera (cubic con 1 %); una retransmisión
  por un enlace no lo cambia y ahorra sus copias (~3 % de los envíos aquí,
  sin significancia con n = 6).
- El contador del servidor (subida) se agregó a `bondtab.py` después de esta
  tanda: la subida no tiene su columna de `tcp_e2e`.

**Costo** (`bench/fecbench.c`, este PC, 1200 B, misma sesión): RS k 21 m 3
codifica en 0,20 µs por paquete contra 0,14 de `origin/master` (la geometría
8 en vez de 4); k 32 m 5 y k 64 m 8, 0,26/0,24 contra 0,19/0,19; decodificar,
igual (0,60 / 0,76 / 0,83). Sigue bajo 0,03 % de un núcleo a 10 Mbit/s.

**Reproducción del rescate mudo** (WireGuard, n = 3, el binario intermedio
con solo el arreglo de la pista y la espera): `protect` con copias 0,256 →
0,000 %, 0 → 60 pedidos y rescatados; `lossy` en reparto sin cambio.

**No medido:** la celda virtual (no se usó: el laboratorio ocupó la máquina),
la Pi y el terreno.

## La retransmisión según la evidencia (`tcp_ack_rule`, 2026-10-25)

`tcp_retx_one` adivinaba: la primera retransmisión de unos bytes por un
enlace («lo más probable es que el original haya llegado»), la repetida
copiada. Ahora se decide por lo que se sabe del original (`tcpack.h`, puro,
tests en `test_tcpack.c`):

| Lo que se sabe del original | Fila | La retransmisión va |
|---|---|---|
| un ACK o un SACK que lo cubre ya volvió por el túnel (retransmisión espuria) | `spurious` | por un enlace, también repetida |
| el túnel lo perdió en todos los enlaces que podían traerlo | `tunnel` | copiada por todos los que la llevarían, también la primera |
| el host lejano lo echa de menos, pero el túnel lo entregó (perdido más allá), viene en camino, o el túnel no sabe | `delivered` | como `tcp_retx_one`: la primera por un enlace, la repetida copiada |
| nada todavía (un ACK vuelve una ida y vuelta después del dato: «sin ACK» no es «perdido») | `unknown` | como `tcp_retx_one` |

**De dónde sale la evidencia.** El extremo que envía los datos de un flujo
(el router para la subida, el VPS para la bajada) lee los ACK de ese flujo
cuando salen del túnel (`down_deliver`, `up_deliver`: el primer ACK libre sin
datos): por flujo, en la tabla de `flow.h` de 4096 entradas sin memoria por
paquete, el ACK acumulado más alto, hasta 4 bloques SACK fusionados y el
marcador del ACK más nuevo con SACK, `[su ACK, su borde SACK más alto)`, con
lo que el otro extremo dijo de él. El otro extremo, por donde TCP sale del
túnel, ya veía los SACK del host lejano (`tcp_e2e`); ahora juzga cada ACK con
SACK al leerlo hacia el túnel y lo marca:

- guarda por flujo hasta 8 **huecos propios** (rangos de TCP que saltó al
  escribir), que una copia tardía o la retransmisión recortan, parten o
  borran, con la secuencia del túnel del segmento que los reveló y la pista
  `CG_F_PREV` de ese paquete (`cg_ro_out` ahora lleva las banderas);
- un hueco propio está **perdido** cuando todo enlace que podía traerlo
  trajo una secuencia del túnel posterior (los enlaces son FIFO): el único
  que nombra la pista, si falta exactamente una secuencia, o si no todos los
  que trajeron alguna copia en los últimos 2 s (`cg_arr_top`: cualquier
  copia, no solo las primeras);
- solo eso se marca (`CG_F_TUNHOLE`). Lo que todavía puede llegar
  (`in_flight`), lo entregado (`beyond`) y lo que no sabe (`unvouched`: un
  flujo visto a la mitad, más de 8 huecos, SACK por encima de lo escrito)
  van sin marca.

**El formato, sin versión nueva.** `CG_F_TUNHOLE` es la bandera 0x02 de DATA,
reservada hasta ahora («se escribe 0, se ignora»), y el par anuncia que marca
con el bit 23 de la cola de las sondas (`CG_BOND_TCPMARK`, también en la
respuesta). Es el argumento de `CG_BOND_TYPE` (historia 015): un motor
anterior escribe 0 y lo ignora, y sin el bit 23 del par la bandera no
significa nada (la fila es `unknown`, el comportamiento de antes). No cambia
nada de lo que un motor del protocolo 9 sin esto lee o rechaza, y
`cengarde-vps-setup` sigue pidiendo el 9.

**Por qué una espuria no se descarta.** Aunque el receptor ya tenga esos
bytes, su llegada produce el D-SACK (RFC 2883) con el que el emisor sabe que
fue espuria, deshace el recorte de su ventana y, con RACK (RFC 8985, 7.2),
agranda su ventana de reordenamiento para que el próximo desorden no la
dispare. Descartarla ahorra un paquete en un enlace y deja a TCP disparando.

**Lo que costó llegar** (cada versión medida contra la conjetura, n = 8):

1. La primera marcaba todo hueco propio que el túnel *todavía* no había
   escrito. En el caso `spurious` (sin pérdida, un enlace detenido 300 ms
   cada 5 s) copiaba retransmisiones que llegaban por el otro enlace antes
   que los originales detenidos, y el RACK del host daba a todos por
   perdidos.
2. La segunda separó lo que viene en camino, pero seguía marcando lo que no
   podía asegurar; con 4 huecos por flujo, un salto de demora abría decenas,
   los olvidaba, y todo pasaba a «no sé» marcado: TCP retransmitió 1104
   contra 484 por corrida en la bajada (p 0,003) y 667 contra 357 en la
   subida.
3. Fundir los huecos de más tomaba por faltante lo escrito entre ellos, y
   esperar a «los enlaces que espera el reordenador» dejaba afuera un enlace
   detenido o silenciado, mientras su secuencia más nueva contaba solo las
   primeras copias: huecos que venían en camino se leían como perdidos (793
   marcados por corrida en `spurious` de subida, sin una sola pérdida en el
   túnel).
4. Con una retransmisión `delivered` siempre por un enlace, en `protectcut`
   la repetida de algo que venía por el enlace lento volvía a ir por el
   rápido, que perdía 60 %: TCP 12,3 → 10,8 Mbit/s en la bajada (p 0,003).
   Un bit no distingue «entregado» de «en camino», así que `delivered` copia
   la repetida, como la conjetura.

**Medidas** (laboratorio de bonding, túnel nativo, `tcp_aware`, copias de
250 ms y rescate como en terreno; cubic, 30 s; `labq.py -j 6 --lab-offset 8`;
n = 8 por brazo y sentido; mediana, p de Mann-Whitney contra `heur`, el
binario de #108 con `tcp_retx_one`, y contra `off`, el mismo con
`tcp_retx_one = no`, todo copiado; `rule` es el binario final `f29e50f`;
tanda `m6`). Casos nuevos: `spurious` (los enlaces de `protect` sin pérdida,
el primero detenido 300 ms cada 5 s, el flujo a 20 Mbit/s, el host del VPS a
50 ms) y `lossall` (dos enlaces de 30 Mbit/s que pierden 10 % cada uno: un
paquete copiado se pierde en los dos el 1 % de las veces).

| Caso, sentido | Mbit/s `rule` / `heur` / `off` | retransmisiones de TCP | copiadas por corrida | bytes por byte útil | RTT p99 ms |
|---|---|---|---|---|---|
| `protect` subida | 15,1 / 13,9 / 13,9 (p 0,87; 1) | 104 / 105 / 106 | 3 / 1 / 65 | 1,36 / 1,39 / 1,39 | 46 / 43 / 50 |
| `protect` bajada | 15,7 / 16,6 / 17,2 (p 0,25; 0,13) | 92 / 87 / 79 (p 0,29; 0,02) | 2,5 / 0 / 41 | 1,33 / 1,31 / 1,30 | 41 / 41 / 43 |
| `protectcut` subida | 11,0 / 9,9 / 12,7 (p 0,44; **0,015**) | 408 / 419 / 369 | 40 / 20 / 315 | 1,49 / 1,53 / 1,42 | 153 / 150 / 148 |
| `protectcut` bajada | 10,9 / 12,2 / 12,4 (p 0,21; 0,67) | 274 / 355 / 295 (p **0,04**) | 25 / 37 / 220 | 1,41 / 1,37 / 1,35 | 160 / 163 / 148 |
| `beyond` subida | 4,5 / 4,7 / 4,6 (p 0,53) | 216 / 205 / 210 | 3,5 / 2 / 188 | 2,08 / 2,07 / 2,08 | 36 / 37 / 32 |
| `beyond` bajada | 4,7 / 4,5 / 4,5 (p 0,17) | 203 / 207 / 213 | 3,5 / 2,5 / 197 | 2,02 / 2,05 / 2,07 | 39 / 38 / 38 |
| `spurious` subida | 19,9 / 19,9 / 19,8 | 381 / 498 / 566 (p 0,38; 0,07) | 0 / 0,5 / 463 | 1,28 / 1,29 / 1,30 | 392 / 397 / 391 |
| `spurious` bajada | 19,8 / 19,8 / 19,7 (p 0,2; 0,02) | 572 / 508 / 742 (p 0,8; **0,03**) | 0 / 0 / 523 | 1,27 / 1,27 / 1,28 | 390 / 391 / 389 |
| `lossall` subida | 4,9 / 5,3 / 5,0 (p 0,24) | 174 / 185 / 176 | 127 / 3,5 / 164 | 2,14 / 2,13 / 2,14 | 47 / 46 / 47 |
| `lossall` bajada | 5,2 / 5,2 / 5,1 (p 0,6) | 175 / 187 / 185 | 130 / 5 / 173 | 2,15 / 2,14 / 2,14 | 46 / 46 / 49 |

Cómo actuó cada fila (retransmisiones por corrida de `rule`, `unknown` /
`spurious` / `delivered` / `tunnel`, y de ellas copiadas):

| Caso, sentido | filas | copiadas (`delivered` repetidas / `tunnel`) |
|---|---|---|
| `protect` subida / bajada | 1 / 0 / 97 / 3,5 — 0 / 0 / 87,5 / 5 | 2 / 1 — 0 / 2,5 |
| `protectcut` subida / bajada | 15 / 1 / 360 / 30,5 — 8,5 / 0 / 208,5 / 36 | 22 / 16,5 — 12 / 10 |
| `beyond` subida / bajada | 1 / 0 / 213,5 / 0 — 1 / 0 / 199 / 3 | 3 / 0 — 3 / 1 |
| `spurious` subida / bajada | 0 / 0 / 411,5 / 0 — 0 / 165 / 319,5 / 0 | 0 / 0 — 0 / 0 |
| `lossall` subida / bajada | 0,5 / 1 / 39,5 / 130,5 — 0,5 / 0,5 / 38,5 / 135,5 | 0 / 127 — 0 / 130 |

- **El caudal no cambia** contra la conjetura en ningún caso (p ≥ 0,17 con
  n = 8; las diferencias de un 10 % entre tandas del mismo brazo son
  ruido: `heur` en `protectcut` de bajada midió 10,9, 12,5, 11,7, 11,4,
  12,3 y 12,2 en seis tandas). Lo que cambia es **qué** se copia.
- **Copiar todas las retransmisiones daña donde son espurias**: en
  `spurious` de bajada `off` hace que TCP retransmita 742 contra 572 de
  `rule` (p 0,03) y copia 523 por corrida; `rule` no copia ninguna y la fila
  `spurious` actúa 165 veces por corrida (en la subida, 0: el ACK pasa por el
  router un instante antes que por el host, que retransmite sin haberlo
  visto; en la bajada, el host del VPS está a 50 ms y sus retransmisiones
  llegan después del ACK que las cubre).
- **Lo que el túnel perdió en los dos enlaces se copia desde la primera**
  (`lossall`: 130 por corrida contra 4), por 0,01 más de bytes por byte útil
  (p 0,03 y 0,0006); el caudal no lo nota (TCP repara una retransmisión
  perdida sin cortar la ventana otra vez).
- **Las pérdidas más allá del túnel** (`beyond`) van a `delivered` (200–214
  por corrida, 0–3 a `tunnel`): como la conjetura, que ya acertaba ahí.
- En `protect`, 9 de cada 10 huecos que TCP retransmite **venían en camino**
  (su copia por el enlace lento, o el paquete entero por él: medido con un
  binario de depuración, la pista nombraba siempre el enlace de 2 s de
  cola): no se copian.
- **Pendiente de esto:** en el microcorte copiar todo rinde más en la subida
  (`off` 12,7 contra 11,0 de `rule`, p 0,015; `heur` 9,9): una ráfaga en el
  enlace rápido deja huecos de varias secuencias sin pista, que la regla no
  puede dar por perdidos hasta que pase el enlace lento.

**Un hallazgo del banco:** con `BOND_NETEM_INFLIGHT=1`, un caso cuya demora
salta y vuelve (`ltespike`, `ltespikerand`, `protectpin`) achica el límite de
netem al volver, y netem descarta todo lo que llega mientras los paquetes del
salto siguen adentro (medido: 807 de 1556 datagramas a 60 Mbit/s, de 310 a
10 ms). Esos casos pierden en cada salto además de demorar; `bond_link`
acepta ahora la demora para la que se dimensiona el límite (`spurious` la
usa), y los casos viejos quedan como estaban hasta revisarlos.

## Pendiente

- **Terreno:** el rescate (`rescue_wait_ms`, `rescue_kb`) en Nostálgica con
  el binario nuevo; mirar `reorder.rescue` (`asked`, `rescued`, `repaired`),
  `rescue.left_to_tcp`, `upload.tcp` y `tcp_e2e` del VPS (RTMP hacia
  restream o Facebook: ahí el presupuesto de TCP es largo y la PARQ tiene
  lugar).
- **PARQ sin ganancia medida:** queda encendida (`parq = yes`, cuesta lo
  mismo que el NACK); buscar dónde gana (pérdida en ráfagas cortas sobre un
  código con poca m, con el enlace de lugar limpio) o apagarla.
- La retención del rescate con copias sube el p99 del flujo (11 → 30 ms en
  `protect`): una perilla para no retener tráfico chico, si molesta.
- La bajada no tiene códigos: solo NACK y el presupuesto de TCP.
- `tcp_e2e` es aproximado (una entrada por cubo, el primer bloque SACK);
  validar contra `nstat` del host lejano.
- UCI/LuCI no exponen las perillas nuevas (los valores por omisión son los
  propuestos).
- `tcp_ack_rule`: en el microcorte copiar todas las retransmisiones rindió
  más (`off`); probar a dar por perdido un hueco sin pista por el tiempo
  (más que el retardo del enlace más lento con copia a tiempo) en vez de
  esperar a que pase. En terreno: `upload.tcp.rule` y `tcp.rule` (cuántas
  `tunnel` y `spurious`), `tcp_e2e.marked` e `in_flight`.
- Los casos con saltos de demora y `BOND_NETEM_INFLIGHT=1` (`ltespike`,
  `protectpin`) pierden en cada salto por el límite de netem: volver a
  medirlos con `bond_link ... INFLIGHT_MS`.

## Cambios

- 2026-10-24: creada.
- 2026-10-25: la retransmisión según la evidencia (`tcp_ack_rule`,
  `tcpack.h`, `CG_F_TUNHOLE` y `CG_BOND_TCPMARK` sin versión nueva), los
  casos `spurious` y `lossall`, y el límite de netem que achican los saltos
  de demora.
- 2026-10-26: historia 025. La PARQ ya no va a un enlace ahogado o lleno
  cuando no hay otro (`parq_link`: no sale; en un microcorte, 1 500
  reparaciones por segundo llevaban la cola del celular a 1,6 s). Para lo
  que sube (datos, paridades, el NACK y la PARQ, el primer enlace de
  `redund_spare`), un enlace con las sondas detenidas que todavía trae
  paquetes sigue vivo (`link_masks_tx`, `cg_link_rx_alive`). Las
  combinaciones de la PARQ con la RLC destapaban el error de la
  eliminación de la RLC (ver la 021): corregido.
