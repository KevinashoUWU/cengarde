---
tags: [mapa, pieza]
archivos:
  - engine/src/eta.h
  - engine/src/linkmodel.h
  - engine/tests/test_eta.c
sello: 450c50e14762f71a
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
- Con `redund_spare`, [[Lo que sobra (spare.h)]] recorta después la máscara de los enlaces que no son el primero según lo que les sobra (`cg_eta_copies_first` da además el enlace donde llega primero, y `cg_eta_unadd` saca de su cola virtual una copia que al final no salió); la perilla deja esta instancia activa aunque ningún enlace tenga ventana.

## Perillas
- [[Mapa/Perillas/redund_pace_ms|redund_pace_ms]]: 0 (apagado); 0–2000.
- Preset `starlink`: `pace_peers_ms = 60`; encima, [[Mapa/Perillas/pace_peers_ms|pace_peers_ms]] (-1: el del preset, 0: apagado; 0–2000). En terreno, 250 (historia 017).
- [[Mapa/Perillas/starlink_edge_copies|starlink_edge_copies]].

## Dónde se midió
- [[015-modelo-de-enlace]] §6: con Starlink, el p99 del ping de 1200 B en `starsat` bajó de ~360 a ~95 ms, caudal ±5 %; cuesta 1,1–3× retransmisiones. Aplicado a todos los celulares, `het` perdía 8–18 %: por eso solo junto a un Starlink.
- `eta.h`: en campo, las copias de Entel llegaban tarde el 72 % y las de WOM todas.
- [[017-terreno-nostalgica]]: con 60 ms WOM llevaba 36–60 % de las copias y la subida perdía 0,19–0,29 % en todos los enlaces; con 250, ~90 % y 0,002 %.
- [[020-copias-parciales-y-paridad]]: con el celular ahogado (celda virtual, WOM a 7 Mbit/s), 250 deja su cola en ~400 ms y pierde 0,18 % fuera de un microcorte; 60, ~160 ms y 0,24 %; todas las copias, 3 s y 0,16 %. `redund_spare = mix` lo bajó a 0,06 %.

## Choques conocidos
- C10: copias, rescates y paridad no se cuentan en el ETA ni en el tope de `cake` ([[audit-pending]]).
- [[Enlace dominante (dominant.h)]] usa estas copias a tiempo cuando se hace a un lado.

## Archivos
- `eta.h` (`cg_eta_copies`, `cg_eta_copies_first`, `cg_eta_unadd`), `linkmodel.h` (`cg_link_pace_windows`); tests `engine/tests/test_eta.c`, `test_linkmodel.c`.
- `client.c`: `link_models`, `cap_tick` (`c->pace`), `send_links`. `server.c`: `path_models`, `down_send`.
