# 029 — MPGRIT: el motor y GRIT en un solo protocolo multienlace

- **Fecha:** 2026-10-28
- **Estado:** vigente (decisión tomada; paso 1 en curso; 2026-10-29: la
  redundancia sobre GRIT)
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
- **Los modos (LuCI, `option mode`):** *Bonding* (lo de arriba: `split`,
  GRIT, `subflow`, `link_cc`; lo que trae un router nuevo), *Redundancia*
  (copias, rescate, el mismo motor; primero con el protocontrolador A,
  porque GRIT perdía ahí) y *Personalizado* (como antes). **GRIT perdía en
  redundancia por dos cosas** (2026-10-29): una copia perdida en todos sus
  enlaces nunca le llegaba como pérdida (solo su temporizador, 300 ms
  tarde) y el ritmo del mejor enlace lo empujaba aunque tuviera cola (srtt
  de 665 ms). Arregladas (`99f04c5`, `6139fd3`), **en terreno le gana a A:
  bajada +9 % con menos ping y 15 % menos datos, subida +12 %**, y la
  redundancia pasa a GRIT. El VPS los sigue con `cengarde-vps-setup
  mode`, y **un router lo administra** con una llave SSH que solo puede
  correr `cengarde-vps-admin` (modo, FEC, protocontrolador y una lista de
  ajustes del motor), desde la pestaña *VPS* de LuCI.
- **La prueba de techo con captura** (`bench/techo.c`, `techo.sh`,
  `techomat.py`): registros de texto plano numerados por cada módem directo
  al VPS, luego por GRIT y luego por UDP a tasa fija, con cada paquete
  capturado en los dos extremos. Encontró que **el VPS partía en dos cada
  paquete lleno de bajada por Claro** (su `cg0` de 1420 contra los 1460 de
  la ruta de vuelta): con `tun_mtu = 1380` la bajada de GRIT pasó de 97 a
  137–152 Mbit/s y Claro de 9 a 41–56. Y mostró que el codo de cada módem
  es abrupto y se mueve (WOM en bajada: limpio a 52 Mbit/s, 1,2 s de cola a
  60): de ahí el **MTPT** (*Maximum Transfer Point Tracking*, nombre del
  usuario), la cola objetivo de cada enlace por perturbar y observar.

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

### Los modos y el VPS administrado desde el router (2026-10-28, noche)

El usuario pidió dejar lo ganador como lo normal del bonding, la redundancia
«como siempre ha servido», un cambio rápido entre las dos en el modo simple,
la FEC, la paridad y el rescate en el avanzado, el VPS espejado, y un panel en
el NUC para cambiar ajustes del VPS sin entrar a él (el NUC administra el VPS
para todos los routers: no hay ajustes por router todavía).

- **El modo del router** (`option mode`, `cengarde-setup` `mode_presets`):
  `bonding` y `redundancy` fijan los cuatro interruptores (`split`,
  `protocontroller`, `subflow`, `link_cc`) y traen el resto de su ajuste
  como valor por defecto, que una opción puesta aparte gana; `custom` (o sin
  `mode`, un router que ya existía) toma todo como está. Bonding: `split`
  con `split_eta`, `capacity_weights`, `split_spread_ms` 250 sin copias,
  `probe_idle_ms` 100, `mute_behind_ms` 0, `reorder_max_wait_ms` 500,
  `reorder_pool_kb` 32768, rescate, `tcp_aware`, GRIT con
  `protocontroller_nack` apagado (el perfil de terreno y lo de esta
  historia). Redundancia: copias, rescate, `tcp_aware` y el protocontrolador
  A. LuCI muestra el modo en *General*, y cada opción que el modo decide
  muestra su valor y se guarda solo si se aparta; el cloud-config lleva el
  modo al VPS (`cengarde-vps-setup mode`).
- **El VPS** (`cengarde-vps-setup mode`): `bonding` pone `BONDING="yes"` en
  `nat.conf` (el motor recibe `subflow`, `link_cc` y `protocontroller_nack =
  no`) y el protocontrolador; `redundancy`, el protocontrolador sin
  `BONDING`; `custom`, sin `BONDING` y el protocontrolador como esté.
  `engine.local` va después y gana (el motor toma la última línea de una
  clave).
- **La administración** (`cengarde-vps-setup admin-key`,
  `cengarde-vps-admin`, `cengarde-setup vps-key` y `vps`): el router hace una
  vez una llave ed25519 de dropbear (`/etc/cengarde/vps_admin_key`, en
  `conffiles`) y LuCI muestra la orden para autorizarla; en el VPS entra a
  `authorized_keys` de root con `command="/usr/local/sbin/cengarde-vps-admin",restrict`.
  Ese comando forzado solo acepta `get`, `mode`, `fec`, `protocontroller`,
  `set CLAVE VALOR` y `unset CLAVE`, con las claves de una lista (FEC,
  rescate, reparto, gobernador, `tcp_aware`, `link_cc_bits`, búfer y cola
  del protocontrolador, `parq`) y valores de letras, cifras, `.`, `_` y `-`
  (hasta 16); parte las palabras sin globos y nunca pasa nada a una shell.
  `set` reescribe `engine.local` y regenera la configuración (`refresh`).
  El router se conecta con `dbclient -y` (acepta la llave de host la
  primera vez y la recuerda) a la primera dirección del VPS o a
  `vps_ssh_host`. Con `vps_admin` encendido, el modo del router va solo al
  VPS en cada cambio.
- **Probado:** la prueba en seco del VPS (`setup-dryrun.sh`: la llave una
  vez, rechazos, `get`, `set`, `unset`, `mode`, `fec`, lo que no está en la
  lista, valores con `;`, `-` o `*`, una shell; encontró que `$(printf
  '\n')` en un `case` dejaba el patrón vacío y rechazaba toda llave) y
  `e2e.sh` (los modos en el archivo del motor; la llave y `cengarde-setup
  vps` contra el dropbear de la VM del VPS con el comando forzado).

**La redundancia con lo nuevo** (`~/campo/2026-10-28/red1`, `73123f7`,
remoto06 y remoto07, `split` apagado, FEC v3 y rescate en todos los brazos,
n = 3 × 30 s, cubic, Mbit/s y su cambio contra el túnel transparente):

| caso | sentido | túnel | A (TCP en el medio) | B (GRIT) | B con `subflow` |
| --- | --- | --- | --- | --- | --- |
| lossy | bajada | 39,7 | **51,7 (+30 %)** | 44,0 (+11 %) | 46,5 (+17 %) |
| lossy | subida | 38,0 | **47,3 (+24 %)** | 44,5 (+17 %) | 46,2 (+22 %) |
| cell4 | bajada | 12,2 | **17,7 (+45 %)** | 14,2 (+17 %) | 17,7 (+45 %) |
| cell4 | subida | 12,5 | **17,7 (+41 %)** | 15,5 (+23 %) | 16,8 (+34 %) |
| protectcut | bajada | 6,5 | **35,3 (×5,4)** | 10,0 | 18,4 |
| protectcut | subida | 6,6 | **33,2 (×5,0)** | 13,4 | 16,9 |
| fade | bajada | **49,8** | 46,3 (−7 %) | 27,4 (−45 %) | 41,4 (−17 %) |
| fade | subida | **49,3** | 45,2 (−8 %) | 27,1 (−45 %) | 39,8 (−19 %) |
| het | bajada | **54,7** | 53,2 (−3 %) | 52,5 (−4 %) | 45,8 (−16 %) |
| linkdeath | bajada | **36,5** | 35,5 (−3 %) | 35,9 (−2 %) | 33,9 (−7 %) |

En redundancia gana A: el TCP del kernel en el medio con las copias por
debajo no ve pérdidas que repetir, y gana mucho donde el túnel solo no
alcanza (pérdida al azar, celulares, el principal cortado), a cambio de
2–8 % en enlaces limpios y un ping p99 mayor (lossy 128 contra 65 ms, cell4
215 contra 92: dentro del presupuesto de ~300 ms). GRIT en redundancia
pierde en `fade` (−45 %): su ritmo sigue la mezcla de copias, que se ve como
un enlace que salta; los subflujos sin `split` reenvían miles de paquetes
(una copia que llega por otro enlace no tapa la pérdida del suyo). Por eso
la redundancia queda con A.

