---
tags: [mapa, pieza]
archivos:
  - engine/src/tickle.h
  - engine/tests/test_tickle.c
sello: b9f384883487c2db
---
# Cosquilla (tickle.h)

**Grupo:** Medir · Decidir — [[Mapa del motor]]

## Qué hace
«Cosquilla» traduce *tickle* (el código dice *tickle*; el nombre en español es del mapa). Como el ProbeBW de BBR: el [[ETA (eta.h)]] no le manda nada a un enlace mientras los rápidos tengan cola corta, y un enlace sin carga nunca se vuelve a medir. Cada `every_us`, el siguiente enlace que lo necesita (medido pero sin carga desde hace 3 s) recibe durante 600 ms (`CG_TICKLE_WIN_US`) los paquetes repartidos, hasta una tasa: 5/4 de su capacidad y 1/16 de la del mayor, al menos 1 Mbit/s. La ventana cubre una muestra de capacidad. Si la capacidad no creció 1/8, la siguiente espera el doble, hasta 2^5 veces.

## Lee
- `cg_cap_tickle` de [[Capacidad (capacity.h)]]: qué enlaces tocan (`cg_cap_stale_p`), su tasa y lo que `cg_cap_probe` les ofrece.

## Decide / escribe
- El enlace en cosquilla toma primero el paquete en [[Reparto (split.h)]] (`cg_split_pick_eta` con `pr`), mientras dure su cubeta.
- `cg_tickle_backoff` → [[Capacidad (capacity.h)]] y [[ETA (eta.h)]]: un enlace en espera tampoco crece con `split_stale_max`.

## Perillas
- [[Mapa/Perillas/split_tickle_ms|split_tickle_ms]]: 0 (nunca); 0–60000. Solo con [[Mapa/Perillas/split_eta|split_eta]].

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: WOM, con n78 detrás, llevó 0 % de un stream RTMP durante minutos con 9,5 Mbit/s medidos una vez.
- `tickle.h`: en el laboratorio (`het`, 10 Mbit/s detrás de 60 ms) una cosquilla cada 2 s costaba 3–6 % del caudal y 268 abandonos RACK; de ahí la espera doble.

## Choques conocidos
- C4: la ventana de 600 ms frente a muestras de ≥ 500 ms alineadas a los informes: solo ~0–20 % de las cosquillas cubren una muestra entera; puede explicar el «no creció» y la espera. Su período de 2 s resuena con G4 y el silenciado ([[audit-pending]], tarea #50).
- C13: bajo `split_stress_single`, la cosquilla no corre.

## Archivos
- `tickle.h`; tests `engine/tests/test_tickle.c`.
- `client.c` y `server.c`: `cap_tick` (`cg_tickle_tick`), y el envío.
