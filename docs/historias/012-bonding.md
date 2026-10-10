# 012 — Bonding para cengarde: qué dice la evidencia y el plan de la Fase 5

- **Fecha:** 2026-10-10
- **Estado:** vigente. Plan de la Fase 5 (bonding). Paso 0 hecho en parte: el laboratorio `bench/lab.sh bond` con su línea base y el registrador de campo (`cengarde-rec`, `bench/fieldrec.py`); falta el cliente QUIC, la dispersión y la semana de datos de la Pi.
- **Fuentes:**
  - estudio con 103 agentes (2026-10-10): 60 fuentes primarias más 11 de huecos (normas RIST/SRT/MPTCP/MPQUIC/MP-DCCP/ATSSS/TR-348/RFC 8157, papers de planificadores y de FEC multicamino, código de SRTLA/MLVPN/glorytun/OpenMPTCProuter/ZeroTier/KCP, productos comerciales, medidas de 5G/LTE/Starlink, RACK-TLP/QUIC/WireGuard); la lista con acceso y veredictos está en [012-bonding/fuentes.md](012-bonding/fuentes.md) y las síntesis por eje en [012-bonding/ejes.md](012-bonding/ejes.md);
  - el estudio completo (con la matriz de SmoothStream y las fichas con cada cita) vive en el repo de SmoothStream, `estudio-bonding/`;
  - código de cengarde leído para la matriz: `proto.h`, `proto.c`, `replay.h`, `clientpath.h`, `epoch.h`, `health.h`, `arrival.h`, `client.c`, `server.c`.

## TL;DR

- **Hoy no hay nada roto:** esto es el plan del bonding futuro. La redundancia pura sigue igual.
- **Primero el laboratorio:** los objetivos de la Fase 5 (≥ 1,8× con 2 enlaces iguales, FEC ≤ 1,34×) solo se pueden medir con TCP y QUIC reales dentro de WireGuard, sobre netem por enlace. Ya está `bench/lab.sh bond` (TCP; QUIC falta), y su línea base dice que la redundancia de hoy entrega lo del mejor enlace y esconde el spread, la muerte de un enlace y los picos.
- **Sin reordenador no se puede repartir:** RACK, el QUIC de Chrome y Windows confunden un spread de 50–150 ms con pérdidas. El reordenador va sobre la **secuencia autenticada de cengarde** (32 bits, bajo el MAC), no sobre el contador de WireGuard como decía el ROADMAP.
- **En bonding un enlace caído pierde su parte:** el mudo de 1–1,5 s sirve para redundancia, no para repartir. Hace falta una exclusión rápida (~200–300 ms, el contador de entregas verificadas no avanza) separada del mudo, y control de cola (un AQM virtual en `cg_sched_mask`).
- **El planificador es un candidato:** la llegada esperada (ETA = OWD + cola/capacidad) tiene que ganarles a un WRR por capacidad y a minRTT con tope. ARQ de un reintento, FEC XOR y k = 2 se comparan entre sí en el mismo paso; solo la FEC densa queda descartada.
- **Proceso:** Haiku extrajo 542 afirmaciones; Sonnet verificó cada cita (78 % verificadas, 21 % matizadas, 0,4 % refutadas); Opus sintetizó, armó las matrices y una auditoría densa corrigió la de cengarde.

## Contexto

El usuario pidió un estudio amplio de bonding (libRIST, SRT/SRTLA/BELABOX, glorytun, MPTCP, MPQUIC, Peplink, Mushroom, Speedify, broadcast comercial, papers) para dos productos: cengarde (redundancia y bonding de internet, velocidad y latencia, CPU de la Pi) y SmoothStream (broadcast sobre RIST, buffers densos, ARQ y FEC RLC). Se hizo con agentes en tres niveles:

