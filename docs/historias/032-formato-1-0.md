# 032 — El formato 1.0: cifrado y frames

- **Fecha:** 2026-11-03
- **Estado:** pasos 1 (las primitivas) y 2 (protocolo 22: todo paquete sellado en el borde) hechos; faltan los frames (3) y congelar (4)
- **Fuentes:** historia 030 (protocolo 13, «Etapa A2: el encabezado de
  subflujo y los frames»; el 16, el enlace y la `sseq` bajo el MAC), 029
  (MPGRIT), 027 (el túnel nativo, «autenticado, no cifrado»); RFC 8439
  (ChaCha20 y Poly1305); `engine/src/proto.h`, `proto.c`, `pair.h`,
  `subflow.h`, `cookie.h`.

## TL;DR

- **El usuario (2026-11-03):** cifrado sí para la 1.0, junto con los frames,
  y después congelar el formato.
- **Cifrado:** ChaCha20-Poly1305 (RFC 8439), el AEAD de WireGuard y TLS 1.3.
  Sin AES porque la Pi 4 (Cortex-A72 del BCM2711) no tiene las extensiones
  criptográficas de ARMv8, ni los routers MIPS: ChaCha20 en software es más
  rápido que AES en software y no filtra por tiempos.
- **Una transmisión, un nonce:** cada paquete que sale por un enlace lleva la
  secuencia de ese enlace (`sseq`, la del subflujo, ahora en todos los
  paquetes, no solo en DATA y STREAM), y el nonce es (sentido, enlace,
  `sseq` de 64 bits). Las copias por otros enlaces se cifran aparte (cada
  una con su `sseq`): el AEAD estándar, sin construcciones propias. Lo que
  cuesta se mide en la Pi y en el CPE (paso 1).
- **Llaves por sesión y por arranque:** las de cada sentido salen del
  secreto y de la sesión (al azar, la elige el cliente) y, de bajada, de la
  época del servidor (al azar en cada arranque, va en el HELLO y en las
  respuestas a la sonda). Un reinicio de cualquiera de los dos cambia sus
  llaves: los contadores vuelven a cero sin repetir un nonce.
- **Frames:** el cuerpo cifrado es una lista de frames (DATA de la TUN,
  mensajes de GRIT, sondas, NACK, SFACK, códigos…), varios por paquete: los
  acuses y lo chico viajan con los datos.
- **Pasos:** 1, las primitivas y su costo; 2, el encabezado de subflujo con
  AEAD (un frame por paquete); 3, varios frames por paquete; 4, congelar.

## Contexto

Desde la 0.12 el túnel nativo va autenticado pero no cifrado: SipHash-2-4
de 8 bytes sobre el encabezado y el payload, con las llaves de cada sentido
derivadas del secreto (BLAKE2s, `pair.h`). Cualquiera en la red del
operador ve el tráfico de adentro (el DNS, el SNI, el HTTP en claro, los
encabezados de GRIT). Para un producto 1.0 que cruza redes celulares de
terceros eso no alcanza. Y cada cambio de formato obliga a actualizar los
dos extremos a la vez: se hace una vez, con los frames que la historia 030
dejó planeados, y se congela.

## Diseño

### Lo que se protege y de quién

- Un observador en el camino (el operador, una red Wi-Fi intermedia): no ve
  el tráfico de adentro ni los mensajes del túnel; ve los tamaños, los
  tiempos, el enlace y la sesión (los necesita el servidor para encontrar la
  llave antes de descifrar, como WireGuard su índice).
- Un atacante activo: no puede inyectar, modificar ni reenviar paquetes
  (Poly1305 de 16 bytes, el anti-replay de siempre después de verificar).
- Fuera de esto: el secreto robado de un extremo (no hay secreto hacia
  adelante en cada sesión: las llaves salen del secreto compartido; un
  intercambio de llaves efímeras, Noise, queda para después de la 1.0), y
  el análisis de tráfico por tamaños y tiempos.

### El encabezado (en claro, va como AAD)

