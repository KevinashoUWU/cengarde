---
tags: [mapa, pieza]
archivos:
  - engine/src/tunroute.h
  - engine/src/tun.h
  - engine/src/ippkt.h
  - engine/tests/test_tun.c
sello: 132d7c553651acf6
---
# Rutas del túnel nativo (tunroute.h)

**Grupo:** Recibir · Enviar — [[Mapa del motor]]

## Qué hace
Con `tunnel = native`, el servidor tiene una TUN para todos los clientes. Un paquete que el kernel mete en ella va al cliente cuyos prefijos contienen su destino, por prefijo más largo, como los AllowedIPs de WireGuard. En el otro sentido comprueba el origen: un paquete de un cliente entra a la TUN solo si su origen es de ese cliente. `ippkt.h` comprueba que un DATA tenga forma de paquete IP antes de la TUN.

## Lee
- El destino (y el origen) de cada paquete IP; los `tun_route` de cada cliente.

## Decide / escribe
- A qué sesión va un paquete de la TUN (`tun_read`) y si un paquete de un cliente entra (`tun_write`).

## Perillas
- [[Mapa/Perillas/tunnel|tunnel]], [[Mapa/Perillas/tun|tun]], [[Mapa/Perillas/tun_addr|tun_addr]], [[Mapa/Perillas/tun_mtu|tun_mtu]], [[Mapa/Perillas/tun_txqueuelen|tun_txqueuelen]], [[Mapa/Perillas/tun_route|tun_route]].

## Dónde se midió
- [[014-tunel-nativo]]: fase 1 (PR #61): 73,5 Mbit/s por sentido con 2 enlaces de 40 Mbit/s; varios routers por VPS con su `tun_route`.

## Choques conocidos
- El túnel nativo está autenticado pero no cifrado (experimental).

## Archivos
- `tunroute.h`, `tun.h`, `tun.c`, `ippkt.h`; tests `engine/tests/test_tun.c`. `server.c`: `tun_read`, `tun_write`.