| Nivel | Modelo | Agentes | Trabajo |
| --- | --- | --- | --- |
| Catálogo | Haiku | 18 | 175 candidatos, 2 modos de búsqueda por familia |
| Selección | Sonnet | 1 | 60 fuentes, 34 descartes con motivo |
| Extracción | Haiku | 60 | una ficha por fuente, citas textuales verificadas por grep sobre el texto descargado |
| Supervisión | Sonnet | 9 | 424 verificadas, 114 matizadas, 2 refutadas, 1 no verificable; 142 correcciones, 50 afirmaciones nuevas |
| Síntesis, matrices | Opus | 6 | 8 ejes, una matriz por producto |
| Auditoría | Opus (esfuerzo máximo) | 1 | 16 problemas (1 grave) y 6 huecos |
| Huecos y edición final | Haiku, Sonnet, Opus | 8 | 11 fichas más, matrices finales |

Duró ~3 h con 2 agentes a la vez y 17,4 M tokens. Lecciones del proceso:
- Haiku no inventa citas, pero interpreta mal ~1 de cada 5 (condiciones omitidas, «should» leído como «debe», modelos llamados «medido», artefactos del PDF leídos como cifras). No sirve sin un supervisor.
- El contexto base de cada subagente fue de ~148 000 tokens (definiciones de herramientas y el `CLAUDE.md` viejo de SmoothStream que esta sesión tenía cargado), así que todas las llamadas de Haiku cayeron en su tarifa de más de 100 000 tokens. Para el próximo estudio: una sesión con contexto liviano.
- El contexto que les di a los agentes decía que cengarde deduplica por el contador de WireGuard. Es falso (lo hacía engarde Go); la auditoría lo cazó leyendo el código. Por eso la síntesis de reordenamiento del anexo está construida sobre esa premisa y la matriz de abajo la corrige.

Convención de la matriz: [ficha Ax] afirmación verificada de una ficha (los ids están en `012-bonding/fuentes.md`); [código: archivo:línea]; [razonado] inferencia; [MEDIR] no se sabe sin medir; [supuesto] valor de escenario.

## Hallazgos

### Lo que el código cambia del plan [código]

1. **La secuencia es de cengarde.**
   - Es de 32 bits por sesión y sentido, va bajo el MAC y la comparten todos los tipos de mensaje [proto.h:13; clientpath.h:71]. La dedup y el anti-replay la usan [replay.h]. No hay dedup por índice receptor + contador + tag de WireGuard.
   - Ventajas para reordenar: solo entran paquetes verificados, no hay rekeys ni índices, los handshakes también llevan secuencia y no depende del orden en que WireGuard emite.
   - Obligaciones: reiniciar el reordenador cuando se reinicia la ventana (`CG_RXV_RESET`, clientpath.h:135; epoch.h) o cambia la sesión, y comparar en aritmética serial de 32 bits, como `cg_replay_check` [replay.h:43].
2. **La secuencia no es densa en datos.** Las sondas comparten la secuencia y cada una va por un solo enlace, así que un hueco no dice si se perdió un DATA o una sonda.
   - En v4: un espacio de secuencia propio para sondas, informes, NACK y paridad. Como mínimo, informar cuántas sondas salieron por intervalo.
   - Costo: hay que rehacer la detección de reinicio de epoch.h, que hoy se apoya en que las respuestas de sonda comparten la secuencia con DATA [razonado].
3. **Marca de tiempo autenticada (bytes 12–15)** [proto.h:14]. Al repartir, cada paquete da una OWD verificada por enlace.
   - Los relojes de las dos puntas no están relacionados («only means something compared with the other paths», proto.h:77), así que la OWD arrastra un desfase arbitrario.
   - Valen las diferencias entre enlaces y el exceso sobre el mínimo propio, con umbrales aditivos (+X ms). Los múltiplos de la base (2×, 3×) no significan nada.
   - Para reglas multiplicativas está el RTT de sonda que calcula el cliente (`srtt8_us`, client.c:800). El servidor no lo tiene: habría que informárselo en v4.
   - La base tiene que ser un mínimo de ventana reciente, porque un retardo medido sin relojes sincronizados exige la misma frecuencia de reloj en las dos puntas [G5-scream a5].
