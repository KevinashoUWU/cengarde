---
tags: [mapa, pieza]
archivos:
  - engine/src/rescue.h
  - engine/tests/test_rescue.c
sello: 778ebdde9c999d11
---
# Rescate (rescue.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Un ARQ estilo XLINK. El emisor guarda lo enviado hace poco en un anillo; cuando el [[Reordenador (reorder.h)]] del receptor va a abandonar un hueco, lo pide (`CG_T_NACK`) y el emisor lo reenvía por su camino más rápido, para que llegue antes de que TCP note la pérdida. Funciona en los dos sentidos: el servidor rescata la bajada y el cliente la subida. **GRIT no se rescata** (2026-10-27, [[025-protocontrolador]]): si lo pedido es un mensaje del protocontrolador B, la pérdida va a su flujo ([[Protocontrolador (protoctl.c, stream.h, streamcc.h)]], `cg_pctl_nack`, con el enlace por el que fue) en vez de reenviarse, y se contesta `CG_F_GONE`. **No se rescata la congestión:** lo perdido en un camino lleno no se reenvía (esa pérdida es la que frena a TCP), y el emisor avisa lo que no vendrá (`CG_F_GONE`). Lo reenviado sale de un crédito: cada paquete suma `pct` % de su tamaño, hasta 1 MiB.

**La evidencia de una pérdida** ([[029-mpgrit]], 2026-10-28): cada paquete guardado junta lo que se sabe de su pérdida desde dos lados, cada uno inseguro solo: su único enlace lo perdió (`CG_RESC_EV_LINK`, un `SFLOSS` del [[Subflujo (subflow.h)]]; pero un código o una copia pudo reconstruirlo en el receptor) y el receptor lo pidió (`CG_RESC_EV_ASKED`, un NACK; pero quizá solo llega tarde por otro enlace: en terreno todo lo que el túnel dio por perdido llegó). `cg_resc_sure` decide: sin código, basta el enlace; con código, los dos, o el enlace solo como **sospechoso** (`CG_RESC_SURE_LATER`, `CG_RESC_EV_SUSPECT`: GRIT lo da por perdido al vencer el horizonte de reparación si su entrega no volvió; el NACK del receptor puede no llegar nunca); el NACK solo, si `protocontroller_nack` lo toma por pérdida (nunca con `subflow`); nunca dos veces (`CG_RESC_EV_DONE`). Por ahora solo decide para GRIT.

**Un paquete copiado** (redundancia, 2026-10-29): el anillo guarda también los enlaces por los que salieron sus copias (`copies`, `cg_resc_copies`) y cuáles dijeron que lo perdieron (`lost`). El `SFLOSS` del último de ellos vale lo mismo que el de un paquete que fue solo por ese enlace (`cg_resc_lost_on`, `all_copies` en el estado). Antes una copia perdida en todos sus enlaces nunca se le decía a GRIT, que con subflujos tiene el RACK apagado; solo su temporizador la encontraba, 300 ms después de la ida y vuelta (`red3`: ~20 mil reenvíos por temporizador en `fade`, ninguno duplicado). Con `mute_trickle` las copias van también donde el anillo no ve, y no se guardan.

## Lee
- Los huecos que pide el reordenador (`send_nacks`), con `rescue_wait_ms` y `rescue_tries`.
- El peso de cada enlace (`cg_cap_rescue_w`) y el bit `full` de [[Capacidad (capacity.h)]]: peso 1 hasta `rescue_target_ms`, 0 desde 4 veces, según dónde estará la cola en 1 s.
- [[FEC (fec.h)]]: con `fec`, un hueco espera al menos 2 × `fec_wait_ms` + 20 ms antes del NACK.
- [[Reparación coherente (harq.h)]]: con un código protegiendo, el servidor no pide hasta que el hueco queda sellado, pide la paridad (PARQ) cuando hay déficit y el paquete solo sin código; con TCP adentro, nada más allá de su presupuesto.
- El enlace de cada paquete en el anillo (`went`) sale de las copias ya decididas (`seal_batch`, `down_seal`, historia 023).

## Decide / escribe
- Qué se reenvía y por dónde (`on_nack`); lo de GRIT, a su flujo con la certeza de `cg_resc_sure` (`to_grit`, `to_grit_bytes` en el estado de los dos extremos); `CG_BOND_RESCUE` y `CG_BOND_FULL` en la cola de bonding ([[Sondas y cola de bonding (proto.h)]]).
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
- Estaba casi mudo en redundancia (112 pedidos en un día de ~25 000 pérdidas, [[017-terreno-nostalgica]]): la pista `CG_F_PREV` se escribía antes de decidir las copias y nada se retenía con `reorder_dup_wait_ms` 0. Arreglado en [[023-reparacion-coherente]].
- C10: el rescate del cliente elige enlace por SRTT, no por OWD; `rescue_by_owd` quedó neutro ([[audit-pending]]).
- C11: el bit `full` parpadeaba; la histéresis quedó como perilla, mixta.
- C12: su peso dependía de `governor_target_ms`; ahora `rescue_target_ms`.

## Archivos
- `rescue.h`; tests `engine/tests/test_rescue.c`.
- `client.c`: `seal_batch` (`cg_resc_put`, `cg_resc_copies`, `cg_resc_earn`), `send_nacks`, `on_nack`, `on_parq`, `on_sfloss` (`cg_resc_lost_on`), `rescue_apply`. `server.c`: `down_seal`, `send_nacks`, `nack_send`, `on_nack`, `on_sfloss`.
