# 030 — Protocolo 13: el túnel dentro de GRIT

- **Fecha:** 2026-11-01
- **Estado:** vigente (etapa A hecha y medida en el laboratorio y en terreno)
- **Fuentes:** el NUC y el VPS de la testnet el 2026-11-01 (estado antes y
  después de 20 s de bajada y 20 s de subida); laboratorio
  `~/campo/2026-11-01/u3a/` (el reordenador apagado),
  `~/campo/2026-11-01/p13a/` (protocolo 12 contra 13) y `p13ts`–`p13ts3`
  (con un flujo como MPEG-TS), `p13skip`, `skipbis`, `p13voice` y
  `p13tsreal`, `p13wg`, `wgbis`, `p13rtp`, `rtp2`–`rtp4`, `rtpbis`, `ts4`, `ts5`, `voice2`, `tsreal2`; terreno `~/campo/2026-11-01/p13field/`; `engine/src/proto.h`,
  `clientpath.h`, `subflow.h`, `client.c`, `server.c`; historia 029 (U1–U7).

## TL;DR

- **Hoy hay dos protocolos apilados:** el túnel (28 B: `seq` global, `ts`,
  MAC, `sseq`) y GRIT dentro (14 B + 4 de tag: `offset`, `ts`, SipHash).
  Las secuencias, las marcas de tiempo y la autenticación se repiten, y
  también las maquinarias de pérdida: el reordenador, el rescate y la PARQ
  del túnel frente a RACK, el temporizador y `SFLOSS` de GRIT.
- **Lo medido en la testnet** (20 s + 20 s): 27 000 NACKs del rescate para
  22 reenvíos reales; el resto eran mensajes de GRIT, que ya se enteran por
  el subflujo (6 300 `SFLOSS`) y reenvían solos. En el laboratorio, el ping
  de 1200 B, que pasa por el reordenador como un paquete de MPEG-TS, RTP o
  QUIC, tiene +200 a +400 ms de p99 sobre el chico (+1009 en `ratswitch`):
  la TUN espera huecos que son de GRIT.
- **Dos acoples más:** el reparto exige que el otro extremo reordene (con
  `reorder = no` todo va copiado y el TCP cae a la mitad), y la pérdida por
  secuencia de la capacidad cuenta contra la `seq` global de DATA.
- **Etapa A, hecha:** los mensajes de GRIT van en su propia clase
  (`CG_T_STREAM`, protocolo 13), con su secuencia y su ventana anti-replay.
  Pasan directo a su stream, sin reordenador, sin rescate y sin códigos; lo
  que pierde un subflujo les llega por un anillo propio. Pueden repartirse
  aunque el otro extremo no reordene. La capacidad cuenta su pérdida contra
  la `sseq` de cada enlace.
- **Medido en el laboratorio** (12 casos × 2 sentidos × 3, semilla 2026, TCP
  por GRIT): el TCP +1 a +3 % (media geométrica) y el p99 del ping de 1200 B
  de 401 a 220 ms. Con un flujo como MPEG-TS al lado (760 paquetes/s de
  1316 B), la primera versión perdía 2,4 veces más (10 → 25 ‰): el rescate
  esperaba 60 ms fijos y los STREAM no le ganaban presupuesto. Con la espera
  de un RTT del enlace más rápido y el presupuesto, 11,2 → 8,3 ‰, y los fuera
  de orden del flujo bajan en los 24 casos (a un tercio o menos en 13).
  Un MPEG-TS real (ffmpeg) decodifica 93,4 % de los cuadros en vez de
  89,9 %; la voz baja de 94 a 68 ms de mediana y casi no pasa los 200 ms;
  el TCP que no va por GRIT (otra zona, un puerto reenviado) y uno dentro
  de un WireGuard quedan igual. En terreno, el mismo TCP y un flujo UDP de
  subida que pierde 3,5 veces menos (7,6–12,9 % → 1,9–4,5 %). Un video RTP
  de verdad destapó que un hueco pedido retenía de más: ahora espera solo
  hasta su plazo, y el RTP decodifica 92,4 % de los cuadros (88,4 % el 12).
