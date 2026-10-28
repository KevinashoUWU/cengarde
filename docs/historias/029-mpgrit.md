# 029 — MPGRIT: el motor y GRIT en un solo protocolo multienlace

- **Fecha:** 2026-10-28
- **Estado:** vigente (decisión tomada; paso 1 en curso)
- **Fuentes:** historia 028 («Terreno»: GRIT contra OMR y el TCP directo);
  `~/campo/2026-10-27/grit-split/README.md` (tandas t2–t6),
  `~/campo/2026-10-27/omr-inside/README.md` (subflujos MPTCP de OMR); estado
  del NUC de la testnet el 2026-10-28 (`reorder.lost`, `reorder.late`,
  `reorder.rescue`); `engine/src/streamcc.h` (`cg_scc_reo_cap`),
  `protoctl.c` (`cg_pctl_nack`), `reorder.h` (`cg_ro_gap_gone`,
  `cg_ro_pend_take`); laboratorio `~/campo/2026-10-28/s1a/`.

## TL;DR

- **Decisión del usuario (2026-10-27/28):** absorber el motor en GRIT. Hoy son
  dos protocolos nuestros apilados, cada uno con su secuencia, sus pérdidas y
  su control de congestión; MPGRIT es uno solo, multienlace de nacimiento
  (de 1 a 16 enlaces), y funciona como proxy (como shadowsocks o v2ray),
  no solo como túnel. «cengarde» queda como nombre de la caja de
  herramientas hasta que se elija otro (el cambio de nombre va con el
  protocolo nuevo, una sola ruptura).
- **Por qué:** en terreno GRIT perdió con el TCP directo por el mismo túnel
  (24–54 frente a 50–95 Mbit/s; OMR 58–155) porque **retransmite lo que no se
  perdió**: decide pérdidas sobre la mezcla de los enlaces. OMR (un subflujo
  MPTCP con BBR por enlace) las decide por enlace y retransmite 0–1,8 %.
- **Dos niveles:** subflujo por enlace (secuencia, pérdidas y ritmo propios,
  llenando hasta una cola objetivo) y flujo por conexión (orden, entrega,
  reparto entre flujos con un tope de ~300 ms de latencia); la protección
  (FEC, copias, PARQ) por clase de tráfico; los datagramas (UDP, QUIC,
  RIST, IP) como una clase más.
- **El túnel tampoco sabe qué se perdió:** en el NUC, el 2026-10-28, los
  51 paquetes que dio por perdidos (4 por evidencia del enlace, 47 por
  plazo) **llegaron todos después**, y de 3538 pedidos de rescate 3509 se
  llenaron.
- **Paso 1:** la ventana de RACK hasta 300 ms (antes 20 ms sin código y 100
  con código) y `protocontroller_nack` (el NACK del túnel como pista, no como
  pérdida): neutro en el laboratorio.
- **Paso U1 (protocolo 12, `subflow`):** cada enlace con su propia secuencia
  (`sseq`) y sus pérdidas (`subflow.h`, `SFLOSS`); una pérdida es cierta
  cuando el enlace la perdió y nada la reconstruye, o el receptor también la
  pidió (`cg_resc_sure`); el NACK solo es una pista; el RACK de GRIT sobre
  la mezcla se apaga y su temporizador espera 300 ms más. **En terreno GRIT
  reenvía 0,6–1 % en vez de 10 %, y casi nada llega duplicado** (3–13 en vez
  de 6–8 mil); el caudal sigue lejos de OMR porque GRIT no llena los
  módems (paso 2).
- **Paso 2b, un ritmo por enlace (`link_cc`, `SFACK`, `linkcc.h`):** un BBR
  por enlace, alimentado por lo que el receptor dice que tomó cada uno,
  maneja el reparto y la oferta de GRIT. **En terreno la bajada pasa de
  63–65 a 98–125 Mbit/s (63–86 % de OMR) con el ping p95 en 45 ms (OMR
  134–147)**. Con la cola de cada enlace por su retardo de ida (sondeo y
  drenaje por la cola, STARTUP a ×3/2, `bw` topado por lo entregado con cola
  en el router) gana +8,7 % en el laboratorio y **la subida llega a 96–101
  Mbit/s, a la par de OMR (93–95), con menos ping**. Con 8 MB por flujo
  (+49 % en cuatro rondas) **la bajada llega al 93 % de OMR (129 contra 140
  de media) con 40 % de su ping, y la subida le gana ~10 %**. Todavía es un
  experimento.

## Contexto

cengarde nació como una capa que gobernaba flujos ajenos (WireGuard, después
el TCP de los usuarios) y por eso tuvo que adivinar desde afuera. Con GRIT
(historia 025) el flujo es nuestro, pero sigue montado sobre el motor: el
motor reparte por enlace, ordena y rescata con su secuencia; GRIT ordena,
detecta pérdidas y controla la congestión con la suya, sobre la mezcla.
La 028 los acercó (retardo por enlace, horizonte de reparación); el terreno
mostró que el roce entre las dos capas es el problema.