4. **`rx` no sirve para decidir.** Se llena con wins + dups contados antes del MAC [client.c:1010, server.c:456], aunque proto.h:84 diga «verified».
5. **Hay flags libres en DATA.** El byte 1 está autenticado y vale 0 en DATA [proto.h:8]. En v4 puede llevar la marca «repartido / k copias» y los 2 bits de ECN del datagrama de WireGuard.
6. **Tipos desconocidos y sondas rellenadas.** En v3, `cg_hdr_parse` rechaza un tipo desconocido, y también una sonda cuya carga no mida exactamente 24 B [proto.c:63-71]. El cliente cuenta ambos casos como malformados [clientpath.h:121]. En v4, los tipos nuevos se descartan en silencio, con un contador propio.
7. **Hilos.** La puerta C1 no pasó: la CPU del cliente sube +127 % a 2 kpps y +89 % a 20 kpps (historia 011). El bonding no puede depender de un hilo por enlace: la cola virtual, la exclusión y el AQM viven en el hilo principal.
8. **La salud se calibró para redundancia.**
   - El mudo tarda 1–1,5 s porque, mientras tanto, «a dead link costs only its own copies» [health.h:42-43; historia 006].
   - Un informe de OWD sigue vigente 3 × probe_idle_ms = 3 s [client.c:825].
   - En bonding eso pierde la parte del enlace: hace falta la exclusión rápida.
9. **Hay dos topes de desorden, ambos de 8128.**
   - cengarde: 8192 − 64 secuencias, sondas incluidas [replay.h:16-18].
   - WireGuard del kernel: 8192 − BITS_PER_LONG = 8128 contadores en 64 bits [G1-wg-kernel A1, A2; código v6.12]. La Pi es aarch64 y el VPS x86_64 (historia 004).
   - En tiempo, el tope depende de la tasa en paquetes [razonado]. Con 1420 B son ~0,9 s a 100 Mbit/s y ~0,3 s a 300 Mbit/s. Con una media de 400 B son ~0,26 s a 100 Mbit/s.
10. **ECN.**
   - cengarde no toca ECN hoy: no hay IP_TOS ni IP_RECVTOS en engine/src [código].
   - WireGuard copia el ECN interior al exterior al cifrar. Al descifrar, pasa un CE exterior al paquete interior si este es ECT, y no descarta las combinaciones inválidas [G1-wg-kernel A3–A5; G1-ecn-2020 B3].

### Por qué este orden

- **Sin reordenador no se puede repartir.**
  - RACK arranca con una ventana de min_RTT/4 y la agranda con DSACK hasta SRTT [F9-rfc8985-rack-tlp; G3-dropbox-rack A8].
  - El QUIC de Chrome declara pérdida por tiempo con max_rtt·(1 + 2^−shift), y por paquetes con un umbral que crece tras cada pérdida espuria. Por defecto, el umbral de tiempo no se adapta [G3-quiche-loss Q1–Q3, Q6, Q7].
  - Windows trae RACK-TLP con heurística de reorden desde la build 21332 [G3-dropbox-rack A5, A6]. Antes, en Dropbox, un reorden de grado ≥ 3 en el borde frenó durante años la subida, solo de los clientes Windows [G3-dropbox-rack A1, A4].
  - Un spread de 50–150 ms expuesto al tráfico interior daría pérdidas falsas. Las normas que reparten por paquete ponen un reordenador en el receptor [F2-rfc8157-gre-bonding, F2-bbf-tr348, F2-rfc8684-mptcp].
- **Hay que estimar antes de planificar.**
  - Si C_i se sobrestima, la cola se va al módem [F3-cech-thesis, F8-dual-lte-mptcp, F1-srt-draft].
  - En celular, la congestión aparece como retardo y no como pérdida [F8-bufferbloat-3g4g]. En un handover, los bearers best-effort encolan en vez de descartar [G5-rmcat-wireless a2].