| Bytes | Campo |
| --- | --- |
| 0 | versión (nibble alto, `CG_PROTO_WIRE`) y clase de paquete (nibble bajo: normal o de saludo) |
| 1 | pista del cliente (`cg_client_hint`): el servidor elige la llave sin probar todas |
| 2 | enlace |
| 3–6 | sesión |
| 7–10 | `sseq`: los 32 bits bajos de la secuencia del enlace (el receptor la extiende a 64 contra lo último visto, como el anti-replay) |
| (saludo) 11–18 | la época del servidor (solo en HELLO y en las respuestas a una sonda) |

Después, el cuerpo cifrado y el tag de 16 bytes. Hoy son 28 bytes de
encabezado (con el MAC); el nuevo son 11 más 16 de tag, y el cuerpo suma el
reloj (4) y el encabezado de cada frame: unos 8 bytes más por paquete con un
frame (0,6 % a 1400), menos con los acuses que hoy viajan solos.

### El nonce y las llaves

- Nonce de 96 bits: sentido (1 bit), enlace (7 bits), tres bytes en cero y
  la `sseq` de 64 bits. Cada paquete que sale por un enlace toma la
  siguiente `sseq` de ese enlace (una copia por otro enlace o un reenvío del
  rescate toman otra): nunca se repite en la vida de una llave.
- Llaves de 32 bytes por sentido:
  - de subida, `BLAKE2s(llave del enlace, "cengarde 1.0 c2s" ‖ sesión)`;
  - de bajada, `BLAKE2s(llave del enlace, "cengarde 1.0 s2c" ‖ sesión ‖
    época del servidor)`.
  Un cliente que arranca elige otra sesión; un servidor que arranca, otra
  época (64 bits al azar). Las secuencias de 64 bits no dan la vuelta.
- El saludo: la sonda del cliente ya va cifrada con la llave de subida (el
  cliente conoce su sesión). El HELLO del servidor y las respuestas a la
  sonda llevan la época en claro (en el AAD), así que el cliente deriva la
  llave de bajada antes de descifrar. La cookie del HELLO (`cookie.h`) sigue
  igual.

### Los frames (el cuerpo cifrado)

- El cuerpo empieza con el reloj del emisor (4 bytes, el `ts` de hoy) y
  sigue con frames: tipo (1 byte) y lo suyo; el largo (varint) solo cuando
  no es el último.
- DATA (un paquete de la TUN): sus flags y su secuencia de clase (la que
  usan el reordenador, el rescate y los códigos). STREAM (un mensaje de
  GRIT): su secuencia de clase y el mensaje tal cual. Los de control (sonda,
  respuesta, NACK, FEC, REPAIR, PARQ, SFLOSS, SFACK): su payload de hoy.
- El anti-replay de cada clase sigue igual, sobre la secuencia de clase
  dentro del cuerpo, y solo después de verificar el tag.

## Plan

1. **Las primitivas:** `chacha20.h` y `poly1305.h` portables, puras, con los
   vectores de la RFC 8439, y el AEAD; su costo por paquete de 1400 bytes en
   el PC, el NUC, la Pi 4 y el CPE (A53), contra la SipHash de hoy
   (`bench/macbench.c`).
2. **El encabezado de subflujo con AEAD:** la `sseq` en todo paquete, las
   llaves por sesión y época, el saludo con la época, un frame por paquete;
   protocolo 22.
3. **Varios frames por paquete:** los acuses de GRIT, los SFACK y lo chico
   con los datos; protocolo 23.
4. **Congelar:** el formato 1.0, el nombre nuevo y el migrador (los dos
   extremos de la misma release).

## Paso 1: las primitivas (2026-11-03)

- `engine/src/aead.c` y `aead.h`: ChaCha20, Poly1305 y el AEAD de la RFC
  8439, en C portable. ChaCha20 hace cuatro bloques a la vez con los
  vectores de GCC donde hay NEON o SSE2 (en ARM, con las rotaciones propias
  de NEON: `vrev32` para 16 y `vsri` para el resto; una transposición 4×4
  en registros para el XOR); MIPS y el resto, el núcleo escalar. Poly1305
  de 44 bits (128 bits por multiplicación) en x86-64 y de 26 en el resto:
  en el A53 el de 44 bits fue más lento (3,5 → 6,0 µs).
