# 011 — Hilos en los dos extremos: colas del servidor (PR 3a) e hilos por enlace en el router (PR 3b)

- **Fecha:** 2026-10-04 (3b) y 2026-10-05 (3a; la revisión de 3b; las dos ramas juntas)
- **Estado:** vigente. PR 3a (colas del servidor, todavía con un hilo, y
  protocolo 3) hecho, con la puerta S1 aceptada a medias por decisión del
  usuario (abajo). PR 3b (hilos de recepción por enlace en el router)
  hecho, apagado por omisión. 3c (hilos de envío) y 3e (trabajadores del
  servidor) pendientes; puertas V (un VPS de verdad) y P (la Pi) sin medir.
- **Fuentes, PR 3a:**
  - diseño combinado del PR 3 y los hilos (revisión 2, 2026-10-04, fuera
    del árbol), secciones A.1, A.2 (reglas R1–R4), A.4.2–A.4.8, D.2 (PR 3a)
    y D.4 (escenarios y puertas); el informe de medidas del estudio de hilos
    (fuera del árbol, secciones 3 y 7);
  - código: `engine/src/steer.h` (el programa), `rcvbudget.h` (el
    presupuesto), `server.c` (grupo de colas, socket de basura, lotes de
    respuesta por cola, rotación), `sock.c` (`cg_udp_bind_opts` con
    `reuseport` y el bind de prueba), `os.c` (`cg_cpus_save`);
  - pruebas: `engine/tests/test_steer.c` con `tests/cbpf.h` (un intérprete
    de cBPF), `test_rcvbudget.c`; laboratorio
    `bench/lab.d/{lanes,deepq,skew,udpmem}.sh`;
  - medidas: `bench/mt.py s1` (`s1 4 KPPS 1,8 5 3 up|down` y, con 5
    enlaces, `s1 4 KPPS 4,8 5 5 up`) y `mt.py table`, `bench/jitter.c`;
    este contenedor (4 vCPU compartidas con otros agentes, kernel 6.18, sin
    IPv6), cuatro sesiones de S1 con el mismo código;
  - kernel: `__sk_mem_raise_allocated` en `net/core/sock.c` (igual en v6.8
    y v6.12); `udp_prot`, `udp_rmem_schedule` y
    `__udp_enqueue_schedule_skb` en `net/ipv4/udp.c` (v6.12); `seq_sk_match`
    en `net/ipv4/udp.c` (v6.8 a v6.17).
- **Fuentes, PR 3b:**
  - diseño combinado del PR 3 y los hilos (revisión 2, 2026-10-04, fuera
    del árbol), secciones A.2–A.8, C, D.2 (PR 3b) y D.4; respuestas del
    usuario a su sección E (E.3: hilos apagados por omisión hasta medir en
    la Pi);
  - código: `engine/src/ring.h` (anillo, timbre, apretón de manos de
    espacio), `hist.h`, `clientpath.h` (`cg_rx_entry`, `cg_up_header`),
    `pump.h`/`pump.c`, `thrplan.h`, `client.c` (`hub_drain`, `link_cmd`,
    `link_pump`, `inline_read`), `arrival.h` (regla de desorden);
  - pruebas: `engine/tests/test_{ring,hist,threads,client_rx,thrplan,arrival}.c`;
    laboratorio `bench/lab.d/{mtstall,mtlat,soak}.sh` y `bench/ringbench.c`;
  - medidas: este contenedor (4 vCPU Xeon 2,1 GHz compartidas con otros
    agentes, kernel 6.18, sin netem), `bench/lab.sh` con 3 enlaces veth.

## TL;DR

**PR 3a, colas del servidor:**

- El servidor escucha con un grupo `SO_REUSEPORT` de `lanes` sockets (8
  por omisión; `lanes = 1` es el socket de antes) y uno de basura. Un
  programa cBPF (`steer.h`) mete cada datagrama en la cola de su número de
  enlace, así que las copias de un paquete esperan en colas distintas.
  Sigue habiendo un solo hilo, y el protocolo sigue siendo el 3.
- Con un socket, el 73–98 % de las copias que tiraba el servidor eran
  paquetes enteros: la VM se para 5–38 ms varias veces cada 10 s y la
  única cola desborda. Con 8 colas, en tres sesiones de S1, 0 frente a
  18.011, ≈12.643 frente a ≈107.977 y 1.650 frente a 42.747 paquetes
  enteros perdidos en el servidor (11,8× menos sumando las tres). `deepq`:
  el túnel pierde un 10,48 % con un socket y nada con colas; `skew`: 33 %
  de primeras llegadas por enlace (puerta C5).
