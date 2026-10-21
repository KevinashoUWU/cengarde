---
tags: [mapa, pieza]
archivos:
  - engine/src/pktinfo.h
  - engine/tests/test_pktinfo.c
sello: 1fd2001d5dc7cb4a
---
# Dirección de llegada (pktinfo.h)

**Grupo:** Enviar — [[Mapa del motor]]

## Qué hace
Un servidor con varias direcciones escucha en un socket comodín. Sus respuestas saldrían por la dirección que prefiere la ruta, y el socket conectado del cliente las tiraría. Por eso el socket pide la dirección de llegada de cada datagrama, cada camino guarda la de sus paquetes verificados, y lo que vuelve sale con esa dirección como origen.

## Lee
- `IPV6_PKTINFO` / `IP_PKTINFO` de cada datagrama recibido.

## Decide / escribe
- La dirección de origen de cada respuesta y de la bajada de cada camino (`server.c`).

## Perillas
- [[Mapa/Perillas/listen|listen]] (solo con un comodín).

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: PR 2, dirección de llegada.

## Choques conocidos
- Ninguno registrado.

## Archivos
- `pktinfo.h`; tests `engine/tests/test_pktinfo.c`. `server.c`: `listen_read`, `path_update`, el envío.
