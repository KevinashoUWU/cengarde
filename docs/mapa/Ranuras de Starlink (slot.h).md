---
tags: [mapa, pieza]
archivos:
  - engine/src/slot.h
  - engine/tests/test_slot.c
sello: 00d33fbb83357402
---
# Ranuras de Starlink (slot.h)

**Grupo:** Medir · Decidir — [[Mapa del motor]]

## Qué hace
Cada 15 s, a los :12, :27, :42 y :57 de cada minuto (UTC), Starlink reconfigura sus enlaces y un enlace se para ~45 ms. Este reloj usa la hora de pared (`CLOCK_REALTIME`, con NTP en router y VPS) y marca una **ventana** alrededor de cada **borde**: en ella, las pérdidas de un enlace con reloj son el cambio de satélite, no un enlace lleno. Con `starlink_edge_copies`, en redundancia las copias a los demás enlaces van solo dentro de la ventana.

## Lee
- La hora de pared; el [[Modelo de enlace (linkmodel.h)]] (`slot_clock`, encendido en el preset `starlink`).
- Para las copias en el borde: un informe del enlace en los últimos 300 ms (`CG_SLOT_FIT_MS`) y ninguna pérdida en 2 s (`CG_SLOT_LOST_MS`, `lost_ms` de [[Capacidad (capacity.h)]]).

## Decide / escribe
- `cg_slot_touches` → [[Capacidad (capacity.h)]]: la muestra no cuenta pérdida (`n_edge`, `edge_samples` en el estado).
- `cg_slot_solo` / `cg_slot_pick` → el envío: fuera de la ventana, un paquete de 300 B o más va solo por Starlink; si el otro extremo reordena y guarda, como DATA repartido (`CG_F_SPLIT`), para que un hueco se pida al [[Rescate (rescue.h)]].

## Perillas
- [[Mapa/Perillas/starlink_slot_clock|starlink_slot_clock]]: 0 (preset); 1 sí, 2 no.
- [[Mapa/Perillas/starlink_edge_ms|starlink_edge_ms]]: 12000; [[Mapa/Perillas/starlink_edge_pre_ms|starlink_edge_pre_ms]]: 100; [[Mapa/Perillas/starlink_edge_post_ms|starlink_edge_post_ms]]: 50.
- [[Mapa/Perillas/starlink_edge_copies|starlink_edge_copies]]: no.

## Dónde se midió
- [[015-modelo-de-enlace]] §7: en «nostalgica», borde a los :12/:27/:42/:57 (±50 ms); ~45 ms parado en la subida en cada borde.
- [[015-modelo-de-enlace]] §9: con la pérdida por secuencia, `lossy` 93 % → 17 % del tiempo, caudal igual. Copias en el borde con rescate: de ~3 a ~1,05 copias por paquete sin pérdida en un stream de 8 Mbit/s, a cambio de hasta +30 % de p99 del ping grande. Por eso es perilla.

## Choques conocidos
- Sin NTP en el router el reloj no sirve (`CG_WALL_SET_MS` lo apaga); falta probarlo en campo ([[015-modelo-de-enlace]] §9).
- El 37 % de las pérdidas de Starlink no cae en los bordes: sin rescate, las copias solo en el borde pierden ~0,11 % de datagramas.
- Cuenta de ranuras distinta entre `slot.h` (477 en 1,7 h) y la historia (479 en ~2 h): ver [[Mapa del motor#Contradicciones]].

## Archivos
- `slot.h`; tests `engine/tests/test_slot.c`.
- `client.c`: `cap_tick` (`cc.slot`), `edge_solo`, `wg_read` (`cg_slot_pick`), `send_links`. `server.c`: lo mismo en `cap_tick` y `down_prepare`.
