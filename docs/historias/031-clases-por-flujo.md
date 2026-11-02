# 031 — Clases por flujo (U5): la FEC y la protección para GRIT, y el proxy UDP

- **Fecha:** 2026-11-02
- **Estado:** en curso (pasos 1 y 2 hechos y medidos en laboratorio: `fec_stream`, protocolo 17, PR #158; el paso 3 por hacer)
- **Fuentes:** historia 030 (protocolos 13–16, el reordenador por clase de
  datagrama), 026 (FEC v3), 020 y 021 (`redund_spare`), 025 (GRIT en
  redundancia), 029 (MPGRIT U5); `engine/src/fec3.h`, `repair.h`, `rlc.h`,
  `spare.h`, `dgclass.h`, `client.c`, `server.c`.

## TL;DR

- **Hoy GRIT no tiene FEC.** Desde el protocolo 13 sus mensajes van como
  STREAM, fuera de la FEC v3 y de la protección de `redund_spare`; la
  recuperación es solo su ARQ (SFLOSS por dos enlaces, RACK, temporizador).
  Con pérdida al azar eso cuesta un RTT por reenvío; la FEC v3 la bajaba 9 a
  24 veces en la historia 026.
- **Paso 1 hecho: `fec_stream` (apagada; el operador decide).** En terreno,
  el video por GRIT con el jitter más bajo (p99 − p50 13–24 ms contra
  14–79 del 16) por ~6 % de la subida. Con los
  enlaces llenos cuesta 4–20 % del caudal (GRIT ya recupera la pérdida
  suelta con su ARQ y nadie espera sus huecos desde el 13); con video en
  vivo (limitado por la aplicación) baja el p99 (`lossy` 83 → 49 ms,
  `cell4` 104 → 58) y las detenciones bajo microcortes (`protect`, 9,8 →
  2,6 s). Solo la reparación de cola (sin parte) no costaba nada, pero casi
  no salía (2–20 por corrida): descartada.
- **Paso 1, la FEC v3 sobre STREAM (protocolo 17):** una segunda ventana
  deslizante de RLC con la secuencia de la clase STREAM, sus reparaciones
  marcadas por clase (el byte 13 de `CG_REP_RLCW`), y lo rehecho revisado
  sin bytes de más en el cable: el símbolo de cada mensaje lleva un CRC32
  que ambos extremos calculan (el emisor al alimentar la ventana, el
  receptor al guardar lo que llega) y que solo se mira en lo rehecho. Con el
  código encendido, una pérdida que avisa el subflujo es «puede rehacerse»
  (`CG_RESC_SURE_LATER`): GRIT espera el horizonte del código antes de
  reenviar, la misma maquinaria que tenía DATA.
- **Paso 2, la protección en redundancia:** hoy el DATA de un flujo de GRIT
  va por un solo enlace (`pnc_first`, historia 025: copiarlo llenaba la cola
  del celular) y queda fuera de la paridad y los códigos (arreglo del
  2026-11-02, PR #157). Con el paso 1, la paridad de `redund_spare` puede
  cubrirlo en su propia clase.
- **Paso 3, el proxy UDP:** frames `DATAGRAM` por flujo (sin los 28 o 48
  bytes de IP y UDP por paquete, ~15 % en voz), con la clase de cada flujo
  explícita: MPEG-TS en orden con su plazo, RTP/RIST con el plazo de lo
  demás, QUIC reordenado por flujo (un hueco de un flujo no frena a otro),
  DNS directo. El UDP sigue siendo UDP: sin ARQ, protección solo dentro del
  plazo.

## Contexto

La historia 030 sacó los mensajes de GRIT del reordenador, del rescate y de
los códigos del túnel (STREAM) y dejó la protección del tráfico de la TUN
por clase de datagrama (`dgclass.h`). El usuario pidió seguir con U5 y la FEC
en los dos modos, bonding (FEC v3) y redundancia (`redund_spare`).

## Diseño del paso 1

- **El emisor:** un `struct cg_rlcw_tx` más por sesión y sentido, alimentado
  con cada STREAM que no es copia (los de `pnc` también: el código cubre lo
  que un enlace pierde, no lo que se copia). La parte (`fec_pct`,
  `fec_pct_min`) y el reparto entre enlaces (`cg_f3_pick`) son los de la FEC
  v3; el crédito de cada clase se gana con sus bytes.
- **El símbolo:** el mensaje y, detrás, su CRC32 (4 bytes, big-endian), con
  la longitud de 16 bits de siempre contándolos. Nada de eso viaja con el
  STREAM: solo existe dentro de la ventana de cada extremo.
- **La reparación:** `CG_REP_RLCW` con el byte 13 en 1 (clase STREAM); su
  base y su tramo, en la secuencia de esa clase.
- **El receptor:** un anillo de STREAM recibidos (como el de DATA de la FEC
  v3) y su receptor de RLC; lo rehecho pasa si su CRC32 cuadra, se marca en
  la ventana anti-replay de STREAM (una copia que llega tarde es duplicada)
  y va a `cg_pctl_rx` sin enlace (`CG_PCTL_NOLINK`: sin muestra de retardo).
- **Con GRIT:** `on_sfloss` de un STREAM pasa `coded` cuando su ventana lo
  cubre; `cg_resc_sure` da `SURE_LATER` y GRIT lo deja sospechoso hasta el
  horizonte del código (`pctl_horizon`) en vez de reenviarlo al tiro.
- **Medir:** los casos con pérdida al azar (`lossy`, `starlink15`, `cell4`)
  y el video real; laboratorio y terreno, con la FEC encendida en los dos
  extremos.

## Lo hecho y medido del paso 1 (2026-11-02)

- **El código** (`streamfec.h`, `client.c`, `server.c`): la ventana de los
  STREAM (`cg_sfec_add`) con el CRC32C detrás de cada mensaje (SSE4.2 o la
  instrucción de ARMv8 si la compilación la trae; si no, tablas de a 8
  bytes); la clase en el byte 13 de `CG_REP_RLCW` (`CG_REP_STREAM`); el
  receptor (`cg_sfec_rx`, ~4,7 MB) creado con la primera reparación de
  STREAM y guardando solo mientras llegan (2 s): sin la FEC no cuesta nada;
  lo rehecho pasa si cuadra su CRC32C, se marca en el anti-replay de STREAM
  y va a GRIT sin enlace. Un solo crédito para las dos clases: cada
  reparación va a la que más se le debe (su parte de bytes menos lo que
  costaron sus reparaciones). Con `fec_stream`, un SFLOSS de un STREAM
  cubierto es `SURE_LATER`.
- **Laboratorio, carga masiva** (`cgb`, cubic, 30 s × 3, `fec = yes`,
  `fec_code = rlc` en los dos arms; p16 = master, p17 = la parte entera):

  | Caso | Sentido | p16 | p17 | Δ |
  | --- | --- | --- | --- | --- |
  | `lossy` | bajada / subida | 118,9 / 119,4 | 100,4 / 98,2 | −16 / −18 % |
  | `cell4` | bajada / subida | 36,7 / 36,8 | 29,2 / 31,0 | −21 / −16 % |
  | `het` | bajada / subida | 75,8 / 79,2 | 71,3 / 73,5 | −6 / −7 % |
  | `equal` | bajada / subida | 83,9 / 83,8 | 81,2 / 80,4 | −3 / −4 % |
  | `starlink15` | bajada / subida | 114,3 / 105,9 | 103,5 / 106,2 | −10 / 0 % |
  | `protect` | bajada / subida | 29,4 / 24,0 | 33,6 / 24,8 | +14 / +3 % |

  En `cell4` el p99 del ping cargado sube de 349 a 583 ms.
- **Laboratorio, video en vivo** (`live`: dos flujos de 8 Mbit/s por GRIT;
  `soak`: lo mismo con conexiones cortas al lado; subida, × 3): p99 del
  retardo del flujo cercano, ms, y detenciones del lejano:

  | Caso | `live` p16 → p17 | `soak` p16 → p17 | detenciones `soak` |
  | --- | --- | --- | --- |
  | `lossy` | 83 → 49 | 454 → 246 | 0 / 0 |
  | `cell4` | 104 → 58 | 563 → 2629 | 0 → 1,9 s |
  | `protect` | 64 → 46 | 3246 → 1780 | 9,8 → 2,6 s |
  | `starlink15` | 373 → 391 | 780 → 579 | 0,3 → 0 s |
  | `het` | 45 → 43 | 346 → 480 | 0 / 0 |

- **Solo la reparación de cola** (sin parte, una al final de cada ráfaga de
  STREAM): igual que p16 en la carga masiva (±0,5 %), pero salieron 2 a 20
  por corrida (el video no deja pausas de `fec_wait_ms`): inútil, se quitó.
  El camino entero sí quedó probado en el motor: todas tomadas, una rehecha
  con su CRC32C bien, ninguna mala.
- **Decisión** (usuario): como la FEC del túnel, con su espera y su lógica,
  en los dos sentidos y con varios enlaces; el operador la enciende
  (`fec_stream`, LuCI «Reparaciones también sobre GRIT»).

## El paso 2: la protección en redundancia (2026-11-02)

- **Qué:** en redundancia el DATA de GRIT va por un solo enlace
  (`pnc_first`). Con `fec_stream` y la protección de `redund_spare`
  (`parity`, `mix`, `rs` o `rlc`), reparaciones de la ventana de los STREAM
  (las mismas del protocolo 17, sin versión nueva) por otro enlace que el
  suyo (`code_send`), a `fec_pct` de sus bytes y a más tardar
  `redund_spare_code_ms` tras el primero sin reparación (`sr_send`,
  `sr_service`). Solo la subida: el servidor no tiene `redund_spare` y en
  redundancia copia la bajada por todos los caminos.
- **Tres rondas** (subida, cubic y `live`, × 3, `redund_spare = rlc`,
  sin y con `fec_stream`):
  1. Como toma prioritaria del balde y con GRIT esperando: peor (`protect`
     cubic 48 → 21 Mbit/s, el p99 del ping 471 → 2106 ms; el video de
     `protect` 48 → 258 ms).
  2. Dentro de la asignación, GRIT esperando: el video todavía peor
     (`protect` 48 → 321 ms, `lossy` 79 → 247): muchas pérdidas no se
     rehacen (lo que sobra no alcanza) y GRIT las espera hasta el horizonte.
  3. **Dentro de la asignación y sin espera** (quedó así): caudal +1–8 %
     (`cell4` 15,1 → 16,3, `starlink15` 60,5 → 62,1), el video de `cell4`
     con el p99 del cercano de 3762 a 2387 ms y el ping cargado de 137 a
     62, `lossy` 79 → 68; peor el ping grande (DATA) en `cell4` (804 →
     1775) y `starlink15`, y el video de `protect` (48 → 104, un caso con
     mucho ruido).
- **La regla que deja:** en bonding la parte va siempre, así que GRIT puede
  esperar el código; en redundancia la protección depende de lo que sobre,
  así que es un extra y GRIT reenvía al tiro.

## Terreno (2026-11-02, testnet: NUC con 4 celulares, VPS Vultr)

Bonding, `protocontroller = b`, los dos extremos cambiados juntos, 3 rondas
alternadas por rama: p16; p17 con la configuración del testnet (`base`,
sin FEC en la subida y XOR en la bajada); p17 con la FEC v3 (`fec = yes`,
`fec_code = rlc`) en los dos extremos (`f3`); y además `fec_stream` (`fs`).
Cada rama: iperf3 `-P 4` 15 s de bajada y de subida por GRIT, y un video en
vivo por GRIT de subida (`livesim`, 8 Mbit/s, 30 s; entre máquinas solo vale
su jitter, p99 − p50, y el peor sobre la mediana).

| Rama | Bajada (Mbit/s) | Subida | Jitter p99 − p50 (ms) | Peor − p50 (ms) | Rehechos (VPS / router) |
| --- | --- | --- | --- | --- | --- |
| p16 | 360/393/381 (378) | 119/125/127 (124) | 14/51/79 | 19/302/243 | — |
| p17 `base` | 249/429/424 (367) | 108/128/129 (122) | 251/34/21 | 376/66/51 | — |
| p17 `f3` | 393/420/413 (409) | 122/123/120 (122) | 18/28/21 | 45/60/63 | 0 |
| p17 `fs` | 338/398/414 (383) | 104/124/116 (115) | 13/16/24 | 22/53/54 | 9270–12 150 / 99–1711 |

- El 17 sin `fec_stream` es el 16 (la bajada y la subida dentro del ruido).
- Con `fec_stream` el video por GRIT es el más parejo de las cuatro ramas;
  cuesta ~6 % de la subida llena. Sobrecarga 15–20 %, ningún CRC32C malo.
- Un tropiezo de la prueba: las claves agregadas al final de un `.conf`
  caen en su última sección (`[link]`, `[client]`), no en la global; la
  primera vuelta midió sin FEC.

## Paso 3a hecho: `flow_class` (2026-11-02)

- **Qué:** `flow_class = ADDR[/LEN][:PORT] CLASE` (o `[IPv6]…`, o `:PORT`),
  repetible hasta 32, en el extremo que recibe; calza el origen o el
  destino de cada paquete (cubre el flujo en los dos sentidos), gana la
  primera regla y manda sobre lo que adivina `dgclass.h`. Clases para el
  reordenador: `ordered` (espera su rescate, como MPEG-TS), `deadline` (la
  regla del plazo, como RTP), `direct` (nunca retenido ni rescatado: QUIC,
  SRT, WebRTC, juegos) y `auto` (lo adivinado). `flowrule.h` puro con sus
  tests (`cg_fc_parse`, `cg_fc_match`, `cg_fc_hold`); sin reglas, una
  comparación por paquete. Las reglas de TCP ya existen en la intercepción
  del protocontrolador (qué va por GRIT); la tabla de LuCI (3b) mostrará las
  dos.
- **Laboratorio** (el flujo chico de udpgen, 1316 B a 760 por segundo, que
  no parece TS; 3 corridas por brazo; el caudal masivo igual en todos):

  | Caso, sentido | Brazo | Perdidos | Desordenados | p50 (ms) | > 200 ms |
  | --- | --- | --- | --- | --- | --- |
  | `cell4` bajada | ninguna / `ordered` / `direct` | 14 / 9 / 13 | 78 / 19 / 11 508 | 147 / 162 / 130 | 3468 / 6578 / 2456 |
  | `het` bajada | ninguna / `ordered` / `direct` | 53 / 67 / 21 | 218 / 104 / 12 145 | 191 / 195 / 92 | 12 074 / 11 325 / 945 |
  | `lossy` bajada | ninguna / `ordered` / `direct` | 58 / 30 / 22 | 731 / 38 / 9885 | 98 / 180 / 93 | 1943 / 9870 / 836 |
  | `lossy` subida | ninguna / `ordered` / `direct` | 59 / 27 / 23 | 553 / 45 / 10 236 | 96 / 182 / 92 | 1109 / 9984 / 686 |

  `ordered` pierde la mitad y casi no desordena a cambio de esperar;
  `direct` entrega al tiro y deja el orden a la aplicación.

## Paso 3b hecho: la tabla «Flujos» (2026-11-02)

- **UCI:** secciones `config flowclass` (`name`, `endpoint`, `class`,
  `enabled`); `cengarde-setup` las escribe como `flow_class` en la sección
  global, en orden, y deja fuera (con su línea en el log) una clase o un
  extremo que no sirven.
- **El NAT del router:** `cg0` está en la zona WAN con masquerade, así que el
  túnel nunca ve la IP del LAN (la subida llega al VPS con el origen ya
  traducido; la bajada sale del motor del router antes de que el kernel
  deshaga el NAT). Una dirección privada con puerto se toma como el puerto
  solo (el NAT casi siempre lo conserva); sin puerto, se deja fuera. Lo que
  ven los dos extremos es la dirección del otro lado o el puerto.
- **El VPS:** `cengarde-vps-admin flows [ENDPOINT CLASS]...` reemplaza las
  líneas `flow_class` de `engine.local` (hasta 32, cada palabra revisada); el
  router se las manda con `vps_admin` en su sincronía de fondo, junto al modo
  (`/var/run/cengarde-vps-flows` guarda lo que el VPS ya tomó).
- **LuCI:** la tabla «Flujos» debajo de las pestañas: una línea con lo que
  hace el protocontrolador con el TCP (a qué destinos y puertos, o apagado)
  y las reglas de lo demás, con su revisión del extremo.
- **El detector `auto`, más adelante** (usuario): un analizador de
  encabezados que reconozca QUIC, RIST, SRT y más, y después firmas
  configurables o aprendidas; la regla IP:puerto queda para lo que no sepa
  reconocer (las máquinas de telemetría).

## Paso 3c hecho: el carril fiable (2026-11-02, protocolo 18)

- **Qué:** `flow_class = … reliable`. Con `protocontroller = b`, el router
  toma por TPROXY el UDP de esas reglas antes de su NAT (por eso aquí sí
  sirve una dirección del LAN) y lo lleva por un **flujo de datagramas** de
  GRIT: cada datagrama detrás de su largo de 16 bits, con el ARQ, el ritmo y
  los códigos de GRIT (`fec_stream` incluido). El OPEN lleva
  `CG_PCTLF_DGRAM`; el VPS abre un UDP conectado al destino y manda cada
  datagrama completo, en orden, como uno; las respuestas vuelven igual. Sin
  FIN propio, el flujo se cierra tras 120 s sin nada en ningún sentido. Un
  datagrama que no cabe en el búfer del flujo se tira y se cuenta (lo que
  haría UDP). En orden, no fuera de orden como pedía el diseño: un flujo de
  GRIT ordena, y para telemetría el orden sobra pero no estorba.
- **OpenWrt:** la cadena `cengarde_reliable` (TPROXY en `mangle`, la marca
  `0x4347`), la regla y la ruta local de la tabla 17223 (secciones propias
  de `network`), `kmod-nft-tproxy` como dependencia; LuCI ofrece la clase con
  su aviso.
- **Laboratorio** (`BOND_RELIABLE=1`: el flujo chico, 1316 B a 760 por
  segundo, del host del LAN al lejano por el carril; subida, cubic, × 2):

  | Caso | UDP: perdidos / desordenados / p50 | Fiable: perdidos / desordenados / p50 |
  | --- | --- | --- |
  | `lossy` | 44 / 593 / 97 ms | 0 / 2 / 172 ms |
  | `cell4` | 6 / 86 / 153 ms | 0 / 2 / 155 ms |
  | `het` | 122 / 1218 / 114 ms | 0 / 3 / ≥ 200 ms |
  | `protect` | 630 / 730 / 35 ms | 5100 / 0 / ≥ 200 ms |

  En `protect` el carril (8 Mbit/s, mucho más que telemetría) no cabe en
  su parte del enlace chico (~5 Mbit/s entre cuatro flujos) y tira lo que
  excede su búfer; el iperf de al lado baja de 23 a 9 Mbit/s. Pendiente:
  prioridad del carril sobre lo masivo.

## Paso 3d hecho: zstd en el carril fiable (2026-11-02)

- **Sin diccionario que acordar:** el carril es un flujo de GRIT, ordenado y
  fiable, así que los dos extremos ven los mismos datagramas en el mismo
  orden. Un contexto de zstd por flujo y sentido, en modo flujo y vaciado
  al final de cada datagrama (`zlane.h`: 128 KB de historia, nivel 3),
  aprende la forma de la telemetría con la primera ráfaga; el diseño del
  2026-11-01 (DC-SYN y DC-ACK) queda para lo que no es ordenado ni fiable.
  El OPEN lo dice (`CG_PCTLF_ZSTD`); un motor sin zstd rechaza el flujo.
  `reliable_zstd` (sí por omisión) en el router; libzstd opcional al
  compilar (`ZSTD=auto`), en el paquete de OpenWrt y en el cloud-config del
  VPS.
- **Laboratorio** (`BOND_SMALL_JSON=1`: cada datagrama una lectura JSON de un
  sensor, ~180 bytes más la cabecera de 24 de udpgen; 760 por segundo por el
  carril; × 2): 3,09 MB → 0,62 MB (5,0 veces) en `lossy` y `cell4`, todos
  entregados, la misma mediana (122–157 ms con y sin zstd).

## Diseño del paso 3: las clases de UDP (2026-11-02, con el usuario)

El UDP manda a su tasa sin saber de pérdida ni de cola: el túnel no puede
frenarlo, solo repartir, proteger o descartar. Cuatro clases, cada una con lo
que le conviene:

| Clase | Ejemplos | Su recuperación | Del túnel |
| --- | --- | --- | --- |
| Constante sin defensa | MPEG-TS crudo, cámaras, telemetría | ninguna, sin búfer de reorden | orden, rescate y FEC, reteniendo hasta el presupuesto (~500 ms) |
| Con búfer, sin reenvío | RTP de voz o video sin NACK | búfer de jitter | rescate y FEC dentro de su plazo; el orden lo pone él |
| Se recupera sola («RUDP») | QUIC, SRT, RIST, WebRTC con NACK, juegos con ARQ | su ARQ, su FEC, su control de congestión | directo: sin retener ni rescatar (duplica sus reenvíos y le enturbia el RTT); a lo más una FEC liviana |
| Tolera la pérdida | VoIP con Opus FEC, DNS, juegos de estado | ninguna o la propia | directo, sin nada |

- **Reconocerlas por flujo, no por paquete:** `dgclass.h` ve MPEG-TS y RTP;
  una tabla de flujos que mire los dos sentidos agrega QUIC (encabezado largo
  y versión), SRT (su handshake y sus paquetes de control) y el RTP con NACK
  en su RTCP de vuelta (RIST, WebRTC), que pasa de la segunda clase a la
  tercera. Es la tabla del proxy UDP (`DATAGRAM` con id de flujo).
- **ECN:** QUIC y WebRTC lo hablan; marcar CE cuando un enlace se encola los
  hace bajar su tasa sin perder un paquete. Para los demás, CAKE.
- **La protección para quien no la tiene:** FEC y rescate para las dos
  primeras clases; las otras dos, directo.
- **Una quinta, a pedido: telemetría fiable** (idea del usuario, 2026-11-02;
  caso de uso guardado, sin fecha). La excepción declarada a «el UDP sigue
  siendo UDP», encendida a mano en las funciones de L4 por puerto o destino:
  el router intercepta ese UDP y lo entrega sí o sí, reintentando hasta que
  llegue, con un búfer de memoria que se pueda configurar (JSON a buena tasa
  de muestreo durante un corte). Llega fuera de orden, sin bloqueo en cabeza
  de línea: el registrador del otro extremo ordena por las marcas de tiempo
  que la telemetría ya trae. Con GRIT sale casi gratis: cada datagrama, un
  mensaje fiable de un flujo propio sin el orden de un flujo TCP (el ARQ y
  el SFLOSS ya están). Con el búfer lleno se va lo más viejo, contado en el
  estado; nunca bloquea. Aviso en LuCI: «pensado para telemetría o UDP que no
  es de tiempo real y debe llegar; suma latencia, no sirve para voz ni
  video».
- **Reglas que mandan sobre lo adivinado** (usuario, 2026-11-02): en UCI,
  una tabla de origen o destino (IP:puerto o prefijo) y su clase (`auto`,
  `ordered` para TS, `deadline` para RTP, `direct`, `reliable`), p. ej.
  `config flowclass` / `option src '192.168.50.158:9330'` / `option class
  'reliable'` para un JSON de telemetría. El motor la aplica al leer cada
  paquete de la TUN (como `dgclass.h`), no con marcas de nftables, que no
  cruzan la TUN; LuCI la muestra como tabla en las funciones de L4.
- **zstd en ese carril** (usuario, 2026-11-02): la telemetría JSON repite
  claves y forma, el caso ideal para los diccionarios de zstd (diseño del
  2026-11-01: DC-SYN/DC-ACK, usar el diccionario solo después de su ACK,
  nunca bloqueo en cabeza de línea). Una ráfaga entrena el diccionario
  rápido y el carril fiable ya lleva su sincronía; cada mensaje sigue
  comprimido por su cuenta, así que el desorden no estorba.

## Pendiente

- El paso 1 en terreno.
- El paso 2 (redundancia) en terreno.
- El paso 3c (el carril fiable) y 3d (zstd en él); el detector `auto` con
  analizador de encabezados (QUIC, RIST, SRT).

## Cambios

- 2026-11-02: creada, con el diseño de los tres pasos.
- 2026-11-02: las cuatro clases de UDP y el ECN, del usuario.
- 2026-11-02: la telemetría fiable como caso de uso, del usuario.
- 2026-11-02: el paso 1 hecho y medido (`fec_stream`, protocolo 17).
- 2026-11-02: el paso 2 (la protección en redundancia), tres rondas.
- 2026-11-02: el paso 1 en terreno.
- 2026-11-02: el paso 3a (`flow_class`), con su laboratorio.
- 2026-11-02: el paso 3b (UCI, LuCI, la copia al VPS, el NAT del router).
