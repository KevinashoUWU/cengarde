---
tags: [mapa, pieza]
archivos:
  - engine/src/epoch.h
  - engine/tests/test_epoch.c
sello: 043d604108787d94
---
# Eco de sondas (epoch.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
En el cliente, ata lo que el servidor manda de vuelta a las sondas que responde. Cada enlace guarda sus últimas `CG_ECHO_N` sondas (su ts exacto). Un HELLO no lleva secuencia, así que solo cuenta si responde a una sonda de este enlace enviada hace poco y una sola vez; las respuestas también toman su entrada, así una sonda se responde una vez.

## Lee
- Las sondas enviadas por enlace y el `echo_ts` de cada respuesta o HELLO ([[Sondas y cola de bonding (proto.h)]]).

## Decide / escribe
- Si un HELLO o una respuesta vale → [[Camino del router (clientpath.h)]] (paso 4).

## Perillas
- Ninguna.

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: protocolo v4.

## Choques conocidos
- Ninguno registrado.

## Archivos
- `epoch.h`; tests `engine/tests/test_epoch.c`. `client.c`: `on_probe_reply`, `on_hello`, `send_probe`.