- La puerta S1 tal como está escrita **no se cumple**: a 110 kpps, 3, 2 y
  2 rondas de 4 con ≤ 0,1 % de pérdida (4, 3 y 3 sin lo que el router tiró
  antes de duplicar). Las colas absorben un parón más corto que el búfer
  de una cola (~33 ms a 110 kpps), no un hilo único que se queda atrás más
  tiempo: entonces desbordan todas a la vez. Decide el usuario.
- `udp_mem`: pasado su primer valor, el kernel solo deja crecer la cola de
  un socket UDP mientras guarda menos de `udp_rmem_min` (4 KiB, un
  datagrama de 1400 B), y pasado el último, nada; UDP no tiene modo de
  presión. Por eso los sockets que reciben se reparten la mitad del umbral
  de presión, que queda por debajo del primer valor. El sysctl solo existe
  en el primer namespace de red.
- TSAN encontró una carrera anterior al PR (el conjunto de CPU inicial),
  ya arreglada. El CI corre las pruebas y el laboratorio con binarios TSAN,
  y las pruebas y el laboratorio en AArch64 de verdad.

**PR 3b, hilos por enlace en el router:**

- `link_threads` elige cómo lee el cliente sus enlaces: `legacy` (el bucle
  de 0.4, **por omisión** y en LuCI «Desactivado» hasta la puerta P), `on`
  (un hilo por enlace, como mucho 8), `off` (la estructura nueva en un solo
  hilo: **no** es el código de antes) y `auto` (en esta versión, `legacy`).
- Todos los modos deciden con el mismo código (`clientpath.h`): secuencia,
  ventana antes del MAC, marca solo tras el MAC, reinicio del servidor. Lo
  único distinto es quién lee, quién envía y quién cierra los sockets.
- Con `on`, un hilo de enlace sin CPU llena solo su socket: en `mtstall`
  (300 ms sin CPU cada 2 s en el hilo de l3) el túnel no pierde nada ni se
  retrasa, y solo l3 descarta en su socket. Un atasco del hilo principal
  sigue frenando todos los enlaces a la vez.
- El coste: a poco tráfico cada lote despierta un hilo (≈ 12 µs de CPU por
  despertar en esta VM, `ringbench`), así que la CPU por paquete de bajada
  sube mucho con `on` a 2 y 20 kpps (cifras abajo): la puerta C1 de CPU
  (≤ +30 %) **no** pasa aquí, y por eso `on` sigue a mano.
- `off` cuesta lo mismo que el bucle de siempre (puerta C0 de velocidad,
  ±5 %; un +6,4 % de una primera tanda no se repitió con 5 pasadas) y pasa
  todos los escenarios del laboratorio y el soak de 1 h; `on` también, y
  `legacy`, los escenarios. El e2e de QEMU pasa en los tres modos, salvo
  una comprobación de tiempo de la propia prueba que sin KVM también falla
  con el PR 2.

## Contexto

El estudio de hilos (informe de medidas, sección 3) midió que, con un
solo socket de escucha, las tres copias de un paquete esperan en la misma
cola: un parón de la VM entera (5–38 ms, varias veces cada 10 s, marcados
con `jitter.c`) la desborda y se lleva las tres. El 73–98 % de las copias
que tiraba el servidor eran paquetes enteros; con descartes
independientes habrían sido 0–43 paquetes. La redundancia no servía de
nada justo ahí. El diseño (A.4) separa las copias por enlace en colas
distintas antes de tocar los hilos: con un hilo, el servidor las lee por
turnos; los trabajadores del servidor llegan en el PR 3e.

El usuario pidió hilos «igual» que el multicliente (PR 3). El diseño elegido
(«minimal» con injertos) pone un dueño por estado: el *hub* (el bucle de
siempre menos la E/S de los sockets de enlace) guarda la sesión, la ventana
anti-replay, la salud y las sondas; una *bomba* por enlace solo lee su
socket. El PR 3b hace la recepción; el 3c hará el envío desde las bombas.


## Hallazgos

### 3a: Qué hace el PR 3a

- **Grupo de colas:** `lanes` (`auto` = 8, o 1, 2, 4, 8, 16; solo al
  reiniciar) sockets atados a la misma dirección y puerto, más uno de
  basura con un búfer fijo de 256 KiB. Antes de montar el grupo, un bind
  sin `SO_REUSEPORT` comprueba que el puerto está libre: un segundo
  servidor no arranca («Address already in use») en vez de colarse en el
  grupo del primero. Un kernel que no admite el programa (anterior a 4.5)
  se queda con un socket y lo dice en `steering_error`.
