# 028 — GRIT y el motor como uno: la vista compartida

- **Fecha:** 2026-10-27
- **Estado:** vigente (dos pasos hechos: el retardo por enlace y el horizonte
  de reparación; los siguientes, en «Pendiente»)
- **Fuentes:** `engine/src/linkowd.h`, `engine/tests/test_linkowd.c`,
  `protoctl.c` (`data_in`, `send_ack`, el estado `pacers`), `client.c`
  (`down_deliver`), `server.c` (`up_deliver`); historias 025 (GRIT, el
  colapso de la bajada, el estimador desbocado, el arranque) y 023 (el
  rescate, la PARQ); tarea #77; laboratorio `bench/labq.py` con
  `BOND_PEP=cgb`, datos en `~/campo/2026-10-27/gshare{1,2,3}/`, `bondvs/`,
  `nackfirst/out45/`, `nackfirst-nofec/`, `spread/`, `reach/`, `start/`,
  `cap/`, `rat/`, `buf/`, `bufedge/` y `buf2/`.

## TL;DR

- **El principio** (decisión del usuario, 2026-10-27): GRIT y el motor
  trabajan como uno, cada uno extensión del otro. Por eso salió TCP: era
  ciego al túnel; GRIT es nuestro y cada lado puede decirle al otro lo que
  sabe en vez de adivinarlo.
- **Primer paso, hecho:** el motor le dice a GRIT **por qué enlace llegó
  cada mensaje**, y GRIT le quita a su retardo lo que ese enlace agrega
  sobre el más corto (`linkowd.h`). Cada ACK informa la cola del **peor de
  los enlaces que llevaron un cuarto o más** de sus mensajes. Así una
  diferencia de camino ya no se lee como cola (la raíz del colapso de la
  bajada y de los finales falsos del arranque), y un enlace secundario
  despejado no tapa la cola del principal.
- **Medido** (n = 8, `BOND_PEP=cgb`, contra `master` 1f19851): **lossy
  ×1,75 de bajada y ×1,87 de subida** (54 → 94 y 51 → 95 Mbit/s, sin costo
  de ping significativo); **protect de bajada ×1,16 y el ping de 1200 B p99
  de 766 a 274 ms**, de subida de 342 a 159 ms; idleslow de subida ×1,34
  (+35 ms de ping), stall50 igual. 0 colapsos.
- **Segundo paso, hecho: el horizonte de reparación, NACK primero.** El
  motor le dice a GRIT cuánto puede esperar un hueco en el receptor: lo
  que tarda el código en repararlo (`rescue_code_ms`, mientras la FEC
  manda reparaciones) más la espera del rescate (con el rescate encendido).
  El NACK del túnel es exacto (un hueco se pide cuando su propio enlace
  trae algo posterior), así que RACK solo cubre un NACK perdido. Antes
  ~85–100 % de lo que RACK reenviaba llegaba duplicado, también sin pérdida
  (`equal`, `het`: 1–4 % de los DATA dos veces, por el desorden entre
  enlaces). Ahora: duplicados de miles a decenas, el ping p99 de 1200 B
  baja a la mitad en `equal`, `lossy` y `stall50`, el caudal igual o
  mejor (+4 a +12 %).
- **Otro error de GRIT, corregido:** el receptor contaba sus rangos fuera
  de orden con 8 bits; con 256 y una fusión los olvidaba todos, el emisor
  los tenía confirmados y nunca los reenviaba: el flujo quedaba trabado
  para siempre. Solo aparece con búferes de más de 1 MB. El emisor ahora
  además se recupera de un SACK «renegado», como TCP.
- **Un error de GRIT, corregido:** `protocontroller_dead_s` contaba desde el
  último mensaje recibido, así que un flujo ocioso más de 60 s (SSH, el
  control de iperf) se reseteaba al mandar su siguiente byte. Ahora cuenta
  desde que vuelve a tener algo en vuelo.
- **Bonding contra los estados anteriores** (n = 8): GRIT le gana al Split
  TCP en `equal`, `lossy`, `protect` y `single1` (+5 a +30 %), empata en
  `het`, `idleslow` y `stall50`, y perdía en `cell4` (−15 a −20 %); al
  TCP directo le gana en 13 de 16 celdas (×1,15–2,15).