### La prueba de techo con captura (2026-10-28, noche)

El plan del usuario (`capture-matrix-plan`): un flujo de texto plano con
números que caigan justo en el MTU, enviado por los cuatro módems a la vez
(TCP BBR, luego GRIT, luego UDP a tasas fijas), con cada paquete guardado en
los dos extremos, el reloj de pared común (chrony con el SHOA), el estado de
TCP y de GRIT y la radio; después ordenarlo y ver el desorden real, cuánto
habría que esperar y cómo TCP y GRIT entregan.

- **Las herramientas** (`bench/techo.c`, `techo.sh`, `techomat.py`, en
  `bench/README.md`): registros de 1300 bytes `CGT L2 Q… T…`; en TCP los dos
  extremos fijan el MSS al registro y la prueba apaga TSO, GSO y GRO, así
  cada segmento lleva uno y su secuencia lo dice (en el campo, todos los
  segmentos de los cuatro módems medían 1300); el VPS y el NUC capturan con
  búfer de 64 MB y dicen lo que descartaron (0 en las tandas válidas). `-s`
  corre un módem tras otro, y `techomat` alinea cada uno por su inicio.
- **Lo que el campo enseñó a la herramienta:** un `ssh` que no volvía (el
  trabajo de fondo se quedaba con su entrada); capturas que no se detenían y
  se apilaban en el único vCPU del VPS; el NUC perdiendo paquetes al mandar
  cuatro capturas completas por SSH (ahora en su RAM); el GRO del NUC y el
  hipervisor del VPS juntando paquetes antes de `tcpdump` (GRO apagado en el
  NUC; `techomat` cuenta por el largo IP).

**El MTU de Claro.** En la bajada de GRIT (`techo1-seq`), 51 966 de los
51 982 paquetes que el VPS mandó por Claro salieron fragmentados: su `cg0`
tenía 1420 (lo del motor) y los paquetes 1476 bytes, y la ruta de vuelta de
Claro acepta 1460 (el VPS lo aprende por ICMP). Perder cualquiera de las dos
mitades pierde el paquete: Claro llevaba 9 Mbit/s con GRIT (68 directo) y
reenviaba la mitad (1,995 envíos por registro). La subida no lo sufría (el
router usa 1380: 1436 bytes). Con `tun_mtu = 1380` en el VPS
(`cengarde-vps-setup` lo escribe desde `4d01b32`):

| tanda | bajada de GRIT | Claro | envíos por registro |
| --- | --- | --- | --- |
| `techo1-seq`, 1420 | 96,9 Mbit/s | 8,9 | 1,307 |
| `grit-mtu1380` | 137,3 | 40,8 | 1,034 |
| `techo2-sim` (los cuatro a la vez) | 152,3 | 55,9 | 1,040 |

**La curva de cada módem** (`curva1`: UDP de 40 a 160 % del techo TCP en
pasos de 10 %, 10 s cada uno, un módem a la vez; retardo de ida p50 / p95):

| módem | bajada limpia hasta | pasado el codo | subida limpia hasta | pasado el codo |
| --- | --- | --- | --- | --- |
| Entel | ~49 Mbit/s (26 / 84 ms) | 64–69: p95 0,8 s, 2–4 % de pérdida | ~14 (34 / 65) | 1 s |
| WOM | ~52 (34 / 175) | 60: p50 1,2 s; 75: 63 % de pérdida y 4,7 s | ~22 (59 / 347) | 28: 21 % |
| Movistar | ~16 (50 / 210), inestable aun abajo | colas de 5 a 60 s | ~21 (38 / 111) | 27: 22 % |
| Claro | ~78 (20 / 155) | 108: 5 % y p95 2 s | ~44 (51 / 158) | 48: 10 % |

- **El codo es abrupto:** 10–15 % de más pasa de decenas de ms a segundos.
  En la bajada los operadores tienen búferes enormes (Movistar 60 s, WOM
  20 s, Claro 2 s, Entel 1 s); en la subida pierden (20–40 % con 0,5–1 s).
- **Bajo el codo los enlaces son rápidos:** p50 de ida de 15–30 ms en la
  bajada y 26–60 en la subida; la espera del reordenador viene de la cola.
- **El codo se mueve:** Entel a 79 Mbit/s salió limpio minutos después de
  perder a 64.
- **La suma de los codos con colas sanas:** ~195 Mbit/s de bajada y ~100 de
  subida; OMR saca 140–150 de bajada con colas de 300–500 ms.

**Los cuatro a la vez** (`techo2-sim`, 30 s): TCP BBR por módem suma 96,5 de
subida y 225,7 de bajada, GRIT 87,3 (90 %) y 140–152 (62–67 %). Juntando los
registros de BBR en el orden en que salieron (el bonding ideal), un
reordenador habría retenido p50 103 ms en la subida y 250 ms en la bajada, y
en 300 ms habría entregado en orden el 89 % y el 57 %: BBR llena las colas
(Movistar p95 870 ms). Con GRIT en la subida, p50 46 ms y 90 % en 300 ms.

**MTPT** (`df322d9`, `link_cc_bits` 128, `link_cc_budget_ms` 300): cada
enlace busca su propia cola objetivo perturbando y observando, como el MPPT
de un cargador solar (el nombre, *Maximum Transfer Point Tracking*, es del
usuario). Cada segundo el objetivo se mueve un paso de 20 ms: sigue si lo
entregado creció 3 % o más, vuelve si cayó, baja si quedó plano, baja dos
pasos con más de 5 % de pérdida y se queda quieto si el enlace no llegó a la
mitad de su objetivo. Su techo es `link_cc_budget_ms` menos cuánto más lento
es el enlace que el más rápido: un enlace chico (Movistar con 2 Mbit/s en
congestión) suma lo que llega a tiempo y no estira la espera de los demás
(la regla del usuario).

- **Laboratorio** (`mtpt1`, n = 3, 8 casos × 2 sentidos, contra 14 / 46):
  con 300 ms casi neutro (+0,6 % de media: `ratswitch` bajada +11 %,
  `lossy` subida +7 %, `cell4` bajada +5 %, `protect` subida −9 %); con
  500 ms peor (−2 a −3 %, y el p99 de `protect` sube). Netem tiene tasa fija
  y no premia una cola más honda como un módem.
- **Terreno** (`field-m1`, ambos extremos en `9616051` con el VPS en
  `tun_mtu = 1380`, 4 rondas de bajada de 100 MB y 2 de subida de 15 s):

  | brazo | bajada | ping p95 | subida | ping p95 |
  | --- | --- | --- | --- | --- |
  | **objetivo fijo (14 / 46)** | **142,7** (129 / 144 / 180 / 118) | 50–58 | **104,9** (98 / 111) | 146–159 |
  | MTPT 300 ms | 111,0 (150 / 101 / 97 / 96) | 47–70 | 104,7 (101 / 108) | 163–190 |
  | OMR | 101,8 (96 / 94 / 76 / 141) | 99–219 | 90,4 (91 / 90) | 271–279 |

  **Con el MTU de Claro arreglado, el bonding le gana a OMR en los dos
  sentidos: +40 % en la bajada y +16 % en la subida, con la mitad o menos
  de su ping.** MTPT v1 queda apagado: neutro en la subida y −22 % en la
  bajada; una descarga de 100 MB dura 6–8 s, cinco a siete épocas, y en
  ellas algunos enlaces bajaron su objetivo a 20–40 ms (el reparto los trata
  como último recurso antes que con 60). Lo que sigue: que explore solo
  hacia arriba desde el objetivo base y que lo plano lo deje quieto.
