---
tags: [mapa, pieza]
archivos:
  - engine/src/rescue.h
  - engine/tests/test_rescue.c
sello: 9902060ce4276886
---
# Rescate (rescue.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Un ARQ estilo XLINK. El emisor guarda lo enviado hace poco en un anillo; cuando el [[Reordenador (reorder.h)]] del receptor va a abandonar un hueco, lo pide (`CG_T_NACK`) y el emisor lo reenvía por su camino más rápido, para que llegue antes de que TCP note la pérdida. Funciona en los dos sentidos: el servidor rescata la bajada y el cliente la subida. **No se rescata la congestión:** lo perdido en un camino lleno no se reenvía (esa pérdida es la que frena a TCP), y el emisor avisa lo que no vendrá (`CG_F_GONE`). Lo reenviado sale de un crédito: cada paquete suma `pct` % de su tamaño, hasta 1 MiB.

## Lee
- Los huecos que pide el reordenador (`send_nacks`), con `rescue_wait_ms` y `rescue_tries`.
- El peso de cada enlace (`cg_cap_rescue_w`) y el bit `full` de [[Capacidad (capacity.h)]]: peso 1 hasta `rescue_target_ms`, 0 desde 4 veces, según dónde estará la cola en 1 s.
- [[FEC (fec.h)]]: con `fec`, un hueco espera al menos 2 × `fec_wait_ms` + 20 ms antes del NACK.

## Decide / escribe
- Qué se reenvía y por dónde (`on_nack`); `CG_BOND_RESCUE` y `CG_BOND_FULL` en la cola de bonding ([[Sondas y cola de bonding (proto.h)]]).
- `CG_DEC_RESCUE_FULL_ON/OFF` → [[Bitácora de decisiones (decision.h)]].
- [[Ranuras de Starlink (slot.h)]] lo usa para las copias solo en el borde.

## Perillas
- [[Mapa/Perillas/rescue_kb|rescue_kb]]: 0 (apagado); hasta 262144. [[Mapa/Perillas/rescue_pct|rescue_pct]]: 5; 1–50.
- [[Mapa/Perillas/rescue_wait_ms|rescue_wait_ms]]: 0 (no pide); hasta 1000. [[Mapa/Perillas/rescue_tries|rescue_tries]]: 2; 1–4.
- [[Mapa/Perillas/rescue_target_ms|rescue_target_ms]]: 0 (= `governor_target_ms`). [[Mapa/Perillas/rescue_by_owd|rescue_by_owd]]: no. [[Mapa/Perillas/rescue_full_hyst|rescue_full_hyst]]: 0.

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: en la red de pruebas, 1–2 % de la bajada se perdía en todos los enlaces. Con TCP saturando rindió algo menos (86,4 contra 91,0 Mbit/s); con MPEG-TS crudo, margen 10 + rescate dio 0 errores. Su lugar es el tráfico de tasa fija.
- `rescue.h`: sin control de congestión, en el laboratorio lo retenido esperaba segundos (un enlace de 10 Mbit/s junto a 60 y 30).

## Choques conocidos
- C10: el rescate del cliente elige enlace por SRTT, no por OWD; `rescue_by_owd` quedó neutro ([[audit-pending]]).
- C11: el bit `full` parpadeaba; la histéresis quedó como perilla, mixta.
- C12: su peso dependía de `governor_target_ms`; ahora `rescue_target_ms`.

## Archivos
- `rescue.h`; tests `engine/tests/test_rescue.c`.
- `client.c`: `wg_read` (`cg_resc_put`, `cg_resc_earn`), `send_nacks`, `on_nack`, `rescue_apply`. `server.c`: `down_prepare`, `send_nacks`, `on_nack`.
