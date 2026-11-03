# 032 — El formato 1.0: cifrado y frames

- **Fecha:** 2026-11-03
- **Estado:** diseño; paso 1 (las primitivas) hecho; paso 2 en curso
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

## Pendiente

- Todo lo de arriba.
- Después de la 1.0: secreto hacia adelante (un intercambio Noise al iniciar
  la sesión), NEON y AVX2 para ChaCha20 si el costo lo pide.

## Cambios

- 2026-11-03: creada, con el diseño y el plan.
- 2026-11-03: el paso 1, las primitivas, sus pruebas y su costo.