- **MTPT v2** (`9c06ce5`: solo por encima de `governor_target_ms`, quieto
  con lo plano). Laboratorio (`mtpt2`, n = 3): −3 % de media (`fade` −9 /
  −6 %, `lossy` subida −10 %, `ratswitch` subida +12 %). Terreno
  (`field-m2`, 3 rondas de bajada y 2 de subida):

  | brazo | bajada | ping p95 | subida | ping p95 |
  | --- | --- | --- | --- | --- |
  | **objetivo fijo** | **146,7** (146 / 148 / 146) | 47–60 | **108,1** (107 / 110) | 156–163 |
  | MTPT v2 | 129,6 (123 / 121 / 145) | 49–53 | 101,4 (98 / 105) | 161–193 |
  | OMR | 115,9 (74 / 160 / 113) | 79–448 | 93,9 (93 / 95) | 355–726 |

  **El MTPT queda apagado** (−12 % y −6 %): con el MTU arreglado, el
  objetivo fijo de 60 ms ya llena bien los módems, y subirlo solo agrega
  cola. El brazo fijo le ganó a OMR en las dos tandas: bajada 142,7 y
  146,7 contra 101,8 y 115,9 (+27 a +40 %), subida 104,9 y 108,1 contra
  90,4 y 93,9 (+15 %), con una fracción de su ping. Lo que el MTPT podría
  aportar queda para enlaces que hoy se quedan cortos (un Starlink, una
  fibra) o colas que cambien más lento que una descarga.
- **El MTU de cada camino, vigilado** (`f9b9f1e`): el servidor lee cada 10 s
  el MTU de la ruta hacia cada enlace del router (el que el kernel aprendió
  por ICMP incluido), lo dice en el estado (`path_mtu`) y avisa con el
  `tun_mtu` que cabría si los paquetes de bajada no caben; el cliente ya lo
  hacía con la subida.

### La redundancia sobre el motor del bonding (2026-10-29)

El usuario: «arreglemos el modo redundancia, que está pobre», y que use el
mismo motor del bonding con el protocontrolador A (GRIT perdió en `red1`).

- **El SQM del NUC** (`sqm.cg0`, `cake` de 22 Mbit/s de subida en `cg0`)
  dejaba la subida de redundancia clavada en 20,3–20,7 Mbit/s: el TCP del
  protocontrolador A pasa por `cg0`; el bonding no lo sentía porque GRIT
  escribe directo en los sockets de los enlaces. Sin él: 43–44. Apagado
  con el OK del usuario.
- **La bajada mandaba copias completas por todos los caminos:** el servidor
  no tiene la lógica de lo que sobra (`redund_spare` es solo del router),
  así que `mix` y `rlc` no cambiaron un byte (3,7–4,2 bytes por byte útil) y
  las colas de los enlaces lentos llegaban a 1–10 s (`field-r1`).
- **Copias a tiempo con el ritmo de cada enlace** (`a31e8b4`): con
  `link_cc` y `redund_pace_ms`, la llegada esperada de las copias toma el
  ritmo, el retardo sin cola y la cola de cada enlace de su `linkcc.h`,
  como el reparto del bonding; una copia va por un enlace solo si llega a
  menos de 150 ms de la primera. Laboratorio (`red2`, n = 3, contra copias
  completas): `fade` +13 / +10 %, `lossy` subida +12 %, `protectcut` p99 del
  RTT de 1,9 s a 0,19 s; bytes por byte útil de ~3 a ~2 (`protectcut` 2,1 →
  1,3). Con la capacidad del motor en vez del ritmo propio (`pace150`)
  ahorra menos y no gana tasa; con MTPT, parecido al ritmo propio.
- **Terreno** (`field-r3`, protocontrolador A, sin SQM, 2 rondas):

  | brazo | bajada | ping p95 | bytes / útil | subida | bytes / útil |
  | --- | --- | --- | --- | --- | --- |
  | copias completas | 60,0 / 58,5 | 48–52 | 3,53 / 3,94 | 42,0 / 40,9 | 2,62 / 2,71 |
  | **ritmo propio + 150 ms** | **117,4 / 112,9** | 182–242 | **1,59 / 2,26** | 43,9 / 35,5 | 2,60 / 2,99 |
  | ritmo propio + MTPT | 111,1 / 74,6 | 137–221 | 1,92 / 2,57 | 42,2 / 48,1 | 2,69 / 2,30 |

  La bajada en redundancia se duplica con 40–55 % menos datos; la subida
  la pone el mejor enlace (Claro, ~40–45), pero la cola de Entel baja de
  1,3–2 s a 0,15 s. **El modo redundancia queda así**: protocontrolador A,
  `split` apagado, `subflow` y `link_cc`, `redund_pace_ms` 150, rescate y
  `tcp_aware` (router, LuCI y `cengarde-vps-setup mode redundancy`, que
  escribe `REDUNDANCY` en `nat.conf`).

### La redundancia con GRIT (2026-10-29)

El usuario: «¿y en B? ¿por qué la redundancia aún pierde en B? Sería el modo
que necesitamos pronto».

- **Lo de `red1` eran dos cosas distintas.** Sin subflujos, GRIT no pierde
  paquetes en `fade` (0 reenvíos) pero va a la mitad: su ritmo sigue la
  mezcla de copias, con el ping más bajo de todos (p99 53–70 ms). Con
  subflujos, el RACK de GRIT se apaga (lo deciden los enlaces), pero el
  `SFLOSS` de una copia se descartaba (`copied`: pudo llegar por otro
  enlace), así que un paquete perdido en *todos* sus enlaces solo lo
  encontraba el temporizador, 300 ms después de la ida y vuelta. En `red1`
  los 1900–2700 reenvíos de `fade` fueron todos por temporizador.
- **La oferta no era el problema:** en redundancia la pista de GRIT ya es el
  ritmo del mejor enlace, no la suma (`*bps = split ? sum : best`).
- **Con el motor nuevo** (`red3`, `afba781`, n = 3, contra A con el mismo
  preset): B con `subflow`, `link_cc` y `redund_pace_ms` 150 ya empata en
  caudal (`fade` +8 / −6 %, `protectcut` +6 / −3 %, `lossy` 0 / −13 %),
  pero con ~20 mil reenvíos por temporizador en `fade` (ninguno duplicado:
  pérdidas reales) y ping p99 más alto (het 253 contra 124 ms). B sin el
  ritmo por enlace (copias completas o según la capacidad) sigue en la
  mitad en `fade` y `protectcut`.
- **Una copia perdida en todos sus enlaces es una pérdida segura**
  (`99f04c5`): el anillo del rescate guarda los enlaces de las copias de
  cada paquete y el `SFLOSS` del último se lo entrega a GRIT como si el
  paquete hubiera ido solo por ese enlace (`cg_resc_lost_on`, `all_copies`
  en el estado). `red4` contra `red3`:

  | caso | sentido | A | B | B con el arreglo |
  | --- | --- | --- | --- | --- |
  | lossy | bajada | 51,8 | 52,0 | **55,5** |
  | lossy | subida | **52,5** | 45,7 | 47,7 |
  | cell4 | bajada | 17,6 | 18,3 | **18,4** |
  | fade | bajada | 52,1 | **56,5** | 54,9 |
  | fade | subida | **53,3** | 50,1 | 47,7 |
  | linkdeath | subida | 35,7 | 35,6 | **37,0** |
  | protectcut | bajada | 33,3 | 35,2 | **35,4** |
  | protectcut | subida | 34,1 | 33,1 | **35,2** |
  | het | bajada | 52,5 | 50,8 | **54,4** |
  | het | subida | 53,0 | 52,8 | **54,7** |

  Los reenvíos por temporizador caen a una fracción (het 903 → 65, lossy
  1295 → 283); los que quedan son pérdidas reales (0 duplicados en el
  receptor) que GRIT ahora repara a tiempo. El ping p99 de B sigue sobre el
  de A (het 311 contra 124 ms, `protectcut` 368 contra 201).