- **El programa** (`steer.h`, 16 instrucciones como mucho): primero la
  longitud, después la versión, después el byte del enlace con
  `& (L − 1)`; lo corto o de otra versión va a la basura (índice `L`),
  nunca al hash del kernel. `test_steer` lo ejecuta en `tests/cbpf.h`, un
  intérprete de cBPF que también exige saltos solo hacia delante, de 8
  bits los condicionales y un `RET` final, contra una función de
  referencia: todas las clases de longitud, versión y byte de enlace con
  `L` ∈ {1, 2, 4, 8, 16}.
- **La bajada sale por la cola de su enlace:** el camino `p` envía desde
  la cola `p & (L − 1)`, con su propio búfer de envío y su propio lote de
  respuestas a sondas. El primer camino de cada lote rota.
- **Estado:** `lanes[]` (recibidos, descartes del kernel por cola, enlaces
  vistos), `junk`, `rx.junk|short|bad_version`, `rcvbuf` (configurado,
  presupuesto, sockets, por socket, efectivo), `rcvbuf_capped` y
  `steering_error`; `cengarde -t` enseña los sockets y el presupuesto.

### 3a: La puerta S1

`bench/mt.py s1` intercala `lanes` ronda a ronda (1 y 8; con 5 enlaces, 4
y 8), con los búferes del servidor por defecto y los del router a 32 MiB,
5 s y 4 rondas por punto; `mt.py table` saca las filas. Cuatro sesiones
con el mismo código: la 1, del autor del PR (40–110 kpps, bajada a 110 y
120 kpps, 5 enlaces a 100 y 110 kpps); la 2 y la 3, de dos revisores
(80–110 kpps; la 2, también bajada y 5 enlaces a 60–80 kpps, y coincidió
con un arranque de QEMU y compilaciones TSAN en la misma VM; la 3,
5 enlaces a 40 y 50 kpps); la 4, después de la revisión (5 enlaces a
40–60 kpps). ≈: estimado por resta, porque en esa ronda también tiró otro
socket.

| Criterio (diseño, D.4) | Medido | Veredicto |
| --- | --- | --- |
| 110 kpps de subida con ≤ 0,1 % de pérdida en 4 de 4 rondas | 8 colas: 3/4, 2/4 y 2/4 (1 cola: 1/4, 0/4 y 0/4); sin lo que el router tiró antes de duplicar, 4/4, 3/4 y 3/4 | **no se cumple** |
| paquetes enteros perdidos en las colas ≥ 10× menos que con una | 0 frente a 18.011; ≈12.643 frente a ≈107.977 (8,5×); 1.650 frente a 42.747 (26×); las tres juntas, 14.293 frente a 168.735 (11,8×) | se cumple en 2 de 3 sesiones y en la suma |
| µs por paquete del servidor a ±5 % de una cola (medias, 40–110 kpps) | entre −4,4 % y +4,9 % | se cumple |
| bajada a saturación a ±5 % en pérdida y en µs por paquete | µs: −0,6 % y −1,3 % (sesión 1), −2,4 % y +0,5 % (sesión 2). Pérdida media, 1 cola frente a 8: 1,84 → 1,31 % y 3,44 → 3,69 % a 110 y 120 kpps (sesión 1); 8,36 → 4,87 % y 9,56 → 8,19 % (sesión 2) | se cumple en µs; en pérdida, menor con 8 colas en 3 de 4 puntos, y el cuarto (+0,25 puntos, +7 % relativo) cae dentro de la dispersión entre rondas de una misma variante (1,75–6,63 %) |
| con `NLINKS=5`, 8 colas no peor que 4 | 40–60 kpps (sesiones 2 a 4): ningún paquete entero perdido en el servidor con ninguna de las dos; copias tiradas en las colas, con 4 colas 4.374 a 50 kpps y 442 y 7.084 a 60 kpps, con 8 ninguna; µs por paquete del servidor entre −0,3 % y +2,8 %. A 60 kpps el router ya va al 90–102 % de CPU y lo que se pierde de punta a punta se pierde en él (4 colas, 1 ronda de 4 en la sesión 2; 8 colas, 2 de 4 en la 4) | se cumple, mirando lo que tira el servidor |

- **Por qué falla a 110 kpps:** el router de este laboratorio lleva un
  solo hilo y a 110 kpps va al 81–102 % de CPU. Aun con su búfer de
  32 MiB tira en su socket de WireGuard, antes de duplicar, donde las
  colas no pueden hacer nada: 2.226, 4.042, 37.035 y 2.802 datagramas en
  cuatro de las cinco rondas que fallaron con 8 colas. Pero no es solo
  eso: en dos de esas cinco el servidor perdió más del 0,1 % en paquetes
  enteros (≈936 y 1.180, un 0,17 % y un 0,215 %), y entonces las tres
  colas desbordaron a la vez (por ejemplo 10.918, 10.926 y 11.178
  copias). Una cola guarda unos 33 ms de su enlace a 110 kpps *(cálculo:
  8 MiB efectivos / (110.000 × 2.304 B))*: un parón más corto se lo come
  la cola; un hilo único que se queda atrás más tiempo las desborda todas.
  Eso es trabajo de los trabajadores del PR 3e, no de las colas.
