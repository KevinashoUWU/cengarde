---
tags: [mapa, pieza]
archivos:
  - engine/src/srvpick.h
  - engine/tests/test_srvpick.c
sello: 4f5e17e5b40a5290
---
# Elección de servidor (srvpick.h)

**Grupo:** Enviar — [[Mapa del motor]]

## Qué hace
Decide a qué dirección del servidor manda cada enlace. `server` es una lista ordenada (la preferencia del operador). Cada enlace usa una sola a la vez, de una familia de la que tenga dirección propia: nunca IPv4 e IPv6 por el mismo módem. Empieza por la primera; sin respuesta verificada durante `failover_ms`, pasa a la siguiente. Se queda donde le responden. Vuelve a la primera cuando cambia su dirección local, gana o pierde una familia, una recarga cambia su lista, o tras una vuelta entera sin respuestas.

## Lee
- La lista resuelta (direcciones y nombres de [[Nombres del servidor (resolve.h)]]), las direcciones del enlace (netlink, [[Dirección de origen (addrpick.h)]]) y la última respuesta verificada.

## Decide / escribe
- La dirección de cada socket de enlace (`reconcile`, `link_open`) → todo el envío de ese enlace.

## Perillas
- [[Mapa/Perillas/server|server]]: hasta 8 entradas, global o por `[link]`.
- [[Mapa/Perillas/server_failover_ms|server_failover_ms]]: el mayor de 10000 y 3 × `probe_idle_ms`; 0 nunca.

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: varias direcciones del VPS, IPv6 por fuera, una familia por enlace.
- [[004-entorno-real]]: por qué no IPv4 e IPv6 a la vez por la misma radio.

## Choques conocidos
- 10 s de failover no confunden un reinicio del servidor con una caída (lo dice `srvpick.h`).
- No reintenta periódicamente una dirección preferida que no respondió: cada intento costaría `failover_ms` de pérdida.

## Archivos
- `srvpick.h`; tests `engine/tests/test_srvpick.c`.
- `client.c`: `tick` (`cg_srv_due`), `reconcile`, `link_open`.