- **Lo que viene:** el encabezado de subflujo con anti-replay por enlace,
  frames `STREAM` y `DATAGRAM` (de ~46 a ~25 B por paquete), las clases por
  flujo para los datagramas (MPEG-TS/RTP en orden con plazo, RIST directo,
  QUIC por flujo) y la capacidad vieja solo para medir.

## Contexto

La historia 029 plantea absorber el motor en GRIT (U1–U7). Al empezar U3
(«una sola maquinaria de pérdidas»), los contadores del NUC y del VPS
mostraron que el reordenador y el rescate del túnel trabajan sobre los
mensajes de GRIT sin poder hacer nada útil con ellos: el rescate ya no los
reenvía (desde la historia 025 se los entrega a GRIT como aviso), pero el
reordenador sí ve sus huecos, porque comparten la secuencia de DATA, y con
eso retiene el tráfico de la TUN que viene detrás.

Apagar el reordenador no sirve: MPEG-TS y RTP sin su propio búfer se
rompen con el desorden (pedido del usuario: dejarles el reordenador), y el
reparto exige un receptor que reordene.

## Hallazgos

### Lo que se repite en cada paquete (protocolo 12)

| | Túnel (28 B) | GRIT dentro (14 B + 4) |
| --- | --- | --- |
| Secuencia | `seq` global; `sseq` por enlace | `offset` del stream |
| Marca de tiempo | `ts` | `ts` |
| Autenticación | MAC de 8 B | SipHash de 4 B (contra lo que rehace mal la FEC) |
| Quién | `session`, `link` | `stream id` |
| Pérdidas | reordenador, rescate, PARQ | RACK, temporizador, `SFLOSS` |
| Ritmo | capacidad vieja y `link_cc` | `streamcc` |

### La testnet, 20 s de bajada y 20 de subida (2026-11-01, protocolo 12)

| | Bajada (el NUC recibe) | Subida (el VPS recibe) |
| --- | --- | --- |
| NACKs que mandó el receptor | 14 910 | 12 092 (27 000 secuencias) |
| De ellos, mensajes de GRIT (solo aviso) | 18 824 | 27 533 |
| Reenviados de verdad por el rescate | 0 | 22 |
| Huecos que el reordenador dio por perdidos | 4 443 | 16 002 |
| De ellos, llegaron tarde | 1 261 | — |

### El laboratorio (`u3a`, TCP por GRIT, configuración de terreno sin FEC)

- El ping chico (≤ 128 B, el reordenador no lo retiene) y el de 1200 B en el
  brazo normal: p99 de 110–260 ms contra 320–600 ms; +200 a +400 ms, +1009
  en `ratswitch` de bajada. Parte de la diferencia es que el chico va
  copiado por todos los enlaces y el grande por uno con su cola.
- Con `reorder = no` el motor deja de repartir (`cg_split_active` pide
  `peer_reorders`): todo va copiado, el TCP cae 47 % (−26 a −74 % por caso).
- El laboratorio no armaba QUIC con el protocontrolador: lo arregla esta
  historia (`bond_pep_traffic`, servidor QUIC en el host lejano). Con el
  protocolo 13, `equal` de bajada: 44,6 Mbit/s, 13 % retransmitido (4 686
  espurios): el desorden del reparto que verá U5.

## Qué hacemos con esto

### Etapa A: la clase STREAM (hecha, `claude/proto13`)

- `proto.h`: versión 13, `CG_T_STREAM = 11`, `cg_type_cls` (DATA, STREAM,
  control). La sonda crece a 52 B: `rx_top_stream`, `tx_next_stream` (un
  servidor reiniciado retoma también esta secuencia) y `rx_sseq` (la `sseq`
  más nueva recibida por ese enlace).
- `clientpath.h` y `server.c`: una ventana anti-replay más por sesión; un
  STREAM verificado va a `cg_pctl_rx` al tiro, cuenta en `rx[].stream` para
  la sonda y su `sseq` va al subflujo como la de DATA.
- El envío numera cada mensaje de GRIT en su clase, no alimenta la FEC v3 ni
  la XOR, no cuenta para la máscara de enlaces activos del reordenador y no
  toca `CG_F_PREV`. Se guarda en un anillo propio (`sresc`, del mismo tamaño
  que el del rescate) para que `on_sfloss` encuentre el mensaje de una `sseq`
  perdida: el anillo del subflujo marca cada `sseq` de STREAM
  (`CG_SF_STREAM`). Para un STREAM no hay GONE (el reordenador no lo espera)
  ni «quizá lo rehaga un código».