- Pruebas (`test_aead`): los vectores de la RFC 8439 (2.3.2, 2.5.2, 2.8.2)
  y 13 casos al azar de largos borde (0, 1, 15, 16, 17, 63, 64, 65, …,
  1424) con AAD de 0 a 28 bytes, generados con la biblioteca `cryptography`
  de Python como referencia independiente; cualquier bit cambiado en el
  cuerpo, el tag o el encabezado se rechaza sin tocar el búfer, y Poly1305
  en pedazos da el mismo tag. Los mismos vectores pasan con aarch64 (NEON,
  qemu), MIPS big-endian de 32 bits (qemu), gcc y clang.
- Costo de sellar un cuerpo de 1413 bytes (`bench/macbench.c`, sección
  1b), contra la SipHash de hoy sobre 1400:

  | Máquina | SipHash (hoy) | Sellar (ChaCha20-Poly1305) | ChaCha20 solo | Poly1305 solo |
  | --- | --- | --- | --- | --- |
  | i7-9700 (remoto06) | 0,51 µs | 1,79 µs | 1,05 µs | 0,60 µs |
  | Cortex-A53 (CPE1) | 2,30 µs | 13,9 µs | 9,3 µs | 3,5 µs |

  En el A53 la versión escalar de partida sellaba en 22,1 µs. Con 13,9 µs
  un núcleo del A53 sella ~100 MB/s (~800 Mbit/s) con una copia; en
  redundancia cada copia se sella aparte. La Pi 4 (A72, NEON de 128 bits)
  se mide cuando esté prendida; el i7 sella ~6 Gbit/s por núcleo.

## Paso 2: cómo se implementa (diseño fino, 2026-11-03)

- **El paquete en memoria no cambia:** el motor sigue armando y leyendo el
  encabezado de 28 bytes de hoy (tipo, flags, pista, enlace, sesión,
  secuencia de clase, reloj, el campo del MAC sin uso y la `sseq`) con el
  payload detrás (`b + CG_HDR_LEN`). El formato de cable solo existe en el
  borde:
  - **sellar** (al enviar, por cada transmisión): encabezado claro de 11
    bytes (versión y clase de paquete, pista, enlace, sesión, `nseq`: los
    32 bits bajos del contador de la sesión), más 8 de época en los de
    saludo; después, cifrado, el encabezado interno de 14 bytes (tipo,
    flags, secuencia, reloj, `sseq`) y el payload; al final, el tag. El
    cable mide 13 bytes más que la memoria (16 de tag, 14 de encabezado
    interno y 11 de claro, contra los 28 de hoy).
  - **abrir** (al recibir): el tag primero (en tiempo constante), después
    se descifran solo los 14 bytes del encabezado interno, y el encabezado
    de 28 se rearma en su lugar; el payload se descifra solo si el paquete
    no es un duplicado (las copias por otros enlaces se descartan como hoy,
    sin pagar el ChaCha20 de su cuerpo).
- **Un contador por sesión y sentido** (64 bits, todos los enlaces): cada
  transmisión, copia o reenvío del rescate toma el siguiente; va en el
  nonce con el sentido. Por sesión y no por enlace, para que recrear el
  estado de un camino en el servidor nunca vuelva un contador a cero. El
  receptor extiende los 32 bits del cable contra lo más alto visto.
- **Las llaves:** el cliente las deriva al arrancar (la de subida) y al
  conocer la época (la de bajada); el servidor, por sesión al aceptarla
  (hoy usa la llave fija de cada cliente). La admisión de una sesión nueva
  prueba la llave de subida de cada cliente de la pista (BLAKE2s y un
  tag por candidato, como hoy un MAC).