## Hallazgos

### Terreno (2026-10-27 noche, historia 028 «Terreno»)

- El reparto de bajada entre módems es parecido al de OMR; con GRIT las colas
  de los módems quedan en 2–3 ms: el emisor no empuja.
- GRIT retransmite 7–40 %: RACK con tope de 100 ms (con código) lee el
  desorden entre módems como pérdida; con piso de 300 ms cae de 8–28 mil a
  1–3 mil retransmisiones y sube 27→50 y 47→54 Mbit/s.
- Los NACK del túnel hacia GRIT: ~5 mil por bajada, casi tantos como los
  duplicados que recibe el NUC; no cambian con la FEC ni con
  `rescue_wait_ms` 60 o 200.
- OMR: un subflujo BBR por módem, 45–60 ms de cola en cada uno, y capacidades
  2–3 veces las que estima el motor (Claro 125 frente a 39 Mbit/s).

### El túnel declara perdido lo que llega tarde (NUC, 2026-10-28)

En 15 min de tráfico real con el protocontrolador A: `lost.rack` 4,
`lost.cap` 47, y `late.after_rack` 4, `late.after_cap` 47: todo lo dado por
perdido llegó. `rescue.asked` 3538, `rescued` 3509. Ni la evidencia por
enlace (`CG_F_PREV`, `cg_ro_pend_take`) es segura con estos módems. Para el
tráfico IP eso solo cuesta duplicados que el túnel descarta; para GRIT cada
pedido era una retransmisión.

### Presupuesto de latencia (usuario, 2026-10-27)

El ping con carga puede estirarse por caudal mientras no llegue a ~300 ms;
CAKE controla el resto. El TCP que toma GRIT no pasa por el CAKE de `cg0`:
ese tope lo tiene que poner el propio protocolo.

## Qué hacemos con esto

### Diseño

| Nivel | Qué hace | De dónde sale |
| --- | --- | --- |
| Subflujo (por enlace) | Secuencia y pérdidas propias; ritmo que llena hasta una cola objetivo (30–60 ms); sondas | enlaces, sondas, capacidad, `linkowd`, `linkmodel`, Starlink (ranuras, dominante), `CG_F_PREV` |
| Flujo (por conexión) | Secuencia de datos, entrega en orden, retransmite lo que un subflujo dio por perdido, reparto entre flujos, tope de latencia | `stream.h`, `streamcc.h` (la parte de flujo) |
| Protección | FEC, copias, PARQ por clase (vivo o bulto) | FEC v3, RLC, `harq.h`, copias en el borde |
| Datagramas | UDP, QUIC, RIST e IP como respaldo | el túnel nativo de hoy |

### Plan por pasos (cada uno medido en el laboratorio y contra OMR en terreno)

1. **Que GRIT no retransmita lo que no se perdió.** La ventana de RACK
   crece con los duplicados hasta el presupuesto (300 ms) y vuelve sola;
   `protocontroller_nack no` toma el NACK del túnel como pista. Éxito: los
   duplicados del NUC bajo el 2 % y GRIT al menos igual al TCP directo.
2. **Ritmo por enlace que llena hasta una cola objetivo** (la capacidad del
   motor, menos conservadora); GRIT solo reparte entre flujos. Éxito:
   caudal cerca de OMR con el ping con carga bajo 300 ms.
3. **Datagramas como clase del mismo protocolo**, la TUN como respaldo.
4. **El modo proxy por defecto**, protocolo nuevo y nombre nuevo, con un
   migrador de la configuración.

Lo que hoy funciona en terreno (la redundancia del directo, Starlink) no se
toca hasta que lo nuevo lo supere.

### Ver la celda por dentro (plan acordado el 2026-10-28)

1. El puerto DIAG de los módems Qualcomm: nuestras asignaciones por
   milisegundo (bloques MAC ya leídos con `~/campo/2026-10-28/diag/cgdiag.py`;
   la versión 50 del registro de bajada falta descifrar).
2. Una SDR (LimeSDR USB 2×2 o USRP B210) con LTESniffer para el laboratorio
   nocturno: la carga de los demás usuarios de la celda.
3. Lo aprendido, dentro de los subflujos.

### Paso 1 en el laboratorio (2026-10-28, `~/campo/2026-10-28/s1a/`)

`e2d8935` (base) contra `1a7f6df` (RACK hasta 300 ms), GRIT por el túnel
nativo con la configuración del terreno, 7 casos × 2 sentidos × 6 réplicas
de 20 s (168 corridas, el PC y remoto06): **neutro**, ninguna diferencia con
p < 0,13; `reorder50` de subida +25 % (p 0,13), `ratswitch` de subida p99
774 → 311 ms, `protect` de bajada −33 % con mucha varianza (p 0,94). En el
laboratorio netem entrega cada enlace en orden: el desorden que confunde a
GRIT es el del terreno, donde se mide. `protocontroller_nack no` queda por
medir en terreno.