- El reparto de un STREAM no exige que el otro extremo reordene
  (`pctl_send`, `pctl_hint`, `pctl_horizon`; `down_paths_of`).
- La capacidad, con `subflow`, cuenta la pérdida por secuencia contra la
  `sseq` del enlace (`tx_seq` = la próxima `sseq`, `rx_top` = `rx_sseq`, sin
  descontar el atraso respecto del más rápido: el número ya es del enlace).

### Lo que el MPEG-TS destapó: la espera y el presupuesto del rescate

Con los STREAM fuera del reordenador, los huecos que quedan en DATA son de
verdad del tráfico de la TUN, y el rescate tiene que salvarlos. Dos cosas lo
impedían:

- **La espera fija:** `rescue_wait_ms` (60) es menor que el RTT de los
  enlaces celulares del laboratorio; el NACK salía antes de que la copia
  pudiera llegar y el reenvío llegaba tarde. `cg_harq_rtt_wait_us`
  (`harq.h`) estira la espera a 1⅛ del RTT del enlace más rápido más 10 ms,
  con el tope de siempre.
- **El presupuesto:** el rescate reenvía en proporción a lo que el emisor
  mandó; los STREAM ya no sumaban y el servidor se quedaba sin presupuesto
  (`over_budget` 113 en un caso, 0 después). Un STREAM vuelve a ganar
  presupuesto aunque él mismo no se rescate.

| Ronda | Brazos | TCP (media geométrica) | Pérdida del flujo TS (‰) |
| --- | --- | --- | --- |
| `p13ts` | 12 frente a 13 (`dbbd884`) | +1,1 % | 10,3 → 24,9 |
| `p13ts2` | + la espera de un RTT (`59b9e5d`) | +1,9 % | 11,4 → 27,2 |
| `p13ts3` | + el presupuesto (`72302ef`) | +1,4 % | 11,2 → 8,3 |

Pérdida: media por caso de la mediana de `1 − únicos/enviados`. En `p13ts3`
los fuera de orden del flujo caen de 70–1369 a 8–1037 por corrida (a un
tercio o menos en 13 de 24, y bajan en todos); lo que sube: `downstall` de bajada (15 → 20 ‰),
`ratswitch` de subida (5 → 10) y `starlink15` (8 → 9). El TCP baja en `het`
de bajada (−6 %), `lossy` y `starlink15` de subida (−6 %), dentro de lo que
varía `protect` (−22 / +15 %, piso de ruido 20–36 %) pero fuera del de los
casos estables (0–3 %): vigilar en la ronda de terreno.

### Lo sensible al orden: voz y MPEG-TS de verdad

Las mismas 144 corridas por ronda, `p12` (`4070eca`) contra `p13b`
(`72302ef`), el TCP por GRIT:

- **Voz** (`p13voice`: 50 paquetes/s de 172 B, como Opus o G.711 en RTP): la
  latencia mediana del flujo baja de 94 a 68 ms y lo que un búfer de
  jitter de 200 ms tiraría por tarde cae en 23 de 24 casos (`dominant` de
  subida: 1177 → 1 paquete por corrida; `het` de subida: 595 → 51). El TCP
  queda igual (−1,9 %, todo por `dominant` de bajada, que en todas las rondas
  salta entre 23 y 83 Mbit/s en los dos brazos; sin él, +1 %).
- **MPEG-TS real** (`p13tsreal`: ffmpeg codifica H.264 a 6 Mbit/s en un TS
  CBR de 8 Mbit/s y otro ffmpeg lo decodifica al otro lado): los errores del
  decodificador bajan 27 % (95 723 → 69 571 en la ronda) y los cuadros
  decodificados suben de 89,9 a 93,4 % (los perdidos, −35 %); mejora en 21
  de 24 casos (5 a 8 veces menos errores en `downstall` de subida y `lossy`). TCP
  +1,3 %.

### El TCP que no pasa por GRIT