- **`cell4`, resuelto en su mayor parte:** la cola de los enlaces chicos de
  búfer hondo (300–590 ms) hacía que RACK diera por perdido lo que solo
  estaba en cola; los bytes contados dos veces inflaban el estimador y GRIT
  llenaba más esas colas, hasta caer a ~17 Mbit/s. El horizonte suma ahora
  la dispersión de los enlaces que llevan los datos (`behind_us`): `cell4`
  +15 y +19 % (a −4 % del Split TCP), `idleslow` +20 %, el resto igual.
- Ya eran parte de esta vista, sin ese nombre: el NACK del túnel como
  pérdida confirmada para GRIT, con su enlace (#122); los bytes de GRIT que
  el reordenador no retiene; las copias prioritarias; la capacidad del
  motor como comienzo y techo del ritmo; los bytes que la pérdida no frena
  (la FEC v3 no los cobra con un enlace, historia 026).

## La vista compartida

| Sentido | Qué | Dónde | Estado |
| --- | --- | --- | --- |
| motor → GRIT | el enlace de llegada de cada mensaje | `linkowd.h`, `cg_pctl_rx` | **hecho** (esta historia) |
| motor → GRIT | la pérdida confirmada por segmento y por enlace (NACK) | `cg_pctl_nack`, `lost_by_link` | hecho (#122); GRIT aún no usa el enlace |
| motor → GRIT | la capacidad del camino (comienzo y techo) | `cg_scc_hint` | hecho (025) |
| GRIT → motor | «estos bytes son míos»: no se retienen, copias prioritarias | `cg_pctl_is`, `nohold`, `pnc`/`prc` | hecho (025) |
| GRIT → motor | bytes que la pérdida no frena | `cg_f3_mix` | hecho (026) |
| GRIT → motor | lo que GRIT recupera (el rescate no lo repite) | `on_nack` | hecho (#122) |
| motor → GRIT | el horizonte de las reparaciones de un hueco | `ops.horizon`, `cg_scc_reo_win` | **hecho** (esta historia) |
| motor → GRIT | microcortes, bordes de ranura de Starlink | — | pendiente |
| GRIT → motor | prioridad y plazo de cada flujo (vivo o bulto) | — | pendiente |
| GRIT → motor | un flujo que empieza (sondear los enlaces antes) | — | pendiente |

## El retardo por enlace (`linkowd.h`)

- **El receptor** (el motor de la otra punta) le pasa a `cg_pctl_rx` el
  enlace por el que llegó cada mensaje; un mensaje que rehicieron los
  códigos no tiene enlace (`CG_PCTL_NOLINK`) y no da muestra: su retardo es
  el de la reparación. Sin cambio en el cable ni en el protocolo.
- **Por enlace**, el menor retardo de sus mensajes en los últimos 5–10 s
  (dos baldes). Lo que agrega un enlace es su menor menos el menor de todos.
- **Se le resta** a cada muestra, pero solo si la diferencia llega al
  objetivo de cola (`protocontroller_queue_ms`): una diferencia menor ya cabe
  en lo que GRIT tolera, y restarla dejaba que GRIT tuviera la cola entera en
  cada enlace (stall50, cuatro enlaces a menos de 8 ms: el ping de 1200 B
  p99 de 262 a 423 ms sin más caudal). Las diferencias que dañan (15–130 ms)
  son más grandes.
- **El ACK** lleva, de los mensajes desde el anterior, el peor de los
  enlaces que trajeron un cuarto o más (cada uno ya corregido). Con el menor
  de todos, un enlace secundario sin cola tapaba la del principal: el ping
  de 1200 B p99 de lossy subía de ~90 a 250–360 ms en la mitad de las
  corridas.
- Estado, por par (`protocontroller.pacers`): `owd_samples`,
  `owd_corrected`, `link_extra_ms` (lo que agrega cada enlace).

## Medido

Tres variantes contra `master` (n = 8, intercaladas, `BOND_PEP=cgb`, 20 s,
cubic). Mbit/s y ping de 1200 B p99 (ms):

| Caso | master | 1: el menor de todos | 2: el peor enlace con carga | **3: y solo diferencias ≥ el objetivo** |
| --- | --- | --- | --- | --- |
| lossy bajada | 53,8 / 109 | 80,9 / 235 | 94,4 / 167 | **93,9 / 172** |
| lossy subida | 50,7 / 107 | 87,1 / 113 | 98,2 / 104 | **95,0 / 104** |
| protect bajada | 40,2 / 766 | 41,6 / 670 | 45,0 / 226 | **46,5 / 274** |
| protect subida | 46,5 / 342 | 44,8 / 354 | 45,5 / 345 | **47,2 / 159** |
| idleslow bajada | 40,1 / 154 | 55,5 / 205 | 66,4 / 304 | **45,0 / 216** |
| idleslow subida | 43,1 / 162 | 68,8 / 264 | 69,9 / 260 | **57,6 / 197** |
| stall50 bajada | 48,1 / 320 | 45,5 / 464 | 50,9 / 398 | **51,4 / 364** |
| stall50 subida | 49,0 / 295 | 49,5 / 410 | 52,7 / 340 | **51,6 / 325** |

(master, 2 y 3 de la misma tanda, `gshare3/`; la columna 1 de otra,
`gshare2/`, cuyo master daba lossy de bajada 51,2 / 127 e idleslow de subida
41,8 / 167: entre tandas el ping varía, compárese dentro de cada una.)
La 3 contra `master`: lossy ×1,75 (p 0,004) y ×1,87 (p < 0,001), protect de
bajada ×1,16 (p 0,04) con el ping de subida más bajo (p 0,024), idleslow de
subida ×1,34 (p 0,003, el ping +35 ms, p 0,007); idleslow de bajada y
stall50, sin diferencia significativa. La 2 daba más caudal en idleslow
(×1,6) a costa de ~100–150 ms de ping: se eligió la 3 por contener el
costo.

## El horizonte de reparación: NACK primero

RACK da por perdido un segmento cuando ya llegó otro enviado más de una
ventana después (un cuarto del RTT mínimo más `protocontroller_reo_ms`, que
cada duplicado agrandaba 4 ms: hasta 20 sin código, hasta el `rescue_code_ms`
con código). Esa ventana era más corta que la reparación del código (una
detención: ~170 ms, historia 026) y que el desorden entre enlaces, así que
RACK le ganaba al código y al NACK y su reenvío llegaba repetido: `lossy`
940 de 940, `stall50` 3918 de 4576; en `equal` y `het`, sin una sola
pérdida, 1000–4000 duplicados por corrida.

El túnel sabe más: cada paquete dice por qué enlace fue el anterior
(`CG_F_PREV`) y los enlaces entregan en orden, así que el reordenador pide
un hueco apenas ese mismo enlace trae algo posterior (`reorder.h`), sea cual
sea el retardo de los otros. Ese NACK sale por los dos enlaces más rápidos y
se repite dentro de la espera del rescate.

- `protoctlio.h`: `ops.horizon(peer, sid)`, lo que un hueco puede esperar:
  `rescue_code_ms` si el código manda reparaciones ahora (FEC v3 con su
  parte o los códigos de `redund_spare`) más `cg_rescue_wait_ms` si el
  rescate está encendido (`reorder` y `rescue_kb`); 0, ninguno.
- `streamcc.h`: `cg_scc_reo_win`, la ventana de RACK sobre un cuarto del RTT
  mínimo es al menos el horizonte (a lo más 1 s) desde el comienzo; con
  pérdida pesada, `protocontroller_reo_ms` como antes. El tick de
  `protoctl.c` lo pregunta por par; el estado lo muestra (`horizon_ms`).
- Así el NACK llega primero (alrededor de un cuarto del RTT antes que RACK)
  y RACK queda de respaldo si se pierden el NACK y su repetición. Sin
  rescate, todo sigue como antes.

## `cell4`: la cola que RACK leía como pérdida

Cuatro celulares de 20/15/10/5 Mbit/s con búferes de 0,3/0,5/1/2 s y
0,3–0,5 % de pérdida al azar. Con el estado muestreado cada 0,5 s (subida,
misma semilla), el ciclo era:

1. el estimador de GRIT por encima de lo que los enlaces entregaban (47–50
   Mbit/s con 32 entregados y 19–37 por su socket);
2. con la FEC, 38–41 Mbit/s ofrecidos a enlaces de 34–37 estimados: el
   reparto ponía lo que sobraba en las colas hondas de los chicos (300–590
   ms);
3. RACK, con un horizonte de 160 ms, daba por perdido en ráfagas (460–1435
   segmentos de una vez) lo que solo estaba en esas colas; los originales
   llegaban después: ~3500 duplicados por corrida;
4. `cg_scc_lost` cuenta lo perdido como lo que el camino se llevó, y al
   llegar los originales se contaba otra vez: el estimador se inflaba más;
5. el corte (cola de ida de 517 ms), el G4 sacaba los enlaces chicos y GRIT
   quedaba en ~17 Mbit/s por un enlace, con el motor ofreciendo 33.

El Split TCP no entraba en el ciclo: BBR no pasa de lo que se entrega y las
colas no pasaban de ~130 ms.

El motor sabe cuánto desordena su reparto: `health.h` mide cuánto va cada
enlace detrás del más rápido (`behind_us`, camino y cola). Con el rescate
encendido, el horizonte suma el mayor de los enlaces que llevan los datos
(`cg_pctl_spread`, a lo más 1 s); las pérdidas reales las sigue avisando
primero el NACK, exacto por enlace. En la corrida de prueba: 24,8 → 33,5
Mbit/s (Split TCP 33,2), sin colapso, 978 reenvíos (casi todos en los
primeros 4 s, antes de conocer los retardos), ping p99 523 → 337 ms.

## Lo probado y descartado después de `cell4`

- **El alcance** (que GRIT tolere la cola que el reparto por ETA necesita
  para llegar a los enlaces lentos, `ops.reach`): `reorder50` +35 / +52 %,
  `het` +5 / +8 %, `idleslow` +8 / +16 %, pero el ping p99 subió (`reorder50`
  278 → 412 y 255 → 466 ms) y `protect` de subida cayó 20 % con miles de
  duplicados en las 6 corridas (`reach/`). Una cola permanente en el enlace
  rápido es la herramienta equivocada: lo correcto es que el reparto mande
  a los lentos una parte de los bytes de GRIT y que GRIT reordene.
  Revertido.
- **La dispersión supuesta al arrancar** (`reorder_max_wait_ms` mientras no
  se conocen los retardos): `cell4` no duplica menos (1097 → 1452) y el ping
  de `protect` de bajada sube (109 → 308 ms). Revertido.
- **La cota del estimador** (fuera de STARTUP, una muestra no pasa de 3
  veces la capacidad del motor; `protect` leyó 389 y 606 Mbit/s en enlaces
  de 60 y 5): neutra en `cap/` (n = 6, nada significativo). Queda de
  resguardo.

## El búfer por conexión y los rangos del receptor

El cambio de RAT (`ratswitch`, `ratsingle`: LTE 30/15 Mbit/s ↔ NR 180/60,
búfer del operador de 3 MB de bajada) mostró a GRIT 15–31 % bajo el Split
TCP (`rat/`). En NR no pasaba de ~120 Mbit/s: `protocontroller_buf_kb`
(1 MB) sobre ~70 ms de ida y vuelta. Con 4 MB, en una corrida, 58 → 76
Mbit/s. Subirlo por defecto destapó un error: con 2–4 MB la mayoría de las
corridas de `equal` caía a 5–12 Mbit/s y una se colgó 35 minutos. El
receptor guarda hasta 256 rangos fuera de orden y `cg_st_rx_add` los
contaba con un cast a 8 bits: llenos, una fusión los dejaba en 0; lo
recibido más allá del hueco quedaba sin registro, el emisor lo tenía
confirmado por SACK y nunca lo reenviaba (en el registro: el segmento en
`una` SACKED, enviado una sola vez). Corregido el conteo, y el emisor se
recupera como TCP de un SACK «renegado»: un RTO que encuentra el segmento
más viejo SACKED con el acumulado aún en él reenvía todo lo SACKED
(`reneged` en el estado; el temporizador se arma también sin nada en
vuelo). Con el arreglo, 64 de 64 corridas con 2–4 MB sin atascos y
`reneged` 0 (`bufedge/`).

Con el arreglo, 4 MB frente a 1 MB (`buf2/`, n = 6): `ratsingle` de bajada
43 → 60 y `ratswitch` de bajada 52 → 60,5 Mbit/s (el Split TCP 63 y 60),
pero `lossy` 97 / 100 → 90 / 93 con el ping p99 de 104–109 a 356–374 ms y
~5000 duplicados, y `cell4` −2 a −4 % con más ping. El valor por defecto
queda en 1 MB; lo correcto es un búfer que crezca solo cuando el flujo
está limitado por la ventana y sin pérdida ni cola.

## El temporizador de flujo muerto

`protocontroller_dead_s` (60) cierra un flujo con algo pendiente que no
avanza (el túnel caído). Contaba desde el último mensaje del otro lado:
un flujo ocioso más de eso (una sesión SSH, el control de iperf, un
websocket) se cerraba con RST al mandar su siguiente byte, antes de que
pudiera volver el ACK. Lo encontró la matriz de 60 s, que falló entera
(iperf: «unable to send control message»); con `dead_s = 300` pasaba, y un
registro temporal mostró el flujo establecido con 1 byte en vuelo. Ahora un
flujo sin nada en vuelo reinicia la cuenta al enviar (`send_one`). Venía al
menos desde 0.12.0.

### El horizonte de reparación

`bondvs` (n = 8, 20 s, FEC v3 y rescate, `grit` = horizonte con código
frente a `grit0` = solo el retardo por enlace): duplicados `equal` 2160 →
472, `het` 2974 → 82, `lossy` ~1000 → 0, `stall50` 3379 → 237 y 4114 → 8;
ping p99 `equal` 322 → 152, `lossy` 216 → 108, `stall50` 357 → 196 y 379 →
170 (p ≤ 0,04); caudal igual o +2 a +7 %; `protect`, `single1` y `cell4` sin
cambio.

`nackfirst/out45` (n = 6, 45 s, para que la FEC deje de arrancar; `grit2` =
NACK primero siempre): caudal frente a `grit0` +6 y +12 % en `equal`
(p ≤ 0,05), +11 % en `het` de bajada (p 0,01), +3/+4 % en `stall50` (p ≤
0,01), igual en el resto; duplicados `het` 3176 → 6, `reorder150` 392–916 →
0, `lossy` 745–1418 → 0–1, `stall50` 6110–8080 → 659–1151; ping p99
`stall50` de bajada 377 → 228 y `equal` 254 → 178 (p 0,01).

Sin FEC, con rescate (`nackfirst-nofec`, n = 6, 30 s): `lossy` duplicados
12546 → 398 y 8450 → 188, ping p99 191 → 144 y 164 → 139 (p ≤ 0,02);
`cell4` caudal +17 y +28 % (p ≤ 0,01), ping 338 → 300 y 458 → 267.

### Bonding: TCP directo, Split TCP y GRIT

`bondvs` (n = 8, 20 s; misma topología: LAN → router → túnel → VPS → 50 ms
al destino; Mbit/s, bajada / subida):

| Caso | TCP directo | Split TCP | GRIT | GRIT / Split |
| --- | --- | --- | --- | --- |
| `equal` 2×50 | 58 / 63 | 68 / 68 | 76 / 72 | +11 / +6 % |
| `het` 60+30+10 | 29 / 54 | 63 / 63 | 62 / 63 | igual |
| `idleslow` | 40 / 39 | 60 / 62 | 53 / 69 | −11 / +12 % (n.s.) |
| `lossy` 1 % | 65 / 67 | 84 / 93 | 95 / 99 | +14 / +7 % |
| `cell4` | 25 / 31 | 33 / 33 | 27 / 28 | −20 / −15 % |
| `stall50` | 50 / 40 | 55 / 53 | 53 / 56 | −4 / +6 % |
| `protect` | 24 / 22 | 40 / 35 | 46 / 46 | +15 / +30 % |
| `single1` | 21 / 19 | 34 / 33 | 36 / 36 | +5 / +11 % |

El ping p99 de GRIT es el más bajo en `equal`, `het`, `lossy` y `stall50`
(152 frente a 398 ms del Split en `equal`) y el más alto en `cell4` (413
frente a 296).

### La dispersión en el horizonte

`spread` (n = 6, 20 s; `grit2` = NACK primero, `spread` = más la dispersión,
`split` = Split TCP con el mismo motor; Mbit/s bajada / subida):

| Caso | `grit2` | `spread` | `split` | `spread` / `grit2` |
| --- | --- | --- | --- | --- |
| `cell4` | 26,5 / 26,7 | 30,4 / 31,8 | 31,7 / 32,9 | +15 / +19 % (p ≤ 0,02) |
| `idleslow` | 54,5 / 58,4 | 66,2 / 70,0 | 58,3 / 59,3 | +21 / +20 % (p 0,08–0,11) |
| `reorder50` | 49,6 / 39,0 | 51,7 / 50,9 | 61,3 / 51,0 | +4 / +31 % (n.s.) |
| `protect` | 47,0 / 43,2 | 42,0 / 47,0 | 44,3 / 39,5 | −11 / +9 % (n.s.) |
| `stall50` | 53,5 / 55,6 | 52,3 / 55,2 | 55,9 / 54,0 | −2 % (p 0,02) / igual |
| `equal`, `het`, `lossy`, `single1` | — | igual | — | sin diferencia significativa |

`cell4`: duplicados 3101 → 1296 y 2956 → 1109, ping p99 de bajada 410 → 323
ms (p 0,02). Frente al Split TCP, GRIT con la dispersión gana en `equal`,
`idleslow`, `lossy` y `single1`, empata en `het` de subida, `stall50` y
`protect`, y pierde en `cell4` (−4 %), `het` de bajada (−12 %) y
`reorder50` de bajada (−16 %); su ping p99 es menor salvo en `reorder50` de
bajada y `protect` de bajada.

## Pendiente

- **El búfer adaptativo:** crecer de 1 a 4 MB solo con el flujo limitado
  por la ventana y sin pérdida ni cola (NR: +40 % en `ratsingle`; fijo,
  triplica el ping de `lossy`).
- **Reponerse con la capacidad del motor:** al caer de NR a LTE GRIT cortó
  a 11,7 Mbit/s y tardó ~7 s en volver a 28, con el motor ya en ~35.
- **La subida de `ratsingle`** (22 frente a 32 del Split TCP) no mejora con
  el búfer: otro freno.
- **Repartir los bytes de GRIT a los enlaces lentos** por su capacidad, sin
  esperar la cola del rápido (`reorder50`, el alcance revertido).
- **Las ráfagas del arranque** en `cell4` (~1100–1300 duplicados) siguen.
- **`cell4`** queda a −4 % del Split TCP; **`reorder50`** de bajada a −16 %
  (51,7 frente a 61,3) y con más ping (390 frente a 141 ms); `het` de
  bajada −12 %; `protect` de bajada −11 % frente a `grit2` (no
  significativo).
- **Usar la pérdida por enlace** que el NACK ya trae: que el reparto la
  tome como pérdida de datos real, no solo de sondas.
- **Un flujo que empieza:** que GRIT le avise al motor para sondear los
  enlaces antes, en vez de repartir con capacidades de sondas (repartir por
  la mejor capacidad reciente trabó el túnel, historia 025).
- **Prioridad y plazo por flujo** (vivo o bulto) para la FEC y las copias.
- **Los bordes de ranura de Starlink y los microcortes** hacia GRIT.
- El terreno.

## Cambios

- 2026-10-27: creada. El retardo por enlace (`linkowd.h`), tres variantes
  medidas.
- 2026-10-27: el horizonte de reparación (NACK primero), el temporizador de
  flujo muerto corregido, el bonding contra el Split TCP y el TCP directo, y
  `cell4` como pendiente.
- 2026-10-27: `cell4` analizado; la dispersión de los enlaces en el
  horizonte.
- 2026-10-27: el alcance y la dispersión de arranque, probados y
  revertidos; la cota del estimador; el cambio de RAT en el laboratorio;
  el error de los 256 rangos y el SACK renegado; el búfer de 4 MB.
