# 011 — Hilos en los dos extremos: hilos por enlace en el router (PR 3b)

- **Fecha:** 2026-10-04 (actualizada el 2026-10-05 con la revisión del PR)
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
  todos los escenarios del laboratorio y el soak de 1 h; `on` también, y
  `legacy`, los escenarios. El e2e de QEMU pasa en los tres modos, salvo
  una comprobación de tiempo de la propia prueba que sin KVM también falla
  con el PR 2.

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
  (`cg_thread_normal`); ahora lo toma un `pthread_once`. Después, `smoke` y
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
  todo, también el paso 7 (`off`, `on` y otra vez `legacy`: cada cambio
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
  bombas, arena, `sched.h`); `jitter.c` para `mtstall` y el soak (llega
  con 3a); `mtcorr` a mano; el e2e de 24.10, que corre el CI del PR.
- Al rebasar sobre 3a: su paso de TSAN del laboratorio en `engine.yml`
  también con `link_threads = on` (`CLIENT_EXTRA`) en su lista de
  escenarios, y `mtstall` una vez, que elige su modo él solo; aquí ya
  pasan así sin avisos (arriba).
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