Una LAN en otra zona del firewall o una conexión que entra por un puerto
reenviado desde el VPS cruzan el túnel como DATA, con el reordenador y el
rescate de siempre, junto a los STREAM de los demás. El laboratorio lo arma
con `BOND_PEP_SKIP=5201` (el iperf3 queda fuera del `REDIRECT`, el resto
igual, con el flujo como MPEG-TS al lado):

- `p13skip` (12 casos): TCP −2,1 %, la pérdida del flujo TS igual
  (2,0 → 1,9 ‰).
- `skipbis`, para separar ruido de efecto (6 casos × 2 × 3, cinco brazos):
  `p12` contra sí mismo da +2,8 % y −16 a +59 % por caso (`cell4` de subida);
  contra `0367caf` +2,0 %, `59b9e5d` −1,5 %, `72302ef` +6,2 %. La media de
  las 36 corridas de cada brazo queda entre 52 y 55 Mbit/s.

Un TCP de punta a punta por el túnel varía mucho más que el de GRIT (su
piso A/A no es el 0–3 % de la historia 029): el protocolo 13 no le cambia
nada que el laboratorio pueda ver.

### WireGuard adentro

`p13wg` (`BOND_PEP_INNER=wg`: el iperf3 dentro de un WireGuard propio del
host de la LAN al lejano, UDP opaco que va como DATA): −3,6 % de media
geométrica, con `lossy` −25 a −32 % y `protect` −23 % en los dos sentidos.
`wgbis` (esos dos casos, 4 repeticiones, cinco brazos, uno el protocolo 13
con la capacidad por la `seq` global): `lossy` 23,6 → 29,5 de bajada y
32,2 → 37,6 de subida, `protect` igual; cada corrida de `lossy` salta entre
15 y 56 Mbit/s en cualquier brazo. Era ruido; el túnel adentro no pierde.

### Terreno (2026-11-01, tarde)

El NUC y el VPS con los dos binarios, cambiados juntos y alternados
(`~/campo/2026-11-01/p13field/`), 6 rondas por brazo; iperf3 de 4 flujos por
GRIT y un flujo UDP de 8 Mbit/s en paquetes de 1316 B al lado (como un TS):

| | Protocolo 12 | Protocolo 13 |
| --- | --- | --- |
| Bajada (media de 6) | 249 Mbit/s | 253 Mbit/s |
| Subida (media de 6) | 124 Mbit/s | 124 Mbit/s |
| Ping con carga en la bajada, p50 / p95 (3 rondas) | 47–57 / 135–190 ms | 48–60 / 146–197 ms |
| Pérdida del flujo UDP de subida (3 rondas) | 7,6–12,9 % | 1,9–4,5 % |
| Pérdida del flujo UDP de bajada (3 rondas) | 0–0,15 % | 0–0,04 % |

La subida es la que se llena (los módems dan ~125 Mbit/s): ahí el flujo UDP
pierde un cuarto de lo que perdía. El terreno quedó en el protocolo 13.

Una trampa del banco: el servidor UDP del VPS (sin `bind`) responde por el
túnel desde 10.79.0.1, no desde la IP pública a la que se le habló, y el
socket conectado de iperf3 descarta la respuesta («unable to read from
stream socket»). Un flujo UDP al propio VPS va a su dirección del túnel.

### RTP de verdad: el rescate que esperaba de más

`BOND_TSREAL=rtp`: ffmpeg manda el mismo H.264 en RTP (sin TS, a ráfagas por
cuadro) y el receptor lo abre con su cola de reorden de 200 ms
(`max_delay`, como un búfer de jitter); `bondtab.py` cuenta los paquetes que
esa cola nunca tuvo (`rtp_missed`) y las veces que esperó de más
(`rtp_late`). Con `72302ef` el video decodificaba 77–80 % de los cuadros
contra 88–89 % del protocolo 12, y perdía el doble de paquetes.

- **Primera sospecha, descartada:** sin los mensajes de GRIT en DATA, la regla
  RACK del reordenador (cada enlace trajo un DATA posterior) casi nunca se
  cumple con DATA ralo. Un STREAM le dice ahora cuán lejos va su enlace por
  el `ts` del emisor (`cg_ro_note`: un enlace que trajo algo enviado después
  del paquete retenido más bajo pasó todos los huecos debajo); correcto,
  pero el RTP no cambió (79,3 %).