- **Con 5 enlaces el router satura antes:** a 70 kpps ya va al 104–108 %
  de CPU, así que la pérdida de punta a punta no dice nada del servidor de
  ahí para arriba. A 70 y 80 kpps, con 4 colas el servidor tiró 3.400 y
  13.036 copias, y con 8, 0 y 49; ningún paquete entero en ninguna; su
  CPU por paquete, +2,7 % y +1,7 %. A 100–110 kpps (sesión 1, el router
  al 102–107 %) no decide: una ronda de 8 colas perdió ≈4.761 paquetes
  enteros frente a ≈633 de una de 4, los dos estimados por resta entre
  decenas de miles de descartes del router. Con 4 colas, los enlaces 0 y 4
  comparten la cola 0 (`4 & 3 = 0`), que recibe dos copias de cada
  paquete: es la que tira; con 8, cada enlace tiene la suya, que es lo que
  justifica `lanes = auto` = 8 (A.4.3).
- **La bajada a saturación** pierde en el socket de WireGuard de la
  sesión, en el servidor, antes de duplicar: el hilo del servidor va al
  85–100 % de CPU. Ahí las colas no entran.
- **C5 y `deepq`:** `skew`, 33,0–33,9 % de primeras llegadas por enlace en
  bajada a 2.000 y 40.000 pps (antes, l1 se llevaba el 90–100 %); la
  subida depende del orden de envío del router y sigue con l1 en el
  92–99,7 %. `deepq` (s3 a 5 Mbit/s con 20 MB de cola, 20.000 pps de
  bajada): con un socket, el túnel perdió un 10,48 % y los caminos sanos
  21.970 y 20.972 copias; con 8 colas, nada.

### 3a: `udp_mem` (corrige al diseño)

El diseño decía que los sockets UDP tiran «por encima de `udp_mem[2]`».
Medido y leído en el kernel, es antes:

- `__sk_mem_raise_allocated` (v6.8 y v6.12, sin cambios entre las dos):
  por debajo de `udp_mem[0]`, concede; por encima de `udp_mem[2]`,
  deniega; entre los dos, una recepción solo se concede si el socket
  guarda menos de `sk_get_rmem0`, que para UDP es `udp_rmem_min` (4096 B
  por omisión, por namespace). Después, como `udp_prot` no tiene
  `memory_pressure`, deniega. `udp_mem[1]` no cambia nada para UDP.
- `__udp_enqueue_schedule_skb` suma el datagrama a `sk_rmem_alloc` antes
  de pedir la memoria: un datagrama de 1400 B (unos 2,3 KB en el kernel)
  entra en una cola vacía, el segundo ya no. Pasado `udp_mem[0]`, cada
  socket UDP de la máquina se queda con un datagrama en la cola.
- Medido (`bench/lab.sh udpmem`, con `udp_mem` bajado a 8192 12288 16384
  páginas y el servidor parado con 8 colas llenas): un par UDP aparte que
  lee cada 250 ms recibió 163 de 599 datagramas cuando el servidor no
  recortaba sus búferes; con el presupuesto, todos.
- Por eso el presupuesto (`rcvbudget.h`) es la mitad de `udp_mem[1]`, que
  con los valores del kernel (`udp_mem[0]` = ¾ de `udp_mem[1]`) queda por
  debajo del primer valor; cada socket que recibe se lleva
  `min(rcvbuf, presupuesto / (2 × sockets))`.
- `net.ipv4.udp_mem` solo existe en el primer namespace de red: en uno
  nuevo (`unshare -n`) `/proc/sys/net/ipv4/udp_mem` no está, y
  `udp_rmem_min` sí. Un servidor en un contenedor o en el laboratorio lo
  estima desde la RAM, como el kernel al arrancar (`budget_from: "RAM"`);
  `udpmem` le enseña los valores bajados con un tmpfs en su namespace de
  montaje.
- `/proc/net/udp` no lista los sockets AF_INET6 (`seq_sk_match` filtra por
  familia), y con `*:59402` las colas son AF_INET6 de doble pila donde el
  kernel tiene IPv6: `udpmem` y `mt.py` leen también `/proc/net/udp6`. En
  este contenedor, sin IPv6, el servidor cae a AF_INET y no se veía.

### 3a: TSAN y CI