- **La ETA es solo un candidato.**
  - Ninguna fuente la mide sobre radio.
  - QAware vale solo como analogía: es un banco Gigabit con < 1 ms de retardo, donde la cola es local y se mide (DQL/BQL). cengarde, en cambio, estima la suya [F3-qaware; razonado].
  - Con proporciones fijas, todo el tráfico paga la OWD del enlace más lento con peso [F5-mlvpn, F2-quic-multipath]. Por eso el WRR por capacidad es el comparador natural.
- **La exclusión y el control de cola van en el mismo paso que el reparto.** Sin ellos, el reparto pierde en ráfaga la parte de un enlace caído y lleva el bufferbloat al CPE y a la celda.
- **La CPU decide el modo.** Duplicar a N cuesta N envíos por paquete por el mismo adaptador USB (historia 004). Repartir los paquetes grandes libera CPU.
- **El ARQ se compara a la par de la FEC.**
  - Con el reordenador reteniendo el hueco, el tráfico interior no ve paquetes posteriores ni manda SACK, así que RACK no puede declarar la pérdida.
  - Solo actúan TLP/PTO (~2·SRTT en Linux; SRTT + 4·RTTVAR + max_ack_delay en QUIC) o el RTO (≥ 200 ms) [F9-rfc8985-rack-tlp, F9-rfc9002-quic-loss, F9-linux-ip-sysctl; razonado].
  - La XOR exige la misma retención y cuesta siempre 25–33 % de bytes. El ARQ solo cuesta cuando hay pérdida.

### Riesgos que no resuelve ninguna fuente

- **Fallos comunes:** USB, Pi, VPS.
- **Correlación entre operadores en Santiago.** En un estadio de EE. UU., la falla conjunta siguió a la arquitectura: los dos operadores NSA, con ancla LTE, cayeron juntos y el operador SA no [G4-apps b1, b4, b5]. cengarde no ve la radio.
- **Bordes de 15 s de Starlink en Chile.** Los valores de starlink15.sh son [supuesto]: la Fig. 16b de Mohan y las figuras de Tanveer siguen sin extraer. cake-autorate quitó su compensación para Starlink por falta de evidencia [G2-cake-autorate a10].
- **FIFO dentro de cada enlace:** no está comprobado.
- **ECN real:** si el TOS del datagrama de WireGuard llega a cengarde por loopback, y cuánto tráfico interior negocia ECN.
- **iOS y macOS (XNU):** su tolerancia al desorden no se leyó.

Todo eso es [MEDIR] antes de fijar umbrales.

### Línea base de la redundancia: el paso 0 [medido]

Fuente: `bond.yml` en CI (run 38077768991, 2026-10-10), una repetición en un
runner compartido, 10 s por corrida (31 s en `starlink15`). Las cifras son de
subida; la bajada da lo mismo con ±1 Mbit/s. Enlaces: Mbit/s / retardo de un
sentido en ms.

| Caso | Enlaces | Mbit/s | Retransm. Cubic / BBR | RTT p50 Cubic / BBR (ms) |
| --- | --- | ---: | --- | --- |
| `equal` | 2 × 50/25 | 43,5 (solo: 43,5) | 34 / 93 | 109 / 52 |
| `het` | 60/20, 30/35, 10/60 | 52,6 (solo: 52,6) | 44 / 0 | 103 / 42 |
| `reorder50` | 50/20, 50/70 | 43,9 | 31 / 0 | 105 / 43 |
| `reorder150` | 50/20, 50/170 | 43,9 | 31 / 0 | 104 / 43 |
| `linkdeath` | 3 × 40/25, l2 cae un tercio | 35,1 | 67 / 73 | 107 / 52 |
| `starlink15` | 50/25, 100/20↔50 con cortes de 1,5 s | 80,5 | 1268 / 2804 | 99 / 75 |
| `ltespike` | 50/25 (+300 ms 1 s de cada 5), 50/30 | 43,6 | 29 / 92 | 109 / 56 |
| `deepq` | 20/25 con 500 ms de cola, 50/25 | 43,5 | 33 / 94 | 108 / 52 |

