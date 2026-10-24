---
tags: [mapa, pieza]
archivos:
  - engine/src/spare.h
  - engine/tests/test_spare.c
sello: b71c5af953d99ad1
---
# Lo que sobra (spare.h)

**Grupo:** Decidir · Recuperar — [[Mapa del motor]]

## Qué hace
En redundancia (o con un [[Enlace dominante (dominant.h)]] a un lado), protege al **primer enlace** con lo que les sobra a los demás, sin llenarles la cola. Cada enlace que no es el primero tiene una **asignación** (una tasa) que sigue a su cola sobre el mínimo de 60 s: baja un cuarto si pasa de `redund_spare_ms` (como mucho cada 500 ms, porque la cola se ve con un viaje de atraso) y sube un dieciseisavo más 64 kbit/s por tick si está bajo la mitad y la asignación está en uso; nunca más de 3/2 de lo que sube el túnel ni menos de 128 kbit/s. Un balde a esa tasa decide cada paquete: uno chico (< 300 B) va siempre y se cuenta; uno prioritario (con `tcp_aware`, los ACK y las retransmisiones de TCP, `CG_F_FREE`) puede dejar el balde hasta una ráfaga en negativo; el resto, solo lo que hay. Según el modo, lo que entra en el balde son **copias** (copias parciales), **paridad** (un XOR de `fec.h` sobre k paquetes que fueron solo por el primero; k el menor cuya paridad cabe en 7/8 de la asignación, de 2 a 15) o la **mezcla** (copias si la asignación lleva todo o si el primero pierde más de 5 % en su última muestra, un microcorte; paridad si no).

El **primer enlace** (`cg_spare_first`) es el de menor retardo sin cola (el mínimo de 10 s de [[Cola por enlace (cg_gov_base)]], relativo al menor) entre los que no tienen una cola parada (sobre el mínimo de 60 s, como mucho 4 × `redund_spare_ms`); se queda con 5 ms de histéresis. Uno que como primero junta una cola así queda **vetado** 60 s (el primero lleva todo, y uno ahogado tomado por primero se llena al instante). En un microcorte suyo (pérdida > 5 % en una muestra) y 3 s después se queda aunque la salud lo saque un momento. **No** decide la capacidad (un enlace que solo lleva lo que sobra mide solo eso: con paridad, el ahogado tomado por primero al arrancar quedaba ahí para siempre, 0 Mbit/s en el laboratorio) ni la llegada esperada de cada paquete del [[ETA (eta.h)]] (se daba vuelta al ahogado unos paquetes por tick); ver [[020-copias-parciales-y-paridad]], «Tropiezos». En todos los modos, cuando la asignación lleva toda la subida van copias (cuestan lo que una paridad de 1 y mantienen al enlace midiendo).

## Lee
- La cola sobre el mínimo de 60 s de cada enlace (`ql_us`), su mínimo de 10 s (`q`, [[Cola por enlace (cg_gov_base)]]) y su capacidad (solo para la primera asignación), de [[Capacidad (capacity.h)]]; la pérdida de la última muestra del primero (`loss_pm`).
- La instancia de [[Copias a tiempo (eta.h)]] (`c->pace`), que sigue activa con la perilla, para la máscara de copias.
- Lo que sube el túnel (bytes de DATA por tick, suavizado 1/4).
- La máscara de copias de [[Copias a tiempo (eta.h)]] (`cmask`, con `redund_pace_ms` si está) y la marca `CG_F_FREE` de [[Liberación TCP (tcpfree.h)]].

## Decide / escribe
- La máscara final de cada paquete (`cmask`) → `send_links`.
- Con paridad, el grupo (`cg_fec_grp` de [[FEC (fec.h)]]) y su paridad `CG_T_FEC` por el enlace fuera del grupo con más lugar en su balde (`fec_send`); el servidor la usa como cualquier paridad (necesita `fec = yes`) y entrega lo rehecho → [[Reordenador (reorder.h)]].
- Estado: `upload.spare` (`mode`, `now`, `k`, `first`, `data_mbit`, `copies`, `copies_skipped`, `fed`, `parities`, `parities_skipped`, `parities_no_link`) y por enlace `spare` (`mbit`, `cuts`, `taken`, `skipped`).

## Perillas
- [[Mapa/Perillas/redund_spare|redund_spare]]: `no`; `copies`, `parity` o `mix` (cliente).
- [[Mapa/Perillas/redund_spare_ms|redund_spare_ms]]: 50; 1–2000.
- [[Mapa/Perillas/fec|fec]] en el servidor (para usar la paridad) y [[Mapa/Perillas/fec_wait_ms|fec_wait_ms]] (el plazo del grupo).

## Dónde se midió
- [[020-copias-parciales-y-paridad]]: celda virtual (WOM y Entel B2 como enlace ahogado, un enlace fijo con pérdida como Starlink) y laboratorio de bonding (`protect`, `protectcut`).

## Choques conocidos
- La paridad XOR rehace uno por grupo: no sirve en un microcorte (50–88 % perdido); para eso la mezcla vuelve a copias, que cubren lo que cabe en el enlace ahogado.
- Lo rehecho llega con la paridad, detrás de la cola del enlace que la lleva: en redundancia el reordenador no espera, y sale fuera de orden.
- Solo la subida (cliente): la bajada la decide el VPS y no tiene la perilla.

## Archivos
- `spare.h`; tests `engine/tests/test_spare.c`.
- `client.c`: `spare_masks` (en `send_links`), `spare_feed`, `spare_close`, `spare_tick` (en `cap_tick`), `fec_service` y `fec_timeout` (el plazo del grupo).