- **Carrera anterior al PR:** `cg_tune` guardaba el conjunto de CPU
  inicial en su primera llamada, en el hilo principal, mientras el hilo
  del estado, arrancado justo antes, lo leía en `cg_thread_normal`. TSAN
  la vio en el `smoke` del laboratorio. Ahora `main` lo guarda una vez
  (`cg_cpus_save`) antes de arrancar ningún extremo. El PR 3b la había
  arreglado con un `pthread_once`; al juntar las ramas quedó solo
  `cg_cpus_save`, que también lee `cg_initial_cpus` (los hilos de enlace).
- **Regla R1** (sin barreras sueltas, que TSAN no modela): `-Werror=tsan`
  solo veía `atomic_thread_fence` dentro de una función que gcc
  expandiera en línea, porque la macro de `<stdatomic.h>` es de una
  cabecera del sistema; con `-ftrack-macro-expansion=0` la ve en
  cualquier sitio y a `-O0`. El CI además busca `atomic_thread_fence` y
  `__sync_synchronize` en el código.
- **Laboratorio con TSAN:** cada proceso escribe sus avisos en un archivo
  propio (`log_path`), que el paso del CI mira después de cada escenario:
  un aviso al parar el motor (donde se juntan los hilos), en una pasada
  anterior o en una llamada a `ctl` no se veía antes.
- El CI corre además las pruebas y el laboratorio en `ubuntu-24.04-arm`,
  y otra vez compilado para ARMv8.0 sin LSE, como el Cortex-A72 de la Pi.

### 3b: Primitivas (`ring.h`)

- Un anillo SPSC con índices libres de 32 bits; las escrituras de índices y
  las lecturas que deciden dormir o si hay sitio son `seq_cst` (pares de
  Dekker), sin `atomic_thread_fence` (el TSAN de gcc no lo modela).
- Timbre: el consumidor dice que duerme (`sleeping = 1`) y mira los anillos;
  el productor, tras publicar, toca solo si dormía. Bajo carga nadie duerme
  y no se escribe ningún eventfd.
- Apretón de manos de espacio (bomba con su anillo lleno): la bomba marca
  `rx_blocked`, vuelve a mirar y, si sigue lleno, saca sus sockets del
  epoll; el hub, tras liberar, escribe el eventfd de la bomba **siempre**,
  no a través de `sleeping`. `test_threads` fuerza el orden que perdía el
  despertar (la bomba marca, el hub libera antes de que la bomba se duerma)
  con un gancho de prueba: con las dos salvaguardas, y con cada una sola, la
  bomba vuelve a leer en menos de 1 s; sin ninguna, no (la prueba ve el
  fallo).
- `ringbench` (esta VM, `bench/bin/ringbench -d 2 -q`):

  | Prueba | Resultado |
  | --- | --- |
  | consumidor que nunca duerme, lotes de 1 / 8 / 64 | 240 / 53 / 8 ns por entrada |
  | consumidor que duerme en el timbre, 2000 / 20 000 / 80 000 por s | traspaso p50 43 / 12 / 6 µs, p99 239 / 59 / 87 µs |
  | despertares por entrada (ídem) | 0,98 / 0,49 / 0,16 |
  | CPU del consumidor por entrada (ídem) | 12,6 / 2,2 / 0,7 µs |
  | consumidor que sondea (`-q`), CPU por entrada a 2000 por s | 494 µs (un núcleo entero) |

  Cada despertar le cuesta al consumidor unos 12 µs de CPU en esta VM: es
  el precio de los hilos a poco tráfico.

### 3b: Estructura y decisiones del PR

- **Generaciones:** el hub sube la generación de un enlace al abrir y al
  cerrar; lo que la bomba leyó de un socket viejo se descarta antes de
  mirarlo (`download.stale`), así que una respuesta de la dirección vieja
  nunca cuenta para la nueva.
- **La bomba cierra lo que se le dio.** Refinamiento sobre el diseño: un
  socket que el epoll de la bomba rechaza **no** se cierra al momento, se
  guarda sin vigilar hasta el `CLOSE` del hub, porque el hub aún usa ese
  número para enviar (3b) y para `getsockopt`; cerrarlo antes dejaría que
  otro socket heredara el número.
- **Bomba atascada:** como mucho 8 órdenes en vuelo; después, un estado
  pendiente por enlace (el más nuevo) y el hub cierra los sockets que nunca
  entregó. Un enlace pendiente no envía ni sondea.
- **El hilo principal no se renombra:** `pthread_setname_np` en el hilo
  principal cambiaría el nombre del proceso, y `cengarde-setup` usa
  `pidof cengarde`; `ctl threads` lo llama `cg-hub` igualmente.