- **Terreno** (`field-r4`, VPS en `99f04c5`, NUC en `a31e8b4`, es decir, la
  subida sin el arreglo; 3 rondas, ~2,3 GB):

  | brazo | bajada | ping p95 | bytes / útil | subida | ping p95 | bytes / útil |
  | --- | --- | --- | --- | --- | --- | --- |
  | A | 133,2 / 125,1 / 108,8 | 140–176 | 2,10–2,59 | 41,5 / 43,5 / 45,5 | 92–95 | 2,65–2,81 |
  | B | 111,5 / 144,9 / 99,5 | 300–349 | 2,02–3,14 | 47,6 / 48,9 / 52,5 | 172–252 | 2,34–2,51 |

  En redundancia B ya no pierde: la bajada empata (118,6 contra 122,4, dentro
  de lo que varía una ronda) y la subida gana 14 % con menos datos. Lo que le
  queda es el ping, el doble que A, en el borde del presupuesto de ~300 ms.
  Con el arreglo en los dos extremos (`field-r5`): bajada 112,4 contra 122,3
  (−8 %), subida 46,5 contra 41,7 (+11 %), el ping igual de alto.
- **Por qué el ping:** el estado de GRIT en el VPS durante la bajada de B
  (`field-r5/pacers.jsonl`) mostró la oferta del motor en 155–273 Mbit/s,
  el ritmo del mejor enlace con su sondeo, para un solo módem (los
  estimados por enlace se inflan con las ráfagas: 68–115 Mbit/s cada uno en
  reposo), ofrecida tenga o no cola el flujo (`CG_SCC_OFFER_LINKS`, pensada
  para el bonding, donde cada enlace cuida su cola). GRIT llegó a un srtt de
  665 ms, 6,7 MB en vuelo, una ventana de 19,7 MB y ~960 reenvíos por
  temporizador en los primeros segundos.
- **Copiado, la oferta solo sin cola** (`6139fd3`): la pista dice cuando el
  motor copia (`CG_PCTL_HINT_COPIED`) y entonces la oferta se calla mientras
  el flujo vea cola sobre su objetivo (`CG_SCC_OFFER_COPIES`, como la
  capacidad del motor); el bonding sigue igual. Laboratorio (`red5`, n = 3,
  contra A de `red3`):

  | caso | sentido | A | B con los dos arreglos | ping p99 A / B |
  | --- | --- | --- | --- | --- |
  | lossy | bajada | 51,8 | **55,0** | 127 / 130 |
  | lossy | subida | 52,5 | **55,3** | 127 / 127 |
  | cell4 | bajada | 17,6 | **18,3** | 211 / 175 |
  | cell4 | subida | 17,7 | **18,4** | 194 / 188 |
  | fade | bajada | 52,1 | **57,2** | 283 / 282 |
  | fade | subida | **53,3** | 46,9 | 285 / 277 |
  | linkdeath | bajada | 35,6 | **36,5** | 131 / 129 |
  | linkdeath | subida | 35,7 | **37,2** | 132 / 132 |
  | protectcut | bajada | 33,3 | **34,3** | 201 / 280 |
  | protectcut | subida | 34,1 | **35,4** | 206 / 293 |
  | het | bajada | 52,5 | **55,4** | 124 / 292 |
  | het | subida | 53,0 | **55,7** | 120 / 239 |

  B gana en 11 de 12, con el ping de A salvo en `het` y `protectcut`; los
  reenvíos bajan a la mitad o menos (`lossy` 3898 → 1416, `linkdeath`
  3264 → 378). En terreno, la bajada (`field-r6`, VPS en `6139fd3`):
  A 125,3 / 123,4 / 120,9 (ping p95 161–204, 2,12–2,30 bytes por útil),
  B 116,7 / 118,8 / 137,7 (161–190, 1,88–2,04): el mismo caudal, el mismo
  ping o menos, ~12 % menos datos.
- **Terreno con los dos arreglos en los dos extremos** (`field-r7`, 3
  rondas, ~2,3 GB):

  | brazo | bajada | ping p95 | bytes / útil | subida | ping p95 | bytes / útil |
  | --- | --- | --- | --- | --- | --- | --- |
  | A | 123,5 / 135,3 / 140,1 | 152–194 | 2,29–2,53 | 44,6 / 44,4 / 50,6 | 96–143 | 2,37–2,72 |
  | **B** | **125,8 / 162,3 / 148,5** | **102–147** | **1,71–2,20** | **52,6 / 52,7 / 51,4** | 129–151 | **2,04–2,40** |

  B gana: bajada +9 % (145,5 contra 133,0) con menos ping y 15 % menos
  datos, subida +12 % (52,2 contra 46,5) con 12 % menos datos y un ping
  algo mayor.
- **El bonding no cambia** (`bondchk`, 8 casos × 2 sentidos, `afba781`
  contra `6139fd3`, media geométrica −1,3 %): las caídas de bajada en
  `equal` y `cell4` son de esos casos, bimodales en cualquier versión; con
  otra semilla (`bondbis`) `afba781` también cae a 75–77 y 38,5.
- **El modo redundancia pasa a B** (`cengarde-setup`, LuCI, `e2e.sh`,
  README; el VPS sirve los dos): lo mismo que el bonding salvo el reparto.
  El usuario, al saberlo: los presets de cada modo pueden diferir en
  cualquier perilla que le sirva a uno y no al otro.

### La noche de los 500 Mbit/s y la memoria de GRIT (2026-10-29, 02:20)

- **El usuario, con Speedtest en un notebook de la LAN del NUC:** 469 Mbit/s
  de bajada y 98,6 de subida, saliendo por el VPS (proveedor «Vultr»). Medido
  en el NUC cada segundo durante la bajada: ~570 Mbit/s entraban por los
  módems (Entel 220, WOM 187, Claro 107, Movistar 58 de media; Entel con
  picos de 284) y ~450 salían a la LAN. De madrugada las celdas están vacías
  y los módems usan 5G desde que se quitaron los bloqueos de banda (el 27):
  el techo de ~195 Mbit/s de la prueba de techo era de la tarde. Cada
  enlace con su propio ritmo (`link_cc`) halló solo esa capacidad.
- **Contra OMR, con el CLI de Ookla y el mismo servidor** (Mundo Chile,
  `~/campo/2026-10-29/st-omr/`), sin FEC en los dos extremos, 3 rondas
  alternadas:

  | | bajada | subida | ping con carga (bajada) |
  | --- | --- | --- | --- |
  | cengarde | 454 / 529 / 678 (554) | 132 / 112 / 133 (126) | 75–135 ms |
  | OMR | 665 / 456 / 746 (622) | 113 / 51 / 146 (103) | 349–871 ms |

  La bajada de OMR ~12 % arriba, dentro de lo que varía una ronda; la
  subida de cengarde +22 % y su ping con carga un quinto del de OMR.
- **La memoria de GRIT se trababa.** Con FEC (RLC), la primera ronda dio
  568; en las dos siguientes el NUC rechazó las conexiones
  (`protocontroller_mem_mb is full`): los dos extremos guardaban 31 flujos
  y sus 64 MB llenos (el VPS 66 MB por enviar, el NUC 26 MB recibidos). El
  fondo es uno para todos los flujos y cada ventana prometía su búfer entero
  (8 MB con `subflow`); lleno de bytes fuera de orden, la trama que tapaba
  cada hueco no hallaba trozo y se descartaba en cada reenvío hasta
  `protocontroller_dead_s`. Con 512 / 256 MB, la memoria volvía a cero tras
  cada prueba. **Arreglo** (`edbfa96`): lo que llega en orden siempre cabe
  (el receptor olvida lo que guarda fuera de orden, `cg_st_rx_renege`, y el
  emisor lo reenvía), cada flujo toma su parte pareja del fondo
  (`flow_cap`) y el fondo es por omisión un octavo de la RAM, de 64 a 1024
  MB (`cg_pctl_mem_auto`).

### Tres ideas para el ritmo por enlace, contra la captura (2026-10-31)

El usuario trajo ideas de otro asistente; se evaluaron sin salir a terreno
con la captura de `curva1` (UDP a tasa fija por módem, 40–160 % de su techo,
cada paquete con su hora de envío y de llegada; `bench/rateeval.py`,
`~/campo/2026-10-31/copilot/results.txt`):

