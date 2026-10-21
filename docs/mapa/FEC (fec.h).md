---
tags: [mapa, pieza]
archivos:
  - engine/src/fec.h
  - engine/tests/test_fec.c
sello: 6774eb5315b72b74
---
# FEC (fec.h)

**Grupo:** Recuperar — [[Mapa del motor]]

## Qué hace
Una paridad XOR «espacial» sobre un grupo de DATA repartidos, como mucho uno por enlace; la paridad va por un enlace de fuera del grupo. Así el atasco de un enlace deja cada grupo con un paquete de menos, que la paridad reconstruye apenas llega el resto (código (n+1, n)). El emisor manda cada DATA al instante y lo suma al grupo abierto más viejo sin nada de su enlace (hasta 32 grupos abiertos). Un grupo cierra con k paquetes, al pasar 512 secuencias, o en su plazo; un grupo de uno no manda paridad. El receptor reconstruye antes de que el rescate pida.

## Lee
- Lo que el [[Reparto (split.h)]] mandó por un solo enlace, y los enlaces portadores.

## Decide / escribe
- Paridades `CG_T_FEC` por el enlace vivo de menor RTT fuera del grupo (`fec_send`).
- `cg_fec_share_mask` (con `fec_share_max`) recorta los elegibles del [[Reparto (split.h)]].
- El receptor entrega lo reconstruido como si hubiera llegado → [[Reordenador (reorder.h)]]; lo que no puede, lo pide el [[Rescate (rescue.h)]].

## Perillas
- [[Mapa/Perillas/fec|fec]]: no (reinicia; en los dos extremos).
- [[Mapa/Perillas/fec_wait_ms|fec_wait_ms]]: 10; 1–200.
- [[Mapa/Perillas/fec_share_max|fec_share_max]]: 0 (sin tope); hasta 100.

## Dónde se midió
- `fec.h`: con la traza de 120 s de los cuatro módems, las 29 esperas de más de 100 ms del reparto pasaron a ninguna, con un tercio más de bytes; duplicar cuesta tres veces más.
- [[engine-README]]: con `fec_share_max` 50 en la red de pruebas (RTMP de 8 Mbit/s), más paridad y de 3 a 12 veces las retransmisiones.

## Choques conocidos
- C10: la paridad elige enlace por RTT, no por OWD, y no se cuenta en el ETA ni en `cake` ([[audit-pending]]).
- C13: bajo `split_stress_single`, la FEC solo arma grupos de uno.
- Si el reparto manda casi todo por un enlace (88 % en el caso `lossy`), casi todos los grupos son de uno (sin paridad); los cubre el rescate.

## Archivos
- `fec.h`; tests `engine/tests/test_fec.c`.
- `client.c`: `fec_feed`, `fec_close`, `fec_send`, `fec_service`. `server.c`: `fec_rx_new`, `fec_try`, `fec_deliver`.