### Paso U1: el subflujo por enlace (protocolo 12, 2026-10-28)

Rama `claude/mpgrit-step1` (`741d267`, `7fba66d`); nota del mapa
`docs/mapa/Subflujo (subflow.h).md`.

- **El formato:** la cabecera pasa de 24 a 28 bytes con `sseq`, fuera del MAC
  como el byte del enlace (una copia por otro enlace lleva el suyo); el tipo
  `CG_T_SFLOSS` (9) lleva el enlace y los rangos de `sseq` perdidos. La sonda
  y su respuesta llevan el siguiente `sseq` del enlace: la pérdida de la cola.
- **El receptor** da por perdido un hueco cuando por ese mismo enlace llegó un
  `sseq` posterior hace más que la ventana del enlace (piso 10 ms; un hueco
  que llega tarde la agranda 4 ms, hasta 300). Un salto de una ventana o más
  no dice nada (un `SFLOSS` tiene que ser cierto). Las copias pagan su MAC:
  dicen que su enlace no perdió.
- **La evidencia:** el primer ensayo (`sf-smoke`) mostró que «el enlace lo
  perdió» no es «el receptor no lo tiene»: en `lossy` de bajada el subflujo
  halló 900 pérdidas, GRIT reenvió 906 y el router recibió 904 duplicados,
  porque la FEC v3 ya las había reconstruido. El anillo del rescate guarda la
  evidencia de cada paquete (su enlace lo perdió; el receptor lo pidió) y
  `cg_resc_sure` decide: sin código basta el enlace; con código, los dos; el
  NACK solo, según `protocontroller_nack`. Con la regla: 0–3 duplicados.
- **El RACK de la mezcla** (`sf1`, n = 3, el PC y remoto06): con él
  encendido, casi todo lo que GRIT reenvió fue por RACK y llegó duplicado, en
  todos los brazos, también con su ventana en 250 ms
  (`protocontroller_reo_ms`): las colas del laboratorio llevan el RTT de un
  enlace más allá (p99 de 783 ms en `ratswitch` de subida). Es la lección de
  MPTCP: la pérdida se decide por subflujo y el nivel de datos solo tiene un
  temporizador de respaldo. Con `subflow`, `cg_pctl_rack` lo apaga.
- **El temporizador** (`sf2`, n = 4, `7fba66d`): sin el RACK de la mezcla se
  fueron sus reenvíos (`reorder50` de subida: 2094 → 210 con `subflow` y 75
  con además `protocontroller_nack no`), pero el temporizador de GRIT ocupó
  su lugar, casi todo falso: `cell4` de subida, 386 reenvíos por él y 514
  duplicados; `ratswitch` de subida, 864. Vence cada segmento enviado hace
  más que `srtt + 4·rttvar` más la ventana, y el RTT de la mezcla es más
  corto que el del enlace más lento. Con `subflow`, `cg_scc_rto_extra` le da
  al menos el presupuesto de latencia (300 ms): las pérdidas normales las
  hallan antes el subflujo y la sonda de cola. El caudal no cambió con
  significancia en ningún caso (p ≥ 0,08).
- **Con el margen** (`sf3`, n = 4, `032cebf`): los duplicados que recibe el
  otro extremo (lo que GRIT reenvió sin que se perdiera), medias por corrida:

  | caso | sin `subflow` | `subflow` | `subflow` + `nack no` |
  | --- | --- | --- | --- |
  | `cell4` bajada / subida | 367 / 460 | 142 / 97 | 2 / 6 |
  | `equal` bajada | 183 | 33 | 1 |
  | `protect` subida | 832 | 3 | 77 |
  | `ratswitch` bajada | 896 | 94 | 115 |
  | `reorder50` subida | 612 | 3 | 213 |

  Con `protocontroller_nack yes`, lo que se reenvía por el NACK solo vuelve
  casi todo duplicado también en el laboratorio; con `no`, solo se reenvía lo
  que el enlace certifica, y casi nada sobra. El caudal casi no cambia (solo
  `cell4` de subida con `nack no`, +15 %, p 0,04). Queda el temporizador en
  ráfagas, en corridas sueltas (`het` de bajada, una de cuatro: 1264 por él,
  657 duplicados; `reorder50` de subida, tres de cuatro): al vencer marca
  perdido todo lo más viejo que el umbral a la vez.