- **La tasa por el máximo de las muestras sobreestima mucho.** En los
  escalones saturados (lo entregado es la capacidad), el máximo de 1,5 s de
  muestras de 40 ms da 5,2 veces la capacidad de bajada y 3,7 de subida; con
  muestras de 100 ms, 3,0 y 2,2; de 200 ms, 2,1 y 1,7 (Movistar el peor:
  6–8 veces). Sin saturar, igual 2,1–2,8 veces lo enviado: el operador
  entrega a ráfagas. **La media de lo llegado en 500 ms acierta**: 0,96 de
  bajada y 1,00 de subida (p90 1,25–1,55), saturado o no. Es lo que explica
  los estimados inflados del terreno (68–115 Mbit/s por enlace en reposo,
  218–273 en la oferta de GRIT).
- **Olvidar rápido no hace falta una vez que no se toma el máximo:** la
  entrega cada 100 ms varía con un coeficiente de 0,45–1,05; un EWMA que se
  acelera con el jitter queda 10–29 % arriba y 12–28 % abajo, puro ruido
  del operador.
- **La pendiente de la cola avisa poco y en falso:** con más de 100 ms/s y
  15 ms de cola, se adelanta al cruce de 60 ms en 65 de 97 escalones, con
  una mediana de 65–70 ms (menos de una ida y vuelta), y saltó también en
  los 7 escalones que nunca cruzaron.
- Las otras dos (la ganancia de sondeo según el jitter, el estado compartido
  entre hilos) no aplican: el sondeo ya se frena por la cola y los módems
  son de operadores distintos.

**El ritmo por la media** (`437ae7c`, `link_cc_bits` + 256): pasado el
arranque, `bw` es la media de lo entregado en los últimos cinco baldes
enteros de 100 ms; con cola en el enlace es su capacidad, sube o baja; sin
cola solo puede subir; tras un silencio queda como estaba hasta tener sus
0,5 s. El máximo de las medias de 500 ms también sobreestima (1,57 y 1,31
saturado), por eso no se usa. Laboratorio (`meanlab`, n = 3, 7 casos × 2
sentidos): neutro (−0,9 % de media geométrica), con menos ping en `fade`
(p99 300 → 223 ms) y `cell4` (214 → 164): netem entrega parejo, sin las
ráfagas que esto corrige. **Terreno** (`meanfield`, 2026-10-31 de tarde,
Ookla, 3 rondas alternadas, bonding):

| brazo | bajada | subida | ping con carga (bajada) |
| --- | --- | --- | --- |
| máximo (14 / 46) | 228,7 / 195,2 / 187,2 (203,7) | 115,8 / 114,2 / 138,1 (122,7) | 79–104 ms |
| **media (270 / 302)** | **238,3 / 250,5 / 250,5 (246,4)** | **131,0 / 128,7 / 131,6 (130,4)** | 69–87 ms |

La bajada +21 % (las tres rondas), la subida +6 %, con algo menos de ping.
Lo que sigue: más rondas (de noche también) antes de dejarlo por omisión, y
la otra mitad de la pieza, quitar la capacidad vieja del reparto.

**Que el ritmo por enlace decida solo** (`3ac9794`, `link_cc_bits` + 512):
un enlace con muestras propias entra al reparto antes de que la capacidad
vieja lo mida, los pesos de la capacidad nunca lo dejan solo con copias
(peso 0) y «lleno» para el rescate es su propia cola sobre el objetivo
(`cg_lc_full`); la capacidad sigue midiendo (estado, pérdidas de la FEC, el
enlace dominante). Laboratorio (`onlylab`, n = 3, contra la media sola):
+3,4 % de media geométrica, `fade` subida +19,7 %, `ratswitch` subida
+12,6 %, `protect` con la mitad de p99 (1493 → 659 y 494 → 249 ms),
`cell4` bajada −4,5 %. Terreno (`onlyfield`, 3 rondas, tarde): la subida
+8 % en las tres (119,5 contra 110,4), la bajada −8 % (234,1 contra 255,0,
perdió dos de tres). Cuatro rondas más (`onlyfield2`) dieron lo contrario:
bajada +7 % (239,1 contra 222,5), subida −21 % (79,1 contra 99,8). Las 7
rondas juntas: bajada igual (237,0 contra 236,5), subida −8 % (96,4 contra
104,4); el mismo brazo varía ±30 % entre rondas (la subida de 512, de 54 a
128). **Queda en el código, apagado**: el terreno no le ve ganancia y el
laboratorio, +3,4 %. La testnet vuelve a 270 / 302 (la media).

**El enlace que muere** (`594ec25`, revertido): al salir un enlace de los
vivos, lo que su subflujo tenía en vuelo tras el último SFACK pasaba a GRIT
como perdido. En el laboratorio (`flushlab`) no ganó: los reenvíos por
temporizador de `linkdeath` no bajaron y el caudal quedó en el ruido
(−1,4 %): lo que muere con un enlace no es lo que espera al temporizador.

### Por qué GRIT reenviaba por temporizador (2026-10-31, tarde)

En una bajada de 30 s por GRIT (`iperf3 -R -P 4`, `~/campo/2026-10-31/rto/`)
el GRIT del VPS reenvió 57 636 mensajes (13 % de lo enviado): 57 118 por su
temporizador, 518 por NACK, y el NUC recibió 37 682 duplicados. El RACK de la
mezcla está apagado con `subflow` (arriba), así que las pérdidas tenían que
llegar por los subflujos. Dos causas:

- **El emisor olvidaba** (`down.forgotten`): los subflujos del NUC hallaban
  las pérdidas (unas 10 000 por corrida, cerca de `lost_all`), pero el emisor
  del VPS solo recordaba las últimas 1024 `sseq` de cada enlace (~110 ms a
  100 Mbit/s). Un `SFLOSS` llega tras la cola del enlace, la ventana del
  receptor (hasta 300 ms) y la vuelta, y para entonces ya se había olvidado
  qué llevaba esa `sseq`: el 63–69 % de las pérdidas sin copias de estrés,
  el 95–99 % con ellas. GRIT las recuperaba solo por su temporizador.
  `CG_SF_WIN` pasa a 8192 (PR #136). A/B en terreno, solo cambia el binario
  del VPS, tres rondas alternadas:

  | ventana | Mbit/s | media | reenviado | por RTO | por NACK | olvidados | duplicados |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | 1024 | 224 / 218 / 228 | 223 | 3,6 % | 23 313 | 779 | 16 975 | 9 197 |
  | 8192 | 263 / 298 / 305 | 289 | 0,9 % | 4 856 | 2 430 | 0 | 4 306 |

- **Las copias de estrés** (`split_stress_dup`, 50 %): bajo el estrés de G4
  van copias a todos los enlaces, también a uno fuera del reparto. Movistar
  (~20 Mbit/s) recibió hasta 56 Mbit/s con capacidad estimada de 8, su cola
  subió a 10 s y no se vació hasta 25 s después; lo que sí le tocaba llegaba
  tan tarde que el NUC lo botaba fuera de la ventana anti-replay
  (`download.too_old`, 42 794 en 30 s). Con `split_stress_dup = 0` (ventana
  vieja, tres rondas): `too_old` 0 en las tres, 189 contra 167 Mbit/s,
  reenviado 4,2 contra 6,8 %. Sobre la ventana nueva, tres rondas más por
  brazo: 213 / 316 / 255 (media 261) contra 258 / 139 / 247 (214). Con 50,
  las rondas sin estrés no mandan copias y quedan como con 0; la que entró
  en estrés cayó a 139 Mbit/s con 266 MB de copias, 17 % reenviado y 26 294
  olvidados aun con 8192. Las copias de estrés son del túnel sin ARQ: con
  GRIT sobran. El modo bonding deja `split_stress_dup = 0` en el VPS y en
  el router (PR #137; la testnet, en `engine.local` y por el modo). En la
  subida (`iperf3 -P 4`, 3+3 rondas): 121 / 123 / 132 (media 125) con 0
  contra 114 / 123 / 126 (121) con 50, sin una sola copia: el estrés no
  saltó, es ruido. La subida va como se diseñó: todo por NACK (unos 15 700
  por corrida, ~4,3 %), sin temporizador, sin olvidados ni duplicados.

Queda: el temporizador todavía reenvía unos 4 900 por corrida y casi todo
llega duplicado (4 306); y a más de 300 Mbit/s por enlace 8192 `sseq` son
~0,3 s: la ventana debería medirse en tiempo, no en paquetes.

**Probado y revertido: el horizonte sobre todos los enlaces.** Muestreando
cada segundo, las ráfagas del temporizador (400–600 reenvíos) coincidían con
un enlace 300–470 ms atrasado que salía del reparto (G4): el horizonte que
el motor le da a GRIT (`ops.horizon`) contaba solo los enlaces del reparto.
Contarlos todos (`claude/grit-horizon-carry`, `d4f5263`) no movió los
reenvíos por temporizador (9 952 contra 9 903 por corrida, 3+3 rondas) y la
diferencia de caudal cambió de signo entre rondas: la causa es otra, y hace
falta medir la edad de lo que vence contra el enlace que lo llevó.

### Paso U2a: el temporizador de GRIT por segmento (2026-10-31, noche)

Primer trozo del paso U2 (GRIT reparte en los subflujos): que GRIT sepa
cuándo llega cada segmento. El motor elige el enlace de cada DATA en el
mismo `ops.send` en que GRIT se lo entrega, y su reparto ya sabe cuándo lo
trae ese enlace, detrás del más rápido (`cg_eta_behind`: su retardo sin
cola sobre el menor y su cola sobre su ritmo). Lo devuelve mensaje a
mensaje; GRIT lo guarda en el segmento (`eta_us`) y con `subflow` cada uno
vence a su propio plazo: el de siempre, o el RTT más vez y media su llegada
si eso pasa el presupuesto de 300 ms (`cg_st_seg_rto`). Antes de esto, el
registro del temporizador (`rto_obs`, `dup_by_link`) mostró por qué vencía:
RTO de ~460 ms con un srtt de ~100 y un rttvar de 14 (el RTT que aprende es
el del enlace más rápido, cada ACK lleva el menor retardo), el segmento más
viejo con ~500–680 ms al vencer, los duplicados por los cuatro enlaces y
el 76 % de los mensajes fuera de orden. Dos arreglos sin el motor, medidos
y descartados (3+3 rondas): esperar como sospechosos los vencidos salvo el
más viejo (F-RTO; 204 contra 214 Mbit/s, casi todos vencían igual) y la
rotación de los rangos de SACK (con ~2600 ACK/s cada rango se dice cada
~10 ms).

Terreno, bajada `iperf3 -R -P 4` de 30 s, solo el binario del VPS cambia,
tres rondas alternadas (de tarde: la red da menos que antes):

| brazo | Mbit/s | media | reenviado | por temporizador | duplicados | vencimientos |
| --- | --- | --- | --- | --- | --- | --- |
| antes | 142 / 147 / 144 | 144 | 2,2 % | 7 813 | 6 474 | 256–312 |
| U2a | 156 / 187 / 166 | 170 | 1,5 % | 6 007 | 4 202 | 112–253 |

La llegada media que dio el motor: 143–159 ms detrás del enlace más rápido,
en 450–530 mil segmentos por corrida. Queda un temporizador que todavía
reenvía ~6000 por corrida (dos tercios duplicados): el paso U2b (GRIT elige
el subflujo) y el RTT por subflujo (U2c).

### Paso U2b: un reenvío deja el enlace que lo perdió (2026-11-01, ~01:30)

Primera decisión de GRIT sobre los enlaces: `ops.send` devuelve además el
enlace de cada DATA, GRIT lo guarda en el segmento y, con `subflow`, un
reenvío le pide al reparto dejar fuera ese enlace cuando otro lo puede
llevar (`cg_split_avoid`): lo que un subflujo perdió va por otro, como en
MPTCP, y por el que entrega primero (la ETA). Terreno, bajada `iperf3 -R
-P 4` de 30 s, solo el binario del VPS cambia, 3 rondas alternadas (la red
holgada de madrugada, más de 350 Mbit/s):

| brazo | Mbit/s | media | por temporizador | duplicados | huecos ≥ 320 ms en el NUC |
| --- | --- | --- | --- | --- | --- |
| U2a | 372 / 329 / 349 | 350 | 2 356 | 2 262 | 71 (de 367) |
| U2b | 354 / 380 / 401 | 378 | 985 | 963 | 35 (de 219) |

Los 18 835 reenvíos cambiaron de enlace, ninguno tuvo que repetir el suyo.
Segunda tanda (3+3): 420 / 392 / 379 (397) contra 433 / 385 / 407 (408),
huecos ≥ 320 ms 69 contra 29. Con las dos: U2b gana 4 de 6 rondas, +5 % de
media (374 → 393), y los huecos largos bajan a menos de la mitad en las dos.
El caudal gana dos de tres rondas (+8 % de media, ruidoso); los huecos
largos y lo que reenvía el temporizador bajan a la mitad o menos. Sigue
U2c: con el enlace de cada segmento, el RTT medido por enlace para su
temporizador.

### Paso U2c: el RTT de cada enlace para el temporizador (2026-11-01, ~02:20)

Con el enlace de cada segmento (U2b), cada ACK da a cada enlace su muestra
de RTT (el envío más nuevo entre lo que entregó que fue una sola vez y
seguía en vuelo: regla de Karn); GRIT la suaviza por enlace como RFC 6298 y
el plazo de un segmento es también el de su enlace (srtt + 4 rttvar +
20 ms) cuando es mayor. El RTT de la mezcla es el del enlace más rápido:
en una ronda, 92 ms con enlaces de 173 / 137 / 103 / 94. Terreno, 3+3:

| brazo | Mbit/s | media | por temporizador | duplicados | huecos ≥ 320 ms |
| --- | --- | --- | --- | --- | --- |
| U2b | 378 / 417 / 379 | 391 | 3 717 | 3 671 | 53 |
| U2c | 410 / 433 / 418 | 420 | 68 | 16 | 35 |

Las tres de U2c sobre las tres de U2b (+7,5 %); el temporizador casi ya no
reenvía de más (−98 %, los duplicados de 3671 a 16). Un evento de pérdida
grande de la red (~75 mil NACK) tocó una ronda de cada brazo.

### Nombres: GRIT con SLATE (2026-11-01)

GRIT sigue siendo el protocolo; su planificador y control de congestión se
llama **SLATE** (*Subflow Latency-bounded Arrival-Time Estimation*), como
QUIC con BBR: el ritmo por enlace (`link_cc`), cada paquete por el enlace
donde llega primero (la ETA), el RTT y el plazo de cada enlace y segmento
(U2a–U2c), el reenvío por otro enlace (U2b) y la pérdida por enlace (los
subflujos). Dos rondas de Sonnet buscaron siglas y choques (IETF, GitHub,
arXiv); SLATE comparte el nombre con un planificador de clústeres de
microservicios (otro rubro). Falta revisar las marcas (USPTO, INAPI) y el
nombre del producto (propuesta: MPDT, *Multipath Deadline Transport*).

### La compresión, como se piensa (2026-11-01)

Primero medir la mezcla de tráfico (lo cifrado y el video no se comprimen);
los encabezados antes que zstd (el protocolo 13; contextos al estilo ROHC
para el UDP/IP de adentro); zstd solo por flujo donde rinde, antes de la
FEC y con presupuesto de CPU. Los flujos de GRIT usan la ventana continua
de zstd (sin diccionario); los datagramas, diccionarios por clase (DNS,
syslog, MQTT/JSON, Modbus) con número por hash. La sincronización (idea del
usuario): **DC-SYN** lleva un diccionario nuevo (los dos extremos aprenden y
proponen, cada uno para lo que envía), **DC-ACK** confirma que está
completo, y el emisor lo usa solo después: ningún paquete espera, salen sin
comprimir (o con el anterior) hasta entonces; viajan por un flujo de GRIT
de fondo, en pedazos, y lo aprendido localmente no sale de los equipos del
cliente. El túnel va autenticado, no cifrado (tarea #27): un diccionario no
expone nada que el túnel no lleve ya.

### Contra OMR con U2a: el cara a cara, el UDP y la caída del principal (2026-10-31, noche)

Los cuatro módems, alternados (nunca a la vez), cengarde con U2a.

- **Speedtest** (Ookla, servidor 51218 de Santiago, 3 rondas,
  `~/campo/2026-10-31/vsomr/`): bajada 181 / 177 / 195 (media 184) contra
  196 / 206 / 181 (194) de OMR, ruido; subida 119 contra 94 (+27 % en la media; cengarde
  arriba en dos de las tres rondas); ping con carga en la bajada 54–62 ms contra 624–798 (12 ×
  menos), en la subida 78–91 contra 128–167. De noche la red da mucho menos
  que la madrugada del 10-29 (más de 500 Mbit/s los dos).
- **UDP** a caudal fijo, 10 s (`~/campo/2026-10-31/omrweak/`): OMR lleva
  el UDP por su VPN (OpenVPN) sobre un solo enlace y se queda en ~25 Mbit/s
  (bajada a 60 / 100 / 150: 53 / 77 / 86 % perdido); cengarde lo reparte:
  bajada a 150 Mbit/s con 0 % perdido, subida hasta ~80 (a 150 manda más de
  lo que la subida de los módems lleva: 53 % y 316 ms de jitter).
- **El enlace principal muerto con la interfaz arriba** (Entel, el `wan1`
  maestro de OMR: nft bota todo lo suyo 25 s en una bajada de 70 s, 2
  rondas): cengarde 0 y 1 segundos casi sin caudal, 0 % de ping perdido en
  las dos; OMR 8 segundos casi muerto y 32 % de ping perdido en la primera
  (su ping va por la VPN del maestro), bien en la segunda (sin verificar:
  su VPN habría quedado en otro módem).
- **TCP con el principal muerto 45 s** (`~/campo/2026-10-31/mptcp/`, 2
  rondas): un solo flujo de 90 s sobrevive en los dos, pero cengarde queda
  0–1 segundos casi sin caudal (de ~190 a ~150 Mbit/s con tres módems) y OMR
  5–14 (unos 15 s casi en cero hasta que MPTCP lo pasa a otros subflujos;
  su flujo único ya iba irregular, 52–62 con caídas a 0). Las conexiones
  nuevas (HTTPS corto a Vultr Santiago cada 2 s): cengarde 23/23 y 23/23,
  0,23–0,26 s de mediana; OMR 12/15 y 8/12, 1,7–1,8 s de mediana y hasta
  3,4: cada conexión MPTCP abre su primer subflujo por el maestro.
- Ventaja chica de cengarde en estas pruebas: el destino del iperf es su
  VPS (OMR llega a él por internet, un salto dentro del mismo datacenter);
  las conexiones nuevas van a un servidor neutro.
- **OMR con Xray VLESS y el UDP por el proxy** (2026-11-01, ~00:30, la red
  más holgada; `~/campo/2026-11-01/xray/`; la configuración de antes en
  `/root/cfg-before-xray/` de la VM): Speedtest, bajada 442 / 376 / 445
  (media 421) contra 301 / 318 / 331 / 442 (348; un par extra, el último,
  empató), subida 111 contra 86, ping con carga 69–83 contra 317–517 ms.
  UDP en bajada: cengarde 150 sin pérdida, OMR 80 a 100 Mbit/s (1,9 %) y
  106 a 150 (29 %); en subida **OMR gana**: a 100 recibe 91 (8 %) contra 68
  (32 %) de cengarde, a 150 recibe 100 (29 %) contra 50 (65 %): Xray lleva el
  UDP dentro de un TCP que reenvía lo perdido (a cambio de retardo). El UDP
  de subida de cengarde va por el túnel sin protección: el paso U5 (los
  datagramas como clase). Entel muerto: con 4 flujos, 0 y 0 contra 1 y 0
  segundos casi sin caudal, 0 % de ping perdido los dos; un flujo, 2 y 0
  contra 2 y 2; conexiones nuevas 46/46 a 0,3 s contra 39/45 a 1,0 s (hasta
  2,4): MPTCP sigue abriendo por el maestro.
- **Hallazgo (no es GRIT):** iperf3 en UDP contra la IP pública del VPS
  desde la LAN aborta («Resource temporarily unavailable»): el UDP llega por
  `cg0`, pero el VPS contesta con origen 10.79.0.1 (la dirección del túnel,
  la que elige la ruta) y el socket del cliente, conectado a la pública, lo
  descarta. Solo afecta a servicios UDP del propio VPS alcanzados por su IP
  pública; el UDP hacia internet no. Por eso el UDP de cengarde se midió
  contra 10.79.0.1. Arreglo posible: que el VPS conteste por el túnel con la
  pública como origen (la ruta de la red del túnel con `src`), por probar.

### La media por omisión: las rondas de madrugada (2026-11-01, 03:33)

El estimador de la media (`link_cc_bits` 256) contra el máximo, de madrugada
(la red holgada, ~500 Mbit/s), brazos alternados, 3 rondas
(`~/campo/2026-11-01/meannight/`): «media» = 270 / 302, «sin media» = 14 / 46.

| brazo | bajada iperf3 | bajada Speedtest | subida iperf3 | subida Speedtest | ping con carga | p95 en la bajada |
| --- | --- | --- | --- | --- | --- | --- |
| media | 486 / 495 / 513 (498) | 577 / 636 / 589 (601) | 151 | 144 | 71 ms | 121 ms |
| sin media | 454 / 385 / 359 (399) | 552 / 598 / 464 (538) | 150 | 156 | 95 ms | 134 ms |

La bajada +25 % (iperf3) y +12 % (Speedtest), las tres rondas en los dos;
GRIT reenvía menos (89 contra 118 mil por bajada); la subida igual. Queda
por omisión: 270 en el router y 302 en el VPS.

Con la media, U2a–U2c y la red de madrugada, una ronda contra OMR (con Xray)
en TCP (Speedtest, servidor 51218, 3 rondas alternadas,
`~/campo/2026-11-01/vsomr-mean/`): bajada 608 / 607 / 630 (615) contra
465 / 444 / 536 (482), +28 %; subida 152 / 145 / 157 (151) contra
101 / 117 / 112 (110), +38 %; ping con carga 74 / 78 ms contra 304 / 101;
cengarde adelante en todas las rondas. De tarde se empataba la bajada
(184 contra 194).

### La capacidad vieja deja sus tres decisiones chicas (2026-11-01, laboratorio)

Con el ritmo por enlace (`link_cc`) la capacidad de `capacity.h` todavía
decidía tres cosas: la semilla del `bw` de cada enlace (y el respaldo de la
pista a GRIT cuando un enlace aún no tiene acuses), el enlace dominante
(`dominant.h`) y la FEC v3 (su guarda por la cola y la pérdida que fija su
parte). Cada una pasó a un bit de `link_cc_bits` para medirla por separado:
1024 (`CG_LC_NOSEED`: el enlace nace de su propio arranque), 2048
(`CG_LC_DOM`: el dominante por el `bw` de cada enlace) y 4096 (`CG_LC_F3`: la
guarda por el retardo de ida propio, `cg_lc_f3_queue`, y la pérdida por el
subflujo en el último tick, `cg_lc_tick_loss`, con carga si la cola pasa la
mitad del objetivo).

Dos tandas en los 14 laboratorios (PC 6, remoto06 4, remoto07 4; 12 casos:
`equal het fade cell4 lossy protect clean4 linkdeath downstall ratswitch
dominant starlink15`, bajada y subida, cubic; 672 corridas, 0 fallidas,
`~/campo/2026-11-01/caplab{,2}/`). Media geométrica de la mediana de Mbit/s
contra la base (270 / 302), juntando las dos tandas (7 corridas por brazo;
los tres juntos solo en la segunda, 4):

| brazo | arranque | dominante | FEC | general | todo | gana |
| --- | --- | --- | --- | --- | --- | --- |
| 1024 | +0,2 % | +2,3 % | +2,9 % | −2,0 % | +0,5 % | 14 / 24 |
| 2048 | +0,2 % | −0,7 % | +5,9 % | −0,8 % | +1,1 % | 13 / 24 |
| 4096 | +7,4 % | +4,4 % | +1,3 % | +4,4 % | +4,4 % | 16 / 24 |
| los tres | +4,9 % | +6,9 % | −0,2 % | +5,5 % | +4,1 % | 14 / 24 |

(arranque: `linkdeath`, `downstall`, `ratswitch`; dominante: `dominant`,
`starlink15`; FEC: `lossy`, `protect`, `clean4`; general: el resto.)

- La FEC por el subflujo es lo que gana: `fade` +11 % de bajada y +19 % de
  subida, `clean4` +12 % y `linkdeath` +18 % de bajada. La capacidad medía la pérdida por
  muestra y la cola sobre el mínimo del minuto; el subflujo la cuenta por
  enlace, exacta, y la cola propia es la de ahora.
- Lo que cuesta: `lossy` de subida de 113 a 104 Mbit/s (el subflujo ve la
  pérdida al azar entera y la FEC manda más reparaciones), con el ping con
  carga de 145 a 105 ms. Los tres juntos bajan `downstall` de subida 15 %
  (4 corridas; 4096 solo, +6 %).
- El dominante es ruido: `dominant` de bajada va de 17 a 119 Mbit/s entre
  corridas en todos los brazos (la primera tanda dio +20 % en el grupo, la
  segunda −9 %). La semilla, pareja. Al ser parejos, igual salen de la
  capacidad: es lo que se buscaba, que la capacidad deje de decidir.
- **Por omisión los tres:** 7438 en el router y 7470 en el VPS. El camino
  viejo queda solo para `link_cc = no` (sin `subflow`), que no tiene el
  ritmo propio.

En terreno (testnet, 2026-11-01 de día, 6 rondas alternadas, 3 en cada
orden, `~/campo/2026-11-01/capfield/`) solo actúa la semilla: el NUC no
tiene la FEC y el VPS no manda reparaciones de la v3, y con cuatro
celulares no hay dominante. Parejo, como en el laboratorio: iperf3 de bajada
428 contra 412 (−4 %), Speedtest de bajada 421 contra 449 (+7 %), las
subidas 116 / 114 y 114 / 115, el ping con carga de bajada 72 en los dos y
el de subida 74 contra 79 ms. La testnet queda en 7438 / 7470.

Con la FEC v3 encendida en los dos extremos (`fec_code = rlc`), solo el
bit 4096 contra la capacidad (3342 / 3374 contra 7438 / 7470; 4 rondas
ABBA, `~/campo/2026-11-01/capfec/`; Ookla cortó 3 Speedtest por límite):

| | capacidad | subflujo |
| --- | --- | --- |
| iperf3 bajada / subida | 355 / 96 | 388 / 104 (+9 % / +9 %) |
| Speedtest bajada / subida | 379 / 106 | 425 / 117 |
| ping con carga bajada / subida | 62 / 81 ms | 63 / 76 ms |
| reparaciones de la bajada (sobre el DATA) | 12,2 % | 7,2 % |
| rehechos en la bajada por ronda | 5233 | 1630 |
| reparaciones de la subida | 18,6 % | 18,3 % |
| rehechos en la subida por ronda (sin la primera, vacía) | 21 985 | 21 887 |

- **La bajada:** el subflujo no ve pérdida al azar en el camino del VPS
  (la que hay es de cola, la cuenta en la cola propia) y la parte cae a 0
  en tres de cuatro rondas: BBR no paga reparaciones que no necesita, y la
  bajada sube 9 % aunque rehace menos. Con la capacidad la parte quedaba
  en 20 % todo el rato, y casi dos tercios de las reparaciones llegaban
  sin hueco que tapar.
- **La subida:** rehace lo mismo con el mismo gasto, y sube 9 %.
- Contra la FEC apagada (las rondas de antes, misma mañana) la FEC todavía
  cuesta: 388 / 104 contra 412 / 115. La testnet vuelve a como estaba (sin
  FEC en el NUC).

### La limpieza del ritmo por enlace (2026-11-01)

`link_cc_bits` tenía 13 bits de experimento. Lo que ganó pasa a ser el
motor y la perilla desaparece; lo que perdió sale del motor a
`docs/desvan/`, desacoplado y con lo medido, para retomarlo en otro
escenario (pedido del usuario: «no lo mates del todo»).

- **Fijo:** los modos por la cola y STARTUP a ×3/2 (2 y 8), el tope por lo
  entregado con cola (4: siempre en la subida del router; solo con cola en
  la bajada del VPS, que era el 32, ahora `satq` lo pone `server.c`), la
  media de 0,5 s (256) y las tres decisiones de la capacidad (1024, 2048,
  4096).
- **Al desván:** el MTPT con `link_cc_budget_ms` (128), la cola del reparto
  por el retardo (1 y 16), la ventana como el planificador de OMR (64, con
  su parte de `eta.h`) y que el ritmo propio decida solo (512); también lo
  que hacían los bits ganadores apagados (la semilla de la capacidad,
  STARTUP a ×2, `bw` por la mayor muestra).
- `link_cc_bits` y `link_cc_budget_ms` en una configuración vieja: un aviso
  y sigue (`retired_keys`), no una clave desconocida; `cengarde-setup` y
  `cengarde-vps-admin` ya no las pasan.

## Pendiente

- `link_cc`: decidir si pasa a ser lo normal en el motor con `subflow` (hoy
  lo encienden los modos del router y del VPS); un enlace de subida débil
  (Claro) sigue con colas de 280–360 ms.
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
- El VPS contesta el UDP de sus propios servicios con origen 10.79.0.1
  cuando la LAN le habla a su IP pública (iperf3 UDP, 2026-10-31).

## Cambios

- 2026-10-28: creada.
- 2026-10-28: el paso U1 (protocolo 12, `subflow`), la regla de la evidencia
  y el RACK de la mezcla apagado; el temporizador con margen; terreno.
- 2026-10-28: el paso 2 (oferta, objetivos por omisión) y el 2b (`SFACK`,
  `linkcc.h`, `link_cc`); el techo de búferes de remoto06.
- 2026-10-28: los modos (bonding, redundancia, personalizado) en el router,
  LuCI y el VPS; el VPS administrado desde el router (`cengarde-vps-admin`);
  la redundancia con lo nuevo (`red1`: queda con el protocontrolador A).
- 2026-10-28: la prueba de techo con captura (`techo`), el MTU de Claro, la
  curva de cada módem y el MTPT.
- 2026-10-31: por qué GRIT reenviaba por temporizador: la ventana del
  subflujo (8192) y las copias de estrés en un enlace con cola de segundos.
- 2026-10-31: el paso U2a, el temporizador de GRIT por segmento con la
  llegada que da el motor (+17 % de bajada, −35 % de duplicados).
- 2026-10-31: contra OMR con U2a: Speedtest, UDP y la caída del enlace
  principal; el origen del UDP que contesta el propio VPS.
- 2026-11-01: el paso U2b, un reenvío deja el enlace que lo perdió.
- 2026-11-01: el paso U2c, el RTT de cada enlace para el temporizador.
- 2026-11-01: los nombres (GRIT con SLATE) y el plan de la compresión
  con diccionarios (DC-SYN, DC-ACK).
- 2026-11-01: la media por omisión (270 / 302), tras las rondas de madrugada.
- 2026-11-01: la capacidad vieja deja la semilla, el dominante y la FEC v3
  (`link_cc_bits` 1024, 2048 y 4096, por omisión: 7438 / 7470).
- 2026-11-01: la limpieza del ritmo por enlace: lo que ganó queda fijo,
  `link_cc_bits` sale y lo que perdió va a `docs/desvan/`.
