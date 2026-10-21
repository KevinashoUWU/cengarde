---
tags: [mapa, pieza]
archivos:
  - engine/src/steer.h
  - engine/tests/test_steer.c
sello: 2d9bd3b3f54bc15b
---
# Colas del servidor (steer.h)

**Grupo:** Recibir — [[Mapa del motor]]

## Qué hace
El servidor escucha con un grupo `SO_REUSEPORT` de `lanes` sockets (carriles) y uno de basura. Un programa BPF clásico manda cada datagrama al carril de su número de enlace (byte 3) módulo L: las copias de un mismo paquete esperan en colas distintas, y un parón que desborda una rara vez se lleva todas las copias. La bajada del camino p sale por el carril `p & (L − 1)`. Lo que no es del protocolo (corto o de otra versión) va al socket de basura, de búfer pequeño.

## Lee
- El byte 3 (número de enlace, sin autenticar) y el nibble de versión de cada datagrama, en el kernel.

## Decide / escribe
- En qué socket espera cada datagrama → `listen_read`, `junk_read`.
- Qué caminos comparte cada carril → [[Dosificación (pace.h)]] (`pace_lanes`) y [[Presupuesto de recepción (rcvbudget.h)]].

## Perillas
- [[Mapa/Perillas/lanes|lanes]]: `auto` (8); 1, 2, 4, 8, 16. Cambiarla reinicia.

## Dónde se midió
- [[011-hilos]]: con un socket, el 73–98 % de las copias tiradas eran paquetes enteros; con 8 carriles, 11,8× menos paquetes enteros perdidos sumando tres sesiones. `deepq`: el túnel perdía 10,48 % con un socket y nada con carriles. La puerta S1 tal como está escrita no se cumple.

## Choques conocidos
- Absorbe un parón más corto que el búfer de un carril (~33 ms a 110 kpps, cálculo); si el único hilo del servidor se atrasa más, desbordan todos a la vez ([[engine-README]]).
- Ocho carriles llenos pueden agotar `udp_mem` de toda la máquina: lo limita [[Presupuesto de recepción (rcvbudget.h)]].

## Archivos
- `steer.h`; tests `engine/tests/test_steer.c`, `cbpf.h`.
- `server.c`: `lanes_open`, `lane_single`, `listen_read`, `junk_read`.