- **Lo que toca:** `proto.h` y `proto.c` (sellar, abrir, el contador, las
  llaves); el envío del cliente y del servidor (cada copia sellada en su
  búfer, en vez de compartir el payload por `iovec`); el anillo del rescate
  (guarda el paquete en memoria y se sella al reenviar); la recepción del
  cliente (`clientpath.h`) y del servidor (la admisión, la sesión); los
  desplazamientos que se leen antes de abrir (pista, sesión: el programa
  BPF de `lanes`); el MTU (13 bytes más por paquete).

## Paso 2: hecho (protocolo 22, 2026-11-03)

- `engine/src/wire.h` y `wire.c`: sellar (`cg_wire_seal`) y abrir en dos
  pasos (`cg_wire_open_head`: el tag y la cabecera interna;
  `cg_wire_open_body`: el cuerpo, y el paquete en memoria en el mismo
  búfer). `proto.c` queda con el paquete en memoria (`cg_hdr_put`,
  `cg_hdr_plen_ok`); la SipHash del protocolo 21 salió del motor (queda en
  `bench/macbench.c` para comparar).
- **Lo que cambió del diseño fino:**
  - La clase 1 (sondas, sus respuestas y HELLO) lleva el contador
    **entero** (la mitad alta, 4 bytes) además de la época: un servidor que
    arma una sesión desde una sonda no sabe por dónde va el contador de un
    cliente que lleva días andando.
  - **Época del emisor, en los dos sentidos:** también el cliente pone la
    suya en las sondas (al azar al arrancar), y la llave de subida sale de
    ella. Así, dos arranques del cliente que eligieran la misma sesión (1 en
    2^32) no repiten nonces.
  - **Época del servidor por estado de sesión y por HELLO**, no por
    arranque: una sesión que vence y se rearma en el mismo proceso empieza
    su contador de 0 con otra llave. El HELLO no tiene estado de sesión:
    lleva una época propia, con el contador en 0, y el cliente lo abre con
    ella sin cambiar la llave de su bajada.
  - Una llave nueva se adopta solo con un paquete que pasó el anti-replay
    (`cg_unseal_accept`): una respuesta vieja repetida se abre con su época
    vieja, pero su secuencia es vieja y no recupera la llave (prueba
    `rx_epoch` de `test_client_rx.c`).
  - Una copia con el tag roto ahora cuenta como falsa (`auth_fail`), no
    como duplicado: el tag se revisa antes del anti-replay. Una copia buena
    repetida paga el tag y la cabecera interna, no el cuerpo.
- **Lo que tocó:** el envío del cliente (`wsend` para lo que sale solo,
  `send_links` sella cada copia en su búfer) y del servidor (`send_path`,
  `down_send`, las respuestas y el HELLO); la recepción de los dos
  (`cg_rx_entry`, `listen_read`, `lookup`: la admisión prueba la llave de
  subida de cada cliente de la pista con la época de la sonda); el programa
  BPF de `lanes` (el enlace va en el byte 2 del cable, en claro); el MTU
  (`cg_outer_len`: 1431 sobre IPv4 y 1411 sobre IPv6 en un camino de 1500;
  `tun_mtu` máximo 1995; LuCI); `cgprobe` y `macbench`; `techomat` abre
  el túnel con `--key` (la fase `grit` leía los registros en claro).
- **Costo** (`bench/macbench.c`, sección 1c, i7-9700): sellar un DATA de
  1400 B 1,9 µs; abrirlo entero 2,1 µs; una copia que llega segunda (tag y
  cabecera interna) 0,87 µs, contra 0,51 µs de la SipHash del 21; sellar una
  sonda 0,43 µs.
- **Laboratorio** (`labq`, remoto06, 3 repeticiones de 30 s, cubic, el
  21 contra el 22, `ab-fmt1`): caudal 0,98–0,99× en `equal`, `het`,
  `cell4` y `clean4`, en los dos sentidos (lo que pesan los 13 bytes, 0,9 %);
  `lossy` 0,95× de subida y 0,98× de bajada, dentro del ruido (rangos
  solapados); RTT p50 +0 a +1 ms; las retransmisiones iguales; CPU de cada
  extremo 1,14–1,47× (de 5–7 % a 6–10 % en `equal` y `het`, de 15–17 % a
  18–20 % en `cell4`).
