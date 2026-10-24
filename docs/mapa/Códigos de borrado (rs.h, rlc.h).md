---
tags: [mapa, pieza]
archivos:
  - engine/src/gf256.h
  - engine/src/repair.h
  - engine/src/rlc.h
  - engine/src/rs.h
  - engine/src/rs.c
  - engine/tests/test_rlc.c
  - engine/tests/test_rs.c
sello: eb87ad6bf0a93999
---
# Códigos de borrado (rs.h, rlc.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Los códigos de `redund_spare = rs | rlc` ([[021-fec-v2]]): en redundancia, protegen lo que fue **solo por el primer enlace** con **reparaciones** (`CG_T_REPAIR`, protocolo 8) que van en lo que les sobra a los demás ([[Lo que sobra (spare.h)]]). Dos códigos sobre GF(2^8):

- **Reed-Solomon en bloques** (`rs.h`, `rs.c`, sobre **Leopard-RS**, `engine/third_party/leopard`, BSD-3, compilado sin libstdc++): k paquetes fuente (dentro de 64 secuencias) y m reparaciones; cualquier k de los k + m devuelven los k (MDS). El emisor copia cada miembro al pasar y calcula las m reparaciones al cerrar el bloque (k miembros, o `redund_spare_code_ms` desde el primero, o un miembro que no cabe en el mapa). Cada símbolo es la longitud del paquete (16 bits) y su carga, rellenado al más largo del bloque redondeado a 64 bytes (lo pide Leopard). Restricciones de Leopard: k + m ≤ 256 y m ≤ k (aquí k ≤ 64, m ≤ 32); sus búferes de trabajo los reserva quien llama, una vez (`CG_RS_WORK_ENC`, `CG_RS_WORK_DEC`). El receptor guarda las reparaciones de un bloque con miembros que faltan (un fondo de 64) hasta tener tantas como faltan y entonces decodifica (`cg_rs_decode`, con el `cg_rs_scratch` del servidor).
- **RLC de ventana deslizante** (`rlc.h`, sobre `gf256.h`, tablas propias): cada reparación es una combinación aleatoria de los últimos W paquetes (coeficientes de una semilla de 16 bits en la cabecera), una cada k/m paquetes (un crédito en 1/256). Una pérdida espera la próxima reparación, no el fin de un bloque. El receptor guarda cada reparación con miembros que faltan como una ecuación (resta al llegar lo que ya tiene, y lo que llega después), elimina coeficientes (hasta 32 ecuaciones sobre 96 incógnitas) en cada cambio y rehace cada incógnita que queda sola en una fila.

Los dos receptores usan el anillo de DATA de la [[FEC (fec.h)]] del servidor (1024 entradas, `cg_rep_ring`), escriben ahí lo que rehacen y lo entregan como la paridad (`fec_deliver`). Una reparación vieja (más de 384 secuencias detrás de la más nueva) se suelta.

## Lee
- Del cliente: el plan del tick de [[Lo que sobra (spare.h)]] (`cg_spare_plan`: k, m o la tasa) y los paquetes que `spare_masks` dejó solo en el primero.
- Del servidor: el anillo de DATA de `fec` (necesita `fec = yes`).

## Decide / escribe
- Reparaciones `CG_T_REPAIR` (`repair.h`: base, mapa de 64 bits de los miembros, tipo, m, índice o semilla, el símbolo) por el enlace, no el primero, con más balde, como toma prioritaria (`code_send`); se cuentan en `tx_fec_packets`/`tx_fec_bytes` del enlace, no como DATA (como las paridades).
- Estado del cliente: `upload.spare.code` (`k`, `m`, `need`, `pps`, `loss_avg_pm`, `repairs`, `repair_bytes`, `skipped`, `no_link`, `failed`). Del servidor, por sesión: `repair` (`recovered`, `unused`, `malformed`, y `rs`/`rlc`: `repairs`, `useless`, `kept` o `decodes`/`failed`, `recovered`, `dropped`, `bad`).
- Lo rehecho → [[Reordenador (reorder.h)]] (en redundancia no espera).

## Perillas
- [[Mapa/Perillas/redund_spare|redund_spare]]: `rs` o `rlc` (cliente; el servidor con `fec = yes`).
- [[Mapa/Perillas/redund_spare_code_ms|redund_spare_code_ms]]: 30; 1–500.

## Dónde se midió
- Costo (`bench/fecbench.c`, x86 de este PC, paquetes de 1200 B, [[021-fec-v2]]): codificar 0,17 µs por paquete con Leopard SSSE3 (k 21, m 3), 0,72 µs con su camino portable, 1,5 µs con la RLC (W 21, 3 por 21) y 0,04 µs con el XOR; decodificar con pérdida 0,6, 2,2, 0,9 y 0,36 µs. A 10 Mbit/s, todo bajo 0,3 % de un núcleo; falta medirlo en la Pi y el NUC.
- [[021-fec-v2]], laboratorio (`protect`, n = 6–8): la pérdida de `mix` con un tercio de los bytes de respaldo (0,17–0,18 contra 0,49), y TCP de 43 (`rlc`) y 36 (`rs`) Mbit/s contra 16 (`mix`); en el microcorte, como `mix`. Celda: sin corte, `rlc` 0,000 % y rehecho en 12 ms (`rs` 0,003 % y 28 ms; `mix` 0,014 % y 13 ms); Entel B2 fuera del corte, 0,005–0,015 % contra 0,071 (`mix`).

## La paridad pedida (PARQ, [[023-reparacion-coherente]])
- `cg_rlc_rx_need` da el déficit de rango de las ecuaciones que echan de menos un hueco (y las que comparten incógnitas, dentro de 64 secuencias); `cg_rlc_combine` arma una combinación sobre miembros dados (el anillo de rescate del emisor, no su ventana).
- Un bloque RS declara en sus reparaciones su geometría `cg_rs_mhdr(k, m)` (el doble de m redondeado a potencia de dos, dentro de k y 32), no las m que manda: los índices de arriba quedan para pedirlos; `cg_rs_rx_need` da el déficit de un bloque y `cg_parq_rs_pick` los índices que el receptor no tiene. Lo arma [[Reparación coherente (harq.h)]].

## Choques conocidos
- En un microcorte que el código no cubre (la pérdida pide más reparaciones de las que caben), el plan vuelve a copias, como `mix`.
- El camino portable de Leopard (armhf, MIPS) tenía un error en el original (`RefMul` sumaba): corregido en la copia vendorizada; ver `engine/third_party/leopard/README.md`.
- La reparación de un bloque RS lleva el símbolo redondeado a 64 bytes y 16 de cabecera: con DATA de 1420 B el datagrama pasa de 1500 (1540 con IP y UDP: fragmenta, como la paridad XOR con sus 35 de cabecera, 1507). La de la RLC no se redondea (1490: cabe).

## Archivos
- `gf256.h`, `repair.h`, `rlc.h`, `rs.h`, `rs.c`; tests `engine/tests/test_rlc.c`, `engine/tests/test_rs.c`; costo `bench/fecbench.c`.
- `client.c`: `code_feed`, `code_send`, `code_rs_close`, `code_rlc_repair`, `code_service`, `code_due`; `server.c`: `rep_rx_new`, `rep_take`, `rep_data`, `rep_deliver`.
