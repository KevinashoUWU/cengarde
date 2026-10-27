---
tags: [mapa, pieza]
archivos:
  - engine/src/cookie.h
  - engine/tests/test_cookie.c
sello: b6eb1118298e6681
---
# Cookies y HELLO (cookie.h)

**Grupo:** Recibir · Configurar — [[Mapa del motor]]

## Qué hace
Un servidor que recibe una sonda que todavía no puede aceptar (sin sesión, camino nuevo o dirección nueva) responde con un HELLO con una **cookie** a la dirección de origen, y solo toma la siguiente sonda de ese enlace si la repite. La cookie es SipHash-2-4 bajo una clave de época de 30 s, sobre cliente, sesión, enlace y dirección; vale 30 a 60 s. No guarda nada por cliente. También limita cuántos HELLO se mandan (el presupuesto).

## Lee
- Las sondas verificadas y su dirección de origen.

## Decide / escribe
- Si se crea una sesión, un camino o se cambia su dirección → `server.c` (`on_probe`, `queue_hello`). El cliente guarda la cookie para sus sondas ([[Camino del router (clientpath.h)]]).

## Perillas
- Ninguna.

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: 3d2a, protocolo v4 (pista, HELLO y cookies).

## Choques conocidos
- Ninguno registrado.

## Archivos
- `cookie.h`; tests `engine/tests/test_cookie.c`. `server.c`: `on_probe`, `queue_hello`.
