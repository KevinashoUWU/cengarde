---
tags: [mapa, pieza]
archivos:
  - engine/src/clients.h
  - engine/src/idmap.h
  - engine/tests/test_clients.c
sello: 69c599882d95980e
---
# Varios clientes (clients.h)

**Grupo:** Recibir · Configurar — [[Mapa del motor]]

## Qué hace
Un servidor con varios routers encuentra, antes de calcular ningún MAC, bajo qué claves puede ir un paquete: por su pista de cliente (byte 2, el primer byte de un hash de la clave). Un paquete prueba solo las claves de la cadena de su pista: un MAC en el caso común, dos o tres en el peor. Las sesiones se buscan por id XOR un valor aleatorio de cada cliente.

## Lee
- La pista (byte 2) y el id de sesión de cada datagrama.

## Decide / escribe
- La sesión y la clave de cada paquete → `server.c` (`client_of`, `lookup`, `session_create`).

## Perillas
- [[Mapa/Perillas/key|key]], [[Mapa/Perillas/enabled|enabled]], [[Mapa/Perillas/tun_route|tun_route]] en `[client NOMBRE]`.
- [[Mapa/Perillas/max_sessions|max_sessions]]: 64. [[Mapa/Perillas/session_timeout_ms|session_timeout_ms]]: 180000. [[Mapa/Perillas/path_timeout_ms|path_timeout_ms]]: 30000.

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: 3d2b, varios routers en el servidor.

## Choques conocidos
- Ninguno registrado.

## Archivos
- `clients.h`, `idmap.h`; tests `engine/tests/test_clients.c`, `test_idmap.c`. `server.c`: `client_of`, `lookup`.