- **Terreno** (2026-10-28, ~05:40, `~/campo/2026-10-28/field-u1/`): el NUC
  y la VPS con el protocolo 12 (`317b9b9`, paquetes del CI), los cuatro
  módems, 100 MB de cachefly detrás del NUC y de OMR, brazos alternados, dos
  rondas:

  | brazo | Mbit/s | reenviado | duplicados en el NUC | ping p50/p95 |
  | --- | --- | --- | --- | --- |
  | GRIT sin `subflow` | 50,3 / 58,5 | 10 % | 8343 / 6279 | 37–40 / 45–47 |
  | `subflow`, el NACK como pérdida | 57,0 / 58,0 | 7,8 / 4,1 % | 6148 / 3416 | 38–39 / 45–46 |
  | `subflow`, el NACK como pista | 86,6 / 43,2 | 0,6 / 1,0 % | 3 / 13 | 38–39 / 46–47 |
  | el túnel sin protocontrolador | 112,2 / 125,4 | — | — | 37–40 / 50–64 |
  | OMR | 233,5 / 281,5 | — | — | 56–61 / 169–385 |

  Lo que el paso U1 buscaba se cumple en terreno: GRIT reenvía 0,6–1 % (OMR
  0–1,8 %) y casi nada llega duplicado. Los subflujos hallaron 76–762
  pérdidas reales por descarga; el reordenador del túnel pidió 4–8 mil cosas
  que ningún enlace había perdido (las `hints`): con `subflow`, el NACK solo
  ya nunca es una pérdida (`protocontroller_nack` deja de contar). El caudal
  no lo frena eso: GRIT deja el ping en reposo (p50 38 ms), no llena los
  módems; OMR sí (p50 56–61, p95 hasta 385). Es el paso 2.
- **El búfer de cada flujo** (terreno, `field-u3` y `field-u4`): sin
  reenvíos falsos, lo que frenaba era el búfer de 1 MB: en la VPS quedaba
  lleno (`held_tx` 1 048 576), lo entregado detrás de un hueco lo ocupa
  hasta que el hueco se llena. Con `subflow`:

  | búfer | `field-u3` | `field-u4` (sospechosos) | por el temporizador |
  | --- | --- | --- | --- |
  | 1 MB | 43,9 / 56,4 | 50,5 / 50,6 | 0–74 |
  | 2 MB | 121,5 / 95,3 | 91,3 / 131,3 | 346–3028 |
  | 4 MB | 57,6 / 65,1 | 90,2 / 79,6 | 4579–5978 |
  | sin protocontrolador | 101,0 / 123,4 | 106,7 / 99,3 | — |

  Con 2 MB GRIT alcanza al túnel sin protocontrolador y una vez lo pasa
  (131 contra 99), con el ping en reposo; con más búfer el temporizador
  vuelve. Dos causas: (1) con la FEC v3 una pérdida del enlace esperaba el
  NACK del receptor, que puede no llegar; ahora es un **sospechoso**
  (`CG_RESC_SURE_LATER`, `cg_st_tx_suspect`), perdido al vencer el
  horizonte de reparación si su entrega no volvió (`5782e61`; el
  temporizador siguió alto en `field-u4`); (2) un ACK lleva 8 rangos, el
  más nuevo y los 7 más bajos: con más huecos, los del medio nunca se
  decían y el temporizador los reenviaba; ahora van por turno
  (`315f724`, en el laboratorio `rot1`: con 4 MB los duplicados bajan de
  cientos a decenas o menos, p. ej. `equal` de bajada 477 → 7).
- **El receptor sin rangos** (terreno `field-u5` con `315f724`): con 4 MB
  los duplicados en el NUC bajaron a 4–14 (antes 900–2600), pero el
  temporizador siguió reenviando 4–5 mil por descarga que **no** eran
  duplicados, sin que el VPS descartara nada al enviar (`tx_drops` 0). El
  receptor de GRIT guardaba 256 rangos fuera de orden; con varios enlaces
  cada segmento que llega por uno más rápido antes de los que van por los
  lentos abre uno, y con 2–4 MB en vuelo se acababan: el receptor
  descartaba la trama y solo el temporizador la reenviaba. En `rot1`,
  `drop_frames` del receptor fue 0, 64 y 412 con 1, 2 y 4 MB, y el
  temporizador 63, 211 y 578. Ahora 1024 rangos (`156c932`, 16 KB por
  sentido y flujo): en el laboratorio (`rng1`) los descartes van a 0 y el
  temporizador baja a 181–205.
- **Terreno con todo** (`field-u6`, `156c932` en el NUC y la VPS, la red
  más cargada que en la mañana):

  | brazo | Mbit/s | por el temporizador | duplicados | ping p50/p95 |
  | --- | --- | --- | --- | --- |
  | `subflow`, 2 MB | 87,5 / 55,0 | 0 / 3 | 9 / 10 | 39–41 / 48 |
  | `subflow`, 4 MB | 72,4 / 78,8 | 0 / 813 | 2 / 838 | 40 / 47–48 |
  | sin protocontrolador | 77,1 / 88,2 | — | — | 40–46 / 74–80 |
  | OMR | 156,7 / 133,2 | — | — | 76–78 / 307–526 |

  GRIT ya va a la par del túnel sin protocontrolador, con el
  temporizador y los duplicados casi en cero (el 10 % de la segunda ronda
  de 2 MB fueron pérdidas reales de un enlace, que el subflujo dijo). OMR
  lleva 1,5–2 veces más, con el ping p95 sobre el presupuesto de 300 ms;
  GRIT lo deja en 48. Lo que falta es el paso 2: llenar los enlaces dentro
  del presupuesto.
