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
  (`315f724`, en el laboratorio `rot1`).
- **Hueco conocido:** lo copiado (redundancia) no tiene un enlace que responda
  por su pérdida; sin el RACK de la mezcla, lo recupera el NACK (con
  `protocontroller_nack yes`) o el temporizador.

## Pendiente

- Las ráfagas del temporizador en corridas sueltas del laboratorio (`sf3`).
- El paso 2: llenar cada enlace hasta su cola objetivo (OMR 45–60 ms por
  módem) dentro del presupuesto de 300 ms.
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
