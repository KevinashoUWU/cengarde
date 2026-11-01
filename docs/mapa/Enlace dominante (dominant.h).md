---
tags: [mapa, pieza]
archivos:
  - engine/src/dominant.h
  - engine/tests/test_dominant.c
sello: 93d382051254f0d4
---
# Enlace dominante (dominant.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Un Starlink muy por encima de los celulares se queda el envío para él. Si un enlace con `dominant_pct` entrega (su mejor tasa en 10 s) al menos ese % de lo que pueden juntos los enlaces más lentos (2 ms o más de base detrás), **el reparto se hace a un lado**: todo va como en redundancia, con las copias a los lentos a tiempo, y el receptor no retiene nada. Vuelve el reparto cuando entrega menos de 4/5 de eso. Con varios platos ([[022-multistarlink]]), los otros enlaces con `dominant_pct` son sus **pares**: nunca cuentan como más lentos (ni fuera ni en la suma), y el candidato solo cambia por un par que entrega más de 5/4 de lo suyo (`CG_DOM_SWITCH_NUM/DEN`). Con `hold`, mientras no pierde se toma su pico como lo que puede, y en reposo se **presume** dominante si su historia lo muestra.

## Lee
- Capacidad, pico y `lost_ms` de [[Capacidad (capacity.h)]], o con `link_cc` el `bw` de cada enlace del [[Ritmo por enlace (linkcc.h)]] como tasa y pico; la base de [[Cola por enlace (cg_gov_base)]].
- `dominant_pct`, `dominant_hold` del [[Modelo de enlace (linkmodel.h)]] (preset `starlink`: 250).

## Decide / escribe
- `dom.out` → [[Reparto (split.h)]]: si no es 0, no reparte: todo va copiado por [[Copias a tiempo (eta.h)]].
- `presumed` y `why` en el estado.

## Perillas
- [[Mapa/Perillas/split_dominant_pct|split_dominant_pct]]: 0 (el del preset); un valor grande lo apaga.
- [[Mapa/Perillas/split_dominant_hold|split_dominant_hold]]: 0 (el del preset: apagado).

## Dónde se midió
- [[015-modelo-de-enlace]] §8: con Starlink a 400 Mbit/s y celulares de 25 y 18, el bonding pasó de 135 a 241 Mbit/s de bajada (n = 16; Starlink solo 294, redundancia 275); a 200, de 101 a 155; a 100 no entra.
- §9: la retención (`hold`) no ganó y costó 17 % en `shallowq` subida: queda como perilla.
- [[022-multistarlink]]: pares e histéresis del candidato, medidos en `msl2c` y `msl3c` (bajada, reparto).

## Choques conocidos
- Varios platos ([[022-multistarlink]]): antes, el plato que iba adelante en retardo base sumaba al otro (2 ms detrás, y cada ranura sortea su base) y no dominaba, y el de atrás sí: el reparto iba y venía según cuál entregaba más, y cada cambio de candidato borraba la ventana y la histéresis. Ahora son pares y el candidato tiene histéresis.
- Con `dom.out` el reparto se hace a un lado **entero**: con dos platos que dominan a los celulares, tampoco se reparte entre los platos (la bajada queda en lo de un plato). Repartir solo entre los pares, con los lentos a copias a tiempo, queda pendiente ([[022-multistarlink]]).
- `cg_dom_evidence` (solo con `hold`) todavía suma a los pares.
- Sacar solo a los lentos (como hace G4) dejaba al reordenador y al rescate sobre un enlace con pérdida, y TCP se paraba segundos (`dominant.h`). Por eso se hace a un lado entero.
- La subida de campo (bonding peor) no se reproduce en laboratorio ([[015-modelo-de-enlace]]).
- El comentario dice que las copias usan «starlink's pace_peers_ms, eta.h»; el campo vive en `linkmodel.h` (detalle menor).

## Archivos
- `dominant.h`; tests `engine/tests/test_dominant.c`.
- `client.c`: `cap_tick` (`cg_dom_tick`), `tun_read`. `server.c`: `cap_tick`, `down_prepare`.
