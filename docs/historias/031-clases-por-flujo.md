# 031 — Clases por flujo (U5): la FEC y la protección para GRIT, y el proxy UDP

- **Fecha:** 2026-11-02
- **Estado:** pasos 1, 2 y 3 hechos (PR #158 a #164, protocolos 17 y 18): `fec_stream`, `flow_class` con su tabla «Flujos», el carril fiable con zstd y `flow_learn`; los envíos por equipo (19), el UDP dentro de GRIT (20, `protocontroller_udp`), DiffServ (21, `protocontroller_dscp`), el crédito de ráfaga (`protocontroller_burst_kb`) y la clase «en vivo» (`livesni.h`); falta terreno de los pasos 2 y 3 y del crédito
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
- **El UDP dentro de GRIT (protocolo 20, `protocontroller_udp`, `udp_grit`
  en OpenWrt):** el resto del UDP de la LAN entra a GRIT como datagramas no
  fiables (DGM), por turnos por equipo con el TCP. En el laboratorio, con
  30–60 Mbit/s de UDP de un equipo, el TCP de otro pasa de 3–9 a 25 Mbit/s
  en `cell4` (ping p99 de 0,8–1 s a 0,2 s) y de 42 a 51 en `equal` y `het`,
  con el mismo total.

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

## Paso 3e hecho: lo aprendido por flujo (2026-11-02)

- **Qué:** `flowlearn.h`: el encabezado largo de QUIC (versión 1, 2, un
  borrador o 0), el handshake de SRT (control de tipo 0, UDT 4 o 5) y un
  NACK de RTCP (PT 205, FMT 1: RIST, WebRTC; con RTCP en el puerto siguiente,
  también su flujo RTP) dicen qué es un flujo. Una tabla de 512 flujos (los
  dos sentidos una clave; cada paquete del flujo lo renueva; olvidado tras
  120 s sin nada) lo guarda, alimentada con lo que cada extremo recibe y
  envía (un NACK va contra su medio). Sin regla y sin reparto, un flujo
  conocido va como `direct`. `flow_learn`, **apagado**; el estado cuenta lo
  aprendido.
- **Lo que enseñó el laboratorio** (QUIC de aioquic, × 2):

  | Modo | Caso | Retenido (Mbit/s) | Aprendido, sin retener |
  | --- | --- | --- | --- |
  | bonding | `equal` bajada / subida | 26,7 / 12,5 | 13,3 / 5,5 |
  | bonding | `het` bajada / subida | 42,0 / 12,6 | 6,2 / 4,8 |
  | redundancia | `equal` bajada / subida | 36,3 / 34,6 | 36,5 / 36,4 |
  | redundancia | `het` bajada / subida | 40,9 / 43,4 | 45,2 / 43,8 |
  | redundancia | `lossy` bajada / subida | 3,8 / 3,1 | 2,6 / 2,4 |
  | redundancia | `cell4` bajada / subida | 4,4 / 4,2 | 3,2 / 3,3 |

  En redundancia el p99 cargado de `cell4` bajó de 228 a 68 ms (bajada) y de
  286 a 81 (subida), y las retransmisiones a la mitad o menos.
- **La lección** (contra la tabla de clases de arriba): un protocolo que se
  recupera solo igual gana con el reordenador del túnel en bonding, porque
  el desorden entre enlaces lo hace el túnel, no la red; su «pasar directo»
  solo vale sin reparto, y aun ahí cambia caudal en enlaces con pérdida (el
  rescate del túnel) por latencia. Por eso actúa solo sin reparto y queda a
  elección del operador.
- **El terreno** (2026-11-02, testnet de 4 módems, p18 en los dos extremos,
  2 rondas por brazo; `~/campo/2026-11-02/quic18` y `quic18b`). Primero
  aioquic (`quicperf.py`, cubic, por `10.79.0.1`: la IP pública del VPS sale
  por fuera del túnel), luego un navegador de verdad: Chromium 155 con
  HTTP/3 forzado (Playwright, `h3perf.py`) contra `dl.google.com` (el QUIC
  de Google) y contra un Caddy del VPS (quic-go), al que Chrome le sube.
  Mbit/s; entre paréntesis el p99 de una consulta chica con carga.

  | Brazo | aioquic bajada / subida | Google → Chrome | quic-go → Chrome | Chrome → quic-go |
  | --- | --- | --- | --- | --- |
  | bonding, retenido | 3,8 y 5,0 / 1,8 y 1,7 | 38 (2677 ms) y 77 (386) | 14,0 y 13,7 | 23,0 y 10,5 |
  | bonding, aprendido | 2,2 y 2,1 / 1,0 y 1,1 | 75 (789) y 86 (187) | 3,4 y 2,5 | 12,9 y 11,8 |
  | redundancia, retenido | 8,4 y 10,4 / 15,9 y 19,1 | 95 (318) y 108 (257) | 9,2 y 2,9 | 15,5 y 17,3 |
  | redundancia, aprendido | 5,3 y 6,8 / 23,1 y 8,2 | 107 (203) y 98 (296) | 2,2 y 2,2 | 19,0 y 19,3 |

  - **Manda la pila del que envía.** La de Google (la de Chrome) adapta sus
    umbrales de pérdida a lo que ve: sin retener empata o gana, también en
    bonding. aioquic y quic-go usan los de la RFC 9002 fijos (3 paquetes,
    9/8 del RTT): con ~34 ms de RTT, unos 4 ms de variación del celular ya
    son «pérdida»; el 98–100 % de lo que aioquic dio por perdido llegó
    después (el túnel no perdió nada: `lost` = `late`), y sin retener caen a
    la cuarta parte o menos.
  - **El reparto por paquete le pega a QUIC:** Google bajó ~100 Mbit/s en
    redundancia y 38–86 en bonding, donde TCP va por GRIT (más de 200 con
    TCP contra Cloudflare). QUIC no entra en GRIT (UDP sigue siendo UDP).
  - **Un defecto del guarda:** se miraba `CG_F_SPLIT` por paquete, y los
    pocos paquetes sin repartir de un flujo repartido (los chicos, un
    momento quieto) pasaban adelante del resto retenido. Ahora el reparto es
    el modo del que envía: nada pasa sin retener hasta `CG_FL_QUIET_MS`
    (2 s) después del último paquete repartido (`cg_fl_split`,
    `cg_fl_unsplit`).
  - **El preset de redundancia no lo enciende:** se probó (el usuario:
    «deduplicado pero desordenado») y se revirtió. Retenido, Chrome con
    Google pierde poco (la bajada empata, la subida 16 contra 19); sin
    retener, un servidor con una pila ingenua cae a la cuarta parte.
    `flow_learn` queda para quien sabe que su QUIC va a Google, Meta y
    parecidos.

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

## El carril fiable con zstd en terreno (2026-11-02)

Testnet (4 módems, bonding, p18 con zstd en los dos extremos;
`~/campo/2026-11-02/lane`): una lectura JSON de sensor por datagrama
(`udpgen -j`, 500 por segundo, 30 s, ~135 bytes) desde detrás del NUC a un
receptor en el VPS, como UDP por el túnel o por el carril (TPROXY a mano en
el NUC, como lo escribe `cengarde-setup`); luego con el VPS descartando al
azar los paquetes del túnel en los dos sentidos (iptables `statistic`).

| Brazo | Entregados | Desordenados | GRIT reenvió |
| --- | --- | --- | --- |
| UDP, la red tal cual | 14 999 / 14 999 | 2 | — |
| carril, la red tal cual | 14 999 / 14 999 | 0 | 0 |
| UDP, 5 % descartado | 14 999 / 14 999 | 551 | — |
| carril, 5 % descartado | 14 999 / 14 999 | 0 | 0 |
| UDP, 40 % descartado | 14 591 (−2,7 %) | 3355 | — |
| carril, 40 % descartado | 14 999 / 14 999 | 0 | 400 (206 por RTO, 194 por NACK) |

- **zstd en terreno igual que en el laboratorio:** 2,03 MB → 0,40 MB (5,0×),
  ~27 bytes por lectura.
- **Los datagramas chicos van copiados:** bajo `split_small` (256 bytes) el
  reparto del bonding manda un paquete por todos los enlaces (era para los
  ACK y los saludos de TCP). Las lecturas, crudas o comprimidas, caben, así
  que con 5 % descartado nadie perdió nada (0,05⁴) y GRIT no tuvo que
  reenviar; con 40 %, el UDP perdió lo que perdieron las cuatro copias
  (0,4⁴ ≈ 2,6 %) y el carril lo reenvió todo, en orden (el VPS recibió
  14 234 marcos fuera de orden y los ordenó). Copiado ×4, el carril con zstd
  manda ~1,6 MB, menos que una sola copia del JSON crudo; con telemetría muy
  pesada, bajar `split_small` deja el trabajo a los reenvíos de GRIT.

## Los envíos por equipo (`protocontroller_fair`, protocolo 19, 2026-11-02)

- **El caso** (el usuario): un stream de Facebook que resolvía a Miami
  acalambraba el enlace y a todos los demás. GRIT ya repartía un segmento
  por flujo en cada vuelta y tiene un tope de memoria por flujo, pero un
  equipo con veinte conexiones se llevaba veinte turnos.
- **Ahora:** cada vuelta da un segmento por equipo, y los flujos de cada
  equipo se turnan (`fair.h`, `cg_fair_group`). El equipo es el dispositivo
  de la LAN que abrió el flujo: en el router, la dirección de `accept4` (u
  el origen de un flujo de datagramas); en el VPS, la que el router manda
  en el OPEN (4 bytes, protocolo 19) junto con la sesión del router. La
  perilla `protocontroller_fair` (sí por omisión) vuelve al reparto por
  flujo.
- **Laboratorio** (`BOND_HOG=8`: un segundo equipo, 192.168.79.3, con 8
  conexiones al lado de la medida, de 1; cubic, n = 2;
  `~/campo/2026-11-02/fair`, `out19`): la parte del equipo de una conexión.

  | Caso | Subida, por flujo → por equipo | Bajada, por flujo → por equipo | Total |
  | --- | --- | --- | --- |
  | `equal` | 18 % → 40 % | 16 % → 41 % | igual (88–92 Mbit/s) |
  | `het` | 18 % → 31 % | 13 % → 31 % | igual (84–86) |
  | `cell4` | 16 % → 16 % | 27 % → 21 % | igual (46) |

  No llega al 50 %: el reparto es por segmentos en cada vuelta mientras los
  ritmos lo dejan, y el TCP de cada equipo en la LAN también limita. En
  `cell4` los enlaces lentos mandan.
- **Lo que falta:** la cola justa por flujo en la TUN (el UDP que no entra
  a GRIT) y las clases encima (el carril fiable y lo `deadline` antes que
  lo masivo).

## La clase `reorder`: orden sin rescate (2026-11-02)

- **Para qué:** un flujo con ARQ propio que igual quiere llegar en orden
  (QUIC que pasa en bonding, una VPN adentro). En terreno el orden subió
  aioquic y quic-go 2–4×, y el rescate del túnel solo repite lo que su
  emisor reenvía.
- **Cómo:** `flow_class = … reorder`. Se retiene en orden como cualquier
  paquete, pero el hueco que tiene delante nunca se pide: ni por RACK ni en
  su plazo (`noask`, bit `CG_RO_NOASK` del enlace de su ranura; el hueco es
  casi siempre de su mismo flujo). Con RACK el hueco se suelta en el acto;
  si no, en su plazo.
- **Laboratorio** (QUIC de aioquic, bonding, n = 2 × 4 casos × 2 sentidos;
  `~/campo/2026-11-02/reorderclass`), en Mbit/s:

  | Clase | `equal` | `het` | Pedidos de rescate (toda la tanda) |
  | --- | --- | --- | --- |
  | `deadline` (lo de hoy) | 22,7 | 26,2 | 9 240 (26 rescatados) |
  | `reorder` | 23,6 | 27,9 | 4 303 (31) |
  | `direct` | 10,3 | 12,1 | 1 475 |

  `lossy` y `cell4` quedan en 1–2 Mbit/s con las tres clases. `reorder`
  rinde igual o un poco mejor que `deadline` (dentro del ruido), con la
  mitad de los pedidos; los que quedan son del resto del tráfico (pings, el
  flujo chico). Para QUIC el rescate casi no sirve: 26 de 9 240 pedidos.

## QUIC por TCP (`quic`, 2026-11-02)

- **La pregunta** (el usuario): ¿se puede hacer proxy de QUIC como el PEP de
  TCP? Partirlo no: su transporte (números de paquete, ACK, control de flujo)
  va cifrado, y terminarlo es terminar TLS 1.3 haciéndose pasar por el sitio
  (una CA propia en cada equipo: se rompe con certificados fijados y ECH).
  Las propuestas sin descifrar (Sidecar y sus quACK, las extensiones de QUIC
  para satélite de la IETF) piden que el servidor coopere; MASQUE solo lo
  mete en otro túnel.
- **Lo que sí:** todo navegador y casi toda app que habla QUIC vuelve a TCP
  si el UDP 443 no responde, y ese TCP lo toma GRIT. Es lo que hacen los
  operadores satelitales con su PEP. Rechazado con «puerto inalcanzable»
  (no descartado) la vuelta es inmediata: en terreno, el HTTP/3 forzado
  falló en 0,03 s y Chrome en `auto` usó solo h2. No mete UDP en TCP: la
  app elige TCP sola.
- **Perilla** (OpenWrt, no del motor): `quic` = `pass` | `tcp` y
  `quic_allow` (direcciones o prefijos cuyo QUIC pasa igual); con el
  protocontrolador encendido, una cadena `cengarde_quic` en la tabla de fw4
  (gancho `forward`): el UDP 443 de la LAN hacia `cg0` → `reject with icmpx
  type port-unreachable`. Bonding la trae en `tcp`; redundancia, en `pass`.
- **Terreno** (p16, 4 módems, Chromium 155, `h3perf.py`, 2 rondas;
  `~/campo/2026-11-02/quic2tcp`). Bajada de `dl.google.com` (el mismo
  servidor, por QUIC o por TCP); subida por QUIC a un quic-go del VPS y por
  TCP a `speed.cloudflare.com` (servidores distintos: la subida la manda
  Chrome en los dos casos). Mbit/s; p50 y p99 de una consulta chica con
  carga.

  | Brazo | Google `auto` | Google forzado (h3 / h2) | Subida de Chrome |
  | --- | --- | --- | --- |
  | bonding, QUIC | 66 y 91 (p50 65–74 ms) | 79 y 112 (p50 79–100) | 6,0 y 10,6 |
  | bonding, TCP | 96 y 173 (p50 210–329, p99 0,7–3,4 s) | 158 y 172 (p50 187–252) | 93 y 71 |
  | redundancia, QUIC | 121 y 100 (p50 102–106) | 86 y 112 (p50 92–96) | 24,0 y 15,6 |
  | redundancia, TCP | 132 y 89 (p50 247–288, p99 0,9–2,5 s) | 102 y 112 (p50 344–356) | 33 y 36 |

  - **Bonding:** por TCP, la bajada ~1,6× y la subida ~8× (un QUIC por el
    reparto no llena los enlaces; GRIT sí, cada uno a su tasa). Cuesta
    retardo con carga: el p50 de ~70–100 a ~190–330 ms (los búferes de GRIT;
    dentro del presupuesto de ~300 ms, con colas de segundos en el p99).
  - **Redundancia:** la bajada empata (~105) y QUIC tiene menos retardo;
    queda en `pass`.
  - Del mismo día, la variación de corrida a corrida es grande (Google por
    QUIC en bonding: 38–86 en una tanda, 66–112 en otra).
- **Se pierde:** lo que solo habla QUIC (WebTransport, algún juego; las VPN
  sobre QUIC como iCloud Private Relay o WARP tienen su propia vuelta) y el
  0-RTT de QUIC (el saludo de TCP lo absorbe GRIT en el router).

## El UDP dentro de GRIT (protocolo 20, 2026-11-02)

- **El caso** (el usuario: «pronto la TUN quedará para cosas chicas y UDP
  entrará pura a GRIT»): los envíos por equipo (19) solo reparten lo que
  pasa por GRIT; el UDP iba por la TUN, que reparte por paquete sin saber
  de equipos. Línea base (`BOND_HOG=1`, `BOND_HOG_UDP`: un segundo equipo
  manda UDP con iperf3 a un ritmo fijo; `~/campo/2026-11-02/udphog`): en
  `cell4`, 30 Mbit/s de UDP dejan el TCP del otro equipo en 7 Mbit/s y su
  ping p50/p99 en 497/1417 ms (sin UDP: 45,9 Mbit/s, 120/196 ms). La TUN
  manda todo lo que llega y llena las colas de los enlaces chicos.
- **El diseño:** un flujo de datagramas no fiable por (origen, destino),
  como el carril fiable pero sin flujo: el OPEN lleva `CG_PCTLF_DGRAM |
  CG_PCTLF_UNREL`, y cada datagrama va entero en un mensaje DGM (tipo 5,
  6 bytes de encabezado, sin offset ni ACK ni reenvío propio). El emisor lo
  guarda en una cola de 32 por flujo (llena: se bota el más viejo; más de
  300 ms esperando: se bota, el presupuesto de latencia de la historia 029)
  y lo manda con los mismos tokens del ritmo que el TCP y en los mismos
  turnos por equipo (`fair.h`). Hasta que el VPS contesta el OPEN, los DGM
  van por todos los enlaces: en cada enlace el OPEN llega antes. El
  receptor lo escribe al socket apenas llega, en el orden en que llega. Un
  NACK del rescate de un DGM lo reenvía mientras dure su horizonte
  (protección dentro del plazo, la regla «el UDP sigue siendo UDP»). En el
  router, un segundo socket TPROXY en el puerto siguiente al del
  protocontrolador (`protocontroller_udp`); en OpenWrt, `udp_grit`: el UDP
  de la LAN a direcciones no privadas, después de las reglas del carril
  fiable, sin el DNS al router, sin UDP 443 con `quic = tcp` (lo rechaza el
  hook forward) y sin paquetes más grandes que el MTU del túnel (un
  datagrama tiene que caber en un mensaje: esos van por la TUN, que los
  fragmenta).
- **Lo que el ritmo no veía:** la primera versión dejó el total en 68–77
  Mbit/s en `equal` y `het` (por la TUN, 90). La estimación de GRIT es lo
  que miden los ACK; el UDP gastaba tokens sin medirse, la estimación se
  achicaba a la parte del TCP y los dos se hundían juntos. Ahora cada par
  lleva la parte de los bytes enviados que son datagramas (`cc.xs`, EWMA de
  ventanas de 100 ms) y el ritmo divide la estimación por lo que queda (a
  lo más 5 veces). Un error del primer intento: el `now` del servicio se
  toma antes de leer los sockets, y `now - t` de un datagrama recién leído
  daba la vuelta: todos parecían viejos.
- **Laboratorio** (cubic, n = 2, 30 s, nodos local, remoto06 y remoto07;
  `~/campo/2026-11-02/udpgrit`, `table.md`): el TCP del equipo medido al
  lado del UDP del otro, por la TUN, por GRIT sin `xs` y con `xs`
  (`e572172`).

  | Caso | Sentido | UDP | TCP: TUN / GRIT / +xs (Mbit/s) | UDP entregado | Total | Ping p99 (ms) |
  | --- | --- | --- | --- | --- | --- | --- |
  | `equal` | subida | 30 | 61,7 / 44,8 / 63,0 | 29,2 / 23,6 / 28,9 | 91 / 68 / 92 | 203 / 105 / 129 |
  | `equal` | subida | 60 | 42,7 / 39,7 / 51,7 | 47,0 / 31,3 / 40,4 | 90 / 71 / 92 | 262 / 103 / 126 |
  | `equal` | bajada | 60 | 41,1 / 39,2 / 51,4 | 48,9 / 30,9 / 40,6 | 90 / 70 / 92 | 192 / 97 / 290 |
  | `het` | subida | 60 | 44,1 / 42,9 / 51,0 | 45,7 / 34,0 / 41,0 | 90 / 77 / 92 | 122 / 102 / 120 |
  | `het` | bajada | 60 | 43,4 / 42,2 / 50,4 | 46,6 / 33,3 / 40,0 | 90 / 75 / 90 | 122 / 82 / 120 |
  | `cell4` | subida | 30 | 8,5 / 23,6 / 25,7 | 21,1 / 19,0 / 20,5 | 30 / 43 / 46 | 852 / 104 / 229 |
  | `cell4` | subida | 60 | 3,0 / 23,4 / 25,4 | 27,6 / 19,0 / 20,9 | 31 / 42 / 46 | 962 / 104 / 250 |
  | `cell4` | bajada | 30 | 9,5 / 23,9 / 25,9 | 21,5 / 18,8 / 20,3 | 31 / 43 / 46 | 799 / 104 / 184 |
  | `cell4` | bajada | 60 | 3,6 / 23,7 / 25,5 | 27,2 / 18,9 / 20,9 | 31 / 43 / 46 | 977 / 104 / 179 |

  Sin UDP al lado, los tres brazos dan lo mismo (91–92, 83–86 y 46 Mbit/s).
  En `cell4` la TUN además perdía capacidad (30 de 46): el UDP llenaba las
  colas de los enlaces chicos y el TCP retrocedía. Con `xs` el reparto queda
  cerca de la mitad cuando el UDP pide más que eso (51 / 40 en `equal`), y
  el ping p99 en `cell4` vuelve a la línea sin UDP (176–184 ms) en vez de
  0,8–1 s; en `equal` de bajada sube algo (290 contra 192 por la TUN), a
  mirar en terreno.
- **Terreno** (2026-11-02, testnet en bonding, NUC y VPS en `880c635`;
  `~/campo/2026-11-02/udpgritfield`): desde `nucc`, el equipo A
  (192.168.50.240) con TCP por GRIT al VPS (4 flujos, 20 s) y el B (.241,
  una segunda dirección) con UDP a 30 Mbit/s, `udp_grit` apagado (el UDP
  por la TUN, a 10.79.0.1) y encendido (dentro de GRIT, a la dirección
  pública del VPS); un ping de A por la TUN todo el rato. Dos rondas:

  | Sentido | TCP de A: TUN / GRIT | UDP de B entregado (pérdida): TUN / GRIT | Ping p50 / p99: TUN / GRIT (ms) |
  | --- | --- | --- | --- |
  | subida | 52 y 66 / 69 y 64 | 24,8 (17 %) / 29,9 (0 %) y 28,5 (5 %) | 198/424 y 169/289 / 134/217 y 89/162 |
  | bajada | 131 y 146 / 135 y 166 | 30 (0 %) / 30 (0–1 %) | 49/204 y 56/223 / 45/312 y 56/177 |

  En la subida, donde los enlaces están cerca de su tope (~90 Mbit/s), el
  UDP dentro de GRIT llega entero en vez de perder 17 %, el TCP del otro
  equipo queda igual o mejor y el ping baja; en la bajada (~180 Mbit/s)
  30 Mbit/s no llenan nada y los dos dan parecido. 62 mil datagramas por
  sentido, ninguno botado por viejo. Para probarlo a mano: `uci commit` y
  `cengarde-setup apply` ponen la regla pero no regeneran la configuración
  del motor (eso lo hace la recarga del script de inicio: LuCI,
  `reload_config` o `/etc/init.d/cengarde reload`, y el motor se reinicia
  solo porque la perilla lo pide); sin el motor escuchando en el puerto
  siguiente, el TPROXY bota el UDP de la LAN.
- **Apagada por omisión** hasta más terreno: `protocontroller_udp` en el
  motor, `udp_grit` en OpenWrt (LuCI: «UDP dentro de GRIT»).

## DiffServ a través de GRIT (protocolo 21, 2026-11-02)

- **El caso** (el usuario: «podríamos considerar compatibilidad con
  servicios diferenciados»): la TUN lleva el paquete entero y la marca DSCP
  va adentro; GRIT termina el TCP (y desde el 20 el UDP) en el router y el
  VPS abre su propio socket: la marca se perdía en los dos sentidos.
- **Lo hecho:** el router lee el DSCP del flujo (el del SYN de un TCP
  aceptado: `rcv_tos` con `IP_PKTOPTIONS`, o `IPV6_2292PKTOPTIONS` con
  `IPV6_RECVTCLASS` puesto en el socket que escucha; el del primer
  datagrama con `IP_RECVTOS`) y lo manda en el OPEN (un byte más,
  protocolo 21). El VPS marca su socket hacia el destino y el router el
  suyo hacia el equipo (la marca del equipo, en los dos sentidos). Y elige
  el nivel del flujo en los envíos de GRIT (`dscp.h`), según RFC 4594:
  - **primero:** EF, VOICE-ADMIT, CS5, AF4x, CS4, CS3, CS6 y CS7, mientras
    el flujo mande menos de un cuarto del ritmo; una aplicación que marca
    todo EF va como el resto.
  - **el resto:** CS0 y lo demás, como antes (por equipo, `fair.h`).
  - **menor esfuerzo:** LE (RFC 8622) y CS1, solo con lo que deja el resto,
    más un segmento cada 50 ms (para que no lo resetee
    `protocontroller_dead_s` ni se queden sin pasar sus mensajes de
    control), y solo mientras quede libre un cuarto de la ventana en vuelo
    del par.
  Perilla `protocontroller_dscp` (sí); en OpenWrt la misma opción, en LuCI
  «DiffServ (DSCP) a través de GRIT».
- **Por qué no se espera mucho de EF en el retardo:** dentro de GRIT los
  flujos ya se turnan segmento a segmento y la cola grande está en los
  enlaces (la meta de cola del reparto), no en el ritmo. Lo que sí cambia
  es el relleno y que la marca llegue.
- **El primer intento** (un segmento cada 250 ms, sin el cuarto libre):
  en `cell4` de subida el equipo sin marca solo llegaba a 59 %, y en
  terreno la bajada a 70 %. Cuando el TCP sin marca se detenía un momento
  en su ventana, los de menor esfuerzo llenaban la ventana en vuelo del par
  y después él esperaba lugar. Y con 250 ms, una corrida de iperf3 marcada
  CS1 no recibió sus mensajes de control una vez.
- **Laboratorio** (`BOND_HOG=8`, `BOND_HOG_DSCP=8`: el segundo equipo con
  8 conexiones en CS1, el medido con una sin marca; cubic, n = 2–3, 30 s;
  `47d5cf2`, `~/campo/2026-11-02/dscp`, `table2.md`; dos corridas de
  remoto07 descartadas, que salieron raras también sin marcas, y repetidas
  en local): apagada / encendida / encendida con el segundo equipo sin
  marca.

  | Caso | Sentido | Equipo de 1 conexión (Mbit/s) | El de 8 en CS1 | Parte del primero | Total |
  | --- | --- | --- | --- | --- | --- |
  | `equal` | subida | 37,9 / 89,6 / 38,5 | 54,0 / 1,9 / 53,6 | 41 / 98 / 42 % | 92 |
  | `equal` | bajada | 25,0 / 90,4 / 38,1 | 66,8 / 1,7 / 53,9 | 27 / 98 / 41 % | 92 |
  | `het` | subida | 26,2 / 84,1 / 26,1 | 59,5 / 1,8 / 59,7 | 31 / 98 / 30 % | 86 |
  | `het` | bajada | 26,1 / 81,8 / 25,6 | 58,0 / 2,0 / 58,1 | 31 / 98 / 31 % | 84 |
  | `cell4` | subida | 9,4 / 44,4 / 7,6 | 36,8 / 1,8 / 38,5 | 20 / 96 / 16 % | 46 |
  | `cell4` | bajada | 9,6 / 44,4 / 9,5 | 36,5 / 1,7 / 36,7 | 21 / 96 / 20 % | 46 |

  Sin marcas da lo mismo que apagada; el total no cambia. Las 8 conexiones
  en CS1 quedan en ~2 Mbit/s entre todas y ninguna se resetea.
- **Terreno** (2026-11-02, testnet en bonding, NUC y VPS en `47d5cf2`;
  `~/campo/2026-11-02/dscpfield`): desde `nucc`, A (.240) con una
  conexión y B (.241) con 8, sin marca o en CS1, los dos por GRIT al VPS,
  20 s y 12 s de pausa entre corridas (sin la pausa, el servidor de iperf3
  seguía ocupado con lo que GRIT aún entregaba de la anterior). La marca
  llega: en el VPS, el TCP de un `iperf3 --dscp ef` sale con tos 0xb8.

  | Sentido | A sin marcas en B | A con B en CS1 | Parte de A |
  | --- | --- | --- | --- |
  | subida | 47,9 y 49,6 | 68,0 y 108,7 (B: 7,5 y 1,3) | 45 % → 90 y 99 % |
  | bajada | 113,5 y 95,6 | 137,6 y 194,3 (B: 73,5 y 77,3) | 43–48 % → 65 y 72 % |

  En la bajada B se lleva lo que una sola conexión de A no alcanza a usar
  (el total sube a 211–271): eso es menor esfuerzo, no un reparto.
- **Fuera de esto:** copiar el DSCP de adentro al encabezado de afuera del
  túnel (para un SQM con `diffserv4` en el WAN del router, o la red del
  operador); y la cola por clase en la TUN.

## El crédito de ráfaga y el RTMP (2026-11-03)

- **El caso** (terreno, 2026-11-02): el usuario transmitía RTMP a Facebook
  ("solo yo") desde el notebook a 8 Mbit/s. Iba por GRIT (RTMPS a
  57.144.246.149:443, la ingesta de Meta en Santiago, a 1 ms del VPS),
  parejo a ~7,4 Mbit/s, repartido ENTEL 4,5 / MOVISTAR 1,4 / CLARO 0,9 /
  WOM 0,7, colas de 3–7 ms y 0,3 reenvíos por segundo. Cada 2 s un
  keyframe: 16–28 Mbit/s por 200–300 ms. El ritmo de GRIT sigue a lo que
  le piden (~12,4 Mbit/s) y su balde tiene 2 ms de hondo: la cola del
  keyframe esperaba en el router hasta ~150 ms (4 % de lo leído, más de
  64 ms) con los enlaces casi vacíos. El gráfico de la pestaña Túnel se ve
  como un electrocardiograma: un pico cada 2 s muestreado a un ritmo
  parecido.
- **El crédito** (`protocontroller_burst_kb`, 256 por omisión;
  `streamcc.h`): lo que rebalsa el balde lleno mientras el par va limitado
  por la aplicación y sin cola se guarda, hasta 256 KB y 100 ms al ritmo,
  y paga los envíos cuando se acaban los tokens. Una pérdida, una cola o
  los flujos usando el ritmo lo vacían; con otro equipo cargando no se
  carga.
- **Lo que encontró de paso, en el UDP dentro de GRIT:** un flujo de
  datagramas no fiable leía su socket antes de mandar (hasta 64 por
  vuelta), así que la ráfaga de un keyframe desbordaba la cola de 32 antes
  de que saliera nada. Ahora, con la cola llena, el resto espera en el
  búfer del socket.
- **Laboratorio** (remoto06 y remoto07, cubic, n = 2, 30 s;
  `~/campo/2026-11-03/burst`, `table.md`). `BOND_FLV` es un FLV por TCP
  tipo RTMP (x264 CBR a 8 Mbit/s, keyframe cada 2 s, VBV de 1 s, ruido
  quieto sobre el patrón: keyframes de ~300 KB contra ~26 KB de los demás
  cuadros), y `flvrx.py` mide cuán tarde llega cada cuadro sobre el 5.º
  percentil. Va con `BOND_RATE=1M` de fondo, sin crédito y con crédito:

  | Caso | Sentido | Keyframe tarde p50 (ms) | p99 | Cuadro tarde p99 | Ping p99 del resto |
  | --- | --- | --- | --- | --- | --- |
  | `equal` | subida | 102 → 32 | 134 → 74 | 78 → 14 | 51 → 54 |
  | `equal` | bajada | 111 → 78 | 155 → 110 | 88 → 69 | 51 → 54 |
  | `het` | subida | 139 → 75 | 185 → 93 | 125 → 53 | 41 → 49 |
  | `het` | bajada | 146 → 84 | 191 → 101 | 134 → 61 | 42 → 46 |
  | `cell4` | subida | 169 → 89 | 220 → 126 | 156 → 94 | 54 → 69 |
  | `cell4` | bajada | 164 → 103 | 223 → 172 | 157 → 99 | 54 → 63 |

  Con otro equipo con 4 TCP al lado (`BOND_HOG=4`) el crédito no se carga.
  Medianas iguales; el p99 de los keyframes (13 por corrida, casi el
  máximo) se mueve en los dos sentidos, y el otro equipo da lo mismo
  (84, 79 y 38 Mbit/s).
- **Video RTP desde la LAN dentro de GRIT** (`BOND_TSREAL=rtp`,
  `BOND_TSREAL_LAN=1`, `BOND_UDP_GRIT=1`; 6 Mbit/s, keyframe cada
  segundo; RTP perdidos por corrida):

  | Caso | Antes | Con crédito | Y sin leer con la cola llena |
  | --- | --- | --- | --- |
  | `equal` | 396–397 | 43–164 | 0–2 |
  | `het` | 279–429 | 100–142 | 9–48 |
  | `cell4` | 288–344 | 122–200 | 84–156 |

  Con lo último, ningún datagrama se bota por cola llena; lo que queda en
  `het` y `cell4` llega después de los 200 ms del jitter buffer del
  receptor (los enlaces lentos), no lo bota GRIT. El paso siguiente sería
  mandar los datagramas solo por los enlaces que llegan a tiempo.
- **Terreno** (2026-11-03, NUC y VPS en `d58c495`, el mismo RTMP del
  usuario a Facebook, 7,6 Mbit/s): lo leído que esperó en GRIT antes de
  salir, en 15 s.

  | | < 1 ms | 1–16 ms | 16–32 ms | 32–64 ms | > 64 ms |
  | --- | --- | --- | --- | --- | --- |
  | sin crédito (11-02) | 33 % | 16 % | 23 % | 10 % | 3,8 % |
  | con crédito (11-03) | 97 % | 1 % | 1,2 % | 0,8 % | 0,2 % |

  Cero reenvíos de GRIT en la ventana; el crédito en 125 KB al medir
  (gastándose y llenándose en cada keyframe); colas de los enlaces de
  2–9 ms, como antes.
- **Pendiente:** en la pestaña Túnel, una línea promedio de 4–6 s sobre la instantánea para que
  un stream no se vea como electrocardiograma.

## La clase «en vivo» y los datagramas a tiempo (2026-11-03)

- **La clase «en vivo» (`livesni.h`):** el router mira lo primero que lee
  de cada TCP interceptado. Si va al puerto 1935 (RTMP) o es un ClientHello
  de TLS que pide una ingesta conocida (YouTube `rtmp(s).youtube.com`,
  Facebook `live-api(-s).facebook.com`, Instagram, Twitch y Kick por
  `live-video.net`, Restream, Mux), el flujo va en la clase de video de
  `dscp.h` (AF41) aunque el equipo no lo marque: antes que el bulto mientras
  no pase de la mitad del ritmo del marcapasos. Solo el orden de los envíos
  (`flow_ds`); la marca que sigue (el OPEN, los sockets) es la del equipo.
  Estado: `protocontroller.dscp.live`.
- **Laboratorio** (`labq`, remoto06 y remoto07, preset bonding, FLV de
  8 Mbit/s al 1935 con otro equipo de 4 TCP al lado, `BOND_FLV=8M`
  `BOND_HOG=4`, cubic, 4 × 30 s, subida; ms de atraso de los keyframes):

  | Caso | Keyframe p50 | Keyframe p99 | TCP del equipo medido |
  | --- | --- | --- | --- |
  | `equal` | 110 → 63 | 236 → 187 | 34,8 → 37,0 Mbit/s |
  | `het` | 179 → 117 | 311 → 210 | 29,1 → 29,3 |
  | `cell4` | 250 → 136 | 370 → 227 | 10,4 → 12,0 |

- **Los datagramas solo por los enlaces a tiempo: no.** Mandar cada DGM
  solo por los enlaces donde el ETA dice que llega a menos de 100 ms del
  primero dejó igual el bonding (el reparto por ETA ya elige el que llega
  primero) y empeoró la redundancia (`cell4` 5–7 → 50–132 RTP perdidos: las
  copias de los enlaces "tarde" sí llegaban dentro del jitter buffer).
  Fuera, en `docs/desvan/dgm-en-plazo.h`.
- **Los datagramas sin marca en la clase de video: no.** `cell4` igual,
  `het` algo peor (bonding 990–1168 → 1079–1568). Los DGM no se botan por
  viejos ni por cola llena (`dgm_old`, `dgm_full` en 0).
- **Lo que sí se vio: los presets de terreno.** El mismo RTP en `het` pierde
  0–5 por corrida con la configuración por omisión del laboratorio y
  ~500 (redundancia) a ~1000 (bonding) con los presets de terreno; sin
  `link_cc` peor (800–1677), sin `redund_pace_ms` algo mejor (152–796).
  Los presets son de versiones anteriores: se revisan contra el motor de hoy
  (TCP, latencia y datagramas a la vez).
- **A favor, de antes:** el DGM no lleva las cabeceras IP/UDP (el router
  conoce las direcciones del flujo): 28–48 bytes menos por paquete que por la
  TUN, la mitad de la compresión de cabeceras del UDP de la tarea #78.

## Pendiente

- Terreno: `fec_stream` en redundancia.
- QUIC en bonding, cuando pasa (`quic = pass` o `quic_allow`): repartir por
  flujo (todo un flujo QUIC por un enlace, el plan por flujo de la historia
  014) en vez de por paquete, que MPGRIT haría natural.
- Prioridad del carril fiable sobre lo masivo dentro de GRIT (las clases
  por DSCP están, protocolo 21); el DSCP de adentro en el encabezado de
  afuera del túnel.
- Terreno del UDP dentro de GRIT (protocolo 20), y los datagramas sin las
  cabeceras IP y UDP (#78).
- Un analizador de encabezados más amplio y firmas configurables o
  aprendidas para el detector (usuario).
- El ahorro de cabeceras (frames `DATAGRAM` sin IP ni UDP) pasa a la tarea
  de compresión de cabeceras (#78).

## Cambios

- 2026-11-02: creada, con el diseño de los tres pasos.
- 2026-11-02: las cuatro clases de UDP y el ECN, del usuario.
- 2026-11-02: la telemetría fiable como caso de uso, del usuario.
- 2026-11-02: el paso 1 hecho y medido (`fec_stream`, protocolo 17).
- 2026-11-02: el paso 2 (la protección en redundancia), tres rondas.
- 2026-11-02: el paso 1 en terreno.
- 2026-11-02: el paso 3a (`flow_class`), con su laboratorio.
- 2026-11-02: el paso 3b (UCI, LuCI, la copia al VPS, el NAT del router).
- 2026-11-02: los pasos 3c (el carril fiable, protocolo 18), 3d (zstd en él)
  y 3e (`flow_learn`), con su laboratorio.
- 2026-11-02: los envíos por equipo (protocolo 19) y el UDP dentro de GRIT
  (protocolo 20, `protocontroller_udp`), con su laboratorio.
- 2026-11-02: DiffServ a través de GRIT (protocolo 21,
  `protocontroller_dscp`), con su laboratorio.
- 2026-11-03: el crédito de ráfaga (`protocontroller_burst_kb`), el RTMP
  del usuario en terreno, el FLV y el RTP desde la LAN en el laboratorio.
