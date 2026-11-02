# 031 — Clases por flujo (U5): la FEC y la protección para GRIT, y el proxy UDP

- **Fecha:** 2026-11-02
- **Estado:** en curso (diseño; el primer paso, la FEC v3 sobre STREAM, en `claude/u5-fec-stream`)
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

- Implementar y medir el paso 1.
- El paso 2 y el 3.

## Cambios

- 2026-11-02: creada, con el diseño de los tres pasos.
- 2026-11-02: las cuatro clases de UDP y el ECN, del usuario.
- 2026-11-02: la telemetría fiable como caso de uso, del usuario.