- **Regla de desorden** (`arrival.h`): una copia con hora anterior a la
  primera registrada se queda el primer puesto. Solo para mostrar: esas
  estadísticas se cuentan antes del MAC.
- **Rotación:** con bombas, el primer enlace de cada lote de subida rota.
- **CPU por hilo** del reloj de CPU de cada hilo (`pthread_getcpuclockid`):
  OpenWrt no tiene `schedstat`.
- **Hilo parado** (`io_stalled_ms` y su aviso): se cuenta desde la primera
  comprobación (una por segundo) que vio trabajo esperando sin ninguna
  vuelta del hilo desde entonces (`cg_stall_check`, `thrplan.h`). Contarlo
  desde la última vuelta, como se hacía, daba por parada una bomba que solo
  había dormido sin nada que hacer: 0,3 s de retraso al volver l3 tras 7 s
  caído marcaban 7,5–8 s y el aviso; ahora 0 en 8 de 8 rondas, y un atasco
  real de 8 s marca 6,3–7,3 s con aviso. `loop_ms` solo se compara, así que
  una vuelta posterior a la lectura del reloj del hub ya no da 49 días.
- **Sondeo activo:** en todas las bombas o en ninguna (`cg_pump_busy_us`).
  Cuando ya no caben en las CPU con una de sobra, el hub se lo quita también
  a las que ya sondeaban (un atómico que la bomba lee en cada vuelta): antes,
  con 4 CPU y 3 enlaces, sondeaban las dos primeras y el registro decía que
  solo el hub. Los avisos del plan de hilos van también al estado
  (`threads.warnings`).
- **Estado pendiente** de un enlace con su bomba atascada: en la API de la
  bomba (`cg_pump_send`), probado con sockets reales en `test_threads` (diez
  reaperturas sin fugas, gana el estado más nuevo y nada se le adelanta).
- **Nombres:** los hilos que escriben el estado se llaman en el kernel como
  en `ctl threads` (`cg-status`, y `cg-pass` en el servidor).

### 3b: Medidas

- **C0, velocidad** (`off` frente al motor de `1c60051`, pasadas
  intercaladas, medias de µs de CPU del cliente por paquete, de todos sus
  hilos; 0 % de pérdida salvo donde se dice):

  | Sentido | 10 kpps (3) | 40 kpps (3) | 80 kpps (3) | 80 kpps (5, repetido) |
  | --- | --- | --- | --- | --- |
  | bajada | 13,6 → 14,1 (+3,5 %) | 8,8 → 8,8 (+0,3 %) | 8,4 → 8,2 (−2,4 %) | 7,6 → 7,4 (−3,4 %) |
  | subida | 16,9 → 16,6 (−1,6 %) | 9,2 → 9,3 (+0,8 %) | 7,8 → 8,3 (+6,4 %) | 8,1 → 8,1 (−0,5 %) |

  El +6,4 % de la primera tanda no se repitió con 5 pasadas: ruido de la
  VM. A 40 y 80 kpps el motor de master perdió algo en alguna pasada (hasta
  2,2 %) y `off` como mucho un 0,2 %: también ruido.
- **C0, corrección:** `ci` entero (smoke, health, control, fallback,
  mtstall, multiip y restart) pasa con `legacy`, `off` y `on` (y antes, en
  cada paso del PR: `legacy` tras la factorización, `off` tras el modo en
  línea; otra vez tras la revisión). El soak de 1 h y el e2e de QEMU
  (abajo) se corrieron sobre la cabeza del PR, con los hilos ya dentro, y
  no «antes de cualquier commit que añada hilos», como pedía el diseño.
- **C1** (bajada, `off` frente a `on`, 3 pasadas de 10 s, medianas;
  `bench/lab.sh mtlat`):

  | kpps | Modo | p50 / p99 de punta a punta | CPU cliente / servidor por paquete | Salto p50 / p99 (motor) |
  | --- | --- | --- | --- | --- |
  | 2 | `off` | 118 / 438 µs | 31,5 / 43,5 µs | 0 / 1 µs |
  | 2 | `on` | 136 / 576 µs | 71,5 / 60,0 µs | 21 / 143 µs |
  | 20 | `off` | 94 / 1054 µs | 9,9 / 12,6 µs | 0 / 1 µs |
  | 20 | `on` | 108 / 1019 µs | 18,7 / 16,1 µs | 14 / 119 µs |

  El salto pasa (p50 21 µs ≤ 60 µs a 2 kpps), pero la CPU por paquete del
  cliente sube un 127 % a 2 kpps y un 89 % a 20 kpps: **la puerta de CPU de
  C1 (≤ +30 %) no pasa**. También sube la del servidor (+38 % y +28 %): el
  kernel despierta los hilos del cliente desde el envío del servidor (veth)
  y se lo cobra a él. Con `mtstall` pasando, el resultado es el que el
  diseño preveía para este caso: `on` sigue a mano y, si la Pi lo confirma,
  primero la propiedad adaptativa de los enlaces.