- **Hueco conocido:** lo copiado (redundancia) no tiene un enlace que responda
  por su pérdida; sin el RACK de la mezcla, lo recupera el NACK (con
  `protocontroller_nack yes`) o el temporizador.

### Paso 2: llenar los enlaces (2026-10-28, tarde)

El usuario: el RTT puede estirarse hasta ~500 ms, «hay que ser permisivo,
pero reordenar mejor que la competencia». Terreno con calentamiento previo
(50 MB: cada brazo reiniciaba el motor de la VPS y las capacidades
partían de cero), descargas de 100 MB, brazos alternados.

- **El objetivo de cola** (`field-s2`, `s2b`): GRIT con 5 ms de cola sobre
  su RTT mínimo se cortaba solo (`congested` 920–1755 por corrida); con 50
  ms (y 60 en el gobernador y la capacidad del motor) sube 53–75 % y llegó
  a 84–88 % de OMR una vez, con el ping p95 en 46–48 (OMR 179–201). En el
  laboratorio, neutro (netem no imita al planificador del operador, que da
  más cuando la cola crece).
- **Por qué para un flujo** (`stops` en el estado, `2a603ab`): casi siempre
  el ritmo, después el búfer todo enviado sin acuse.
- **La oferta del motor** (`protocontroller_offer`, `9bcadd8`,
  `field-s5`): con `subflow`, el ritmo es al menos la capacidad del motor y
  un cuarto mientras no estén todos los enlaces llenos: 97,6 contra 53,9
  Mbit/s en promedio (+81 %, tres de tres rondas).
- **Lo que no ayudó:** el búfer de 4 MB (empate con 2 MB, más pérdida), la
  dosificación por socket en la VPS (83 contra 92; además su tarjeta tenía
  `fq_codel` y la dosificación de UDP no se aplicaba).
- **Un módem que pierde la mitad** (`field-s7`, `s8`): Claro perdía 28–78 %
  de lo que llevaba con la cola casi vacía (el operador descarta sin
  encolar) y seguía con un cuarto del tráfico: con su capacidad en cero, el
  reparto por llegada esperada se rendía entero y repartía por pesos. Ahora
  un enlace sin tasa no recibe mientras otro tenga (`bf4b603`). Y la
  capacidad baja con la pérdida del enlace (`capacity_loss_pct 5`):
  85,1 / 51,6 contra 45,5 / 36,0 (`field-s9`).
- **Claro en su mejor celda:** anclado en B28 con SINR −5 (RSRQ −18); el
  usuario pidió fijarlo a la mejor: bandas LTE 2, 4 y 66 → ancla B4 PCI 90
  (SINR +11) con B2 de 20 MHz (SINR +13). Pierde 5–6 % en vez de 28–78 %.
- **Por omisión con `subflow`** (si la configuración no los dice):
  `protocontroller_queue_ms` 50, `governor_target_ms` 60,
  `capacity_loss_pct` 5, `protocontroller_buf_kb` 2048.
- **La VPS de OMR:** BBR, `fq`, `netdev_max_backlog` 10000,
  `tcp_slow_start_after_idle` 0 y el planificador MPTCP **BLEST** (no usa
  un subflujo lento si bloquearía al rápido en el receptor: su forma de
  reordenar). Llevamos los dos primeros ajustes a `contrib/vps/sysctl.conf`.
- **Dónde quedamos:** 40–105 Mbit/s según la ronda contra 26–205 de OMR en
  las mismas rondas; ping p95 de GRIT 43–51 ms contra 82–397 de OMR. La red
  cambia mucho entre rondas: comparar solo dentro de la misma ronda.

### Paso 2b: un ritmo por enlace (`link_cc`, 2026-10-28, tarde)

El usuario: «el scheduler por enlace será la mejor ganancia». OMR corre un
BBR por subflujo MPTCP y su VPS usa **BLEST** como planificador: no pone un
paquete en el subflujo lento si llegaría después de que el rápido, con su
ventana, ya hubiera mandado lo siguiente (evita el bloqueo en el receptor).
Nuestro reparto por llegada esperada (ETA) ya busca lo mismo; lo que faltaba
era que cada enlace supiera su propio ritmo y su cola **ahora**, no por la
capacidad del motor (lo más que entregó, cortado al ver cola) ni por un
reporte de retardo de hace 300–500 ms.

- **`SFACK`** (tipo 10, protocolo 12 sin versión nueva): cada 10 ms como
  mucho, por el enlace más rápido, el receptor dice por enlace su `sseq` más
  nuevo, los bytes que tomó, cuándo llegaron (en su reloj) y cuánto esperó
  el más nuevo. El emisor saca de ahí lo que va en vuelo por cada enlace, su
  tasa de entrega (como BBR, pero medida en el reloj del receptor: las
  respuestas en lote no la inflan) y su RTT.