- **La causa** (`rtpbis`, 3 casos, cinco brazos): antes del presupuesto el
  video decodificaba 93–94 %, con él 52 %. Con presupuesto se pide todo
  hueco, y un hueco pedido retenía lo que venía detrás el tope **más**
  `rescue_us` (historia 023): 200 + 60–120 ms, más que el búfer del video.
  Sin presupuesto el servidor respondía «no viene» y el hueco salía al tiro.
- **La regla:** un hueco pedido espera `rescue_us` desde el pedido, nunca más
  allá del plazo que ya tenía (el enlace más lento con su desvío, a lo más
  el tope); con tope 0 (redundancia) sigue esperando `rescue_us`. Un hueco
  que RACK encontró temprano le deja al rescate hasta ese plazo; uno pedido
  en su plazo sale. Lo que llega tarde igual se entrega (tarde).

| Ronda | Brazo | Cuadros RTP | Paquetes RTP perdidos | Pérdida del flujo TS | TCP |
| --- | --- | --- | --- | --- | --- |
| `rtp4`, `ts5` | `p12` | 88,4 % | 159 k | 10,5 ‰ | — |
| | tope o `rescue_us`, el mayor (`825509c`) | 81,3 % | 272 k | 8,2 ‰ | +5,4 / −1,6 % |
| | el plazo propio (`136b9ab`) | 92,4 % | 110 k | 9,8 ‰ | +6,4 / +0,6 % |

El protocolo 13 queda mejor que el 12 en las tres cosas: el video RTP, el
flujo constante tipo TS y el TCP. Las rondas anteriores de esta historia
(WireGuard, terreno) son de `72302ef`, sin esta regla. Repetidas con el 13
final (`232d4c7`, `voice2` y `tsreal2`):

- **Voz:** latencia mediana 99 → 68 ms; paquetes de más de 200 ms 20 423 →
  1 934 en la ronda (10 veces menos). TCP −1,1 %.
- **MPEG-TS real:** cuadros decodificados 90,6 → 89,2 %, errores del
  decodificador +19 % (91 310 → 108 991), en 17 de 24 casos; con `72302ef`
  eran −27 %. El rescate largo le servía al TS constante y la regla del
  plazo se lo quita: es el precio del RTP (88 → 92 %) y de la voz. La
  salida de verdad son las clases por flujo de U5 (el TS en orden con su
  plazo, el RTP sin esperas). TCP −0,6 %.

### Etapa A2: el encabezado de subflujo y los frames

- Encabezado por paquete y enlace: sesión, enlace y `sseq` dentro del MAC,
  una sola marca de tiempo, MAC; anti-replay por enlace; cada copia se sella
  aparte (medir el costo en la Pi).
- Frames: `STREAM` (id y offset en varint), `DATAGRAM` (flujo y, si la clase
  lo pide, una secuencia propia), los acuses por enlace y las sondas. GRIT
  usa la marca de tiempo del subflujo (sin la suya) y deja el tag cuando la
  FEC no cubre su clase.
- Desaparecen la `seq` global y el rescate; la secuencia de DATA queda solo
  para la clase ordenada de datagramas.

### Etapas siguientes

- U4: la capacidad vieja solo mide (quién entra al reparto, sus pesos, el
  rescate lleno y el tope de `cake`, por el ritmo propio).
- U5: clases por flujo para los datagramas: MPEG-TS/RTP en orden con plazo,
  RIST y SRT directos, QUIC reordenado por flujo, DNS y pings sin espera.
- La compresión de encabezados y zstd por frame (#78); el proxy por omisión,
  el nombre y la 1.0 (U7).

## Pendiente

- Repetir el terreno con `232d4c7` (la regla del plazo): está desplegado.
- La FEC v3 sobre STREAM: hoy no lo cubre (en terreno está apagada); si se
  enciende, decidir si protege también la clase STREAM.

## Cambios

- 2026-11-01: creada, con la etapa A.
- 2026-11-01: las rondas con un flujo como MPEG-TS, la espera de un RTT y el
  presupuesto del rescate para los STREAM; la voz, el MPEG-TS real y el TCP
  que no pasa por GRIT; WireGuard adentro y el terreno; el RTP real, `cg_ro_note`
  y el hueco pedido que espera solo hasta su plazo.
