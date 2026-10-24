# 018 — ns-3 con verdad conocida: auditar los estimadores

- **Fecha:** 2026-10-23
- **Estado:** vigente
- **Fuentes:** ns-3.48 con LTE (LENA) y 5G-LENA nr v5.1 en `~/sdr/ns3/` del PC
  del banco: `escenarios/` (`lte-ul-probe.cc`, `sweep.sh`, `README.md`) y
  `engranes/` (`cg-trace.cc`, `harness/replay.c`, `resultados/`, `README.md`,
  633 corridas).

## TL;DR

- ns-3 sabe qué pasa en la celda (SINR, carga de otros, búfer): sirve para medir
  si los estimadores del motor aciertan, pasándole sus trazas a las funciones
  puras (`health.h`, `split.h`, `govern.h`, `capacity.h`) en lazo abierto.
- **La capacidad se sobreestima +275 a +510 % durante 40 s** con búfer corto y un
  emisor que se adapta (TCP): ningún disparador actúa. El prototipo `lossq`
  (pérdida ≥ 1 % y cola sobre el mínimo de 60 s ≥ 10 ms, dos muestras) lo corrige
  en 1,0–1,4 s sin dispararse con pérdida aleatoria.
- **Choque C5 medido:** la cola fija se vuelve base a los 10 s y el arranque rápido
  devuelve +523 %. El prototipo `stand` (sin arranque rápido mientras haya cola
  sobre el mínimo largo), junto a `lossq`, deja +3 %.
- «¿La cola es mía o de otros?»: si la entrega cae > 15 % frente a la de 2–4 s
  antes, es de otros (88 % de acierto). Firma de pérdida: aleatoria si la cola
  sobre el mínimo de 60 s ≤ 10 ms y lo entregado > 90 % (89 %).
- Dos enlaces en la misma celda sin desvanecimiento pierden los mismos paquetes;
  conviene copiar entre operadores o bandas distintas.

## Hallazgos

### Primer barrido (escenarios, 720 corridas)

Celda FDD de 15 MHz; nuestro UE a 1 Mb/s con eco; 3 UE cargando la subida.
Solo en la celda, el RTT queda en ~18 ms hasta −2 dB y se corta a −5 dB (sin el
colapso de HARQ de srsRAN, historia 016). Con la celda llena, el reparto casi
igual de PRB a MCS 2 da ~0,7 Mb/s: entre 2 y −2 dB, 25–54 % de pérdida y
113–184 ms. Segundos de RTT solo con búfer hondo (512 KB: 2,4–8 s). PF ≈ RR.

### Estimadores (engranes, n = 3 semillas, perfil `default`)

- Con búfer de 512 KB y tasa constante: cola > 50 ms en 0,4–0,8 s, corte `full`
  en 1,0–1,6 s, G4 en 0,4–0,8 s, silencio en 2,8 s; 0 falsas alarmas.
- Con búfer de 10 KB nunca se silencia (la cola máxima 25–114 ms queda bajo
  150 ms) aunque se pierda el 88 %.
- `mute80` (80 ms, espera 1 s) silencia en 1,6–1,8 s sin silencios falsos.
- Mantener `capacity_sat_ms` 20 y `governor_window_ms` 10 s; `capacity_loss_pct`
  en 0 o condicionado a la cola.

### Cola honda (360 corridas)

Sobre la capacidad, RTT ≈ búfer / C y pérdida ≈ 1 − C / tasa. Receta para la
celda virtual: Entel B2 ≈ cola de 1,2–1,5 s de su capacidad (~200 KB a
1,2 Mb/s) con descarte por cola llena y carga de 1,05–1,25 × C; WOM ≈ C de
~5 Mb/s con 1–2 MB.

### Misma celda o celdas distintas

Sin desvanecimiento, mismo SINR y misma celda: φ = 1 (pierden lo mismo); en
celdas distintas φ ≈ 0,05. Con desvanecimiento EPA, φ ≈ 0 en ambos, la misma
celda algo peor (pérdida conjunta 1,3 contra 0,9 veces la independiente).

## Límites

Una antena; scheduler casi igualitario; SINR de banda completa, no el del módem
por antena; sin desvanecimiento es determinista; búfer RLC con descarte por cola
llena, sin AQM; el arnés es de lazo abierto, con referencias ideales y modelo
`auto`.

## Qué hacemos con esto

- `lossq` y `stand` pasan al motor como perillas, validadas en la celda virtual
  calibrada con la cola honda y en el laboratorio de bonding.
- La celda virtual (`bench/celda/`, historia 016) suma la cola honda de la receta.
- Las reglas de «cola mía o de otros» y de firma de pérdida, primero como
  observación en el estado, son la base para que el modelo de enlace se detecte
  solo (tarea #59).

## Cambios

- 2026-10-23: creada.
