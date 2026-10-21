---
tags: [mapa, pieza]
archivos:
  - engine/src/dominant.h
  - engine/tests/test_dominant.c
sello: 03a4d3a2775aaaaf
---
# Enlace dominante (dominant.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Un Starlink muy por encima de los celulares se queda el envío para él. Si un enlace con `dominant_pct` entrega (su mejor tasa en 10 s) al menos ese % de lo que pueden juntos los enlaces más lentos (2 ms o más de base detrás), **el reparto se hace a un lado**: todo va como en redundancia, con las copias a los lentos a tiempo, y el receptor no retiene nada. Vuelve el reparto cuando entrega menos de 4/5 de eso. Con `hold`, mientras no pierde se toma su pico como lo que puede, y en reposo se **presume** dominante si su historia lo muestra.

## Lee
- Capacidad, pico y `lost_ms` de [[Capacidad (capacity.h)]]; la base de [[Cola por enlace (cg_gov_base)]].
- `dominant_pct`, `dominant_hold` del [[Modelo de enlace (linkmodel.h)]] (preset `starlink`: 250).

## Decide / escribe
- `dom.out` → [[Reparto (split.h)]]: si no es 0, `cg_slot_pick` en vez de `cg_split_pick_eta`; las copias van por [[Copias a tiempo (eta.h)]].
- `presumed` y `why` en el estado.

## Perillas
- [[Mapa/Perillas/split_dominant_pct|split_dominant_pct]]: 0 (el del preset); un valor grande lo apaga.
- [[Mapa/Perillas/split_dominant_hold|split_dominant_hold]]: 0 (el del preset: apagado).

## Dónde se midió
- [[015-modelo-de-enlace]] §8: con Starlink a 400 Mbit/s y celulares de 25 y 18, el bonding pasó de 135 a 241 Mbit/s de bajada (n = 16; Starlink solo 294, redundancia 275); a 200, de 101 a 155; a 100 no entra.
- §9: la retención (`hold`) no ganó y costó 17 % en `shallowq` subida: queda como perilla.

## Choques conocidos
- Sacar solo a los lentos (como hace G4) dejaba al reordenador y al rescate sobre un enlace con pérdida, y TCP se paraba segundos (`dominant.h`). Por eso se hace a un lado entero.
- La subida de campo (bonding peor) no se reproduce en laboratorio ([[015-modelo-de-enlace]]).
- El comentario dice que las copias usan «starlink's pace_peers_ms, eta.h»; el campo vive en `linkmodel.h` (detalle menor).

## Archivos
- `dominant.h`; tests `engine/tests/test_dominant.c`.
- `client.c`: `cap_tick` (`cg_dom_tick`), `wg_read`. `server.c`: `cap_tick`, `down_prepare`.
