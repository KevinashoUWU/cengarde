# 006 — Salud de los enlaces y baja latencia (Fase 2)

- **Fecha:** 2026-10-03 (actualizada el 2026-10-05 con la historia 011)
- **Estado:** vigente
- **Fuentes:** `engine/src/health.h` (lógica y tests), `client.c`, `server.c`,
  `proto.h`; `sudo bench/lab.sh health`, `latency` y `compare`; historias 003
  y 005.

## TL;DR

- **Quién decide:** cada extremo decide por qué enlaces envía (el cliente, la
  subida; el servidor, la bajada). Lo hace con el retraso de ida que mide el
  otro extremo en las sondas (protocolo v2).
- **Silenciado:**
  - un enlace que va más de 150 ms por detrás del más rápido durante 2 s deja
    de llevar datos, pero conserva socket y sondas;
  - vuelve tras 4 s a menos de 120 ms, y cada vuelta que no aguanta duplica
    esa espera (hasta 32 s);
  - siempre llevan todo al menos 2 enlaces, y el más rápido nunca se silencia.
- **Mudo:**
  - un enlace mudo en cualquiera de los dos sentidos deja de llevar datos en
    los dos, en unos 1–1,5 s;
  - el umbral es mayor que los 300 ms de libRIST porque un enlace que se llena
    también enmudece un rato, y eso lo tiene que resolver el silenciado.
- **Sondeo:** cada 100 ms con tráfico y cada segundo en reposo.
- **Laboratorio:**
  - con un enlace de 500 ms de cola, se silencia en 3–3,6 s en los dos sentidos;
  - el túnel no pierde nada y el enlace no recae al recuperarse;
  - con 3 enlaces el coste por paquete no cambia.
- **Baja latencia:**
  - `busy_poll_us = 200` baja la mediana de 107–142 µs a 57–74 µs a cambio de
    ~75 % de CPU por extremo (antes ~12 %); `busy_poll_us = 50` la deja en
    69–113 µs con ~27 %;
  - también hay `cpu` y `rt_priority`;
  - el estado JSON se escribe desde un hilo aparte.

## Contexto

Fase 2 del roadmap. El usuario pidió seguir con ella y dijo que las perillas
de baja latencia son buena idea. Después viene la compilación para OpenWrt
limpio (historia 004). El diseño parte de libRIST (historia 003), pero
adaptado a la redundancia pura: todos los enlaces activos llevan todo.

## Decisiones

### Señal: retraso de ida relativo, medido con sondas

- **Medida:** cada sonda y cada respuesta llevan la marca de tiempo del
  emisor. El receptor calcula `llegada - ts` y la devuelve en la siguiente
  sonda o respuesta del mismo enlace (`owd` con `CG_F_OWD`). Los relojes no
  están sincronizados, pero el desfase es el mismo para todos los enlaces de
  una sesión y se anula al compararlos. La deriva también.
- **No el RTT** (el `rtt-drop` de libRIST): el RTT mezcla los dos sentidos.
  En celular lo habitual es que se hinche la subida, y por el RTT se
  silenciaría también la bajada. Con el retraso de ida, cada sentido lo decide
  su emisor.
- **Solo sondas, no datos:** los duplicados se descartan antes del MAC, así
  que su marca de tiempo no está autenticada. Las sondas son siempre nuevas y
  se verifican, y todos los enlaces se muestrean por igual.
- **Relativo al enlace activo más rápido**, no un techo absoluto:
  - no depende del RTT base (sirve igual con un enlace por satélite);
  - un enlace con más retraso base que el umbral queda silenciado como reserva
    y vuelve si hace falta.
- **Filtro:** EWMA con ganancia 1/4 en el emisor. Un informe vale durante
  3 intervalos de sondeo en reposo; sin informes frescos no se decide nada.

### Máquina de estados (`src/health.h`)

- **Estados:** ACTIVO ⇄ SILENCIADO, con histéresis (150 / 120 ms) y esperas
  asimétricas (2 s para silenciar, 4 s para volver).
- **Espera exponencial:** cada vuelta que no dura 30 s duplica la siguiente
  espera, hasta ×8.
- **`min_active_links = 2`:** primero la redundancia. Silenciar nunca deja
  menos de 2 enlaces llevando todo, y si un activo enmudece, el silenciado más
  rápido vuelve al instante. Con 2 enlaces, nada se silencia.
- **Un mudo congela su estado, no lo reinicia:** un enlace que se llena
  enmudece mientras dura el salto de retraso y pierde la mayoría de las
  sondas. Si eso reiniciara la espera de 2 s, nunca se silenciaría (pasó en
  el laboratorio: ver Medidas).
