---
tags: [mapa, pieza]
archivos:
  - engine/src/split.h
  - engine/tests/test_split.c
sello: 22c0e59c52526614
---
# G3 enlace sin respuesta (split.h)

**Grupo:** Decidir — [[Mapa del motor]]

## Qué hace
Saca del reparto a un enlace cuyo otro extremo no mandó nada verificado durante más de 3 intervalos de sonda (`CG_SPLIT_STALE_PROBES`). Sigue llevando los paquetes duplicados. El mudo de [[Salud y silenciado (health.h)]] toma los mismos tres intervalos más un segundo; G3 acorta en ese segundo el tiempo que un enlace muerto conserva su parte.

## Lee
- `last_reply_ms` (cliente) o el último paquete del camino (servidor), de [[Sondas y cola de bonding (proto.h)]].
- El intervalo de sonda anunciado (el mayor de los dos últimos, porque al empezar el tráfico la última respuesta puede ser del intervalo de reposo).

## Decide / escribe
- La máscara de elegibles → [[Reparto (split.h)]], [[G4 guarda de dispersión (split.h)]] y [[Gobernador (govern.h)]].
- `CG_DEC_STALE` / `CG_DEC_FRESH` → [[Bitácora de decisiones (decision.h)]].

## Perillas
- [[Mapa/Perillas/probe_interval_ms|probe_interval_ms]] (lo que cuenta como intervalo).

## Dónde se midió
- [[012-bonding]]: la ventana de un enlace muerto pasa de ~1,3 s a ~300 ms (estimación, pendiente de medir en `linkdeath`).

## Choques conocidos
- C6: en el VPS, G3 miraba `last_rx_ms`, que la subida mantiene vivo; una bajada atascada seguía ~1,3 s en el reparto. Arreglado con la frescura por sentido (PR #79, [[audit-pending]]).
- C7: un hueco corto de G3 (300 ms) borraba `qleft` de G4 y apagaba el estrés; parte de lo mismo (PR #79).

## Archivos
- `split.h` (`cg_split_stale`); tests `engine/tests/test_split.c`.
- `client.c`: `split_eligible_g3`, `spread_tick`. `server.c`: `split_eligible_g3` (mira `last_rx_ms` y `last_owd_ms`), `spread_tick`.
