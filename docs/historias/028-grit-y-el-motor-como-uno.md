# 028 — GRIT y el motor como uno: la vista compartida

- **Fecha:** 2026-10-27
- **Estado:** vigente (primer paso hecho; los siguientes, en «Pendiente»)
- **Fuentes:** `engine/src/linkowd.h`, `engine/tests/test_linkowd.c`,
  `protoctl.c` (`data_in`, `send_ack`, el estado `pacers`), `client.c`
  (`down_deliver`), `server.c` (`up_deliver`); historias 025 (GRIT, el
  colapso de la bajada, el estimador desbocado, el arranque) y 023 (el
  rescate, la PARQ); tarea #77; laboratorio `bench/labq.py` con
  `BOND_PEP=cgb`, datos en `~/campo/2026-10-27/gshare{1,2,3}/`.

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
| motor → GRIT | el horizonte de las reparaciones de un hueco | — | pendiente |
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

## Pendiente

- **El horizonte de las reparaciones:** que el motor le diga a GRIT si un
  hueco lo cubre un código y cuándo llega la reparación, en vez del margen
  fijo de su RACK (`rescue_code_ms`, hasta 100 ms).
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