- **Sin rampa:** libRIST sube el peso en rampa porque reparte (WRR). En
  redundancia pura, duplicar solo una parte de los paquetes por un enlace más
  rápido que los demás hace que esos paquetes se adelanten a los otros. Eso
  es reordenación, que a TCP le sienta mal. Además, las copias tardías de un
  enlace que vuelve son duplicados inofensivos. Así que la vuelta es de golpe,
  con espera exponencial; la rampa queda para el WRR de la Fase 5.
- **Goteo opcional** (`mute_trickle`, 0 por defecto): las sondas ya miden, y
  el goteo solo añadiría reordenación.
- **Nombres:** `mute_behind_ms`, `unmute_behind_ms`, `mute_settle_ms`,
  `mute_trickle`, `min_active_links`. Son más claros que los `drop` /
  `restore` de libRIST que proponía la historia 003.

### Mudo (stall)

- **Cliente:** 3 sondas seguidas sin respuesta y la más vieja fuera más de
  2 RTT + 1 s.
- **Servidor:** 3 intervalos anunciados + 1 s sin paquetes del cliente, o sin
  sondas que digan que les llegan las respuestas.
- **Exige los dos sentidos:** un enlace que solo funciona en un sentido no se
  usa en ninguno.
- **Por qué no los 300 ms de libRIST:** con 100 ms de margen, el enlace con
  cola alternaba entre mudo y vivo y nunca se silenciaba (ver Medidas).
- **Coste:** un enlace muerto se detecta en 1–1,5 s en vez de 0,3 s. Con
  `min_active_links = 2`, mientras tanto otro enlace lleva todo y no se
  pierde nada.

### Sondeo adaptativo

- **Ritmo:** cada 100 ms si hubo datos en el último `probe_idle_ms`, y si no,
  cada `probe_idle_ms` (1 s).
- **Intervalo anunciado:** el cliente anuncia su intervalo en cada sonda y el
  servidor calcula con él su umbral de mudo. Un cambio de ritmo se anuncia en
  el acto.
- **Coste:** ~68 B por sonda o respuesta.
  - Con tráfico: 5,4 kbit/s por enlace y sentido.
  - En reposo: ~6 MB al día por enlace y sentido (~47 MB al día con 4 enlaces
    contando los dos sentidos).
  - Con planes de datos limitados conviene subir `probe_idle_ms`.

### Protocolo v2

`cg_probe_info` crece a 24 B (`echo_ts`, `owd`, `interval_ms`, `rx`, `wins`,
`lag_us`). Las sondas y respuestas usan dos flags de cabecera:
- `CG_F_OWD`: hay medida;
- `CG_F_MUTED`: el emisor no lleva datos por ese enlace. Así cada extremo
  muestra lo que decidió el otro.

Un extremo v2 rechaza paquetes v1.

### Baja latencia

- **`busy_poll_us`:** tras tráfico, `epoll_wait` con espera 0 durante ese
  tiempo en vez de dormir. Sin tráfico duerme como siempre.
- **`cpu` y `rt_priority`:** afinidad y `SCHED_FIFO`. Si hay prioridad de
  tiempo real con una sola CPU o con el proceso fijado a una, el motor ignora
  `busy_poll_us`: un proceso de tiempo real que no duerme dejaría sin CPU a
  los hilos del kernel que le entregan los paquetes.
- **Estado en un hilo aparte** (`cg_status_writer`): el bucle entrega el JSON
  con `trylock`, y si el escritor está ocupado se pierde esa instantánea; el
  bucle nunca espera. Esto quita el bloqueo del `rename` en ext4 o en una SD
  lenta (historia 005).
- **Logs:** una sola escritura por línea, para que no se mezclen entre hilos.

## Medidas

Misma VM que en las historias 001 y 005 (Xeon 2,1 GHz, 4 vCPU), paquetes de
1400 B.

### Escenario `health`

- **Montaje:** 2.000 pps (22 Mbit/s) durante 50 s con 3 enlaces. Entre los
  segundos 3 y 25, l3 pasa a 5 Mbit/s con 500 ms de cola (`tbf`).
- **Socket:** el escenario sube el `sndbuf` a 4 MiB. Con el valor por defecto
  (~208 KiB), el buffer del socket limita la cola local a ~190 ms a 5 Mbit/s.
- **Resultado en los dos sentidos:**
  - silenciado 3,0–3,6 s después de formarse la cola (dos pasadas), a
    549 ms por detrás;
  - 2–3 silenciados mientras duró, por las vueltas fallidas con espera
    creciente;
  - 99.999 de 99.999 paquetes entregados;
  - ningún silenciado después de quitarse la cola y estado final activo;
  - en subida, volvió para quedarse 17,7 s después de quitarse la cola, por la
    espera exponencial.

