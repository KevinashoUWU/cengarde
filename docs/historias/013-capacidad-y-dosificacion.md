# 013 — Capacidad por enlace y dosificación (paso 4 del bonding)

- **Fecha:** 2026-10-14
- **Estado:** vigente. PR 1 hecho (PR #32, 0.6.5): el plazo del reordenador según la oscilación de cada enlace. PR 2 hecho (PR #33, 0.6.6): la capacidad de cada enlace. PR 3 (PR #34, 0.6.7): los pesos según la capacidad (`capacity_weights`), +50 % en la bajada (n = 5, p = 0,075). Medidos en la red de pruebas. PR 4 (PR #35, 0.6.8): `shape_mbit` y `bench/shaper.sh`, experimental; todavía cuesta ~40 % de la bajada. 2026-10-16: la vuelta rápida de la capacidad, sin rebote por vaciado y `split_predict` (opcional), por el interferente controlado. Ese día, más tarde: `split_predict` amortiguado (`predict.h`), porque en el campo movía el tráfico más seguido.
- **Fuentes:**
  - las medidas del 2026-10-14 en el banco y en la Pi, en [012](012-bonding.md) («El gobernador en el banco…», «El gobernador en la bajada y `cake`…», «… en la Pi»);
  - código leído: `engine/src/reorder.h` (plazo de un hueco, líneas 70–80 del comentario), `govern.h`, `split.h`, `health.h`, `proto.h` (`struct cg_probe_info`: `rx` por camino), `clientpath.h`, `server.c` (`peer_asks`, `apply_config`);
  - medidas de esta historia: contadores del reordenador del VPS y de la Pi (`reorder.lost`, `reorder.late_by`), contadores `nft` en el PC, la Pi y el VPS, `nstat` en los dos extremos de TCP; scripts en `~/campo/2026-10-14/` (fuera del repo: llevan direcciones).

## TL;DR

- **Los enlaces no pierden paquetes; el reordenador los da por perdidos antes de tiempo** [medido]. En la subida, por enlace, lo que la Pi envía llega entero al VPS (0,0–0,2 %). Pero el reordenador del VPS daba por perdido el 4–8 % de lo repartido (`lost.cap`), y todos esos paquetes llegaban igual (`late.after_cap` es idéntico): el 60 % menos de 10 ms tarde y el 90 % menos de 50 ms tarde. En la bajada, el de la Pi lo hace con el 10 %, con una cola más larga (hasta 500 ms). TCP los recibe desordenados y retransmite.
- **Con `reorder_margin_ms = 50` en el VPS**, `lost.cap` baja a 0,2–3 % y la subida de la Pi pasa de 16–17 a 33–48 Mbit/s en las mismas rondas [medido, n = 3 por lado]. Quedó así en la red de pruebas. Un margen fijo es un parche: el plazo tiene que seguir la variación de retardo de cada enlace (**primer PR**).
- **El gobernador por cola sirve en la bajada** (+22 % a +108 %; los enlaces bajan entre 20 y 230 Mbit/s), **no en la subida** (los cuatro suben parecido). Un `cake` con tope fijo cuesta 35–40 % del caudal de bajada, porque la capacidad cambia con la hora. Lo que falta es **la capacidad de cada enlace**, medida, para repartir según ella y fijar el tope de un `cake` que la siga.
- **La capacidad sale de lo que el otro extremo dice haber recibido** por cada camino (`rx` en cada sonda, ya en el protocolo v4): no hace falta cambiar el formato.
- **La cola queda en `cake`, no en el motor ni en el módem:** el motor calcula los pesos del reparto y el tope total, y un `cake` sobre la interfaz del túnel (router: subida; VPS: bajada) lo aplica, con su reparto justo entre flujos y su AQM. El motor sigue sin colas propias ni bloqueos (reglas del plano de datos). Dosificar cada enlace dentro del motor queda para después, solo si esto no alcanza.
- **Abierto:** un equipo de la LAN que sube con Cubic rinde la mitad que la Pi misma, sin pérdidas en ningún tramo medido (LAN, Pi, enlaces, entrada del VPS) y con retransmisiones que no son espurias. Con `fq` o BBR en el emisor, casi se iguala.

## Contexto

El paso 2 del plan de la Fase 5 (historia 012) fue el gobernador: los pesos del reparto según la cola de cada enlace. En el banco y en la Pi (2026-10-14) mejora la bajada y no la subida, y no responde lo que el usuario quiere: que el bonding «se note». En la misma jornada aparecieron tres cosas que este paso tiene que resolver:

1. un `cake` con tope fijo baja el ping pero cuesta caudal, porque los enlaces entregan entre 85 y 230 Mbit/s según la hora;
2. un equipo de la LAN sube mucho peor que la Pi misma;
3. al buscar dónde se perdían sus paquetes, los enlaces resultaron no perder nada: el que «perdía» era el reordenador.

## Hallazgos

### El reordenador se rinde antes de tiempo [medido]

El plazo de un hueco es `dl = min(s.ts + M + margen, s.t_arr + C)`, con `M` el mayor retardo de los enlaces (una media móvil de ganancia 1/8) y el margen de 10 ms (`reorder.h`). Con carga, el retardo de un enlace celular salta por encima de su media en decenas de milisegundos, y el paquete del hueco llega después del plazo.

| Extremo | Repartidos | `lost.cap` | Llegaron después (`late.after_cap`) | Tardanza de esos |
| --- | --- | --- | --- | --- |
| VPS (subida de la Pi, por corrida de 15 s) | 18–55 mil | 4–8 % | todos | 60 % < 10 ms, 81 % < 20, 91 % < 50 |
| Pi (bajada, acumulado del día) | 5,6 M | 10 % | 88 % | 30 % < 10 ms, 64 % < 50, 80 % < 100, 2 % > 500 |

Con el margen en 50 ms en el VPS (alternando 10 y 50, 3 rondas cada uno):

| Margen | `lost.cap` | Subida de la Pi (Mbit/s) | Subida del PC por la Pi |
| --- | --- | --- | --- |
| 10 ms | 4,4–7,6 % | 17,1 / 16,4 / 33,2 | 7,5 / 7,0 / 11,6 |
| 50 ms | 0,2–3,1 % | 47,6 / 32,9 / 37,6 | 8,4 / 12,0 / 12,5 |

Esperar más tiene un costo solo cuando un paquete se pierde de verdad, y eso es raro: los enlaces entregan el 99,8–100 %.

### Dónde no se pierden los paquetes del PC [medido]

Una subida desde el PC del banco, como equipo de la LAN detrás de la Pi (Cubic, 4 flujos, 15 s), contada en cada tramo:

- **PC → Pi:** los bytes que salen por `eno2` y los que entran a la Pi por `br-lan` coinciden.
- **Pi → túnel:** sin descartes en `wgcg`, `br-lan`, `eth1` ni en el motor (`tx_drops`).
- **Enlaces:** lo enviado por cada uno y lo que el VPS dice haber recibido coinciden.
- **Túnel → VPS:** los bytes en `cg-router` coinciden con los que salieron de la Pi.
- **VPS:** la regla `INVALID` de ufw no cuenta nada nuevo.

Aun así, el PC retransmite 270–400 segmentos por corrida. Solo 3 son espurios (DSACK), hay 11 timeouts y hasta los SYN se retransmiten. Con `fq` en la interfaz del PC la subida pasa de 15 a 29 Mbit/s, y con BBR a 19–45 (la Pi misma, 35–40). [PENDIENTE: el tramo de vuelta, los ACK repartidos por la bajada y reordenados en la Pi; ver qué hace TCP del PC con esos ACK.]

### De dónde sale la capacidad

Cada sonda lleva `rx`: los paquetes verificados que ese extremo recibió por ese camino (`proto.h`, `struct cg_probe_info`). El otro extremo sabe cuántos paquetes y bytes envió por ese camino, así que:

- **entregado por el enlace** = Δ`rx` entre dos sondas;
- **bytes entregados** ≈ Δ`rx` × el tamaño medio de lo enviado por ese enlace en ese intervalo;
- **ritmo de entrega** = bytes entregados / el tiempo entre las dos sondas, medido con el reloj del otro extremo. Las sondas van cada 100 ms con tráfico.

El ritmo de entrega es la capacidad solo cuando el enlace va lleno; si se le manda menos, mide lo que se le mandó. La cola (el OWD sobre su mínimo, `govern.h`) dice cuál de los dos casos es: con cola creciendo, el enlace va lleno.

### PR 1, medido [red de pruebas, 2026-10-14 de tarde]

El plazo según la oscilación de cada enlace (PR #32, 0.6.5), en los dos
extremos:

- **Subida (el reordenador del VPS),** alternando la 0.6.4 con margen 10 y
  50 ms y la nueva con 10 ms, 3 corridas de 15 s cada una:

  | VPS | `lost.cap` (Pi / PC) | Subida de la Pi (Mbit/s) |
  | --- | --- | --- |
  | 0.6.4, margen 10 | 3,9–6,3 % / 7,5–8,8 % | 26,4 / 39,2 / 35,9 |
  | 0.6.4, margen 50 | 0,6–0,9 % / 0,7–1,1 % | 42,5 / 43,5 / 49,3 |
  | nueva, margen 10 | 0,15–0,6 % / 0,2–1,05 % | 52,6 / 46,8 / 42,3 |

  Cumple el criterio (< 1 %) sin margen fijo: la espera sobre la media
  (`rx_owd_swing_ms`) va de 8 a 80 ms según el enlace y la hora. El ping
  no cuenta aquí: va en paquetes chicos, que el reordenador no retiene.
  El PC por la Pi sigue en 11–15 Mbit/s con las tres (su problema es
  otro, arriba).
- **Bajada (el reordenador de la Pi):** con la 0.6.4, `lost.cap` de 6,3–12 %;
  con el plazo nuevo, 2,6–7 %. La mitad de lo que quedaba llegaba menos
  de 1 ms tarde: con `link_threads`, el hilo central juzgaba los plazos
  con la hora del principio de la pasada, mientras el paquete del hueco
  esperaba en el anillo de otro enlace. Juzgados por la lectura más vieja
  que aún espera, esos casi desaparecen (8375 → 1739 por corrida) y queda
  2,6–6,4 %. El contador nuevo por enlace (`rx_reorder_late`) dice de
  dónde viene el resto: de Movistar (19 % de lo que trae llega tarde,
  con colas de 0,4–1,3 s y fuera del reparto) y de Claro (6 %, cuando su
  retardo salta de 5 a 650 ms). No es oscilación sino colas que se
  llenan de golpe: lo que ya iba en camino llega más tarde que
  `reorder_max_wait_ms`. Eso no lo arregla esperar más sino no llenar el
  enlace: es el PR 3 (pesos según la capacidad).
- La bajada se queda en 120–175 Mbit/s con la 0.6.4 y con la nueva: la
  pérdida falsa que queda está en los enlaces que se atoran, no en el
  plazo.

### PR 2, medido [red de pruebas, 2026-10-14 de tarde]

El estimador de capacidad (PR #33, 0.6.6), validado con un enlace por
vez dentro del túnel (los otros tres en pausa con `cengarde ctl link
NOMBRE off`), 15 s de subida y 15 s de bajada cada uno, con el estado de
los dos extremos leído cada ~1 s:

- **La primera versión sobreestimaba:** con muestras de 250 ms y su
  máximo, la capacidad salía 1,5–2 veces lo que el enlace llevaba. Ahora
  cada muestra cubre al menos 500 ms, el ritmo se suaviza (un cuarto de
  cada muestra nueva) y la capacidad es el máximo de eso.
- **Dos fallas corregidas:**
  - un enlace que vacía su cola entrega en un intervalo más de lo que se
    le mandó en él; el estimador lo tomaba como contadores nuevos y
    reiniciaba. Ahora compara los totales;
  - copiando por los cuatro, un enlace con segundos de atraso solo trae
    copias viejas que el receptor descarta antes de contarlas: para medir
    capacidad, el enlace tiene que llevar paquetes propios (repartir o
    ir solo).

| Enlace | Bajada: túnel / capacidad | Subida: lo que entregó el enlace / capacidad |
| --- | --- | --- |
| Claro | 81,9 / 90,8 (+11 %) | 18,8 / 17,2 |
| Entel | 35,6 / 35,8 (+1 %) | 18,6 / 18,1 |
| Movistar | 6,5 / 5,3 (−19 %) | 25,5 / 25,2 |
| WOM | 97,0 / 103,4 (+7 %) | 32,3 / 31,3 |

En la subida, el iperf del túnel dio menos (10–24 Mbit/s) porque por el
mismo túnel pasaba otro tráfico de la Pi: lo que cuenta es lo que el
enlace llevó (sus bytes enviados y lo que el VPS dice haber recibido,
que coinciden). Contra eso, el estimador queda dentro del 1–9 %. Cumple
el criterio de la historia (±20 %).

### PR 3, medido [red de pruebas, 2026-10-14 al anochecer]

`capacity_weights` en el VPS (la bajada de la Pi, repartida), contra el
gobernador solo que tenía la red de pruebas; 5 rondas alternadas de 30 s:

| Ronda | Gobernador (A) | Gobernador + `capacity_weights` (B) |
| --- | --- | --- |
| 1 | 60,8 | 107,6 |
| 2 | 46,6 | 102,1 |
| 3 | 55,6 | 55,2 |
| 4 | 48,1 | 85,4 |
| 5 | 61,6 | 59,5 |
| media | 54,5 Mbit/s | 82,0 Mbit/s (+50 %; Mann-Whitney unilateral p = 0,075) |

- B gana tres rondas por mucho y empata dos; el ping por el túnel, igual
  (medias 38–48 ms); `lost.cap` en la Pi, 0,8–5,1 % en B frente a
  1,0–7,5 % en A.
- Lo que hace: WOM (capacidad 45–87 Mbit/s) lleva el peso mayor, Claro
  (25–38) la mitad o menos, Movistar (1–9) un 2–14 %, y Entel, al piso.
  Con el gobernador solo, Claro y WOM quedan casi parejos aunque uno
  entrega el doble.
- **En la subida** (la Pi con y sin `capacity_weights`, el VPS con los
  suyos; 5 rondas de 30 s): 30,0 / 42,8 / 46,3 / 51,6 / 46,4 Mbit/s sin,
  41,1 / 47,8 / 59,2 / 48,9 / 44,9 con (medias 43,4 y 48,4: +11 %,
  p = 0,27). Poco, como el gobernador: los cuatro enlaces suben parecido.
  No empeora nada; queda encendido en los dos extremos de la red de
  pruebas.
- Entel estaba enfermo en ese momento (bloqueado en la PCI 37: 20 % de
  pérdida y 60–250 ms de ping en reposo; capacidad medida 0–3 Mbit/s): los
  pesos por capacidad lo dejaron al piso, que es lo esperado.

### PR 4, primeras medidas [red de pruebas, 2026-10-14 de noche]

`shape_mbit` (PR #35) aplicado por `bench/shaper.sh` como `cake` en
`cg-router` del VPS (la bajada de la Pi), contra sin `cake`; 4 rondas
alternadas de 30 s, con la Pi y el VPS con `capacity_weights`:

- **Primera regla (85 % de la suma de capacidades): se hunde sola.** Detrás
  de un `cake` a ese ritmo los enlaces no entregan más, su capacidad pasa a
  ser eso y el ritmo siguiente es el 85 % de eso; en reposo publicaba casi
  0. Rehecha como `cake-autorate`: nada hasta que hay cola; entonces 9/10
  de lo entregado; sube mientras va lleno sin cola.
- **Segunda y tercera: arrancaban en pocos Mbit/s.** El retardo de un
  enlace en reposo oscila sobre su mínimo, y la cola de un solo enlace lento
  (Movistar, con los pesos por capacidad) bastaba para bajar el ritmo de
  todo el túnel. Ahora baja solo cuando los enlaces con cola llevan la
  mayor parte de lo entregado y eso pasa de 2 Mbit/s, y sube 1/32 por tick.
- **Con eso:** bajada 134 / 102 / 94 / 99 Mbit/s sin `cake` (media 107) y
  73 / 73 / 33 / 69 con él (media 62); ping p90 59–73 ms sin, 45–58 con; p99
  66–88 sin, 46–63 con. **No cumple** el criterio (≥ 90 % del caudal): el
  ritmo oscila entre 80 y 125 Mbit/s, porque cada vez que aparece cola baja
  al 90 % de lo entregado, y lo entregado ya viene frenado por el propio
  `cake`. [PENDIENTE: bajar menos (95 %), solo con cola sostenida, y seguir
  lo entregado sin suavizar al subir; o dejar el tope fijo con margen y
  ajustar solo hacia abajo cuando los enlaces se degradan.]

### Cuánto del techo usa el túnel, y por qué no más [red de pruebas, 2026-10-14 de noche]

- **El techo, medido al mismo tiempo:** los cuatro enlaces cargados a la
  vez fuera del túnel dan 70–80 Mbit/s de subida y 156–326 de bajada
  (según la hora). Por el túnel de la Pi, al principio: 30–31 de subida
  (~40 %) y 73–104 de bajada (~47–50 %).
- **La capacidad se retroalimentaba (PR #36):** con `capacity_weights`, un
  enlace con poca parte entregaba poco, su capacidad (el máximo de la
  ventana) bajaba a eso y su parte se achicaba: dos o tres enlaces al piso
  (Claro y Entel a 0,1 Mbit/s en la subida, WOM a 1–5 en la bajada). Ahora
  la capacidad se mantiene y solo una cola la baja, y un enlace sin cola
  recibe además 1/16 de la mayor para mostrar lo que da. Con eso el túnel
  usa los cuatro: bajada 175–187 contra 326 (54–57 %), subida ~49 %.
- **Los enlaces no pierden; TCP retransmite por desorden:** en la bajada,
  contando en reposo antes y después, los enlaces entregan el 99,5–100 %
  (Movistar 95–99 % sobre una parte chica), pero TCP retransmite el 3–6 %
  y el 72 % de eso es espurio (DSACK). Un margen de 100 ms en el
  reordenador de la Pi baja los huecos soltados a ~1 % pero también el
  caudal (124–126 contra 139–143) y no las espurias: esperar más agrega
  retardo variable. Con 16 o 32 flujos TCP en vez de 4 el túnel no sube
  (113–158): el límite no es la dinámica de pocos flujos.
- **`split_eta` (PR #37):** cada paquete al enlace donde llega antes. En
  simulación, 1,2 % de los paquetes fuera de orden contra 79 % por pesos;
  en la red de pruebas, +5 % (171 contra 163, n = 4, sin significancia) y
  el reordenador de la Pi retiene lo mismo (77–82 %). Los retardos sin
  cola de los enlaces difieren solo 2–6 ms: lo que desordena son colas que
  cambian más rápido que las medidas cada 100 ms. Queda experimental.
- **`pacing` (PR #38):** el socket de cada enlace dosificado
  (`SO_MAX_PACING_RATE`, `fq` en `eth0` de la Pi). A 5/4 de la capacidad
  se hundió (subida a 0–2 Mbit/s: tras un reinicio la capacidad es casi
  nada, las sondas van por el mismo socket y la capacidad tras la
  dosificación se quedaba ahí). Al doble de la capacidad con piso de 20
  Mbit/s: subida 33,5 / 31,7 / 36,6 / 35,8 contra 22,7 / 30,7 / 35,3 / 39,9
  sin (medias 34,4 y 32,2, n = 4, sin significancia); las retransmisiones
  de la Pi bajan a la mitad (0,2–0,6 % contra 0,7–1,7 %, DSACK 0–0,3 %
  contra 0,5–1,5 %), pero el reordenador del VPS retiene lo mismo
  (65–74 %). Queda experimental. [PENDIENTE: dónde está el resto del techo
  en la subida, con pocas retransmisiones y enlaces que no se llenan.]
- **Buscando el freno de la subida** (con un agente que revisó el código y
  medidas en vivo):
  - el código del cliente no tiene topes: lee 512 datagramas por despertar
    y nunca espera al enviar;
  - la CPU de la Pi, 9–16 % por núcleo; sin descartes en UDP, `fq` ni en
    el motor; los enlaces pasan paquetes de 1500 bytes (sin
    fragmentación); TCP con RTT medio de 150–175 ms (mínimo ~60) por las
    colas de WOM y Entel (89–114 ms) mientras Movistar, con poca cola, se
    usa a la mitad: el reparto por capacidad sobrecarga a los que se llenan
    y la capacidad de los otros crece lento. El gobernador encima de los
    pesos por capacidad: 41 / 24 / 35 contra 33 / 20 / 33 Mbit/s (+16 %, sin
    significancia: los enlaces variaban mucho);
  - UDP por el túnel: la subida se queda en ~47 Mbit/s pidiendo 70 o 100;
    la bajada a 100 Mbit/s pierde 11–31 %, en parte porque un enlace con la
    cola llena (Claro, luego Movistar) sigue recibiendo su parte y todo lo
    suyo llega tarde para el anti-replay (`too_old`), y en parte porque a
    esa hora los enlaces daban menos de 100 en total (colas de 0,4–2,4 s).
    Unos 4000 errores de buffer de recepción en UDP por prueba en la Pi
    (2500 en el VPS) no se van con `rmem_default` de 4 MB.
  - [PENDIENTE: repetir de día con el techo medido al mismo tiempo; un
    enlace cuyas respuestas no vuelven no debe conservar su parte.]
- **Relojes:** chrony en el VPS y en la Pi contra `ntp.shoa.cl` (SHOA,
  stratum 1; quedan en stratum 2, ±2 ms el VPS y ±18 ms la Pi, que
  sincroniza por el túnel); `sysntpd` apagado en la Pi.

### Ramas de antena y CSI [medido, 2026-10-14]

Con `AT+QRSRP`/`AT+QSINR` (por rama) y el reporte CSI activado
(`AT+QNWCFG="csi_ctrl",1,1` en los cuatro módems; `lte_csi` y `nr5g_csi`
dan MCS, RI, CQI y PMI):

- en reposo, Entel y Claro muestran −140 en las ramas 3 y 4: el módem
  las apaga sin tráfico; con carga vuelven las cuatro. Para ver una
  antena rota hay que medir con carga;
- Entel, con carga: la rama principal 15–18 dB bajo las otras tres
  (−86 a −89 frente a −71 a −81 dBm) y SINR 2–10 frente a 9–20. Engancha
  un monoposte a ~100 m (PCI 151/152, sin 5G) que alterna; hay una
  macrocelda a ~1 km con 5G, 256QAM de subida y 4x4. [PROBAR el bloqueo
  de PCI a la macro y antenas más grandes; medir antes y después];
- WOM y Claro, parejas; Movistar engancha NR además de LTE, con RSRQ −20
  en las cuatro ramas de LTE (celda cargada).
- **Bloqueo de PCI de Entel** (`AT+QNWLOCK="common/4g",4,675,P,1125,P,3100,P,9485,P`
  y reinicio de la interfaz, no del módem; Entel solo, 15 s por sentido):

  | Celda | Ancla | 5G | Bajada | Subida |
  | --- | --- | --- | --- | --- |
  | monoposte, PCI 152 (sin bloqueo) | B7 −78 dBm + B2 + B2 | no | 27,4 | 15,3 |
  | macro, PCI 37 | B7 −96 dBm, SINR −3 a −5; B2 en la PCI 15 (SINR −10) | NSA n78, PCI 27 | 51,8 y 32,2 | 14,5 y 10,0 |
  | PCI 36 | no aparece en esas frecuencias: cae a 3G | — | — | — |
  | otra macro, PCI 15 | B28 −104 dBm | NSA n78, PCI 27 | 24,8 | 2,4 |

  Con la 37 el 5G engancha al instante (MCS 9–30, rank 2) y la bajada
  sube cuando el NR da bien, pero la subida sigue por LTE B7, que en la
  macro llega débil: no mejora. Con n = 2 no se puede decir que la 37 le
  gane al monoposte. Queda puesta la 37 (decisión del usuario), a la
  espera de las antenas más grandes, que deberían subir el LTE de la
  macro.

### 2026-10-14/15: ETA, dosificación, MPTCP, speedtest y la capacidad con cola

Medido en la red de pruebas con las celdas bloqueadas (historia 004); TCP
con 4 flujos y 20 s, 3 rondas alternadas, salvo que se diga otra cosa.

| A/B | Sin | Con | Lectura |
| --- | --- | --- | --- |
| `split_eta`, bajada | media 109 | media 157 | +44 % |
| `split_eta`, subida | media 56 | media 70 | +25 % |
| `pacing`, subida | media 57 | media 40 | empeora: queda experimental y apagado |
| `reorder_max_wait_ms` 500 frente a 150, bajada | 103 | 99,5 | igual |

- **El falso hundimiento después de MPTCP no era cengarde.** La subida por
  el túnel caía a 0,2–3 Mbit/s tras una o dos corridas MPTCP, y volvía en
  30–60 s. Ninguno de los dos motores perdía nada; la captura en `cg-router`
  mostró que el receptor TCP del VPS anunciaba ventanas de 6 KB y de 0. Los
  sockets MPTCP, al cerrarse, retenían hasta 180 MB de memoria TCP, y el VPS
  de 2 GB entra en presión de memoria con `tcp_mem` a ~111 MB. Con
  `tcp_mem` en 384/512/768 MB (`/etc/sysctl.d/90-cengarde-tcpmem.conf`)
  desaparece. Solo afecta a las pruebas que terminan en el VPS: en uso real
  el VPS reenvía con NAT.
- **MPTCP (fuera del túnel, 4 subflujos) frente a cengarde con ETA**, ya
  sin ese efecto: subida pareja (MPTCP 59–84, cengarde 53–64), bajada para
  cengarde (128–148 contra 56–58). Se queda el reparto por paquete.
- **speedtest** (Ookla, servidor EdgeUno de Santiago), desde la LAN de la
  Pi una vez arreglada (historia 004: el adaptador USB y el control de
  flujo), y por el túnel propio del PC del banco con el mismo motor:
  - Subida 60–91 Mbit/s: la suma de los enlaces medidos solos (~94). Un
    notebook en la LAN midió 200/100 contra Grupo GTD.
  - Bajada 93–132 en speedtest y 196 en iperf3, contra ~389 de la suma de
    los enlaces solos (Entel 93, WOM 66, Movistar 63, Claro 167 con tres
    portadoras). Con carga, el ping del túnel queda en 60–90 ms; el de cada
    enlace solo, en 300–760 ms.
  - Reparto contra duplicación en la bajada: 66–70 frente a 63–75, igual.
    El desorden del reparto no es lo que la limita.
  - Con 16 flujos (`iperf3 -R -P 16`) la bajada se vuelve inestable
    (179 → 86 → 16 → 69). En el VPS, las colas suben a 30–80 ms y la
    capacidad del mejor enlace cae de 72 a 27 Mbit/s: con la cola sobre
    `sat_us`, la capacidad pasaba a ser el ritmo de entrega suavizado,
    que arrastra las muestras de antes de la cola, cuando los flujos aún
    aceleraban.
- **La capacidad con cola (PR 5, `capacity.h`).** Un enlace lleno se mide
  por lo que entrega mientras está lleno: su propio promedio de las muestras
  con cola, que parte de la segunda (en la primera la cola se formó a
  mitad de muestra), y lleno recién con 2 muestras seguidas sobre `sat_us`
  (un vaivén del retardo celular no lo vacía). Tests nuevos:
  `test_capacity_ramp` (de 5 a 80 sobre un enlace de 50: queda en 50, no en
  los 18–26 del promedio) y `test_capacity_swing`. En el laboratorio, con
  16 flujos (`BOND_P=16`), da lo mismo que antes (het: 51,5 frente a 50,7
  de bajada; bloat: 25,6 frente a 26,3): sus enlaces tienen tasa fija y la
  rampa pesa poco.
- **El PR 5 en la red de pruebas (2026-10-15): neutro, la hipótesis no se
  confirmó.** Con la misma medición que la línea base de las 3:33, a las
  6:23 da lo mismo: túnel de bajada 202 / 229 (antes 225 / 198), speedtest
  157 / 193 / 131 (antes 140 / 170 / 160). El A/B intercalado en el VPS
  (el motor anterior y el nuevo alternados, 16 flujos de bajada por el
  túnel del PC, 20 s, 3 rondas) da 320 de media con el anterior (344, 325,
  290) y 312 con el nuevo (332, 281, 325), con caídas parecidas. La
  capacidad que caía con la cola era real, pero no era lo que limitaba la
  bajada: con 16 flujos, la misma prueba que anoche daba 104–119 hoy da
  280–344, porque los enlaces dan más (sumados solos: 446–568 contra
  414–443; Movistar 123 de bajada contra 63–77). Queda desplegado: no
  empeora nada y un vaivén del retardo ya no vacía la capacidad.
- **De dónde salen las retransmisiones de la bajada** (16 flujos, 20 s,
  2026-10-15 en la mañana). En el VPS: 7.099 retransmisiones de 318.268
  segmentos (2,2 %), solo ~21 % espurias (DSACK). En el cliente:
  `lost_all` 5.404 (1,7 %, pérdida real en las colas celulares) y 15.225
  huecos abandonados por el reordenador (RACK 5.423, plazo 8.285, desborde
  1.517), de los que 9.821 llegaron después, 4.426 con más de 500 ms. Por
  enlace, Claro lleva el 59 % y la mayoría de los tardíos (8,5 % de lo
  suyo); Movistar llegó a 2,6 s de cola en el VPS con la capacidad
  estimada en 0–22. El reordenador no es la causa: los paquetes llegan
  medio segundo tarde porque a algunos enlaces se les mete más de lo que
  drenan, y esperar más solo sumaría latencia.
- **Silenciar al atrasado no ayuda:** `mute_behind_ms` 0 contra 150 en el
  VPS, intercalado, 3 rondas: 167,8 contra 150,7 de media. Queda en 0.
- **UDP contra TCP fuera del túnel** (por si un operador limita el UDP):
  Movistar TCP 52,7, UDP a 30 Mbit/s 27,6 con 2,6 % de pérdida; Entel TCP
  36,8, UDP a 60 Mbit/s 57,0 con 1,8 %. Sin evidencia de un límite al UDP.
  El VPS abre 5201–5204/udp (reservados con `forward reserve`).
- **Lo que queda:** dosificar en el servidor cada enlace a su capacidad, para
  que la cola se forme en el VPS y no en la celda. En el servidor todos los
  enlaces comparten un socket, y `SO_MAX_PACING_RATE` es por socket: hace
  falta la hora de salida por paquete (`SO_TXTIME` y `fq`, que traba los
  enlaces entre sí en un mismo flujo) o un socket por enlace. En la subida,
  la dosificación del cliente empeoró (arriba); en la bajada no se ha
  probado.
- **La dosificación en el servidor (prototipo, rama `claude/server-pacing`):
  neutra.** Con 8 lanes, la bajada de cada ruta ya sale por su propio
  socket (`down_send`: la lane `p & (lanes − 1)`), así que basta
  `SO_MAX_PACING_RATE` en cada lane, a la suma de `cg_pace_rate` de las
  rutas que salen por ella (`cg_pace_add` en `pace.h`), con `fq` en la
  salida del VPS. El `fq` por omisión trae `flow_limit 100p`, y cada lane
  es un solo flujo: descartó 3.285 paquetes hasta subirlo a 2000. A/B
  intercalado con el mismo `fq` en los dos lados, 16 flujos de bajada, 20 s,
  3 rondas, con la red de la mañana congestionada: 82,5 sin dosificar
  contra 80,4 dosificando, con retransmisiones parecidas. No se mergea:
  dosificar al doble de la capacidad no evita las colas celulares de
  segundos, que crecen cuando un enlace baja de golpe y los paquetes ya
  están en camino.

### 2026-10-15: la ventana del reordenador (PR #46)

- **El desborde era el reordenador, no la red.** A ~27.000 paquetes por
  segundo, la ventana de 4.096 posiciones cubría ~150 ms: de 635 mil
  huecos abandonados en el PC, 231 mil fueron desbordes, y el 43 % de los
  tardíos llegó menos de 1 ms después de abandonado su hueco. La ventana
  anti-replay de 8.192 además rechazaba como viejo un paquete 300 ms tarde.
- **PR #46:** `CG_RO_SLOTS` 16.384 y `CG_REPLAY_BITS` 32.768 (~600 ms a esa
  tasa; un reordenador pasa de ~81 a ~322 KiB), con el pool en 32 MB.
- **A/B intercalado en el cliente del PC** (16 flujos de bajada, 20 s, 3
  rondas):

  | Variante | Mbit/s (media) | Retransmisiones | Desbordes |
  | --- | --- | --- | --- |
  | `master`, plazo 500 ms | 152,3 (133, 181, 143) | 16.922 | 1.468–15.972 |
  | ventana nueva, plazo 500 ms | 124,2 (125, 131, 117) | 5.445 | 0 |
  | `master`, plazo 500 ms (2.ª tanda) | 152,6 (158, 164, 136) | 9.227 | 823–11.715 |
  | ventana nueva, plazo 150 ms | 140,4 (112, 150, 160) | 12.698 | 0 |

  La ventana grande quita los desbordes y, con el plazo de 500 ms, baja
  las retransmisiones a un tercio, pero rinde menos: el reordenador
  retiene ahora hasta el plazo, y TCP ve un RTT más largo. El desborde
  soltaba los huecos a los ~150 ms y TCP avanzaba antes, aun
  retransmitiendo más. Con 150 ms tampoco gana. **Lo que manda el
  rendimiento es cuánto se retiene, no cuántas retransmisiones hay**: el
  PR queda abierto, sin mergear, hasta probar un plazo que siga al
  enlace (el desvío de cada uno, `CG_RO_DEV_K`) en vez de uno fijo.
- **El plazo por enlace (PR #47, protocolo v5, `reorder_by_link`):** cada
  paquete dice por qué enlace salió la secuencia anterior cuando salió por
  uno solo (`CG_F_PREV`), y el reordenador espera un hueco lo que tarda ese
  enlace, no el más lento, y lo suelta apenas ese enlace trae una
  secuencia posterior (RACK por enlace). A/B intercalado en el cliente del
  PC, ventana de 16.384 y pool de 32 MB en los dos lados, 16 flujos de
  bajada, 20 s, 3 rondas: 151,2 sin la opción (145, 150, 159) contra 141,5
  con ella (148, 137, 139). Suelta antes, como se esperaba (abandonos por
  RACK 8–16 mil contra 5–11 mil), pero ni el rendimiento ni las
  retransmisiones mejoran. Queda apagada por omisión; el protocolo v5 queda
  desplegado.
- **Lo que dicen las tres pruebas del reordenador juntas** (ventana, plazo
  fijo más corto, plazo por enlace): con 16 flujos y la red de la mañana,
  ninguna saca al túnel del rango de 125–160 Mbit/s, y la variación entre
  rondas iguales (±15 %) es mayor que cualquier efecto. El reordenador ya no
  parece ser lo que limita la bajada; lo que queda sin tocar es la pérdida
  real (`lost_all`, 1–11 mil paquetes por corrida de 20 s), que es lo que
  ataca el ARQ de rescate (historia 012: estilo XLINK, reinyectar por el
  enlace más rápido cuando el plazo se agota).

### 2026-10-15: el rescate (PR #48, ARQ estilo XLINK)

- **Qué es:** el reordenador del cliente pide (`CG_T_NACK`) el hueco que va
  a abandonar; el VPS guarda lo enviado (`rescue_kb`, `rescue.h`) y lo
  reenvía por su ruta más rápida, con presupuesto (`rescue_pct`). No se
  rescata la pérdida en una ruta con la cola llena (congestión: TCP tiene
  que verla), y las respuestas lo avisan por ruta (`CG_BOND_FULL`).
- **Laboratorio** (4 flujos Cubic de bajada, 3 rondas): con 1 % de pérdida
  al azar por enlace (`lossy`), de 10–11 a ~50 Mbit/s y de ~1 % a 0,04 % de
  retransmisiones; donde la pérdida es de un enlace lleno (`het`), cuesta
  ~15 %. Tres errores de diseño, medidos y corregidos: pedir solo en la
  cabeza serializaba los pedidos (esperas de 200–500 ms; ahora se pide en
  cuanto el enlace del hueco trae algo posterior); rescatar la congestión
  dejaba a TCP sin frenar (esperas de segundos); y un hueco pedido podía
  esperar más que el tope del reordenador.
- **Red de pruebas** (A/B intercalado en el cliente del PC, 16 flujos de
  bajada, 20 s, 3 rondas, `rescue_wait_ms` 60 contra 0): 86,4 contra 91,0
  Mbit/s. El VPS recibió ~8.000 pedidos y rechazó 5.143 por congestión y
  1.181 por presupuesto; se rescató el 10–20 % de lo pedido. **Bajo carga,
  la pérdida de la red real es casi toda de colas llenas, no de radio**, y
  esa no conviene taparla. El aviso de ruta llena llega tarde respecto de
  cuándo se llena la cola, así que el cliente pide y espera en vano.
- **Pendiente:** medirlo donde la pérdida es de radio y no de congestión:
  tráfico a tasa fija (un stream de 20–30 Mbit/s, la radio, video), no
  iperf saturando. El rescate de la subida. Queda apagado por omisión.

### 2026-10-15: MPEG-TS crudo, el margen adaptativo y el rescate de la subida

- **MPEG-TS crudo por UDP** (HEVC NVENC 1080p60 CBR 15 Mbit/s + AAC, 60 s,
  del PC al VPS por su túnel): con `reorder_margin_ms` 10, 106 errores al
  decodificar sin un solo paquete perdido en todos los enlaces: 705 huecos
  abandonados cuyos paquetes llegaron 100–500 ms después (Claro y WOM, ~6
  Mbit/s cada uno, cerca de su capacidad de subida). Con 400, 0 errores. RTMP
  (TCP) con el mismo video pasó limpio: 3.600 cuadros, saltos de a lo más
  17 ms.
- **El margen adaptativo (PR #49, `reorder_adaptive`)** pierde contra el fijo
  de 400 (56 y 116 errores contra 3 y 6): aprende después de equivocarse y
  olvidaba a la mitad cada 5 s, más rápido de lo que vuelven los saltos de
  cola de un módem cargado. Para transmitir, el fijo; el adaptativo pide
  olvidar mucho más lento.
- **El rescate de la subida (PR #50):** en el laboratorio, `lossy` de 10,4 a
  50,7 Mbit/s (94 % rescatado), `het` de 62,6 a 54,3. En el TS crudo de campo
  la pérdida real desaparece (`lost_all` de 96 a 0), pero los errores no
  bajan (10, 24 y 71 en tres corridas: sin rescate, margen 10 + rescate,
  margen 400 + rescate), con Entel anclado en su B2 sola, que es lossy en
  la subida como PCC (medido por el usuario): el HARQ del módem le sube la
  cola, la regla «cola sobre 20 ms = congestión» la toma por congestión y
  no se rescata justo la pérdida de radio. Una corrida por variante con la
  red así no decide nada.
- **La regla de congestión, probada (PR #51, borrador):** una cola pareja
  contra el mínimo de 60 s, y además cerca de su máximo de 60 s (la lectura de
  RED y ECN). En el laboratorio ninguna sirve con TCP saturando: tapar la
  pérdida aleatoria deja que TCP llene el buffer del enlace, y cualquier
  señal de cola lo ve lleno y apaga el rescate (`lossy` vuelve a 11 Mbit/s;
  `het` llegó a 32 con esperas de 2,4 s). El buen `lossy` de la regla
  instantánea venía en parte de que no veía esa cola. **El rescate y TCP
  saturando compiten; su lugar es el tráfico a tasa fija** (la radio, SRT,
  RIST, video, llamadas), donde nadie acelera hasta llenar el buffer. Las
  ráfagas largas de pérdida, que cualquier regla leerá como congestión, son
  de la FEC con HARQ (historia 012). Entel quedó anclado en B7 (3CA: B7 + B2
  + B28), fuera de su B2 lossy.
- **MPEG-TS crudo con rescate, Entel ya en B7** (60 s cada uno, dos rondas
  intercaladas): margen 400 sin rescate, 108 errores en un mal momento
  (`lost_all` 294) y 0 en uno tranquilo; **margen 10 + rescate (60 ms), 0 y
  0**, con todo lo pedido rescatado (26 de 26, 11 de 11) y la menor espera
  (3,0–3,8 % de los retenidos sobre 50 ms, contra 3,5–9,2 % de las otras);
  margen 400 + rescate, 0 y 0. Para transmitir: **margen corto y rescate**,
  no margen largo; ningún margen cubre la pérdida real. Lo siguiente es un
  rescate graduado por enlace (menos presupuesto cuanto más cerca de
  congestionarse), en vez de encendido o apagado.
- **Lo pendiente del rescate:** una regla de congestión que no confunda el
  HARQ con una cola llena (la cola sube y además el enlace entrega al tope de
  su capacidad); NACK por dos caminos, reintento a ~1 RTT, NACK con máscara
  de bits; la copia de rescate por dos caminos.

### 2026-10-15: el arranque rápido (PR #56)

- **El problema [red de pruebas]:** en reposo el VPS estimaba
  [96,7 / 1,4 / 0,3 / 7,9] Mbit/s para la bajada hacia la Pi, y una bajada
  tardaba 3–4 s en darles ~20 a los enlaces; desde el notebook, «varía al
  principio y después arranca como ferrari».
- **El cambio (`capacity.h`):** cada enlace recuerda su mejor capacidad de
  5 min; tras 10 s sin llenarse vuelve al menos a la mitad de ella, y bajo
  esa mitad el reparto le ofrece el doble en vez de 5/4 (el arranque de BBR).
- **Terreno, una sola corrida con B [red de pruebas]:** tras 40 s de reposo
  las capacidades quedaron en 29 / 25 / 18 / 16; bajada 45 en el primer
  segundo, 87 en el 1–3 y 118 de media en 10 s. Sin A intercalado (el
  usuario se llevó la Pi). Más tarde, con la sesión del router en reposo,
  33 y 63: la memoria se sostiene.
- **Laboratorio, A/B ABBAAB [medido]:** `lab.sh bond` con `BOND_WARM=8`,
  `BOND_REST=40` (perillas nuevas), `het` y `bloat`, 4 flujos Cubic,
  `split-sp250-gov`. **Neutro:** `het` bajada, mediana de 10 s 75,2 (A) y
  76,1 (B), primer segundo 52,4 y 53,3; `bloat` bajada 33,4 y 34,2; subida
  sin cambio. El laboratorio no reproduce el enfriamiento: en reposo A casi
  siempre conservó sus capacidades (25–27 en el enlace de 30). En el campo
  las capacidades caían en reposo y aquí no; lo que las bajaba ahí (un
  vaivén del retardo que parece cola, o las rotaciones del máximo) está por
  ver, y con él si `full_ms` llega a quedar quieto 10 s en un enlace celular.
- **Pendiente:** el A/B en la red de pruebas cuando vuelvan los módems
  (`fastab.sh`: reinicio, carga de 8 s, reposo de 40 s y la rampa).

### 2026-10-16: el interferente controlado (la vuelta rápida, el vaciado y `split_predict`)

- **El experimento [terreno, celda de Entel]:** nuestro RTMP de 8 Mbit/s
  subiendo solo por Entel, y un teléfono cargando la misma celda.

  | interferente | cola de Entel | RTT máx. de nuestro TCP (ms) |
  | --- | ---: | ---: |
  | bajada TCP 58–67 Mbit/s | 5–6 ms, sin efecto | 75–113 |
  | subida UDP 5 / 10 | 9 ms | 59–71 |
  | subida TCP ~20 Mbit/s, 10 s | crece lineal hasta 354–502 ms (35–40 ms por s) | 541–757 |
  | subida UDP 20 | 435 ms | 523 |
  | subida UDP 40 | 2023 ms | 3,5 s |
  | subida TCP en ráfagas de 2 s | 107–150 ms | 176–217 |

- **Lo aprendido:** solo daña la carga en el mismo sentido; la celda encola
  (bufferbloat) en vez de tirar, así que la señal temprana es la
  **pendiente** de la cola; el planificador reparte la subida más o menos
  por igual; al parar el interferente la cola se vacía en menos de 0,2 s.
  Nuestro detector de cola cruzó los 50 ms 0,8–2,2 s después de empezar la
  carga, pero la capacidad estimada fue atrasada toda la fase, y tras la de
  40 Mbit/s **se pasó a 24 Mbit/s (de verdad ~9) durante ~10 s** por la
  ráfaga del vaciado. En una corrida RTMP anterior con 4 enlaces, Entel tardó
  ~20 s en recuperar su parte tras cargarse su celda.
- **Por qué, en el código (`capacity.h`):** tras un episodio lleno,
  `cg_cap_best` devolvía la capacidad solo a la mitad de la mejor y solo
  tras 10 s sin llenarse (`CG_CAP_REST_MS`), y después crecía sondeando
  (ganancia 1/4 por muestra de 500 ms): los ~20 s. En `cg_cap_tick`, una
  muestra en la que el enlace entregó más de lo enviado (vaciando su cola)
  subía el máximo: el rebote. Y el reparto (`cg_cap_eta`, `cg_cap_weights`)
  miraba la cola de ahora, con el 5/4 del sondeo para un enlace cuya cola
  estaba subiendo.
- **Los cambios (PR de esta fecha):**
  1. **Vuelta rápida (siempre):** cada enlace guarda la capacidad de antes
     de llenarse (`pre_bps`). Si el episodio (del primer lleno hasta que la
     cola se fue) duró menos de 30 s y la cola sobre el mínimo de 60 s lleva
     1 s bajo `probe_us` (la mitad de `governor_target_ms`; con la cuarta
     parte, los 5–6 ms de Entel en reposo no la dejarían volver) con
     pendiente bajo 5 ms/s, la recupera de una vez. Si se llena de nuevo
     antes de 5 s, la vuelta falló y la siguiente espera 10, 20, 40, hasta
     80 s. En los tests: 80 % de vuelta en ≤ 2,5 s tras 10 s de contienda y
     tras ráfagas de 2 s; un enlace que de verdad bajó de 50 a 13 no
     rebota (con el sondeo su cola nunca queda quieta 1 s); un episodio
     de más de 30 s no vuelve por aquí.
  2. **Sin rebote por vaciado (siempre):** una muestra que entregó más de
     1/16 sobre lo enviado no sube el máximo, y el máximo no sube más de lo
     que entregó la propia muestra (el ritmo suavizado arrastra el vaciado).
     Sigue contando para `delivery_mbit` y, con el enlace lleno, para el
     promedio bajo cola. Con la guarda de otra corrida de contadores
     (`CG_CAP_SLACK`) no choca: compara totales desde el inicio, y un
     vaciado entrega lo enviado antes.
  3. **`split_predict` (opcional, apagado):** con `capacity_weights` o
     `split_eta`, el reparto usa la cola dentro de un segundo (cola +
     pendiente + la mitad de la pendiente de la pendiente, hasta 500 ms
     sobre la cola), y sin el 5/4 del sondeo si esa cola pasa de `probe_us`.
     Solo con la pendiente sobre 10 ms/s durante 4 muestras seguidas, y
     mientras siga positiva. En los tests: con un retardo que oscila al azar
     30 ms cada 100 ms se enciende < 1 % del tiempo; con una cola que crece
     40 ms/s se enciende en 0,6–1 s. El estado muestra `queue_pred_ms`.
- **Laboratorio [medido, `lab.sh interferer`, n = 3–6]:** túnel nativo,
  l1 de 40 Mbit/s y 20 ms con 2 s de cola, l2 de 20 Mbit/s y 30 ms, TCP de
  subida a 24 Mbit/s constantes, la subida de l1 cortada a un tercio
  (`long`: 10 s; `burst`: 3 × 2 s; `drain`: 10 s, 1 s a pleno y 10 s a la
  mitad), base `split = on`, `capacity_weights`, `split_eta`. Medianas:

  | variante | caso | n | off_s | back_s | cap_s | cap_over | cola máx. ms | s > 100 ms | RTT máx. TCP ms | retrans. (mediana; corridas > 400) | ping p99 ms |
  | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
  | master | long | 6 | 0,02 | 0,53 | 2,53 | – | 1602 | 8,5 | 250 | 78; 0/6 | 247 |
  | rama | long | 3 | 0,02 | 0,53 | 2,53 | – | 1602 | 8,5 | 249 | 77; 0/3 | 247 |
  | rama + `split_predict` | long | 3 | 0,02 | 0,53 | 2,53 | – | 1602 | 8,5 | 249 | 37; 0/3 | 248 |
  | master | burst | 3 | 0,02 | 0,42 | 2,52 | – | 538 | 6,2 | 259 | 355; 0/3 | 249 |
  | rama | burst | 3 | 0,02 | 0,41 | 2,52 | – | 682 | 6,6 | 265 | 188; 0/3 | 264 |
  | rama + `split_predict` | burst | 3 | 0,02 | 0,62 | 2,52 | – | 686 | 6,5 | 255 | 258; 0/3 | 241 |
  | master | drain | 6 | 0,02 | 0,03 | 1,53 | 0,95 | 1600 | 18,0 | 278 | 169; 1/6 | 265 |
  | rama | drain | 9 | 0,02 | 0,03 | 1,53 | 0,96 | 1599 | 17,7 | 282 | 247; 4/9 | 266 |
  | rama + `split_predict` | drain | 3 | 0,03 | 0,03 | 1,53 | 0,95 | 1606 | 18,0 | 264 | 159; 1/3 | 264 |

  `off_s`: del corte hasta el primer segundo en que el reparto le ofrece a
  l1 no más que su nuevo ritmo; `back_s`: del fin del corte hasta que le
  ofrece el 80 % de antes; `cap_s`: lo mismo con `capacity_mbit` (resolución
  de 1 s, el estado); `cap_over`: en `drain`, la mayor capacidad con la celda
  a la mitad, sobre esa mitad. El caudal fue 24 Mbit/s en todas.

  - **El laboratorio no reproduce lo que estos cambios arreglan:** con
    master, `capacity_mbit` ya vuelve al 80 % en 1,5–2,5 s y no rebota
    (`cap_over` 0,95). Aquí el `split_eta` le vuelve a cargar l1 apenas
    termina el corte (TCP recupera su atraso), y esa muestra, sin vaciado,
    ya mide 40; en el campo, con 4 enlaces con sitio, Entel quedaba con poca
    parte y solo crecía sondeando. El rebote del campo tampoco aparece: el
    vaciado del laboratorio ocurre con la cola aún sobre `sat_us`, y ahí
    manda el promedio bajo cola.
  - **Lo que sí muestra:** durante el corte el `split_eta` oscila con
    período de ~4 s (le da todo a l1 cuando su cola medida es vieja, nada
    cuando la ve): colas de 1,6–1,8 s y 8–9 s sobre 100 ms en `long`, igual
    en master. Ahí se pierde lo que hay que ganar, y no es la capacidad
    sino la realimentación atrasada de la cola (la sonda llega tarde por
    la cola misma).
  - **Retransmisiones bimodales:** cada corrida cae en un modo de ~30–250
    o en uno de ~450–650 (con la cola de l1 en ~1,8 s en vez de ~1,6). En
    `drain`, master cayó en el alto 1 de 6 veces y la rama 4 de 9; en
    `long` y `burst`, ninguna de las dos. Con n tan chico (Fisher, p ≈ 0,3)
    no se distingue de ruido, pero queda anotado. Una primera versión del
    vaciado que además retenía el promedio bajo cola cayó en el alto 4 de 6
    (y 2 de 4 sola, sin la vuelta rápida; la vuelta rápida sola, 0 de 4): se
    quitó esa parte.
- **Pendiente:** medir en el campo con el mismo interferente (el tiempo de
  vuelta de Entel con 4 enlaces y el rebote tras 40 Mbit/s); ajustar los
  umbrales (abajo); la oscilación de `split_eta` bajo un corte, que el
  laboratorio sí muestra.

### 2026-10-16 (tarde): `split_predict` amortiguado

- **El campo [medido, n = 1 por condición]:** RTMP de 8 Mbit/s de subida
  desde el cliente del PC por el túnel nativo sobre los 4 enlaces celulares,
  de día, 90 s por condición. «Rampas»: la cola de un enlace de < 20 a
  > 80 ms en ≤ 4 s; «saltos»: un enlace gana 15 puntos o más de la parte en
  1 s; después de un salto, ¿subió la cola del que ganó más de 30 ms en 2 s?

  | | rampas | saltos | cola subió tras el salto | rescate pedido | el reordenador se rindió (rack) | RTT máx. |
  | --- | ---: | ---: | --- | ---: | ---: | ---: |
  | master | 1 | 22 | 2/22 | 328 | 10 | 215 ms |
  | PR #67 | 1 | 21 | 1/21 | 312 | 6 | 168 ms |
  | PR #67 (repetición) | 2 | 35 | 7/35 | 434 | 12 | 252 ms |
  | PR #67 + `split_predict` | 4 | **52** | 0/52 | **773** | **36** | 295 ms |

  `split_predict` no sacaba el tráfico de los enlaces a punto de encolarse:
  **lo movía más seguido**, y cada movida desordena las llegadas (huecos,
  NACK del rescate, el reordenador que se rinde). Su predicción se adelantaba
  solo 0–0,4 s a la cola real en las rampas cortas del barrio (picos de
  82–179 ms subiendo en 1–4 s), porque pedía 4 muestras con pendiente
  > 10 ms/s, y su término (pendiente más media segunda derivada, de una
  media exponencial de diferencias) seguía cada oscilación del retardo. El
  caso para el que está es el del interferente: la cola subiendo en línea
  recta 35–40 ms/s durante 10 s hasta 350–500 ms (y 2 s a 40 Mbit/s).
- **El cambio (`predict.h`, puro, solo para `split_predict`; los otros dos
  cambios del PR quedan igual):**
  1. **Una pendiente que mide:** mínimos cuadrados sobre las muestras de
     los últimos 1,3 s (`CG_PRED_WIN_MS`), no la media exponencial: con un
     retardo que oscila al azar 30 ms cada 100 ms, aquella oscila ~45 ms/s
     (desviación estándar) y esta ~6. Sin segunda derivada.
  2. **Solo pendiente fuerte:** se engancha con ≥ 30 ms/s
     (`CG_PRED_ON_S1`) en 2 muestras seguidas. El umbral sale de la tabla:
     las rampas del barrio de 3–4 s hasta ~100 ms suben 20–28 ms/s, el
     interferente 35–40.
  3. **Histéresis:** enganchada, sigue hasta que la pendiente pasa 0,5 s
     bajo 5 ms/s; suelta, no vuelve antes de 5 s (`CG_PRED_HOLD_MS`); un
     enganche de menos de 3 s fue una rampa corta y dobla la espera
     siguiente, hasta 20 s, y se olvida tras 60 s sin enganches.
  4. **Límite de ritmo:** lo que suma a la cola es la pendiente en
     escalones de 20 ms (`CG_PRED_Q_US`, hasta 500 ms), fijado al
     engancharse y cambiado como mucho cada 500 ms; entre escalones el
     reparto ve la cola moverse igual que sin `split_predict`.
  5. **Estado:** por enlace, `predict` (`on`, `add_ms`, `slope_ms_s`,
     `engaged`, `steps`), calculado aunque `split_predict` esté apagado,
     para contarlo en el campo. Sin cambio de formato.
- **Tests unitarios [medido, `test_predict.c`, `test_capacity.c`]:**

  | caso | resultado |
  | --- | --- |
  | oscilación al azar de 30 ms cada 100 ms, 10 min, 5 semillas | 0 enganches |
  | rampa de 40 ms/s bajo 20 ms de oscilación (200 semillas) | se engancha en 1,0 s de mediana, p95 1,2 s, máx. 1,6 s; no se suelta durante la rampa; se suelta ≤ 1,5 s tras vaciarse; una segunda rampa espera 5 s |
  | rampa de 35 ms/s | mediana 1,2 s, p95 1,6 s, máx. 2,0 s |
  | rampa de 4 s hasta ~100 ms (21 ms/s, 500 semillas) | se engancha en el 5 % |
  | rampas de 1, 2 y 3 s hasta ~100 ms | 100, 100 y 90 %: tan empinadas como el comienzo del interferente (28–85 ms/s), nada las distingue en el primer segundo; como mucho 1 enganche y ~4 escalones cada una, suelta ≤ 1,2 s después de bajar la cola |
  | cola oscilando 0–50 ms con período de 2, 4 y 6 s más 20 ms de oscilación, 10 min | 28, 17 y 0 enganches y 67, 34 y 0 escalones, contra 1236, 843 y 321 cambios de ≥ 20 ms del término viejo; los pesos cambian con la predicción no más que sin ella más 2 por enganche |
  | enganches cortos seguidos | la espera pasa de 5 a 10 y a 20 s y queda ahí; tras 60 s sin enganches, o tras un enganche largo, vuelve a 5 s |

  Lo pedido («no se engancha con rampas de 1–4 s hasta ~100 ms») se cumple
  para las de 4 s, no para las de 1–3 s: con 30 ms/s de umbral y ~1 s de
  reacción no se pueden separar de forma causal. Lo que se acota es lo que
  cuestan: un enganche, pocos escalones, y una espera que crece.
- **Laboratorio [medido, `lab.sh interferer`, n = 2–3 por variante,
  intercaladas]:** como arriba, con el rescate y el reordenador del cliente
  del PC (`rescue_kb = 32768`, `rescue_wait_ms = 60`, `reorder_max_wait_ms
  = 500`, `tcp_aware = yes`); «apagado» es este binario sin
  `split_predict`, «viejo» el PR en `900da63` con él, «amortiguado» este con
  él. Caso nuevo `ramps`: 6 cortes parciales de 2 y 3 s cada 7 s. Por
  corrida:

  | caso | variante | saltos | rescate pedido | rack | cola máx. de l1 (ms) | enganches |
  | --- | --- | --- | --- | --- | --- | --- |
  | `ramps`, l1 a 16 Mbit/s | apagado | 32, 30, 32 | 359, 960, 873 | 386, 339, 467 | 621, 1064, 799 | (7, 7, 8) |
  | | viejo | 34, 32, 35 | 593, 895, 1151 | 208, 468, 670 | 751, 799, 811 | – |
  | | amortiguado | 34, 31, 33 | 509, 285, 265 | 261, 111, 274 | 716, 698, 840 | 7, 7, 7 |
  | `ramps`, l1 a 20 Mbit/s | apagado | 6, 32, 6 | 0, 650, 0 | 0, 445, 0 | 220, 504, 247 | (2, 6, 2) |
  | | viejo | 3, 8, 5 | 0 | 0 | 62, 247, 60 | – |
  | | amortiguado | 3, 7, 33 | 0, 0, 356 | 0, 0, 260 | 60, 248, 494 | 1, 2, 6 |
  | `ramps`, 20 Mbit/s, `IF_JITTER_MS = 10` | apagado | 2, 2, 2 | 0 | 0 | 30, 33, 34 | (0) |
  | | viejo | 5, 2, 4 | 0 | 0 | 51, 35, 60 | – |
  | | amortiguado | 5, 6, 2 | 0 | 0 | 54, 244, 36 | 0, 2, 0 |
  | `long` (l1 a un tercio 10 s) | apagado | 14, 15 | 220, 89 | 113, 9 | 1452, 1763 | (3, 3) |
  | | viejo | 14, 14 | 76, 59 | 11, 0 | 1829, 1765 | – |
  | | amortiguado | 15, 15 | 192, 70 | 45, 0 | 1597, 1765 | 3, 3 |

  (Entre paréntesis, los enganches que el estado cuenta con
  `split_predict` apagado: no actúan.)

  - **El laboratorio no reproduce el exceso de saltos del campo:** el
    viejo salta tanto como el apagado (32–35 contra 30–32). Los saltos de
    aquí son la oscilación de `split_eta` bajo un corte (período ~4 s, ya
    anotada arriba), que es igual en las tres variantes, y las corridas son
    bimodales (cada una cae en «l1 nunca se llena», ~0 rescates, o en
    «se llena», cientos). Con 16 Mbit/s el amortiguado pidió menos rescates
    en 2 de 3 corridas (265–509 contra 593–1151 del viejo), pero con n = 3 y
    esa bimodalidad no es una diferencia.
  - **Sí muestra que el amortiguado se engancha donde debe:** 7 veces en
    los 6 cortes que llenan l1 (la cola sube cientos de ms), 0–2 en las
    corridas en que no se llena (6 en la que sí), y 3 en `long` (el corte de 10 s), donde
    sale de l1 igual de rápido que los otros (`off_s` 0,02 s).
- **Pendiente:** medirlo en el campo (el mismo RTMP de 90 s, contando
  saltos, rescate y `predict.engaged` por enlace), con y sin el
  interferente controlado.

## Qué hacemos con esto (diseño)

### Piezas

1. **El plazo del reordenador según la variación de cada enlace** (`reorder.h`, puro).
   - Cada enlace lleva, además de su retardo medio, su desviación: una media de |retardo − media|, como el RTTVAR de TCP.
   - El plazo usa el peor `retardo + 4 × desviación` de los enlaces en vez de `M + margen`, con `reorder_margin_ms` como piso.
   - Tests con trazas sintéticas: un enlace con saltos de 30 ms no debe dar `lost.cap`, y un paquete perdido de verdad debe salir en menos de `max_wait`.
   - Medir en el laboratorio (`reorder50`, `het`) y en la red de pruebas (`lost.cap`, `late_by`, caudal), en los dos extremos.
2. **El estimador de capacidad** (`capacity.h`, puro), por enlace y por sentido:
   - entrada: en cada sonda recibida, Δ`rx` del otro extremo, lo enviado por ese enlace (paquetes y bytes) y la cola de ese enlace;
   - salida: el ritmo de entrega (filtrado) y la capacidad estimada: el máximo del ritmo de entrega en una ventana (~10 s), como el ancho de banda de BBR, y si la cola pasa del objetivo, el ritmo actual (el enlace no da más);
   - sin efecto en el reparto todavía: solo estado (`capacity_mbit`, `delivery_mbit` por enlace) para el registrador;
   - validar contra `fieldperf` solo y todos juntos, con horas de registro.
3. **Pesos según la capacidad** (el gobernador v2):
   - el peso de cada enlace, proporcional a su capacidad;
   - el gobernador de hoy queda como corrección: baja el peso de un enlace cuya cola pasa del objetivo;
   - en los dos extremos (subida en el router, bajada en el VPS), con el A/B de siempre contra el gobernador de hoy.
4. **El tope total para `cake`:**
   - el motor publica el total, Σ capacidades × un factor (~0,9), en su estado y en un archivo pequeño;
   - un servicio aparte (no el motor) aplica `tc qdisc change … cake bandwidth` cuando cambia más de un umbral:
     - en el router, sobre `wgcg` (subida);
     - en el VPS, sobre `cg-NOMBRE` (bajada, antes del reparto: el VPS tiene CPU de sobra y evita el IFB, que en la Pi costaba caro);
   - A/B de ping bajo carga y caudal contra sin `cake` y contra `cake` fijo.
5. **Dosificar cada enlace en el motor:** solo si 3 y 4 no bastan. Pide colas por enlace en los hilos de bombeo (`pump.c`), con presupuesto fijo y sin `malloc` por paquete. Queda fuera de este plan hasta medir.

### Lo que no cambia

- **El protocolo:** las piezas 1 a 4 usan lo que ya viaja en las sondas. Si hiciera falta contar bytes en vez de paquetes, sería v5 y subiría `CG_PROTO_VERSION`.
- **El plano de datos:** las decisiones van en cabeceras puras con tests (patrón de la historia 003), y el plano de datos no bloquea.

### Orden y criterio de éxito

| PR | Qué | Se acepta si |
| --- | --- | --- |
| 1 | Plazo del reordenador por variación | `lost.cap` < 1 % en la red de pruebas, en los dos extremos, sin subir el p99 del ping; subida de la Pi ≥ lo medido con margen 50 |
| 2 | Estimador de capacidad (solo estado) | Con un enlace saturado, la capacidad cae dentro del ±20 % de su `fieldperf` solo |
| 3 | Pesos según la capacidad | La bajada gana al gobernador de hoy en un A/B n ≥ 5; la subida no pierde |
| 4 | Tope para `cake` | Ping p90 bajo carga como con `cake` fijo, caudal ≥ 90 % del de sin `cake` |

## Pendiente

- El misterio del PC (arriba): ver los ACK que vuelven repartidos y reordenados por la Pi; probar con el reordenador de la Pi con margen más largo.
- Los tardíos de la bajada que quedan vienen de enlaces que se atoran (Movistar, Claro): PR 3.
- Volver a medir el gobernador de subida con el reordenador corregido: sus A/B se midieron con un 4–8 % de pérdidas falsas.

## Cambios

- 2026-10-14: creada, con el diseño del paso 4 y la investigación del reordenador y de la subida desde la LAN.
- 2026-10-14: el PR 1 medido en los dos extremos; el reloj de los plazos con `link_threads`; los tardíos por enlace.
- 2026-10-14: el PR 2 validado enlace por enlace; las ramas de antena y el CSI de los cuatro módems.
- 2026-10-14: el PR 3 (pesos según la capacidad) medido en la bajada y la subida; las primeras medidas del PR 4.
- 2026-10-14: el techo contra el túnel; la capacidad que se retroalimentaba (PR #36); `split_eta` (PR #37) y `pacing` (PR #38), experimentales; chrony con SHOA.
- 2026-10-15: los A/B de ETA y de la dosificación, MPTCP, el falso hundimiento por `tcp_mem` del VPS, speedtest y la capacidad con cola (PR 5).
- 2026-10-15: el PR 5 medido en la red de pruebas: neutro; la bajada de 16 flujos llega a 280–344 con los enlaces de la mañana.
- 2026-10-15: el arranque rápido (PR #56): neutro en el laboratorio, que no reproduce el enfriamiento; el A/B en terreno, pendiente.
- 2026-10-16: el interferente controlado en el campo; la vuelta rápida tras un episodio corto, sin rebote por vaciado y `split_predict` (opcional); `lab.sh interferer`, que no reproduce el atraso del campo.
- 2026-10-16: `split_predict` en el campo (más saltos de reparto y el doble de rescates) y su amortiguación (`predict.h`: pendiente por mínimos cuadrados, umbral de 30 ms/s, histéresis, escalones); `lab.sh interferer` con `ramps`, saltos y rescates.
