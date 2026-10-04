# 011 — Hilos en los dos extremos: hilos por enlace en el router (PR 3b)

- **Fecha:** 2026-10-04
- **Estado:** vigente. PR 3b (hilos de recepción por enlace en el router)
  hecho; 3a (carriles del servidor), 3c (hilos de envío) y 3e (trabajadores
  del servidor) en otras ramas o pendientes; puerta P (la Pi) sin medir.
- **Fuentes:**
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
  todos los escenarios del laboratorio; `on` y `legacy` también.

## Contexto

El usuario pidió hilos «igual» que el multicliente (PR 3). El diseño elegido
(«minimal» con injertos) pone un dueño por estado: el *hub* (el bucle de
siempre menos la E/S de los sockets de enlace) guarda la sesión, la ventana
anti-replay, la salud y las sondas; una *bomba* por enlace solo lee su
socket. El PR 3b hace la recepción; el 3c hará el envío desde las bombas.

## Hallazgos

### Primitivas (`ring.h`)

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

### Estructura y decisiones del PR

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

### Medidas

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
  línea). Falta el soak de 1 h (abajo, uno corto) y el e2e de QEMU.
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
  en el hilo de l3 o, en `off` y `legacy`, en el bucle único):

  | Modo | Bajada: pérdida, tarde (≥ 50 ms) | Subida: pérdida, tarde | Descartes en el socket l1 / l2 / l3 |
  | --- | --- | --- | --- |
  | `on` | 0, 0 | 0, 0 | 0 / 0 / 1859 |
  | `off` | 7,8 %, 4,7 % | 7,7 %, 4,7 % | 1862 / 1874 / 1876 |
  | `legacy` | 8,0 %, 4,7 % | 8,0 %, 4,7 % | 1914 / 1919 / 1919 |

  Con `on`, el atasco de un hilo de enlace queda en su enlace; con un solo
  bucle, el mismo atasco tira copias de todos los enlaces a la vez y el
  túnel pierde (la subida también: el socket de WireGuard se llena).
- **TSAN:** las pruebas unitarias (gcc y clang, también con las carreras
  forzadas del apretón de manos) sin avisos. En el laboratorio, `smoke` y
  `control` con binarios TSAN en los dos extremos encontraron una carrera
  anterior a este PR: `cg_tune` escribía el conjunto de CPU inicial en el
  hilo principal mientras el hilo del estado ya lo leía
  (`cg_thread_normal`); ahora lo toma un `pthread_once`. Después, `smoke` y
  `control` pasan sin avisos con `on` y con `off`. (El `cengarde` compilado
  con TSAN por gcc da dos avisos falsos de `-Wstringop-overflow` en
  `ctl.h`, también en master.)
- **Soak corto** (`SOAK_S=300`, 5 min por modo, `off` y `on`; sin netem en
  este contenedor, así que l3 alterna `tbf`): 4 tramos por modo sin pérdida,
  sin avisos de hilos parados ni sockets sin vigilar, RSS final 2,8 MB
  (`off`) y 3,5 MB (`on`). En 5 min no llegan ni la caída de l2 (cada
  10 min) ni la referencia de RSS: el soak de 1 h queda pendiente.

## Qué hacemos con esto

- `legacy` sigue por omisión en el motor y en OpenWrt (decisión del usuario,
  E.3); `on` queda a mano en LuCI («Activado (un hilo por enlace)») y `off`
  («Un hilo (estructura nueva)») sirve para separar un fallo de los hilos de
  uno del código nuevo.
- `legacy` se quita en la primera versión después de pasar la puerta P.
- Si la puerta P confirma el coste de CPU a poco tráfico, la primera
  alternativa es la propiedad adaptativa de los enlaces (el diseño, A.8):
  los enlaces se quedan en el hub mientras tenga CPU de sobra.

## Pendiente

- Puerta P en la Pi (banco cableado y enlaces reales); 3c (envío desde las
  bombas, arena, `sched.h`); `jitter.c` para `mtstall` (llega con 3a); el
  soak de 1 h y `mtcorr` a mano; el e2e de QEMU con `off` y `on` (añadido,
  sin correr aquí).

## Cambios

- 2026-10-04: creada con el PR 3b.
