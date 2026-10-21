---
tags: [mapa, pieza]
archivos:
  - engine/src/split.h
  - engine/tests/test_split.c
sello: 339a653b9afbca41
---
# Reparto (split.h)

**Grupo:** Decidir · Enviar — [[Mapa del motor]]

## Qué hace
Decide por qué enlace va cada paquete cuando se reparte en vez de copiar todo por todos. Por omisión usa un round-robin ponderado suave (el de nginx): pesos 5:1:1 dan `a a b a c a a`. Duplica (pick −1) los handshakes de WireGuard, los datagramas chicos y todo cuando no hay enlace elegible. **G2:** con menos de 2 elegibles, todo va por todos. Peso 0 (sentido de libRIST): copia de todo. Marca `CG_F_SPLIT` bajo el MAC. Si un extremo reparte o no lo decide `cg_split_active` con la cola de bonding del otro.

## Lee
- Portadores de [[Salud y silenciado (health.h)]] (G1: nunca un silenciado), elegibles de [[G3 enlace sin respuesta (split.h)]] y la máscara de [[G4 guarda de dispersión (split.h)]].
- Pesos: `weight` del `[link]`, o los de [[Gobernador (govern.h)]], o los de [[Capacidad (capacity.h)]].
- Con `split_eta`, [[ETA (eta.h)]] elige entre 2 o más enlaces; con cosquilla, [[Cosquilla (tickle.h)]] va primero.
- [[Enlace dominante (dominant.h)]]: con `dom.out`, no se reparte.
- [[Copias bajo estrés (split.h)]] y `fec_share_max` de [[FEC (fec.h)]] recortan la máscara.

## Decide / escribe
- `pick[]` por paquete → el envío (`send_links` / `down_send`, `cg_split_dst`).
- `CG_F_SPLIT` y `CG_F_PREV` → [[Reordenador (reorder.h)]] del otro extremo.
- La cola de bonding propia (`cg_split_probe_tail`, `cg_split_reply_tail`) → [[Sondas y cola de bonding (proto.h)]].
- Qué decidió cada paquete (`split_by`) → estado.

## Perillas
- [[Mapa/Perillas/split|split]]: cliente `off`, servidor `client`.
- [[Mapa/Perillas/split_small|split_small]]: 256; 0–2048.
- [[Mapa/Perillas/weight|weight]]: 1; 0–1000. [[Mapa/Perillas/split_spread_copy|split_spread_copy]].
- [[Mapa/Perillas/capacity_weights|capacity_weights]], [[Mapa/Perillas/governor|governor]], [[Mapa/Perillas/split_eta|split_eta]], [[Mapa/Perillas/fec_share_max|fec_share_max]].

## Dónde se midió
- [[012-bonding]]: dos enlaces iguales dan 1,98× uno solo (Cubic y BBR); con 50–150 ms de dispersión el reparto por turnos se hunde. En la Pi, la subida repartida llegó al doble de pico que copiando, la bajada a menos.
- [[015-modelo-de-enlace]] §8: con un Starlink dominante, el reparto daba 135 frente a 294 de Starlink solo.

## Choques conocidos
- C13: G4, la cosquilla, `split_stress_single`, `cg_sdup` y la FEC reescriben la misma máscara `eligible`; bajo `stress_single` la FEC solo arma grupos de uno y la cosquilla no corre ([[audit-pending]], tarea #52).
- Sin reordenador del otro lado, repartir es peor que redundancia: por eso es experimental.

## Archivos
- `split.h`; tests `engine/tests/test_split.c`.
- `client.c`: `split_active`, `split_weights`, `split_masks`, `wg_read`, `send_links`. `server.c`: `down_prepare`, `down_send`.
