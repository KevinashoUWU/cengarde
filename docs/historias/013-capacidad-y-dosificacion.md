# 013 — Capacidad por enlace y dosificación (paso 4 del bonding)

- **Fecha:** 2026-10-14
- **Estado:** vigente. Diseño; nada implementado todavía salvo el margen del reordenador puesto a mano en la red de pruebas.
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
- Poner el margen también en la Pi, o esperar al PR 1: la bajada tiene la cola de tardanza más larga.
- Volver a medir el gobernador de subida con el reordenador corregido: sus A/B se midieron con un 4–8 % de pérdidas falsas.

## Cambios

- 2026-10-14: creada, con el diseño del paso 4 y la investigación del reordenador y de la subida desde la LAN.
