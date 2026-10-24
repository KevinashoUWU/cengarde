# 018 — ns-3 con verdad conocida: auditar los estimadores

- **Fecha:** 2026-10-23 (actualizada el mismo día: las dos reglas, en el motor como estado)
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
  sobre el mínimo de 60 s ≤ 10 ms y lo entregado > 90 % (89 %). Las dos están
  en el motor solo como estado (`linksig.h`, `sig` por enlace): por
  `capacity.h`, 86 % y 94,5 % sobre las mismas trazas, y leen bien en el
  laboratorio (casos `sigrand`, `sigdeep`, `sigothers`, n = 6).
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
  solo (tarea #59). **Hecho como estado** (2026-10-23, `linksig.h`, ver
  Cambios): ninguna decisión del motor las lee todavía; falta mirarlas en
  terreno antes de que el modelo `auto` las use.

## Cambios

- 2026-10-23: creada.
- 2026-10-23: las dos reglas en el motor, solo en el estado (`engine/src/linksig.h`,
  `sig` por enlace en el cliente y en el servidor; nota del mapa
  «Observaciones del enlace»). Se alimentan una vez por tick de lo que ya mide
  `capacity.h` (`queue_us`, `ql_us`, la entrega y la pérdida de cada muestra);
  estado fijo, sin perillas.
  - **`queue_owner`**: un episodio empieza cuando `queue_us` pasa 20 ms tras
    1,5 s sin pasarlo; la entrega de las muestras desde 0,8 s antes hasta 0,5 s
    después del inicio contra la de 2–4 s antes: cae > 15 % → `others`, si no
    `mine`. El motor ve la cola ~0,7 s tarde (la respuesta de la sonda viene
    detrás de la cola y el retardo está suavizado): con «el segundo siguiente»
    acertaba 82 %, con la ventana corrida 86 %. El veredicto dura mientras haya
    cola sobre el mínimo de la ventana o del minuto (una cola que dura más que
    la ventana se vuelve su mínimo, C5) y 10 s más.
  - **`loss_kind`**: una muestra con pérdida de más de 5 por mil es `random` si
    `ql_us` no pasó de 10 ms en ella y perdió menos de 10 %; si no,
    `congestive`. Cambia con dos muestras con pérdida seguidas que coinciden; una
    sin pérdida corta la racha (en el laboratorio, el estimador de pérdida da
    alguna muestra de hasta 1,4 % en un enlace limpio, y dos sueltas lo volvían
    `congestive`). Vuelve a `none` a los 10 s sin pérdida. Lo entregado se mide
    con la pérdida de la muestra y no con entrega/envío: en ns-3 da lo mismo
    (94,5 frente a 94,7 %), y en el laboratorio el cociente de tasas de una
    muestra oscila ±10 % y volvía `congestive` a un enlace con 2 % aleatorio.
  - **ns-3** (`~/sdr/ns3/engranes/harness/sigreplay.c` y `sigeval.py`: las
    trazas por `cg_cap_tick` y `linksig.h`, contra las etiquetas de `t34.py`).
    `queue_owner`: 86,0 % en los 207 episodios que el motor decide (otros 71/71,
    míos 107/136); 112 episodios sin cola de 20 ms para el motor, 93 de ellos con
    búfer de 10 KB a 12,6 Mbit/s (cola ≤ 6 ms: pierde sin cola); 55,6 % contando
    todos esos como fallos. El veredicto llega
    1,2 s (mediana; p90 1,8 s) después del inicio verdadero. Con `ql_us` en vez
    de `queue_us` para el inicio, igual (85,6 %). `loss_kind`: 94,5 % en las
    2530 ventanas con una muestra con pérdida de más de 5 por mil (aleatorias
    696/763, congestión 1696/1767), 80,6 % sobre las 2967; con 3 por mil,
    93,1 %; subir la cola a 15 o 20 ms baja a 85 %. Sostenido (20–60 s): iid
    1–3 % `random` el 100 % del tiempo; Gilbert-Elliott 64 % `random`, 19 %
    `congestive` (las ráfagas de 40 ms pierden > 10 % en una muestra), 17 %
    `none`; congestión con 10 KB o 512 KB, 97–98 % `congestive`; el búfer corto
    a tasa alta, 49 % `congestive` y 51 % `none`. En los tramos limpios de CBR,
    0 muestras con pérdida.
  - **Laboratorio** (`bench/lab.d/bond.sh`, casos nuevos `sigrand`, `sigdeep` y
    `sigothers`; subida, Cubic, 30 s, n = 6, `labq.py -j 4`; `bondtab.py` suma
    `sig` por enlace: la fracción de muestras de la segunda mitad con cada
    valor y los veredictos de la corrida). `sigrand` (enlace 1 con 2 %
    aleatorio y 3,7 ms de cola, flujo de 20 Mbit/s): `random` 99 % del tiempo,
    0 episodios; el enlace limpio, `none` 100 %. `sigdeep` (10 Mbit/s tras 1 s
    de cola, 50 % descartado por netem; y 20 Mbit/s tras 200 ms): enlace 1
    `mine` 100 % y `congestive` 100 %, 1 episodio por corrida; enlace 2 `mine`
    100 %, 3–4 episodios, `none` (pierde 0,05 %). `sigothers` (flujo de
    8 Mbit/s; a la mitad, el enlace 1 baja de 20 a 5 Mbit/s): un episodio
    `others` por corrida (6/6) justo al bajar, `others` 93 % de la segunda mitad,
    `congestive` 94 %; antes, 2–3 episodios `mine` por las ráfagas del propio
    flujo; el enlace 2, `unknown` y `none`.
  - Falta en terreno: el lazo cerrado (si el motor le quita tráfico a un enlace
    al ver su cola, su entrega baja y puede leerse `others`), el jitter de
    Starlink contra los 10 ms de cola (en el laboratorio un flujo al 40 % ya
    lleva `ql_us` a 4–12 ms) y celdas con otros usuarios de verdad.
