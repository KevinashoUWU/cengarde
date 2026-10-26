---
tags: [mapa, pieza]
archivos:
  - engine/src/harq.h
  - engine/tests/test_harq.c
sello: 771ead11b10fd4ed
---
# Reparación coherente (harq.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Decide quién repara un hueco, para que el código, el rescate y el TCP de adentro no se hagan el trabajo unos a otros ([[023-reparacion-coherente]]). En el receptor de un hueco, en este orden:

1. **El código** ([[Códigos de borrado (rs.h, rlc.h)]] o la paridad XOR de [[FEC (fec.h)]]) lo rehace solo: no se pide nada mientras puedan llegar reparaciones que lo cubran, hasta que queda **sellado** (llegó una reparación o paridad cuya base lo pasó) o se cumple `rescue_code_ms`.
2. **Sellado y faltando**, con ecuaciones (RLC) o un bloque (RS) que lo echan de menos: se pide la **paridad** de nuevo (PARQ, `CG_T_PARQ`, protocolo 9), tantas reparaciones nuevas como el **déficit de rango** (`cg_rlc_rx_need`, `cg_rs_rx_need`): con RLC, combinaciones de exactamente los que faltan; con RS, índices nuevos del mismo bloque, porque sus reparaciones declaran una geometría `m` mayor que las que manda el plan (`cg_rs_mhdr`). Nunca lo que ya es decodificable.
3. **Sin código que lo cubra:** el paquete ([[Rescate (rescue.h)]], NACK).
4. **Nunca contra TCP:** con TCP adentro (túnel nativo), una respuesta que no llega dentro del presupuesto de TCP (su RTT más un cuarto, el `reo_wnd` de RACK) no se pide, y el hueco no se retiene por ella: lo repara la retransmisión de TCP (`CG_HARQ_TCP`).

Con las **ventanas anchas de la FEC v3** ([[026-fec-v3]]) cambian tres cosas. Un hueco que la ventana todavía cubre no espera al sello si sus reparaciones por venir no alcanzan: `cg_harq_future` estima cuántas llegarán todavía sobre él (la ventana larga de la reparación más nueva, una cada `gap16`/16 secuencias, la mitad para un hueco más viejo que el cuarto de la ventana), y con el déficit sobre eso se pide **ya** la diferencia (PARQ sobre las columnas libres más viejas), una ida y vuelta, no después de que la ventana lo pase (en `stall100` con el túnel de WireGuard, de ~700 a 43 retransmisiones en una corrida de cada una: un indicio, no una medida). Sin rescate (`rescue_wait_ms` 0), el hueco espera al código igual (`cg_harq_code_wait_us`: `rescue_code_ms`, dentro del presupuesto de TCP) mientras llegan reparaciones anchas. Y sin anillo en el cliente (`client_keeps` falso) no se manda NACK: el hueco se suelta al momento (`no_nack`). Dos huecos del mismo sistema no piden dos veces las mismas columnas en un lote (`cg_parq_overlap`). Retener los huecos de la ventana ancha también con TCP se probó y se descartó: el mismo caudal y más retardo (ping de 1200 B p99 400 contra 240 ms en `stall100`, medianas de n = 3, sin prueba estadística).

También dice qué escribe un emisor en `CG_F_PREV` una vez decididas las copias (`cg_harq_only`): antes se escribía antes de que [[Copias a tiempo (eta.h)]] y [[Lo que sobra (spare.h)]] quitaran copias, y un paquete que iba solo por el primer enlace decía «varios»; el receptor nunca sabía que se había perdido en todos los enlaces y el rescate casi no pedía (112 pedidos en un día de ~25 000 pérdidas, [[017-terreno-nostalgica]]).

## Lee
- Del servidor (receptor de la subida): el anillo de DATA y las ecuaciones o bloques pendientes de los códigos, la base de la reparación más nueva (`rep_seal`), el RTT del camino más rápido (subida del reordenador más bajada que reporta el cliente: el desfase de relojes se cancela), y el RTT de los flujos TCP que salen del túnel ([[Liberación TCP (tcpfree.h)]], `struct cg_tcpe`: el tramo lejano del SYN al SYN-ACK más el del túnel).
- Del cliente (receptor de la bajada): el presupuesto de TCP y, con la FEC v3 del servidor (`fec_code = rlc` allí, `fec = yes` en el router), su propio anillo y receptor de la RLC (`dfec`, `drlc`) y lo mismo que el servidor lee de la reparación ancha más nueva (`df3_top`, `df3_span`, `df3_gap16`).
- De la FEC v3: la base y el largo de la reparación ancha más nueva y cada cuánto llegan (`f3_top`, `f3_span`, `f3_gap16` por sesión).

