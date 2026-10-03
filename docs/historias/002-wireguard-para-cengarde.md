# 002 — WireGuard: lo que cengarde necesita saber

- **Fecha:** 2026-10-03
- **Estado:** vigente
- **Fuentes:** wireguard-go `golang.zx2c4.com/wireguard@v0.0.0-20260522210424-ecfc5a8d5446`
  (verificado en `device/noise-protocol.go`, `device/constants.go`,
  `device/send.go`, `device/receive.go`, `device/cookie.go` y
  `replay/replay.go`). Lo del kernel Linux está marcado como no verificado.

## TL;DR

- engarde ve datagramas WireGuard opacos, pero su cabecera va en claro:
  - tipo (1 byte + 3 a cero);
  - índice receptor (offset 4);
  - contador (offset 8, u64 little-endian);
  - en los de datos, el tag Poly1305 son los últimos 16 B.
- Clave de deduplicación segura: (índice receptor, contador, tag), es decir,
  solo duplicados exactos.
- WireGuard descifra antes de comprobar el anti-replay, así que hoy descifra
  las N copias de cada paquete.
- Sesiones: la iniciación lleva el índice del cliente, la respuesta lleva los
  dos, y cada mensaje de datos lleva el índice de quien lo recibe.
- Tamaños fijos (148 / 92 / 64 / ≥32 B): validar el formato cuesta muy poco.
- Mensaje de datos = paquete interior rellenado a múltiplo de 16 (sin pasar
  del MTU) + 32 B.

## Hallazgos

### Formato de los mensajes (verificado)

| Mensaje | Tipo | Tamaño | Campos (offset en bytes) |
| --- | ---: | ---: | --- |
| Iniciación | 1 | 148 | sender (4), efímera (8, 32 B), estática cifrada (40, 48 B), timestamp cifrado (88, 28 B), mac1 (116), mac2 (132) |
| Respuesta | 2 | 92 | sender (4), receiver (8), efímera (12, 32 B), vacío cifrado (44, 16 B), mac1 (60), mac2 (76) |
| Cookie reply | 3 | 64 | receiver (4), nonce (8, 24 B), cookie cifrada (32, 32 B) |
| Datos | 4 | 16 + n + 16 | receiver (4), contador (8, u64 LE), contenido cifrado (16), tag (últimos 16 B) |

El tipo se escribe como un u32 little-endian: el primer byte vale 1–4 y los
tres siguientes son 0. Un keepalive es un mensaje de datos sin carga: 32 B
(`MessageKeepaliveSize`).

### Constantes (verificado, `device/constants.go`)

| Constante | Valor | Constante | Valor |
| --- | --- | --- | --- |
| RekeyAfterTime | 120 s | RejectAfterTime | 180 s |
| RekeyAttemptTime | 90 s | RekeyTimeout | 5 s |
| KeepaliveTimeout | 10 s | CookieRefreshTime | 120 s |
| HandshakeInitationRate | 1/50 s | PaddingMultiple | 16 |
| RekeyAfterMessages | 2^60 | RejectAfterMessages | 2^64 − 2^13 − 1 |

La ventana anti-replay (`replay/replay.go`) es de (128 − 1) × 64 = 8.128
contadores.

### Comportamientos que importan (verificado)

- **Descifrar antes del anti-replay:** `receive.go:255` (`Open`) va antes que
  `receive.go:453` (`ValidateCounter`), así que cada duplicado se descifra y
  autentica entero antes de descartarse.
- **Roaming:** el endpoint del peer pasa a ser el origen del último paquete
  autenticado y no repetido (`receive.go:459`). Detrás de engarde, ese origen
  es el socket local de engarde, que es estable.
- **Relleno:** `calculatePaddingSize` (`send.go:419-431`) redondea a múltiplo
  de 16 sin pasar del MTU. Un mensaje de datos ocupa como mucho MTU + 32, así
  que con el buffer de 1500 B de engarde Go se trunca si el MTU de WireGuard
  pasa de 1468.
- **Contador:** u64 little-endian (`send.go:457`).
- **mac1:** BLAKE2s-128 con clave = BLAKE2s-256(`"mac1----"` ‖ clave pública
  del que responde) (`cookie.go:50-54`, `noise-protocol.go:52`). Un
  engarde-server que conozca la clave pública del WireGuard del servidor (un
  dato público) puede verificar el `mac1` de las iniciaciones sin ningún
  secreto.

### Sin verificar (recuerdo; comprobar antes de depender de ello)

- WireGuard del kernel Linux: ventana anti-replay de ~8.128 en 64 bits
  (`COUNTER_BITS_TOTAL` 8192) y también descifra antes de validar el contador.
- El kernel solo copia ECN, no DSCP, al paquete exterior. Si es así, engarde
  no puede priorizar por DSCP, solo por tamaño.
- El kernel recibe con `encap_rcv` sobre su socket UDP, sin buffer de socket,
  y encola el descifrado en workers por CPU.

## Qué hacemos con esto

- **Validación barata** de tipo y tamaño para descartar basura y para admitir
  caminos en el servidor, más `mac1` opcional si se configura la clave
  pública.
- **Deduplicación:** anillo indexado por (índice receptor, contador); en cada
  hueco se guardan 8 B del tag. Mismo contador con distinto tag = paquete
  distinto (posible falsificación): se reenvía y que WireGuard decida.
- **Sesiones en el servidor, sin cambiar el protocolo:**
  - la iniciación lleva el índice del cliente;
  - la respuesta lleva el índice del servidor y el del cliente;
  - los datos de subida llevan el índice del servidor y los de bajada el del
    cliente.

  Hay rekey cada ≤120 s, y los índices viejos se mantienen hasta
  RejectAfterTime (180 s).
- **Reordenación** (bonding, Fase 5): el contador es secuencial por par de
  claves, así que se puede reordenar por (índice receptor, contador) sin
  cabecera propia.
- **MTU:** buffers de 64 KiB, y avisar cuando el MTU de camino de un enlace
  sea menor que el MTU de WireGuard + 32 + IP/UDP.

## Pendiente

- Verificar los tres puntos del kernel antes de diseñar en función de ellos.

## Cambios

- 2026-10-03: creada; datos de wireguard-go verificados en el código.