- **`mtstall`** (2000 pps en cada sentido, 12 s, 300 ms sin CPU cada 2 s
  en el hilo de l3 o, en `off` y `legacy`, en el bucle único; tras la
  revisión, cada modo en su propio subshell, con el bucle ocupado y el hilo
  atascado en la última CPU y todo lo demás en las otras; dos pasadas):

  | Modo | Bajada: pérdida, tarde (≥ 50 ms) | Subida: pérdida, tarde | Descartes en el socket l1 / l2 / l3 |
  | --- | --- | --- | --- |
  | `on` | 0, 0 | 0, 0 | 0 / 0 / 1852–1854 |
  | `off` | 7,7 %, 4,7 % | 7,7 %, 4,7 % | 1854–1860 / 1875–1879 / 1877–1880 |
  | `legacy` | 7,7 %, 4,7 % | 7,7 %, 4,7 % | 1854 / 1864 / 1867 |

  Antes de la revisión, `legacy` daba 8,0 % y unos 1915 descartes: cada
  modo heredaba una CPU menos del anterior (con `legacy`, el servidor, el
  cliente y los WireGuard falsos compartían una sola CPU).

  Con `on`, el atasco de un hilo de enlace queda en su enlace; con un solo
  bucle, el mismo atasco tira copias de todos los enlaces a la vez y el
  túnel pierde (la subida también: el socket de WireGuard se llena).
- **TSAN:** las pruebas unitarias (gcc y clang, también con las carreras
  forzadas del apretón de manos) sin avisos. En el laboratorio, `smoke` y
  `control` con binarios TSAN en los dos extremos encontraron una carrera
  anterior a este PR: `cg_tune` escribía el conjunto de CPU inicial en el
  hilo principal mientras el hilo del estado ya lo leía
  (`cg_thread_normal`); la arregló también 3a (quedó su
  `cg_cpus_save`). Después, `smoke` y
  `control` pasan sin avisos con `on` y con `off`. Tras la revisión, con
  `link_threads = on` y binarios TSAN de gcc y de clang en los dos
  extremos, `smoke`, `health`, `control`, `fallback`, `multiip`, `restart`
  y `mtstall` pasan sin avisos (y `mtstall` sin perder ni un paquete). (El
  `cengarde` compilado con TSAN por gcc da dos avisos falsos de
  `-Wstringop-overflow` en `ctl.h`, también en master.)
- **Soak de 1 h** (`SOAK_S=3600`, `off` y luego `on`, cada uno con el
  candado de los namespaces; sin netem en este contenedor, así que l3
  alterna `tbf`): por modo, 57 tramos de 60 s, 11 recargas (todas `ok`), 5
  caídas de l2 de 10 s y 29 cambios de l3; ningún aviso de hilo parado ni
  de socket sin vigilar; RSS del cliente 2868 → 3008 kB con `off` y 3584 →
  3724 kB con `on` desde los 5 min. Los dos pasan con la regla del diseño
  (toda pérdida queda explicada por descartes contados en su sentido):

  | kpps | `off`: bajada, subida perdidas | `on`: bajada, subida perdidas |
  | --- | --- | --- |
  | 2 y 10 | 0, 0 | 0, 0 |
  | 20 | 0,004 %, 0,036 % | 0,003 %, 0,015 % |
  | 40 | 0,58 %, 3,2 % | 0,15 %, 0,89 % |

  A 40 kpps en los dos sentidos esta VM no da abasto: con `off`, el hilo
  único deja llenarse el socket de WireGuard del cliente (la subida perdida
  coincide a veces exacta con sus descartes) y los de enlace; con `on`, la
  subida se pierde en el socket único del servidor, lo que arreglan los
  carriles de 3a. El soak corto de antes (`SOAK_S=300`) solo llegó a 4
  tramos (unos 248 s): ni recargas ni caídas de l2, solo dos cambios de
  `tbf` en l3.
- **e2e de QEMU** (25.12.5 x86-64, sin KVM en este contenedor; los
  paquetes de esta rama, aún numerados 0.4.5, con el mismo código): pasa
  todo, también el paso 8 de ahora (`off`, `on` y otra vez `legacy`: cada cambio
  reinicia el motor en el lugar, los tres enlaces vuelven vivos, el ping
  pasa sin pérdida y `ctl threads` muestra el hub y un hilo por enlace con
  su CPU), salvo una comprobación del paso 4 que en esta máquina falla
  igual con la imagen y la prueba del PR 2: la prueba lee el archivo de IP
  pass del VPS justo al acabar su ping de 12 s, y sin KVM la aplicación del
  router tarda unos 9 s (33 s con la máquina cargada), así que el VPS
  cambia en el mismo segundo en que se lee (cambió: «IP pass off» en su
  registro, `off` en el archivo). El CI, con KVM, corre el e2e en 25.12 y
  en 24.10.