- **`linkcc.h`**, un BBR por enlace: `bw` la mayor muestra de 1–2 s,
  `rtprop` el menor RTT de 10 s, STARTUP ×2, DRAIN, PROBE (5/4, 3/4, 1×6);
  una ronda que pierde más del 5 % baja `bw` a 7/8 (las pérdidas son las del
  enlace, por `SFLOSS`); con menos en vuelo que `bw × rtprop` la muestra
  solo cuenta si es mayor (*app-limited*). La capacidad del motor es la
  semilla.
- **`link_cc`** (experimento, apagado): el ETA toma de cada enlace su ritmo,
  su retardo sin cola (`rtprop` sobre el menor: los `SFACK` vuelven todos
  por el mismo enlace, así que la diferencia es la de ida) y su cola en
  vuelo; pasado `governor_target_ms` de cola el enlace es último recurso. La
  oferta de GRIT es la suma de los ritmos de los enlaces, tenga o no cola el
  flujo: cada enlace sondea y cuida la suya.
- **Laboratorio** (`~/campo/2026-10-28/lc4`, n = 2, 7 casos × 2 sentidos,
  `1a16000`): gana donde los enlaces son distintos — `cell4` +11 % bajando
  y +31 % subiendo, `ratswitch` +21 / +5, `reorder50` +8 / +10, `het` +4 /
  +9 — y pierde en `equal` (−13 / −5: empuja más de lo que la cola de netem
  aguanta, más pérdidas y p99 de 126 a 354 ms) y en `protect` de bajada
  (−10). netem no premia la cola como un módem real (lo vimos en el paso 2):
  decide el terreno.
- **Lo que el laboratorio escondía:** remoto06 tenía los techos de fábrica
  de los búferes de socket (`net.core.rmem_max` 208 KB) y toda bajada ahí
  quedaba en ~20 Mbit/s con el RTT plano: el socket del protocontrolador hacia
  el destino fija su `SO_RCVBUF` (eso apaga el autoajuste) y el techo lo
  dejaba en ~200 KB a 100 ms. Las corridas de la mañana que mezclaban nodos
  (q1, rot1, rng1, s2q) tienen la bajada sesgada hacia abajo. Arreglado en
  el nodo (`/etc/sysctl.d/90-cengarde-lab.conf`, también en
  `setup-node.sh`), en el laboratorio (aviso si el techo es menor que 4 MB)
  y en el motor (`SO_RCVBUFFORCE`, como los sockets del túnel: cualquier VPS
  sin nuestro `sysctl.conf` habría tenido lo mismo).
- **Terreno** (`~/campo/2026-10-28/field-l1`, `1a16000` en el NUC y el VPS,
  2 rondas alternadas, 100 MB de bajada de cachefly y 15 s de subida con
  iperf3, calentamiento de 50 MB por brazo):

  | brazo | bajada R1 / R2 | ping p95 | subida R1 / R2 | ping p95 |
  | --- | --- | --- | --- | --- |
  | `subflow` solo | 62,8 / 64,7 | 45 / 44 | 84,5 / 81,2 | 65 / 68 |
  | **`link_cc`** | **124,6 / 97,7** | 46 / 45 | 86,3 / 83,7 | 129 / 128 |
  | OMR | 144,8 / 154,7 | 147 / 134 | 94,8 / 76,7 | 301 / 157 |

  La bajada sube 98 y 51 % y llega a 86 y 63 % de OMR con un tercio de su
  ping p95. El VPS midió 231 Mbit/s en un enlace (94 sin `link_cc`): el
  módem daba eso y la capacidad del motor no lo veía. El enlace que pierde
  (Claro) se recorta solo (59–70 rondas con más de 5 % perdido). La subida
  no cambia: GRIT se queda sin qué enviar (`empty`, 98 mil turnos), así que
  el límite está en lo que llega desde la LAN, no en los enlaces; su ping
  p95 sí sube (65 → 128 ms, OMR 157–301). Datos móviles de la tanda: ~1,7 GB.
- **Por qué la subida «se queda sin datos»** (`~/campo/2026-10-28/up1`, ~1,1
  GB): no se queda. `empty` es el equilibrio de un proxy (cada pasada manda
  todo lo que leyó), y el emisor de la LAN pasa el 99 % del tiempo limitado
  por la ventana del NUC (`rwnd_limited`) porque GRIT lee al ritmo al que
  despacha. El socket de la LAN nunca se llena (0 la mitad de las muestras,
  64–174 KB la otra). Duplicar el búfer de GRIT no cambia nada (4 MB: 99,7 y
  81,7; 2 MB: 95,9, 94,8 y 89,3). Durante la subida cada enlace tiene 40–100
  ms de cola sobre su `rtprop`, el ping sube con `link_cc` sin que suba el
  caudal, y OMR pasa lo mismo o menos (77–95): **la subida está en la
  capacidad de subida de los módems**. Lo que sí está mal: `link_cc` suma
  140 Mbit/s de `bw` en la subida para 86–100 entregados (el máximo de 1–2 s
  toma las ráfagas de concesiones del operador), así que mete cola sin
  ganar. Sin protocontrolador, el TCP directo sube a 12–16 Mbit/s (una
  pérdida temprana, `ssthresh` 25, y CUBIC crece lento a 50–70 ms) y la
  variante `a` a 20.