- Pruebas: `test_wire` (ida y vuelta en los bordes de bloque, igual al AEAD
  de la RFC sobre la cabecera interna y el cuerpo, cualquier bit cambiado,
  el contador equivocado o el sentido equivocado se rechazan, la extensión
  del contador a los dos lados de 2^32, las épocas), `test_client_rx`
  (el orden de `cg_rx_entry` con paquetes sellados y el cambio de época),
  `test_steer` (el byte del enlace en el cable).

## Paso 3: lo que valen los frames (medido, 2026-11-03)

- Testnet en el 22 (`c8d74d9`), una bajada de 449 Mbit/s (iperf3 -R, 4
  TCP, 10 s, desde `nucc`): el router mandó 23 100 acuses de GRIT (uno cada
  18 segmentos de bajada, ~2 300 por segundo, 84 bytes), cada uno copiado a
  los 4 módems (23 202 paquetes por enlace): ~1,5 Mbit/s de subida en total
  (0,35 % de la bajada), ~2 300 pps por módem. Sin datos de subida no hay
  con qué juntarlos: los frames ganarían poco con este tráfico.
- **Decisión:** el paso 3 queda para después de la 1.0 (lo que más
  ahorraría es otra cosa: no copiar cada acuse a todos los enlaces, o
  espaciarlos, en GRIT). El formato 1.0 queda en el protocolo 22.

## Terreno con el 22: techo y OMR (2026-11-03, tarde)

- **La prueba de techo** (`bench/techo.sh`, NUC y VPS en `c8d74d9`/`485079c`,
  30 s por fase; `~/campo/2026-11-03/techo/final-p22*`): un TCP BBR por
  módem directo al VPS, los cuatro a la vez, y después un TCP por GRIT sobre
  los cuatro.

  | | ENTEL | WOM | MOVISTAR | CLARO | Suma | GRIT | GRIT / suma |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | Subida | 15,0 | 25,0 | 22,9 | 29,8 | 92,7 | 130,5 | 141 % |
  | Bajada | 122,9 | 99,3 | 113,4 | 187,0 | 522,6 | 405,6 | 78 % |

  (La bajada directa se repitió 2 minutos después: en la primera ENTEL dio
  0,0 y MOVISTAR 46,6.) El 2026-10-28 GRIT daba 90 % de subida y 62–67 % de
  bajada de ese techo; un BBR por módem no llena la subida de un módem
  celular, GRIT sí.
- **Contra OMR** (v0.63, MPTCP con BBR, los mismos 4 módems; Ookla 51218,
  dos rondas alternadas; `~/campo/2026-11-03/vsomr2`):

  | Ronda | Bajada cengarde / OMR | Subida | Ping cargado bajada | Ping cargado subida |
  | --- | --- | --- | --- | --- |
  | 1 | 541 / 427 | 135 / 88 | 107 / 730 ms | 75 / 74 ms |
  | 2 | 511 / 462 | 115 / 34 | 91 / 255 ms | 76 / 49 ms |

  Con el cifrado, cengarde saca 11–27 % más de bajada, 1,5–3,4 veces la
  subida y un ping cargado de bajada 3 a 7 veces menor.

## Pendiente

- El paso 2 en el terreno, y el CPU de la Pi y del VPS a plena carga; medir sellar y abrir en la Pi 4 y en el
  CPE (A53).
- Paso 3: los frames; paso 4: congelar.
- Después de la 1.0: secreto hacia adelante (un intercambio Noise al iniciar
  la sesión), NEON y AVX2 para ChaCha20 si el costo lo pide.

## Cambios

- 2026-11-03: creada, con el diseño y el plan.
- 2026-11-03: el paso 1, las primitivas, sus pruebas y su costo.
- 2026-11-03: el paso 2 hecho (protocolo 22), con lo que cambió del diseño fino.
