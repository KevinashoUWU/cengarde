---
tags: [mapa, pieza]
archivos:
  - engine/src/wgwatch.h
  - engine/src/wgport.h
  - engine/tests/test_wgwatch.c
  - engine/tests/test_wgport.c
sello: 6663fce189e6e855
---
# Reloj atrasado de WireGuard (wgwatch.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Un router sin reloj con batería (una Pi) arranca con la fecha de su imagen; WireGuard del VPS ignora entonces sus handshakes (protección de repetición) y, con todo por el túnel, el router no llega a NTP. El servidor vigila las iniciaciones que entrega a WireGuard; tras varias sin respuesta, le manda un datagrama a la dirección del cliente en el túnel (`wireguard_poke`), y WireGuard inicia su propio handshake, que el router acepta. `wgport.h` guarda en un archivo el puerto que WireGuard conoce de cada cliente, para no perderlo al reiniciar el motor.

## Lee
- Las iniciaciones de handshake que pasan hacia WireGuard y sus respuestas.

## Decide / escribe
- Cuándo «picar» a WireGuard (`wg_poke`) y por qué sesión redirigir su handshake (`cg_wgw_redirect`).
- El archivo de puertos (`wgport_sync`, `wgport_load`).

## Perillas
- [[Mapa/Perillas/wireguard_poke|wireguard_poke]]: `10.79.0.2:9`; `none` lo apaga.
- [[Mapa/Perillas/wireguard_port_file|wireguard_port_file]]: vacía.

## Dónde se midió
- [[002-wireguard-para-cengarde]]: el reloj del router.
- [[010-ipv6-varias-ip-multicliente-nombres]]: PR 1, reinicio del servidor y el puerto de WireGuard.

## Choques conocidos
- No aplica con `tunnel = native` (sin WireGuard).

## Archivos
- `wgwatch.h`, `wgport.h`; tests `engine/tests/test_wgwatch.c`, `test_wgport.c`.
- `server.c`: `wg_read`, `wg_poke`, `wgport_sync`, `wgport_load`.