- **El estimador inflado y el sondeo a ×2** (`360c78d` … `c6da8e5`): el
  `SFACK` dice ahora cuándo llegó el `sseq` más nuevo, así que el emisor
  saca el retardo de ida de cada enlace y su cola (sobre su mínimo de 15–30
  s), sin depender de `bw` ni del camino de vuelta. Con eso, cuatro piezas
  (`link_cc_bits`): 1, la cola del reparto por retardo; 2, los modos por la
  cola (STARTUP termina al pasar el objetivo, DRAIN al bajar de la mitad,
  PROBE drena sobre el objetivo y sondea solo bajo la mitad); 4, `bw` a lo
  más 5/4 de lo entregado con cola; 8, STARTUP a ×3/2. Las cuatro juntas
  perdieron 20–46 % en el laboratorio; por piezas (`lc6`, `lc7`, n = 2):
  **2 + 8 ganan +8,7 %** de media geométrica sobre `link_cc` sin cola, con
  3–10 veces menos pérdidas por enlace; **1 pierde 20–53 %** (su retardo es
  el del paquete más nuevo con acuse y deja fuera lo enviado después: el
  reparto llenó de más los enlaces); 4 es algo peor en netem.
- **Terreno** (`field-l2`, 2 rondas, ~1,8 GB):

  | brazo | bajada R1 / R2 | ping p95 | subida R1 / R2 | ping p95 |
  | --- | --- | --- | --- | --- |
  | 2 + 8 (10) | 85,0 / 97,6 | 45 / 50 | 77,9 / 59,4 | 113 / 133 |
  | + tope (14) | 83,0 / 75,4 | 44 / 45 | **95,5 / 101,4** | 114 / 110 |
  | OMR | 165,8 / 89,8 | 74 / 135 | 92,7 / 94,8 | 168 / 144 |

  El tope gana la subida (el operador la concede en ráfagas: es lo que
  inflaba `bw`) y llega a OMR con menos ping; en la bajada no ayuda. Por
  omisión, entonces, 14 en el router (manda la subida) y 10 en el VPS.
- **La bajada, después** (terreno, solo bajada, 100 MB, 2 rondas cada una,
  ~4 GB en total):
  - **El búfer por flujo era el techo** (`field-l3`): lo enviado queda
    retenido hasta su acuse (~150–240 ms con las colas de los enlaces y la
    espera de los huecos), así que 2 MB daban 87,2 / 51,3 Mbit/s y 4 MB
    104,7 / 119,4 (OMR 167,5 / 179,5). Por omisión con `subflow`, 4 MB
    (`6bf192f`); el anillo admite ahora hasta 8 MB (`CG_ST_SLOTS` 512).
  - **8 MB** (`field-l4`): 91,5 / 152,1 contra 108,0 / 108,0 con 4 MB (OMR
    154 / 221): sin decidir.
  - **Lo que retiene los bytes:** con 4 MB el ritmo de GRIT (280–476
    Mbit/s) ya no frena; frena el tiempo que cada byte espera su acuse. Un
    enlace con `bw` 3–6 veces lo que entrega se llena hasta 165–587 ms de
    cola, lo que va por él llega tarde, abre huecos y el temporizador
    reenvía 500–1800 por descarga.
  - **El tope en el VPS** (`field-l5`, 14 contra 10): sin colas hondas y la
    mitad de reenvíos, pero 114,1 / 65,7 contra 89,9 / 104,9: en la ronda 2
    dejó al enlace rápido en ~60 Mbit/s donde sin tope dio 176. Las
    muestras «con cola» salen justo cuando el enlace se frena: subestiman.
  - **Tope solo con cola (`SATQ`, 32) y la cola del reparto como la mayor
    de las dos estimaciones (`QMAX`, 16)** (`968c3ab`, `field-l6`):
    neutros en el laboratorio (−2,5 y −0,9 %); en terreno 126,3 / 87,9 y
    126,3 / 90,1 contra 120,3 / 119,7 (OMR 146,8 / 144,5), y en la ronda 2
    el enlace rápido se degradó solo (su `bw` de 118 a 10–21). Con n = 2 el
    terreno no separa diferencias de ±20 %: OMR anda estable en 145–167 y
    nosotros entre 66 y 126 según la ronda.
  - **`fade` en el laboratorio** (`019be64`, `fade1`, n = 4): tres enlaces
    cuya tasa salta al azar (150/40/15 Mbit/s por 100–500 ms, etc.), como en
    terreno: `bw` 108–146 para ~65 de promedio. En la bajada, 46 (el tope
    solo con cola) gana +23 % sobre 10, 14 +14 %, 62 +6 %; 8 MB nada (los
    RTT del laboratorio son cortos). En la subida, el router con 46 +9 %
    sobre 14 (`fade2`).
  - **Cuatro rondas** (`field-l7`, ~2,2 GB):

    | brazo | media | rondas | contra A | ping p95 |
    | --- | --- | --- | --- | --- |
    | A: 14 / 10, 4 MB | 89,0 | 102 / 84 / 98 / 72 | | 44 |
    | B: 46 / 46, 4 MB | 98,2 | 108 / 105 / 93 / 87 | +11 % | 44 |
    | **C: 46 / 46, 8 MB** | **133,3** | **132 / 163 / 134 / 104** | **+49 %** | 45 |
    | OMR | 153,2 | 154 / 184 / 172 / 103 | +70 % | 82 |

    8 MB gana las cuatro rondas (+29 a +95 %) y llega a 78–102 % de OMR
    con la mitad de su ping. Por omisión con `subflow`: 8 MB, y 46 en el
    VPS (el router sigue en 14 hasta medir su subida); el anillo admite
    ahora 16 MB, como el TCP de OMR.
