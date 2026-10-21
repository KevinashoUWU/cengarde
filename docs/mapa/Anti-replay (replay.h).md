---
tags: [mapa, pieza]
archivos:
  - engine/src/replay.h
  - engine/tests/test_replay.c
sello: dcb3794b5f00a244
---
# Anti-replay (replay.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
Es la **deduplicación** de la redundancia y la defensa contra repeticiones: una ventana sobre las secuencias autenticadas, con el mismo diseño que el filtro de WireGuard (un anillo de bloques de 64 bits). El receptor llama a `cg_replay_check` **antes** del MAC (una secuencia ya marcada es duplicado y se tira sin pagar el MAC) y a `cg_replay_mark` **solo después** de verificarlo, así un paquete falso nunca marca una secuencia y tapa la copia buena. Cuenta además `lost`: secuencias que salieron de la ventana sin marcarse, es decir, perdidas por todos los enlaces.

## Lee
- La secuencia de cada DATA y de cada mensaje de control (dos ventanas: DATA y control).

## Decide / escribe
- NEW / DUP / OLD → [[Camino del router (clientpath.h)]] (cliente) y `listen_read` (servidor). Solo lo NEW verificado sigue al [[Reordenador (reorder.h)]].
- `lost` (la pérdida en todos los enlaces) → estado y registrador de campo; ningún veredicto lo lee.

## Perillas
- Ninguna. La ventana es fija: `CG_REPLAY_BITS` 32768.

## Dónde se midió
- [[005-motor-c-v1]]: recepción y deduplicación del motor v1.
- [[013-capacidad-y-dosificacion]]: la ventana de 8192 rechazaba como viejo un paquete 300 ms tarde; el PR #46 la llevó a 32768 bits.
- [[012-bonding]]: `lost` como instrumento pasivo (`lost_all`).

## Choques conocidos
- La ventana del [[Reordenador (reorder.h)]] tiene que quedar debajo de esta, para que un paquete esperado siga pasando `cg_replay_check`.
- Regla del proyecto: marcar solo después de verificar el MAC (CLAUDE.md).
- El comentario de `replay.h` dice que `lost` va atrasado «8129 a 8255 secuencias», de la ventana vieja (ver [[Mapa del motor#Contradicciones]]).

## Archivos
- `replay.h`; tests `engine/tests/test_replay.c`.
- `clientpath.h` (`cg_rx_entry`, pasos 5–7); `server.c`: `listen_read`, `session_create`.