## Qué hacemos con esto

**3a:**

- `lanes = 8` por omisión, `lanes = 1` de interruptor al menos una versión
  (D.1).
- `udp_mem` no se toca: subirlo en una máquina pequeña cambia descartes de
  UDP por el OOM killer; el servidor se ajusta al presupuesto.
- **S1, decisión del usuario.** Tal como está escrita no pasa. En parte
  porque el router de este laboratorio no tiene la CPU de sobra que
  supone el criterio, y en parte porque el único hilo del servidor a veces
  se queda atrás más de lo que guarda una cola: con 8 colas tampoco hubo 4
  de 4 en todas las sesiones a 90 y 100 kpps (la 2 dio 3/4 en los dos
  puntos, con las tres colas desbordadas a la vez). Lo que dan los datos
  con cada salida:
  - juzgar la pérdida solo después de duplicar: a 110 kpps, 4/4, 3/4 y
    3/4; tampoco pasa;
  - bajar el punto a 80 kpps: 4/4 en las tres sesiones (a 90 y 100 kpps,
    3/4 en la sesión 2);
  - dejar el criterio de pérdida para los trabajadores del servidor
    (PR 3e) o para la puerta V en un VPS de verdad, y aceptar 3a por lo que
    sí cumple: paquetes enteros 11,8× menos con las sesiones sumadas, CPU,
    bajada y 5 enlaces;
  - repetir S1 en una máquina más tranquila o más rápida.

- **Decisión del usuario (2026-10-05):** aceptar el 3a por lo que cumple y
  exigir el criterio de pérdida a 110 kpps a los trabajadores del servidor
  (PR 3e) y a la puerta V en un VPS de verdad.

**3b:**

- `legacy` sigue por omisión en el motor y en OpenWrt (decisión del usuario,
  E.3); `on` queda a mano en LuCI («Activado (un hilo por enlace)») y `off`
  («Un hilo (estructura nueva)») sirve para separar un fallo de los hilos de
  uno del código nuevo.
- `legacy` se quita en la primera versión después de pasar la puerta P.
- Si la puerta P confirma el coste de CPU a poco tráfico, la primera
  alternativa es la propiedad adaptativa de los enlaces (el diseño, A.8):
  los enlaces se quedan en el hub mientras tenga CPU de sobra.

## Pendiente

- S1 a 110 kpps de subida: en el PR 3e y en la puerta V.
- Puerta V: en un VPS de 1 vCPU, el techo de subida con `lanes` 1 y 8.
- `udpmem` con IPv6: solo en el CI (este contenedor no tiene IPv6).
- Grupos de colas por router y la pista del protocolo 4 (PR 3d2);
  trabajadores del servidor (PR 3e).
- Puerta P en la Pi (banco cableado y enlaces reales); 3c (envío desde las
  bombas, arena, `sched.h`); cruzar `mtstall` y el soak con las pausas
  que marca `jitter.c`; `mtcorr` a mano; el e2e de 24.10, que corre el CI del PR.
- Que la comprobación del VPS en el paso 4 del e2e espere al cambio en vez
  de leerlo una sola vez (falla sin KVM, también con el PR 2).

## Cambios

- 2026-10-04: creada con el PR 3b.
- 2026-10-05: revisión del PR: hilo parado contado desde que se vio el
  trabajo esperando (y sin dar 49 días), sondeo activo en todas las bombas
  o en ninguna, avisos del plan de hilos en el estado, nombres de los hilos
  que escriben el estado, estado pendiente en la API de la bomba con su
  prueba; `mtstall` corre el modo de `CLIENT_EXTRA`, cada modo en su propio
  subshell y sin tolerancia de pérdida; el soak recarga cada 5 min, llega a
  40 kpps y juzga con los descartes contados; soak de 1 h y e2e de QEMU;
  paquetes 0.4.6.
- 2026-10-05: creada con el PR 3a (colas del servidor).
- 2026-10-05: las dos ramas juntas: un solo arreglo de la carrera del
  conjunto de CPU (`cg_cpus_save`), el paso de TSAN del laboratorio en el CI
  también con `link_threads = on` y `mtstall`, y la matriz `lab` de 3b con
  `LAB_UDPMEM` y `lanes = 1` de 3a.
