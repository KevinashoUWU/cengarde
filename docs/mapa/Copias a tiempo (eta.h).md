---
tags: [mapa, pieza]
archivos:
  - engine/src/eta.h
  - engine/src/linkmodel.h
  - engine/tests/test_eta.c
sello: cada6f51e8ec33bc
---
# Copias a tiempo (eta.h)

**Grupo:** Decidir · Enviar — [[Mapa del motor]]

## Qué hace
En redundancia (o en el modo dominante), decide qué enlaces reciben copia de un paquete que va por todos. El enlace donde llega primero siempre la recibe; otro con ventana solo si su copia llegaría a menos de esa ventana de la primera (`cg_eta_copies`). Una copia detrás de una cola que crece llega después de que el receptor abandonó el hueco y no tapa nada. Los paquetes de menos de 300 B (`CG_PACE_SMALL`) van por todos.

## Lee
- La ventana de cada enlace (`cg_link_pace_windows`): su `pace_ms` o el `pace_peers_ms` de otro enlace de la sesión, el menor, del [[Modelo de enlace (linkmodel.h)]].
- Una instancia aparte de [[ETA (eta.h)]] alimentada por [[Capacidad (capacity.h)]].

## Decide / escribe
- `cmask[]`, la máscara de copias de cada paquete → el envío (`send_links`, `down_send`).
- Con `starlink_edge_copies`, [[Ranuras de Starlink (slot.h)]] recorta además esa máscara fuera del borde.

## Perillas
- [[Mapa/Perillas/redund_pace_ms|redund_pace_ms]]: 0 (apagado); 0–2000.
- Preset `starlink`: `pace_peers_ms = 60` (no es perilla).
- [[Mapa/Perillas/starlink_edge_copies|starlink_edge_copies]].

## Dónde se midió
- [[015-modelo-de-enlace]] §6: con Starlink, el p99 del ping de 1200 B en `starsat` bajó de ~360 a ~95 ms, caudal ±5 %; cuesta 1,1–3× retransmisiones. Aplicado a todos los celulares, `het` perdía 8–18 %: por eso solo junto a un Starlink.
- `eta.h`: en campo, las copias de Entel llegaban tarde el 72 % y las de WOM todas.

## Choques conocidos
- C10: copias, rescates y paridad no se cuentan en el ETA ni en el tope de `cake` ([[audit-pending]]).
- [[Enlace dominante (dominant.h)]] usa estas copias a tiempo cuando se hace a un lado.

## Archivos
- `eta.h` (`cg_eta_copies`), `linkmodel.h` (`cg_link_pace_windows`); tests `engine/tests/test_eta.c`, `test_linkmodel.c`.
- `client.c`: `link_models`, `cap_tick` (`c->pace`), `send_links`. `server.c`: `path_models`, `down_send`.
