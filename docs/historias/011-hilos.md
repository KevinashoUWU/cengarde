# 011 — Hilos en los dos extremos: colas del servidor (PR 3a)

- **Fecha:** 2026-10-05
- **Estado:** vigente. PR 3a (colas del servidor, todavía con un hilo, y
  protocolo 3) hecho, con la puerta S1 **sin cumplir tal como está
  escrita**: decide el usuario (abajo). 3b (hilos por enlace del router),
  3c (hilos de envío) y 3e (trabajadores del servidor) en otras ramas o
  pendientes; puertas V (un VPS de verdad) y P (la Pi) sin medir.
- **Fuentes:**
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

## TL;DR

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

## Hallazgos

### Qué hace el PR 3a

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

### La puerta S1

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

### `udp_mem` (corrige al diseño)

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

### TSAN y CI

- **Carrera anterior al PR:** `cg_tune` guardaba el conjunto de CPU
  inicial en su primera llamada, en el hilo principal, mientras el hilo
  del estado, arrancado justo antes, lo leía en `cg_thread_normal`. TSAN
  la vio en el `smoke` del laboratorio. Ahora `main` lo guarda una vez
  (`cg_cpus_save`) antes de arrancar ningún extremo. El PR 3b arregla la
  misma carrera con un `pthread_once`: al juntar las dos ramas hay que
  quedarse con un solo arreglo.
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

## Qué hacemos con esto

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

## Pendiente

- S1 a 110 kpps de subida: en el PR 3e y en la puerta V.
- Puerta V: en un VPS de 1 vCPU, el techo de subida con `lanes` 1 y 8.
- `udpmem` con IPv6: solo en el CI (este contenedor no tiene IPv6).
- Grupos de colas por router y la pista del protocolo 4 (PR 3d2);
  trabajadores del servidor (PR 3e).

## Cambios

- 2026-10-05: creada con el PR 3a (colas del servidor).
