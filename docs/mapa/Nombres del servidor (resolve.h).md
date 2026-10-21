---
tags: [mapa, pieza]
archivos:
  - engine/src/resolve.h
  - engine/src/dns.h
  - engine/src/names.c
  - engine/tests/test_resolve.c
  - engine/tests/test_dns.c
sello: a6ec9a8aafc22af0
---
# Nombres del servidor (resolve.h)

**Grupo:** Configurar · Enviar — [[Mapa del motor]]

## Qué hace
El cliente resuelve él mismo los nombres de `server`, por los enlaces, nunca con el resolvedor del sistema: sus consultas irían por el túnel, caído justo cuando cambió la dirección, y `getaddrinfo` bloquea. Cada búsqueda sale por un enlace, atada a su dirección, y pregunta primero a los DNS de ese enlace (los de la operadora) y después a los globales. A y AAAA son dos preguntas aparte; reintenta según el TTL y en cuanto el servidor deja de responder.

## Lee
- Las direcciones de cada enlace, sus resolvedores y `/etc/hosts` (una vez).

## Decide / escribe
- Las direcciones de cada nombre → [[Elección de servidor (srvpick.h)]] (`cg_config_build`).

## Perillas
- [[Mapa/Perillas/server|server]], [[Mapa/Perillas/resolver|resolver]] (global y por `[link]`).

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: PR 5, nombres con resolvedor propio por los enlaces.

## Choques conocidos
- Nunca en el plano de datos: como mucho dos búsquedas por nombre en vuelo (`names.h`).

## Archivos
- `resolve.h`, `dns.h` (el códec, entrada no confiable), `names.c` (los sockets).
- Tests: `engine/tests/test_resolve.c`, `test_dns.c`.
- `client.c`: `names_tick`, `names_changed`.
