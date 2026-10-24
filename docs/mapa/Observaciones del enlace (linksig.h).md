---
tags: [mapa, pieza]
archivos:
  - engine/src/linksig.h
  - engine/tests/test_linksig.c
sello: bf1f17f0ad9a4fd3
---
# Observaciones del enlace (linksig.h)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
Dos lecturas por enlace, **solo para el estado** (ninguna decisión del motor las usa todavía), sacadas de la verdad conocida de ns-3 ([[018-ns3-verdad-conocida]]):

- **`queue_owner`** (`mine` / `others` / `unknown`): cuando la cola sobre el mínimo de la ventana (`queue_us`) pasa 20 ms tras 1,5 s sin pasarla, empieza un episodio. La entrega del segundo alrededor del inicio (muestras desde 0,8 s antes hasta 0,5 s después: la cola se ve ~0,7 s tarde) contra la de 2–4 s antes: si cayó **más de 15 %**, la cola es **de otros** (la celda le da menos); si no, **mía** (un enlace lleno sigue entregando su capacidad). Sin entrega de referencia (enlace de menos de 4 s, o quieto entonces), o sin muestras en 3 s: `unknown`. El veredicto dura mientras haya cola de más de 20 ms sobre el mínimo de la ventana o del minuto (`ql_us`: una cola que dura más que la ventana se vuelve su mínimo) y 10 s más; luego vuelve a `unknown`. Uno nuevo lo reemplaza.
- **`loss_kind`** (`random` / `congestive` / `none`): una muestra de capacidad que pierde más de 5 por mil es **aleatoria** si la cola sobre el mínimo de 60 s (`ql_us`, su máximo en los ticks de la muestra) no pasó de 10 ms y perdió menos de 10 % (Starlink: pierde sin cola); si no, **de congestión** (un celular pierde cuando su cola honda se llena, o un búfer corto a tasa alta pierde mucho). Cambia con 2 muestras con pérdida seguidas que coinciden (una sin pérdida corta la racha; una sin pérdida conocida, no); vuelve a `none` 10 s después de la última pérdida.

Estado fijo por enlace (un anillo de 16 muestras de entrega, sin `malloc`), alimentado una vez por tick (`cg_sig_cap`) después de `cg_cap_tick`; nada por paquete.

## Lee
- De [[Capacidad (capacity.h)]]: `queue_us`, `ql_us`, la hora de la muestra (`t_ms`), `last_rx_bps`, `loss_pm` y `loss_pm_ms`. La cola viene de [[Cola por enlace (cg_gov_base)]].

## Decide / escribe
- Nada del motor. `sig` por enlace en el estado (`cg_json_sig`, en `status.c`): `queue_owner`, `loss_kind`, `delivery_drop_pct` (del último veredicto), `since_ms` y `loss_since_ms` (cuánto lleva cada uno; null si nunca cambió) los conteos `episodes_mine`, `episodes_others`, `episodes_unknown`, `samples_random`, `samples_congestive`, y `last_loss_pm` y `last_ql_ms` (la última muestra con pérdida conocida). En el cliente, la subida de cada enlace; en el servidor, la bajada de cada camino.

## Perillas
- Ninguna: los umbrales son los de ns-3 (`CG_SIG_*`).

## Dónde se midió
- [[018-ns3-verdad-conocida]], Cambios del 2026-10-23: las trazas de ns-3 pasadas por `cg_cap_tick` y `linksig.h` (`~/sdr/ns3/engranes/harness/sigreplay.c`, `sigeval.py`). `queue_owner`: 86,0 % de acierto en los 207 episodios que el motor decide (otros 71/71, míos 107/136), frente al 88 % de la regla en Python; 112 episodios sin cola de 20 ms para el motor, 93 de ellos con búfer de 10 KB a 12,6 Mbit/s (cola ≤ 6 ms: pierde sin cola). `loss_kind`: 94,5 % en las 2530 ventanas con una muestra con pérdida de más de 5 por mil (89,2 % la regla en Python; 80,6 % sobre las 2967).
- Laboratorio de bonding (`bench/lab.d/bond.sh`, casos `sigrand`, `sigdeep`, `sigothers`, n = 6): 2 % aleatorio sin cola → `random` 99 % del tiempo; cola honda llena por nuestro flujo → `mine` y `congestive`; el enlace que baja de 20 a 5 Mbit/s con flujo fijo → un episodio `others` por corrida (6/6); los enlaces limpios, `unknown` y `none`.

## Choques conocidos
- Lazo cerrado: si el motor le quita tráfico a un enlace al ver su cola (capacidad, G4), su entrega baja y puede leerse como `others`. En ns-3 (lazo abierto) no se ve.
- Un búfer corto a tasa alta (10 KB a 12,6 Mbit/s) pierde sin cola: `loss_kind` lo da `congestive` solo cuando pierde 10 % o más (ns-3: 49 % del tiempo, 51 % `none`), y `queue_owner` no ve episodio.
- El jitter: con un flujo al 40 % del enlace, `ql_us` ya llega a 4–12 ms en el laboratorio; el umbral de 10 ms es el de ns-3, sin desvanecimiento.
- El estimador de pérdida de `capacity.h` da alguna muestra de hasta 1,4 % en un enlace limpio (laboratorio): por eso 5 por mil y la racha que corta una muestra sin pérdida.
- `delivery_drop_pct` se queda con el último veredicto decidido aunque `queue_owner` vuelva a `unknown`.

## Archivos
- `linksig.h`; tests `engine/tests/test_linksig.c`.
- `client.c`: `cap_tick` (`cg_sig_cap`), el estado por enlace. `server.c`: `cap_tick`, el estado por camino. `status.c`: `cg_json_sig`.