### Cómo se llegó a los umbrales de mudo

Misma prueba con el `sndbuf` por defecto:

| Margen de mudo | Resultado |
| --- | --- |
| 100 ms (como libRIST) | l3 alterna mudo/vivo, lleva el ~21 % de los paquetes (reordenación) y no se silencia en 22 s |
| 500 ms y estado congelado | se silencia, pero a los 20 s |
| 1 s | se silencia a los 6 s, con ~186 ms medidos (justo por encima del umbral por el límite del `sndbuf`) |

### Caída en un solo sentido

- **Montaje:** bajada a 2.000 pps; el servidor deja de alcanzar l3 (ruta
  `blackhole`).
- **Resultado:** los dos extremos lo dan por mudo en menos de 2 s, lo
  recuperan en ~1 s y no se pierde nada.

### Latencia y CPU (`latency`)

- **Montaje:** 1 enlace (sin copias que compitan), 10.000 pps; rango de dos
  pasadas. Mediana de latencia; CPU por extremo.

| Configuración | Bajada | Subida |
| --- | --- | --- |
| cengarde, `busy_poll_us = 0` | 107–111 µs, 11–13 % | 139–142 µs, 11–13 % |
| cengarde, `busy_poll_us = 50` | 69 µs, ~28 % | 110–113 µs, ~27 % |
| cengarde, `busy_poll_us = 200` | 57–60 µs, ~76 % | 71–74 µs, ~72 % |
| engarde Go | 85–89 µs, 22–26 % | 90–94 µs, 23–27 % |

- **Margen de espera:** sirve si cubre el hueco entre paquetes, que a
  10.000 pps es de 100 µs. Por eso 50 µs ayuda poco en la subida.
- **Picos de p99.9:** salen de 2 a 54 ms, con y sin archivo de estado (4
  pasadas de cada). Son ruido de la VM, no el escritor del estado.

### Regresión (`compare`, 3 enlaces)

- **CPU** (µs/paquete, cliente / servidor), frente a la historia 005:
  - bajada a 10.000 pps: 13,2 / 17,0 (antes 13,4–14,2 / 17–19);
  - subida a 60.000 pps: 8,4 / 7,7 (antes 8,3–8,6 / 7,4–7,8).
- **Pérdida:** 0 en todas las pruebas de cengarde.

## Qué hacemos con esto

- **CI:** el laboratorio corre `smoke` y `health`.
- **Configuración por defecto:** silenciado a 150 ms y sin espera activa
  (`busy_poll_us = 0`). La espera activa interesa en el VPS o en un x86 con
  CPU de sobra, no en una Pi justa de CPU.
- **OpenWrt (Fase 3):** recomendar SQM (cake) en los uplinks con cola local,
  y documentar `probe_idle_ms` para planes con cuota.

## Pendiente

- **Servidor multihilo** (`SO_REUSEPORT`) y GSO/GRO: aplazados hasta medir en
  el VPS. Las colas `SO_REUSEPORT`, todavía con un hilo, llegaron con el
  PR 3a (historia 011); los hilos del servidor siguen pendientes (PR 3e).
  - Estimación: un cliente a ~100 Mbit/s con 4 enlaces son ~36.000 copias/s.
  - A 8–10 µs por copia (laboratorio), es un 30–40 % de una vCPU.
- **Perfilado en la Pi** (`perf`) y ajuste del tamaño de lote.
- **Cola remota:** el laboratorio solo emula colas locales. La cola real
  estará en el módem 5G; validar los umbrales con los enlaces reales, y con
  jitter y pérdida (`netem`) en una máquina que lo tenga.
- **Enlaces asimétricos:** usar el sentido que sí funciona exigiría informar
  de un enlace por otro (Fase 5).
- **Espera exponencial hasta 32 s:** puede ser conservadora; ajustarla con
  datos de campo.
- **`UdpRcvbufErrors`:** no se contaron en esta fase. La pérdida 0 hasta
  60.000 pps lo sugiere, pero no lo mide.
- **Socket compartido del servidor:** la cola local de un camino lento en el
  servidor ocuparía su `sndbuf`, compartido con los demás caminos. Con 4 MiB y
  sin cuello local en el VPS no se nota, pero un servidor con cuello local
  necesitaría un socket por camino.

## Cambios

- 2026-10-03: creada con la Fase 2 (salud de enlaces, protocolo v2 y perillas
  de latencia).
- 2026-10-05: las colas `SO_REUSEPORT` del servidor, con un hilo (historia
  011, PR 3a).
