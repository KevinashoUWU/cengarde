---
tags: [mapa, pieza]
archivos:
  - engine/src/wire.h
  - engine/src/wire.c
  - engine/tests/test_wire.c
sello: fedf6cdf4cadd302
---
# Formato de cable (wire.h)

**Grupo:** Recibir · Enviar — [[Mapa del motor]]

## Qué hace
El formato 1.0 (protocolo 22, [[032-formato-1-0]]): cómo va cada datagrama por la red. El motor arma y lee el paquete en memoria de siempre (cabecera de 28 bytes de `proto.h`, el payload detrás) y solo en el borde lo sella o lo abre:
- **en claro** (el AAD): versión y clase de paquete (`kind`), pista del cliente, enlace, sesión y `nseq` (los 32 bits bajos del contador), 11 bytes; la clase 1 (sondas, respuestas y HELLO) suma la mitad alta del contador y la **época** del emisor, 12 más;
- **cifrado** con ChaCha20 ([[Utilidades]], `aead.h`): la cabecera interna de 14 bytes (tipo, flags, secuencia de clase, reloj, `sseq`) y el payload;
- el **tag** de Poly1305 al final, 16 bytes.

Un contador de 64 bits por emisor, sesión y sentido (todos los enlaces; cada copia y cada reenvío toman el siguiente) va en el nonce. La llave sale de BLAKE2s del secreto con el sentido, la sesión y la época del emisor, que es nueva cada vez que un contador arranca de 0: el cliente al arrancar, el servidor por cada estado de sesión que crea y por cada HELLO. Así nunca corre dos veces un contador con la misma llave, se reinicie o expire lo que sea. El receptor extiende el `nseq` contra lo más alto que abrió con esa llave (`cg_wire_ctr`).

Abrir va en dos pasos: `cg_wire_open_head` revisa el tag y descifra solo la cabecera interna (lo que necesita el anti-replay), y `cg_wire_open_body` descifra el payload y deja en el mismo búfer el paquete en memoria, solo si el anti-replay lo toma. Una copia que llega segunda paga el tag, no el cuerpo.

## Lee
- El paquete en memoria y el estado de sellado (`struct cg_seal`: llave, contador, época) al enviar; el datagrama y el estado de apertura (`struct cg_unseal`: llave de la época más nueva y el contador más alto) al recibir.

## Decide / escribe
- El datagrama sellado → `client.c` (`wsend`, `send_links`) y `server.c` (`send_path`, `down_send`, `queue_reply`, `queue_hello`).
- Qué llave abre un paquete (`cg_unseal_key`): la propia o, para una clase 1 con otra época, la de esa época; `cg_unseal_accept` la adopta solo para lo que pasó el anti-replay ([[Camino del router (clientpath.h)]], `listen_read` del servidor). Un HELLO se abre con su época pero nunca cambia la llave de la bajada.
- La admisión de una sesión nueva en el servidor: la llave de subida de cada cliente de la pista con la época de la sonda ([[Varios clientes (clients.h)]]).

## Perillas
- Ninguna: el cifrado siempre va (13 bytes más por paquete que en el protocolo 21).

## Dónde se midió
- `bench/macbench.c` (sección 1c), i7-9700: sellar un DATA de 1400 B 1,9 µs, abrirlo entero 2,1 µs, una copia segunda (tag y cabecera interna) 0,87 µs, contra 0,51 µs de la SipHash del protocolo 21.

## Choques conocidos
- Lo que se lee antes de abrir (pista, enlace, sesión) va en claro: el programa BPF de [[Colas del servidor (steer.h)]] lee el enlace en el byte 2 del cable.
- El MTU: 1431 sobre IPv4 y 1411 sobre IPv6 en un camino de 1500 (`cg_outer_len`).

## Archivos
- `wire.h`, `wire.c`; tests `engine/tests/test_wire.c` y `test_client_rx.c`.
- `clientpath.h`: `cg_rx_entry`, `cg_up_wire`; `client.c`: `wsend`, `seal_start`, `send_links`; `server.c`: `lookup`, `session_create`, `send_path`, `down_send`, `queue_reply`, `queue_hello`, `listen_read`.