- **Hoy se recibe el mejor enlace, no la suma:** 43,5 Mbit/s con dos de 50, y
  lo mismo que con el primero solo. Es la vara del objetivo de ≥ 1,8×
  (≥ 78 Mbit/s en `equal`).
- **La redundancia esconde lo que el bonding tendrá que manejar:** el spread
  de 100 y 300 ms (gana la primera copia), la muerte de un enlace, los picos
  de LTE y la cola profunda no cambian ni el goodput ni el RTT.
- **Cubic llena la cola del cuello de botella** (RTT p50 ≈ el doble de la base
  de 50 ms); BBR no. El control de cola del paso 3 tiene que conservar eso
  al repartir.
- **`starlink15` es el único caso con muchas retransmisiones.** La
  explicación probable [razonado; MEDIR]: mientras l2 va a 50 ms, l1 llega
  primero pero solo lleva 50 Mbit/s, y el resto llega 25 ms después por l2,
  desordenado; en cada corte, el TCP que iba a 80 Mbit/s choca con la cola de
  l1. O sea que la redundancia de hoy ya reordena cuando el enlace de menor
  retardo es el de menos capacidad, y el reordenador del paso 1 también le
  serviría.

## Qué hacemos con esto

### Orden de la Fase 5 y criterios de salida

Los umbrales son propuestas [razonado] y se miden en `bench/`.

| Paso | Qué | Criterio de salida medible |
| --- | --- | --- |
| 0 | Laboratorio: WireGuard del kernel + iperf3 (Cubic/BBR) + un cliente QUIC, sobre netem por enlace; mismo build, n ≥ 10 con dispersión. Instrumentos pasivos: tardanza contra plazo absoluto, pérdida en todos los enlaces, histograma de tamaños y tipos | Corren en el CI `equal`, `het`, `reorder50`/`reorder150`, `linkdeath`, `starlink15` [supuesto], `ltespike` y `deepq` (hecho, sin QUIC y con n = 1). Hay una semana de datos de la Pi con 4×5G + Starlink, con las métricas de RFC 8869 (pérdida, retardo, estabilidad de la tasa) |
| 1 | Reordenador por la secuencia de cengarde (8192 ranuras, liberación por enlace, nunca descarta) y formato v4 de DATA (marca de reparto, bits ECN, secuencia de control aparte) | Con reparto forzado y 0/50/150 ms de spread, las retransmisiones del TCP interior quedan ≤ 1,2× las de un solo enlace. 0 descartes OLD. Desborde con IMIX sin descartes. Reinicio correcto tras CG_RXV_RESET y tras reiniciar el servidor (restart.sh). En redundancia pura, +0 ms y ≤ +5 % de CPU |
| 2 | Estimador por enlace (OWD verificada, base, exceso aditivo, C_i) e informe v4 | C_i dentro de ±15 % del límite de netem 2 s después de un escalón. 50 ms de exceso detectados en ≤ 300 ms. Un escalón de +30 ms cada 15 s [supuesto] no se marca como congestión |
| 3 | `cg_sched_mask`: exclusión rápida, AQM virtual, clases y planificador (ETA frente a WRR por capacidad y minRTT con tope), guarda y rampa | Ver la lista debajo de la tabla |
| 4 | k-de-N y duplicación disparada (experimentos) | Se adoptan solo si, con los instrumentos del paso 0, P(el par elegido tarde o perdido) ≤ 2× P(todos tarde o perdidos) |
| 5 | Recuperación ante caída o pérdida: ARQ de un reintento, FEC XOR y k = 2, en la misma comparación | En linkdeath.sh y netem con 0,5–1 % de pérdida, el TCP interior no hace RTO durante la detección. XOR con ≤ 1,34× de bytes. Gana el que menos HOL suma a un flujo de VoIP que comparte el túnel, por cada byte extra. Si ninguno supera a duplicar con k = 2 los paquetes grandes durante la sospecha, queda k = 2 |
| 6 | Pacing real y AQM en hilos | Solo tras la puerta P en la Pi: la OWD con carga al 90 % de C_i baja ≥ 50 % sin perder más de 5 % de goodput, y mejora lo que logra el AQM virtual |