## Decide / escribe
- `send_nacks` del servidor: cada hueco pedido por el [[Reordenador (reorder.h)]] espera al código (`hq`, hasta 256 por sesión), o va como PARQ o NACK, o se suelta para TCP (`cg_ro_refused`). `ro_timeout` despierta al cumplirse `rescue_code_ms`.
- Lo mismo en el cliente para la bajada con reparaciones anchas (`d_harq_one`, `d_harq_service`, `d_send_asks`; `dhq`, hasta 256): la PARQ sube al servidor (`parq_out`), que la responde desde su anillo de rescate o su ventana (`on_parq` de `server.c`, con el presupuesto del rescate que gana cada DATA) por su camino vivo de menor retardo.
- La espera de un hueco pedido (`rocf.rescue_us`, `cg_harq_wait_us`): `rescue_wait_ms`, más `rescue_code_ms` con un código, nunca más que el presupuesto de TCP.
- El cliente responde una PARQ desde su anillo de rescate (`on_parq`): RLC con semillas nuevas (`cg_rlc_combine`), RS codificando otra vez el bloque (`cg_rs_encode` con la geometría) y los índices que el servidor no tiene (`cg_parq_rs_pick`); por un enlace con lugar (no ahogado ni lleno, el de menor RTT; si no hay ninguno, no sale: antes iba por el ahogado, y en un microcorte 1 500 reparaciones por segundo llevaron la cola del celular a 1,6 s, [[025-protocontrolador]]), dentro del crédito del rescate. Un enlace con las sondas detenidas que todavía trae paquetes cuenta como vivo (`link_masks_tx`, [[Salud y silenciado (health.h)]]), también para el NACK.
- Estado del cliente con la FEC v3 de la bajada: `download_fec` (`repairs`, `rlc_recovered`, `rlc_wide`, `rlc_peak`, `unknowns`, `waited_code`, `parq_asked`, `parq_repairs_asked`, `no_nack`, `left_to_tcp`, …); del servidor por sesión, `download_fec` (`rlc`, `parq_received`).
- Estado: servidor por sesión `rescue` (`nack_seqs`, `wait_ms`, `tcp_budget_ms`, `waited_code`, `not_asked`, `left_to_tcp`, `wait_full`, `waiting`, `no_nack`), `repair.parq` (`asked`, `repairs_asked`, `messages`), `tcp` (`retx_one`, `retx_copied`); global `tcp_e2e`. Cliente: `rescue.parq` (`received`, `requests`, `repairs`, `bytes`, `missing`, `over_budget`, `no_link`), `rescue.left_to_tcp`, `upload.tcp`, `tcp_e2e`. El reordenador separa `rescue.rescued` (llegó el paquete) de `rescue.repaired` (lo rehizo un código).

## Perillas
- [[Mapa/Perillas/rescue_code_ms|rescue_code_ms]]: 100; 0–1000. [[Mapa/Perillas/parq|parq]]: sí. [[Mapa/Perillas/rescue_tcp_race|rescue_tcp_race]]: sí. [[Mapa/Perillas/tcp_retx_one|tcp_retx_one]]: sí.
- Actúa con [[Mapa/Perillas/rescue_wait_ms|rescue_wait_ms]] y [[Mapa/Perillas/rescue_kb|rescue_kb]] (el rescate encendido); PARQ con `redund_spare = rs | rlc` y `fec = yes` en el servidor.

## Dónde se midió
- [[023-reparacion-coherente]], laboratorio (túnel nativo, n = 6): `protect` con copias de 250 ms y rescate, flujo perdido 0,275 → 0,005 % y TCP 7,6 → 14,4 Mbit/s; con código, el código lo rehace y con TCP los huecos no se retienen (retener hasta el sello bajó un TCP con el host a 100 ms de 23,6 a 16,9 Mbit/s; suelto, 25,2). PARQ contra NACK en el microcorte: mismos bytes y pérdida (p ≥ 0,39). Una retransmisión por un enlace: copiadas 188 → 2,5 por corrida.

## Choques conocidos
- Retener un hueco hasta que el código lo rehace cuesta demora a todo lo que viene detrás: sin TCP adentro (UDP o túnel WireGuard) la espera puede llegar a `rescue_wait_ms` + `rescue_code_ms`; con TCP visible la acota su presupuesto.
- En un microcorte el enlace «con lugar» puede ser el mismo que pierde: las reparaciones pedidas también se pierden ([[023-reparacion-coherente]]).

## Archivos
- `harq.h`; tests `engine/tests/test_harq.c` (y las partes de `test_rlc.c`, `test_rs.c`, `test_reorder.c`, `test_tcpfree.c` de la historia 023).
- `client.c`: `seal_batch`, `retx_masks`, `on_parq`, `parq_link`, `parq_one`, `tcp_budget_us`, `rescue_apply`, `send_nacks`; la bajada con la FEC v3: `d_rep_take`, `d_harq_one`, `d_harq_service`, `d_send_asks`, `parq_out`, `nack_out`. `server.c`: `down_seal`, `down_retx_masks`, `send_nacks`, `harq_one`, `harq_service`, `harq_wait`, `harq_budget_us`, `rep_seal`, `nack_send`, `parq_send`; `on_parq` (la PARQ de la bajada).