- **OMR por dentro, en la bajada** (`~/campo/2026-10-28/omr-down`, solo
  lectura): xanmod 6.12, BBR y `fq`, `tcp_rmem`/`wmem` hasta 16 MB en los
  dos extremos, `tcp_notsent_lowat` 128 KB, `ss-server --mptcp` con el
  planificador MPTCP `default`. Sus BBR también se inflan (suman 460–500
  Mbit/s para 109–167) y también llenan colas (WOM a 380 ms); lo que tiene
  de más es volumen: ~2 MB de ventana en cada enlace rápido y hasta 7 MB
  esperando en la VPS, y que el planificador da datos a un subflujo solo
  mientras su ventana tiene espacio (un enlace que se frena deja de
  recibir solo). Nuestro reparto es por ritmo; `cg_lc_cwnd` existe pero
  todavía no frena nada.
- **16 MB contra 8 MB y el router con 46** (`field-l8`, `73123f7`, ~3 GB):

  | brazo | media | rondas | ping p95 |
  | --- | --- | --- | --- |
  | **bajada, 8 MB** | **129,2** | 73,7 / 157,3 / 174,9 / 111,1 | 50 |
  | bajada, 16 MB | 97,4 | 142,2 / 64,7 / 103,8 / 78,9 | 48 |
  | OMR | 139,5 | 116,2 / 160,3 / 120,0 / 161,3 | 130 |
  | **subida, router 14** | **100,5** | 98,0 / 103,0 | 182 |
  | subida, router 46 | 97,0 | 90,8 / 103,3 | 186 |
  | OMR | 91,6 | 90,5 / 92,7 | 272 |

  16 MB pierde (−24 % de media geométrica): más volumen en vuelo llena de
  más los enlaces. Queda 8 MB por omisión (el tope de 16 se queda). Con 8 MB
  la bajada es el 93 % de OMR con 40 % de su ping, y gana una ronda (175
  contra 120); la subida le gana ~10 % con 2/3 de su ping. El router sigue
  en 14.
- **El freno de ventana** (`43f32cd`, `link_cc_bits` 64, `wnd1`): el reparto
  le da datos a un enlace solo mientras su ventana tiene espacio, como el
  planificador de OMR. Neutro en el laboratorio (n = 4; `fade`, `cell4`,
  `equal`, `het`, `reorder50`, `lossy`: bajada −6 a 0 %, subida 0 a +4 %);
  apagado. La ventana de un enlace sale del mismo `bw` inflado, así que
  frena tarde, igual que la de BBR en OMR.

## Pendiente

- `link_cc`: decidir si pasa a ser lo normal con `subflow` (más rondas de
  terreno: la bajada de OMR varía 90–166 entre rondas) y quitar
  `link_cc_bits` una vez fijo; un enlace de subida débil (Claro) sigue con
  colas de 280–360 ms.
- La salida WAN del NUC por el anillo MAN (remoto07 detrás de un CPE): si el
  trunk o el enrutamiento del NUC limitan.
- Un enlace que muere en plena descarga hace caer el ritmo de GRIT y deja
  sus pérdidas al temporizador (sin paquetes posteriores ni sondas que las
  digan).
- La pérdida de lo copiado, por enlace (qué enlaces llevaron cada copia).
- El DATA del túnel (IP) con la misma evidencia: el rescate reenvía hoy lo
  que el reordenador pide, y en terreno eso llegó tarde.
- Por qué el túnel declara perdido lo que llega tarde con estos módems (¿los
  enlaces no son FIFO?, ¿plazos cortos para la cola de Claro?).
- El nombre nuevo (propuestas de Sonnet: Rhizon, Cordyn, Ligra…; revisar
  choques).

## Cambios

- 2026-10-28: creada.
- 2026-10-28: el paso U1 (protocolo 12, `subflow`), la regla de la evidencia
  y el RACK de la mezcla apagado; el temporizador con margen; terreno.
- 2026-10-28: el paso 2 (oferta, objetivos por omisión) y el 2b (`SFACK`,
  `linkcc.h`, `link_cc`); el techo de búferes de remoto06.
