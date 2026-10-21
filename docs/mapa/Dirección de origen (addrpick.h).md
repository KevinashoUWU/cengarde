---
tags: [mapa, pieza]
archivos:
  - engine/src/addrpick.h
  - engine/tests/test_addrpick.c
sello: 1249eddb20333fc7
---
# Dirección de origen (addrpick.h)

**Grupo:** Enviar — [[Mapa del motor]]

## Qué hace
Elige desde qué dirección local manda cada enlace. En IPv4, la primaria de la interfaz antes que las secundarias, nunca loopback, enlace local ni 0.0.0.0. En IPv6 sigue la RFC 6724 en lo que un enlace puede saber: Linux da a las ULA alcance global, así que las ordena por rango (`cg_ip6_src_rank`) y desempata por la más nueva, para que un prefijo nuevo tome el lugar al instante.

## Lee
- Las direcciones de cada interfaz, por netlink.

## Decide / escribe
- La dirección a la que se ata el socket de cada enlace → [[Elección de servidor (srvpick.h)]] (qué familias tiene el enlace).

## Perillas
- Ninguna propia; las interfaces salen de [[Mapa/Perillas/interfaces|interfaces]] y [[Mapa/Perillas/exclude|exclude]].

## Dónde se midió
- [[010-ipv6-varias-ip-multicliente-nombres]]: la elección de origen IPv6 (PR 2).

## Choques conocidos
- Ninguno registrado.

## Archivos
- `addrpick.h`; tests `engine/tests/test_addrpick.c`. Lo usa `netlink.c`.
