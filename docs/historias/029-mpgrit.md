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
- **Paso U1 (en curso, protocolo 12, `subflow`):** cada enlace con su propia
  secuencia (`sseq`) y sus pérdidas (`subflow.h`, `SFLOSS`); una pérdida es
  cierta cuando el enlace la perdió y nada la reconstruye, o el receptor
  también la pidió (`cg_resc_sure`); el RACK de GRIT sobre la mezcla se
  apaga. En el laboratorio, con el RACK de la mezcla, casi todo lo que GRIT
  reenviaba era por RACK y llegaba duplicado.

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
- **Hueco conocido:** lo copiado (redundancia) no tiene un enlace que responda
  por su pérdida; sin el RACK de la mezcla, lo recupera el NACK (con
  `protocontroller_nack yes`) o el temporizador.

## Pendiente

- Medir el paso U1 en el laboratorio (`~/campo/2026-10-28/sf2/`) y en la
  testnet contra OMR (sube el protocolo: el NUC y el VPS a la vez).
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
  y el RACK de la mezcla apagado.
