# 013 — Capacidad por enlace y dosificación (paso 4 del bonding)

- **Fecha:** 2026-10-14
- **Estado:** vigente. PR 1 hecho (PR #32, 0.6.5): el plazo del reordenador según la oscilación de cada enlace. PR 2 hecho (PR #33, 0.6.6): la capacidad de cada enlace. PR 3 (PR #34, 0.6.7): los pesos según la capacidad (`capacity_weights`), +50 % en la bajada (n = 5, p = 0,075). Medidos en la red de pruebas. PR 4 por hacer.
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
- 2026-10-14: el PR 3 (pesos según la capacidad) medido en la bajada.
