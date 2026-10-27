---
tags: [mapa, pieza]
archivos:
  - engine/src/rcvbudget.h
  - engine/tests/test_rcvbudget.c
sello: 298dfd0abd71c949
---
# Presupuesto de recepción (rcvbudget.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
Todos los sockets UDP de la máquina sacan su cola de un mismo fondo, `net.ipv4.udp_mem`. Pasado su primer valor, el kernel deja un datagrama por socket; pasado el último, ninguno. Para que los carriles no se lleven por delante otros sockets, los que reciben se reparten la mitad del umbral de presión: cada uno recibe `min(rcvbuf, presupuesto / (2 × sockets))` como `SO_RCVBUF`. `udp_mem` no se toca.

## Lee
- `udp_mem` (o la RAM, en un namespace propio), los carriles de [[Colas del servidor (steer.h)]] y el de basura (la TUN no es un socket UDP; hasta 0.11 contaba también un socket hacia WireGuard por router).

## Decide / escribe
- El `SO_RCVBUF` de cada socket del servidor (`rcvbuf_apply`); `rcvbuf` y `rcvbuf_capped` en el estado.

## Perillas
- [[Mapa/Perillas/rcvbuf|rcvbuf]]: 4 MiB; 0–2^30.

## Dónde se midió
- [[011-hilos]]: un socket leído cada 250 ms recibió 163 de 599 datagramas mientras ocho carriles llenos tenían el fondo pasado su mínimo. En un VPS de 1 GB con un router: 9 sockets, unos 3,6 MiB cada uno (cálculo; 10 hasta 0.11, con el socket hacia WireGuard).

## Choques conocidos
- Subir `udp_mem` en una máquina chica cambia pérdidas UDP por el OOM killer (lo dice `rcvbudget.h`).

## Archivos
- `rcvbudget.h`; tests `engine/tests/test_rcvbudget.c`.
- `server.c`: `rcvbuf_apply` (al arrancar y en cada recarga).
