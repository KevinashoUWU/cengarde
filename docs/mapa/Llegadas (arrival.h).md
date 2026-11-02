---
tags: [mapa, pieza]
archivos:
  - engine/src/arrival.h
  - engine/tests/test_arrival.c
sello: 1819b06c9b0253e3
---
# Llegadas (arrival.h)

**Grupo:** Observar — [[Mapa del motor]]

## Qué hace
Estadísticas de recepción por enlace en un flujo redundante: qué enlace trajo primero cada paquete (`wins`), cuánto atrás llegaron los otros (`lag_us`) y qué enlaces nunca trajeron copia. También la **tardanza conjunta**: para plazos de 50, 100 y 150 ms tras la primera copia, un histograma por máscara de enlaces tardíos, lo que haría falta para decidir un k-de-N. Las copias duplicadas se anotan sin verificar el MAC: **solo para mostrar**. Solo DATA entra a las casillas; un STREAM (protocolo 13) se cuenta aparte por enlace (`stream`), para que el `rx` de la sonda siga contando todo lo que llegó.

## Lee
- Las primeras copias y los duplicados de [[Camino del router (clientpath.h)]] (cliente) y `listen_read` (servidor), con la hora de cada lote.

## Decide / escribe
- `wins`, `lag_us` en las sondas ([[Sondas y cola de bonding (proto.h)]]) y el estado; los histogramas para `bench/fieldrec.py`.
- Otra: la secuencia más nueva que trajo cada enlace, de cualquier copia (`top`, `cg_arr_top`), y su `last_us` → el juicio de los huecos de TCP en el extremo de salida ([[Liberación TCP (tcpfree.h)]], `tcpe_view`): un hueco del túnel está perdido cuando cada enlace que podía traerlo trajo algo posterior ([[023-reparacion-coherente]]). Las copias tardías cuentan, y no se verifican: un `top` falso solo copiaría una retransmisión.

## Perillas
- [[Mapa/Perillas/tcp_ack_rule|tcp_ack_rule]]: sí (usa `top`).

## Dónde se midió
- [[012-bonding]]: instrumentos pasivos del paso 0 (`joint`, `lost_all`, `sizes`) y el registrador de campo.
- [[011-hilos]]: `skew`, 33 % de primeras llegadas por enlace.

## Choques conocidos
- Con bombas por enlace, el hub puede tomar una copia tardía antes que una temprana ([[Hilos por enlace (thrplan.h)]]).

## Archivos
- `arrival.h`; tests `engine/tests/test_arrival.c`.
- `clientpath.h` (`cg_rx_entry` anota primeras copias y duplicados); `client.c`: `ro_rset`. `server.c`: `listen_read`, `ro_rset`.