**Criterios de salida del paso 3:**
- Con 2 enlaces iguales, ≥ 1,8× uno solo, con 1 flujo Cubic y con 4 BBR.
- Con enlaces heterogéneos, nunca menos que el mejor enlace solo.
- Con 10 % de carga, p50 ≤ redundancia + 2 ms. La ETA se queda solo si gana en el p99 sin perder goodput en saturación.
- En linkdeath.sh y con un corte de 1,5 s en starlink15.sh: paquetes perdidos por evento ≤ la parte del enlace × 300 ms, y 0 RTO del TCP interior con Cubic y con BBR.
- Con cola profunda (deepq, bond con 500 ms): OWD bajo carga ≤ base + objetivo, con goodput ≥ 90 %.
- CPU por paquete útil con 4 enlaces ≤ 60 % de la de redundancia.

**Salida global de la Fase 5:** en campo, el agregado da ≥ 1,5× el mejor enlace en 4 de 5 franjas horarias, y el p99 del RTT interior bajo carga queda ≤ el del mejor enlace solo + 30 ms [razonado; MEDIR].

### Decisiones para el dueño

- **Pesos tipo libRIST.** Propongo conservar la semántica: 0 = duplicar siempre; > 0 = tope o valor inicial de C_i. El WRR fijo no sería el planificador, salvo que gane en el paso 3.
- **FEC ≤ 1,34× con 4 enlaces.** Solo es posible si ningún enlace lleva más del ~25 % del tráfico protegido, porque la expansión mínima es 1/(1−s) [razonado]. Eso choca con repartir por capacidad: hay que elegir qué prima.
- **ARQ.** Pasa a experimento. El ROADMAP ya lo pide como «ARQ opcional».
- **ECN.** Marcar CE solo en los datagramas ECT y descartar los Not-ECT es una política de cengarde: WireGuard no descarta [G1-ecn-2020 B3, B6].
- **Formato v4.** Incluye la marca de reparto, los bits de ECN, la secuencia de control aparte (toca epoch.h), el informe por enlace y, si se quiere una prueba activa de MTU, una sonda rellenada. Puede ir con el v4 del multicliente o en la versión siguiente.
- **Corregir los textos.** ROADMAP.md:582-584 («reordenar usando el contador de WireGuard») y el contexto del estudio («dedup exacta por índice receptor + contador + tag») tienen que pasar a la secuencia de cengarde.

## Pendiente

- Paso 0, lo que falta:
  - un cliente QUIC dentro del túnel;
  - n ≥ 10 con dispersión (`workflow_dispatch` de `bond.yml` con más repeticiones);
  - los instrumentos pasivos: tardanza contra plazo, histograma de tamaños;
  - comprobar por qué `starlink15` retransmite tanto;
  - una semana de la Pi con `cengarde-rec` (guía de OpenWrt) y su informe de `bench/fieldrec.py`.
- Medir lo marcado [MEDIR] y [supuesto] antes de fijar umbrales: el patrón de 15 s de Starlink en Chile, la correlación entre operadores en Santiago, si el TOS del datagrama de WireGuard llega a cengarde por loopback, y la tolerancia al desorden de iOS/macOS.
- Elegir con el protocolo v4: marca de reparto, bits ECN, secuencia de control aparte (toca `epoch.h`) e informe por enlace.

## Cambios

- 2026-10-10: creada con el estudio de bonding.
- 2026-10-10: paso 0 en parte. `bench/lab.sh bond` y `bond.yml`, con la línea base de arriba. El registrador de campo: `cengarde-rec` en el paquete de OpenWrt, `bench/fieldrec.py` con su prueba y `lab.d/fieldrec.sh`, y `time_ms` en el estado del motor.
