# 012 — Bonding para cengarde: qué dice la evidencia y el plan de la Fase 5

- **Fecha:** 2026-10-10 (actualizada el 2026-10-12 con el paso 1 y el 2026-10-13 con la primera prueba de campo del reparto y `split_spread_copy`)
- **Estado:** vigente. Plan de la Fase 5 (bonding). Paso 0 hecho salvo los datos de campo: el laboratorio `bench/lab.sh bond` con TCP y QUIC y su línea base, la dispersión con repeticiones, los instrumentos pasivos del motor (tardanza conjunta, pérdida en todos los enlaces, tamaños; paquetes 0.5.5) y el registrador de campo con su análisis (`cengarde-rec`, `bench/fieldrec.py`); falta la semana de datos de la Pi. Paso 1 hecho (paquetes 0.6.0): el reordenador en los dos extremos y el reparto experimental con sus guardas, medidos en el laboratorio.
- **Fuentes:**
  - estudio con 103 agentes (2026-10-10): 60 fuentes primarias más 11 de huecos (normas RIST/SRT/MPTCP/MPQUIC/MP-DCCP/ATSSS/TR-348/RFC 8157, papers de planificadores y de FEC multicamino, código de SRTLA/MLVPN/glorytun/OpenMPTCProuter/ZeroTier/KCP, productos comerciales, medidas de 5G/LTE/Starlink, RACK-TLP/QUIC/WireGuard); la lista con acceso y veredictos está en [012-bonding/fuentes.md](012-bonding/fuentes.md) y las síntesis por eje en [012-bonding/ejes.md](012-bonding/ejes.md);
  - el estudio completo (con la matriz de SmoothStream y las fichas con cada cita) vive en el repo de SmoothStream, `estudio-bonding/`;
  - código de cengarde leído para la matriz: `proto.h`, `proto.c`, `replay.h`, `clientpath.h`, `epoch.h`, `health.h`, `arrival.h`, `client.c`, `server.c`.

## TL;DR

- **Hoy no hay nada roto:** esto es el plan del bonding futuro. La redundancia pura sigue igual.
- **Primero el laboratorio:** los objetivos de la Fase 5 (≥ 1,8× con 2 enlaces iguales, FEC ≤ 1,34×) solo se pueden medir con TCP y QUIC reales dentro de WireGuard, sobre netem por enlace. Ya está `bench/lab.sh bond` (iperf3 con Cubic y BBR, y QUIC con aioquic), y su línea base dice que la redundancia de hoy entrega lo del mejor enlace y esconde el spread, la muerte de un enlace y los picos. Con 10 repeticiones, `equal` es estable, pero `reorder150` (Cubic) y `starlink15` (BBR) tienen dos modos: una corrida no basta para comparar.
- **Las retransmisiones de `starlink15` son pérdidas en los cortes, no desorden** [medido, 2026-10-11]: el 93–99 % son paquetes perdidos por todos los enlaces cuando, en cada corte de 1,5 s, el flujo de 80–90 Mbit/s cae sobre el enlace de 50 Mbit/s y su cola de 100 ms. Con dos enlaces FIFO, la redundancia no desordena. Lo que sí desordena, y es para el reordenador del paso 1, es la vuelta tras un corte: el enlace 5 ms más rápido adelanta a lo que fue solo por el otro.
- **Sin reordenador no se puede repartir:** RACK, el QUIC de Chrome y Windows confunden un spread de 50–150 ms con pérdidas. El reordenador va sobre la **secuencia autenticada de cengarde** (32 bits, bajo el MAC), no sobre el contador de WireGuard como decía el ROADMAP.
- **Paso 1, medido** [2026-10-12]: en redundancia el reordenador no cambia nada medible (10 corridas; esperar 20 ms en duplicado empeora QUIC, así que la espera queda en 0). Con el reordenador, el reparto de dos enlaces iguales da 1,98× uno solo con Cubic y BBR (QUIC 1,2–1,4×). Pero un reparto por turnos con 50–150 ms de dispersión se hunde (BBR a 1 Mbit/s, QUIC a 0,4) con el reordenador limpio: el receptor entrega al ritmo del enlace lento. La guarda `split_spread_ms` (30 ms) deja al lento con copias; repartir bien entre retardos distintos es el paso 3.
- **El reparto en la Pi, medido en campo** [2026-10-13, 2 tandas de 7 perfiles, 4 enlaces 5G]: repartida y con guardas permisivas, la subida llegó al doble de pico que copiando (115–126 frente a 63–68 Mbit/s); la bajada repartida, a menos (90–114 frente a 126–129). Con `split_spread_ms` 30 las guardas entraron en espiral: copias sobre los enlaces que ya iban atrasados, miles de envíos perdidos en el router, hasta 2,6 s de atraso. El laboratorio no reproduce esas pérdidas, y ahí las copias ayudan a QUIC: queda la perilla `split_spread_copy` (por omisión, las copias de la 0.6.2) para un A/B en campo.
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
   - Obligaciones: ~~reiniciar el reordenador cuando se reinicia la ventana (`CG_RXV_RESET`, clientpath.h:135; epoch.h) o cambia la sesión~~ (en v4 no hay reinicio de ventana: un servidor reiniciado sigue 2^20 más adelante, y el reordenador lo toma como un salto lejano; corregido el 2026-10-12 [código]), y comparar en aritmética serial de 32 bits, como `cg_replay_check` [replay.h:43].
2. ~~**La secuencia no es densa en datos.**~~ Lo era en v3; en v4 las sondas y sus respuestas van en una secuencia de control aparte, así que un hueco en DATA es un DATA que falta (corregido el 2026-10-12 [código: proto.h, clientpath.h]). Lo de abajo era el plan para v4. Las sondas comparten la secuencia y cada una va por un solo enlace, así que un hueco no dice si se perdió un DATA o una sonda.
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
- **`starlink15` es el único caso con muchas retransmisiones.** ~~La
  explicación probable [razonado; MEDIR]: mientras l2 va a 50 ms, l1 llega
  primero pero solo lleva 50 Mbit/s, y el resto llega 25 ms después por l2,
  desordenado; en cada corte, el TCP que iba a 80 Mbit/s choca con la cola de
  l1. O sea que la redundancia de hoy ya reordena cuando el enlace de menor
  retardo es el de menos capacidad, y el reordenador del paso 1 también le
  serviría.~~ Corregido el 2026-10-11 con la medida de abajo: no es desorden.

### `starlink15`, medido (2026-10-11)

Con WireGuard de espacio de usuario (wireguard-go) y un sustituto de netem
en espacio de usuario (el contenedor no tiene los módulos; reproduce la línea
base: `equal` 43,1–44,3 Mbit/s frente a 43,5, y `starlink15` Cubic de subida
1281–1690 retransmisiones frente a 1268), Cubic, 31 s de subida, con
`lost_all` (paquetes perdidos por todos los enlaces, del estado del motor):

| Variante | Retransmisiones | `lost_all` |
| --- | --- | --- |
| `starlink15` tal cual (4 corridas) | 1690 / 1491 / 1520 / 1281 | 1565 / 1396 / 1401 / 1150 |
| solo los cortes de 1,5 s | 1245 | 1038 |
| solo la alternancia 20↔50 ms (4) | 68 / 104 / 114 / 98 | 67 / 104 / 111 / 93 |
| l2 fijo a 50 ms / a 20 ms | 69 / 275 | 69 / 275 |
| capacidades cambiadas (l1 100, l2 50), con cortes (4) | 502 / 695 / 226 / 573 | 266 / 67 / 69 / 73 |
| las dos a 100 Mbit/s (4) | 585 / 53 / 523 / 59 | 59 / 52 / 264 / 48 |

- **Pérdida en los cortes:** el 93–99 % de las retransmisiones son paquetes
  perdidos por todos los enlaces, en picos de ~600 por corte. Al caer l2,
  el flujo que iba a 80–90 Mbit/s sigue solo por l1 (50 Mbit/s, 100 ms de
  cola). Con las capacidades cambiadas, el corte no cuesta nada: lo que se
  pierde es el arranque lento.
- **La redundancia no desordena:** l1, el de menos retardo, casi nunca gana
  (su cola está llena: el 46 % de sus copias no llega); sin cortes, gana
  6–119 paquetes de unos 262 000. Con dos enlaces FIFO y cada paquete por
  los dos, las primeras copias salen en orden.
- **Lo espurio sí es desorden:** tras un corte, el enlace que vuelve 5 ms
  más rápido adelanta a lo que se mandó solo por el otro durante el corte
  (unos 56 paquetes a 90 Mbit/s): ráfagas de 400–900 retransmisiones
  espurias en la mitad de las corridas con capacidades iguales o cambiadas.
  Es el caso del reordenador del paso 1.
- **QUIC** (aioquic, umbral de pérdida fijo de RFC 9002) muestra lo mismo:
  muchas pérdidas declaradas que eran tardanza en `linkdeath` (280, 188
  espurias) y `starlink15` (857 y 194 de bajada), y ≤ 5 espurias en
  `reorder50` y `reorder150`.
- **Qué mide de verdad el caso:** sobre todo la capacidad que queda tras un
  corte. `starlink15sw` (opcional, capacidades cambiadas) separa eso del
  efecto del cambio de camino.

### Medidas con dispersión (2026-10-11) [medido]

Fuente: `bond.yml` por `workflow_dispatch` (run 38159667834, commit
`1d94ddf`), 10 repeticiones de `equal`, `reorder150` y `starlink15` con
WireGuard del kernel y netem en un runner compartido; 10 s por corrida (31 s
en `starlink15`), 66 min en total. Mediana (p25–p75), de subida salvo que se
diga otra cosa.

| Caso | cc | Mbit/s | Retransm. | Espurias (QUIC) | RTT p50 (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| `equal` | Cubic | 43,5 (43,5–43,5) | 32 (30–33) | | 109 (108–109) |
| `equal` | BBR | 43,3 (43,3–43,4) | 91 (91–92) | | 52 (52–52) |
| `equal` | QUIC | 43,3 (43,3–43,3) | 74 (69–78) | 0 (0–0) | 105 (104–105) |
| `equal`, un enlace solo | Cubic | 43,5 (43,5–43,5) | 32 (32–34) | | 109 (108–109) |
| `reorder150` | Cubic | 43,8 (43,6–43,9) | 139 (30–383) | | 92 (79–104) |
| `reorder150` | BBR | 43,8 (43,8–43,8) | 0 (0–0) | | 42,5 (42,3–43,1) |
| `reorder150` | QUIC | 43,5 (43,5–43,5) | 13 (8–15) | 2 (1–4) | 100 (99–115) |
| `starlink15` | Cubic | 80,5 (80,5–80,6) | 1255 (1238–1262) | | 98,5 (98,4–98,6) |
| `starlink15` | BBR | 63,5 (63,4–76,1) | 1057 (1024–2196) | | 51 (51–69) |
| `starlink15`, bajada | BBR | 79,4 (63,5–80,1) | 2871 (1050–3181) | | 73,5 (51–75) |
| `starlink15` | QUIC | 66,5 (66,2–66,7) | 1122 (1098–1148) | 376 (362–388) | 76,3 (76,3–76,5) |

- **`equal` es estable:** el p25–p75 del goodput cabe en 0,1 Mbit/s, y la
  redundancia y un enlace solo dan lo mismo con los tres controles de
  congestión. La vara de ≥ 1,8× (≥ 78 Mbit/s) no tiene ambigüedad.
- **Dos casos tienen dos modos, no ruido:**
  - `reorder150` con Cubic: o unas 30 retransmisiones con un RTT p50 de
    104 ms, o 247–395 con 79 ms (5 de 10 corridas de subida y 3 de 10 de
    bajada), con el mismo goodput. QUIC también se parte en el RTT (98–101
    frente a 119–120 ms).
  - `starlink15` con BBR: o ~63,5 Mbit/s con 970–1380 retransmisiones y
    51 ms, o ~80 Mbit/s con 2470–3350 y 73–75 ms (3 de 10 de subida, 6 de 10 de
    bajada). Cubic, en cambio, repite 80,5 Mbit/s y 1208–1466 retransmisiones en las 20.
  - La línea base de una corrida (arriba) cayó en el modo bajo de
    `reorder150` (31) y en el alto de `starlink15` con BBR (80,5 Mbit/s y
    2804): ninguna de las dos es «el» valor. Por qué una corrida cae en un
    modo u otro no está medido [MEDIR: `lost_all` y la tardanza conjunta por
    corrida, que en corridas de 10 s todavía no sirven].
- **Para los criterios de salida:** comparar el paso 1 o el 3 con esto pide
  10 corridas de cada lado y contar cuántas caen en cada modo (o una prueba
  de rangos), no la mediana sola ni una corrida.
- **QUIC en `starlink15`:** un tercio de las pérdidas que declara son
  espurias (376 de 1122 de subida, 392 de 1148 de bajada), estable de corrida
  a corrida; en `equal` no hay espurias y en `reorder150`, 0–5. Su goodput
  (66 Mbit/s) queda bajo el de Cubic.

### Instrumentos pasivos del motor (paso 0, 2026-10-11)

Solo miden: ninguna decisión los lee (las copias tardías no pasan por el
MAC). Los guarda el registrador de campo y los analiza `bench/fieldrec.py`.

- **`joint`, la tardanza conjunta** (`arrival.h`): cada casilla de secuencia
  guarda, además de los enlaces esperados y los que entregaron, los que
  entregaron dentro de 50, 100 y 150 ms de la primera copia. Al reutilizarse
  la casilla (4096 secuencias después) suma uno en un histograma por la
  máscara de los tarde a cada plazo (no llegar nunca también es tarde) y
  otro por la máscara de los esperados. P(todos los de S tarde) sale de las
  sumas de los cubos que contienen a S: el número de k-de-N. Fuera
  (`other`): enlaces con id 8 o más y casillas reutilizadas antes de 150 ms
  (unos 27 kpps), decidido por el tiempo y nunca por lo que llegó, para no
  sesgar.
- **`lost_all`, la pérdida del túnel** (`replay.h`): las secuencias que salen
  de la ventana anti-replay sin marcar, contadas al limpiar cada bloque (un
  popcount cada 64), desde la primera secuencia de la ventana y nunca tras
  `init_marked`; un salto que limpia todo el anillo (el servidor que retoma
  la sesión 2^20 más adelante) empieza otra vez. Ninguna decisión de la
  ventana cambió: una prueba al azar compara cada veredicto con el código
  anterior. Va ~8200 secuencias por detrás.
- **`sizes`** (`wgsize.h`): los datagramas de WireGuard por tipo y, los de
  datos, por tamaño (12 cubos: 32 el keepalive, 96 un ACK vacío, hasta 1440 y
  más), en cada sentido.
- **Costo:** una casilla pasa de 16 a 20 bytes y cada tabla de llegadas crece
  24 KiB (el servidor tiene una por sesión: ~1,5 MiB con 64). Por paquete,
  unas decenas de instrucciones [estimación: un microbenchmark en un núcleo
  del contenedor pasó de 7–9 a 13–15 ns por paquete, ventana, primera copia,
  dos copias tardías y tamaños; el laboratorio mide 40–50 µs de CPU por
  paquete con el kernel].
- **Verificado en el laboratorio:** con los tres enlaces cortados 0,2 s en
  una bajada, `lost_all` dio exactamente los 414 paquetes perdidos; con un
  enlace a 10 Mbit/s, su cubo de tarde a 150 ms coincidió con su `rx_missed`
  (3043). 14 mutantes del código nuevo, todos cazados por los tests.

### Paso 1: el reordenador y el reparto experimental (2026-10-12)

Hecho en el PR #24 (paquetes 0.6.0). Lo que hay [código]:

- **Reordenador** (`engine/src/reorder.h`, puro; en el cliente para la
  bajada y en el servidor para la subida de cada sesión):
  - una ventana de 4096 secuencias (no 8192: tiene que quedar holgada bajo
    las 8128 del anti-replay, o un paquete liberado tarde sería OLD) y un
    pool de buffers de 2 KiB reservado una vez (`reorder_pool_kb`, 8192);
  - un hueco se suelta cuando todos los enlaces que podían traerlo
    entregaron algo posterior (RACK por enlace, enlaces FIFO) o en su
    plazo: la marca de tiempo del emisor del primer retenido + la OWD de
    recepción del enlace más lento + un margen (10 ms), con tope
    `reorder_max_wait_ms` (200) en tráfico repartido y
    `reorder_dup_wait_ms` (0) en tráfico duplicado;
  - con `reorder_dup_wait_ms` 0, la redundancia no retiene nada: el orden
    de entrega es el de siempre, y los contadores registran el desorden que
    arreglaría una espera;
  - nunca descarta: si la ventana se desborda, los huecos salen en orden;
    un salto lejano (los 2^20 de un servidor reiniciado) pasa y
    resincroniza; los handshakes y los datagramas de hasta 128 B pasan sin
    esperar.
- **Reparto** (`split.h`, puro):
  - round-robin ponderado suave entre los enlaces elegibles; los
    datagramas de hasta `split_small` (256 B) y los que no son datos de
    WireGuard van por todos; peso 0 = una copia de todo (la semántica de
    libRIST);
  - modos: el cliente `off`, `on` o `force` (laboratorio); el servidor
    `off`, `client` (por omisión: sigue al cliente) o `force`;
  - con `on`, los extremos se dicen en una cola de 4 B de las sondas si
    reordenan, los pesos y si piden reparto, sin subir `CG_PROTO_VERSION`
    (lo dejaron previsto las historias 010 y 012); vale solo la cola del
    mensaje de control más nuevo.
- **Guardas del reparto:**
  - G1 y G2: nunca por un enlace silenciado o en reserva, y con menos de 2
    elegibles, duplica. Con `split_spread_copy = no` (2026-10-13, paquetes
    0.6.3), con uno solo elegible ese lleva todo lo que se reparte, y solo
    sin ninguno se duplica (abajo, «La espiral de las guardas, en la Pi»);
  - G3: un enlace sin nada verificado del otro extremo durante 3
    intervalos de sonda deja de recibir parte (la ventana de un enlace
    muerto pasa de ~1,3 s a ~300 ms [estimación; MEDIR en `linkdeath`]);
  - G4, `split_spread_ms` (30 ms; 0 = sin límite; decidido como perilla
    por el dueño): un enlace que va más de eso detrás del más rápido en su
    retardo de envío lleva copias en vez de parte, o con
    `split_spread_copy = no` solo lo que va por todos (handshakes y paquetes
    chicos), y vuelve tras quedarse dentro de 3/4 de esa diferencia durante
    `mute_settle_ms`.
- **Costo por paquete** [medido, `bench/robench.c`, un núcleo de Xeon a
  2,1 GHz del contenedor]: 15 ns en orden; 25 ns repartido con 50 ms de
  dispersión, y 47 ns contando la copia al pool. El laboratorio mide
  40–50 µs de CPU por paquete con el kernel.

Correcciones a «Lo que el código cambia del plan» [código]: en v4 no hay
`CG_RXV_RESET` (un servidor reiniciado sigue 2^20 más adelante), la
secuencia de DATA es densa (las sondas van en una secuencia de control
aparte) y la ventana es de 4096, no de 8192 (tachado arriba).

#### El reparto, medido [medido]

Fuente: `bond.yml` por `workflow_dispatch` (run 38189468737, commit
`c0a75b4`), netem real con `BOND_NETEM_INFLIGHT=1` (netem cuenta lo que
está en su retardo contra su límite: sin otra copia, un enlace lento
desborda), 5 corridas de 10 s; mediana de Mbit/s de subida (la bajada da
lo mismo ±10 %, salvo QUIC en `equal`: 53,0 con la guarda). `split` es el
reparto forzado en los dos extremos con la guarda G4 de 30 ms, y
`split-sp0` el mismo sin la guarda.

| Caso | cc | Un enlace | `split` | `split-sp0` |
| --- | --- | ---: | ---: | ---: |
| `equal` | Cubic | 43,5 | 86,2 (1,98×) | 86,2 |
| `equal` | BBR | 43,4 | 85,8 (1,98×) | 86,2 |
| `equal` | QUIC | 43,2 | 60,9 (1,41×) | 60,7 |
| `reorder50` | Cubic | 43,8 | 43,4 (duplica) | 51,0 |
| `reorder50` | BBR | 43,6 | 43,6 (duplica) | 65,0 |
| `reorder50` | QUIC | 43,4 | 43,4 (duplica) | 5,1 |
| `reorder150` | Cubic | 43,8 | 43,8 (duplica) | 13,7 (7,5–20,6) |
| `reorder150` | BBR | 43,6 | 43,6 (duplica) | 1,2 |
| `reorder150` | QUIC | 43,4 | 43,4 (duplica) | 0,4 |

- **Con enlaces iguales, el reparto duplica la velocidad de TCP:** 1,98×
  con Cubic y con BBR, en los dos sentidos, con la misma tasa de
  retransmisiones que un enlace solo (0,09 % en Cubic; BBR, ninguna). El
  reordenador no perdió nada: sin huecos soltados por plazo ni por
  desborde, sin llegadas tarde y `too_old` 0 en las dos puntas.
- **QUIC (aioquic) gana menos:** 1,41× de subida y 1,23× de bajada, con
  5–17 veces la tasa de pérdidas declaradas de un enlace solo (0,2–0,3 %),
  casi todas espurias. Sin reordenador (`splitnoro`, run 38179417386, 3
  corridas) caía a 16 Mbit/s de subida. Por qué declara pérdidas si el
  reordenador entrega en orden no está medido [MEDIR].
- **Sin la guarda, la dispersión hunde el reparto** (`split-sp0`):
  - con 50 ms, TCP todavía gana algo sobre duplicar (Cubic 1,17×, BBR
    1,49×), pero los paquetes de 1200 B esperan 37–54 ms más en el
    reordenador, y QUIC cae a 5 Mbit/s;
  - con 150 ms se hunde todo: Cubic 0,31×, BBR 0,03×, QUIC 0,01× (por
    debajo del mínimo de 0,5 Mbit/s del laboratorio, que marcó la corrida
    como fallida aunque los pings seguían pasando);
  - los contadores del reordenador siguen limpios: no es desorden, es que
    el receptor entrega al ritmo del enlace más lento. Con el tubo casi
    vacío, los paquetes del enlace rápido no esperan y el flujo interior
    mide el RTT rápido; con carga, todo espera al lento. BBR fija su
    ventana con el RTT mínimo y entra en espiral, y HyStart saca a Cubic
    temprano de slow start [razonado].
- **La guarda G4 evita el hundimiento:** con 50 y 150 ms el enlace lento
  pasa a llevar copias, nada se reparte (`split_rx` 0) y queda lo de un
  enlace solo (1,00× en todas las celdas). (Con `split_spread_copy = no`,
  desde la 0.6.3, el lento no lleva copias: el rápido lo lleva todo, marcado
  como repartido.) Cuesta la ganancia que TCP
  habría tenido con 50 ms; recuperarla sin hundir QUIC es trabajo del
  planificador por llegada esperada (paso 3), que reparte según cuándo
  llegará cada paquete y no por turnos.
- **Con la guarda, `reorder50` es redundancia:** Cubic retransmite 1,5 %
  frente a 0,08 % de un enlace solo, con 27 ms menos de RTT p50. Es la
  redundancia de hoy con esa dispersión (`lost_all` 228 por corrida:
  pérdidas en las dos colas a la vez), no el reparto.

#### Criterios de salida del paso 1

- *Retransmisiones del TCP interior ≤ 1,2× las de un enlace solo, con
  reparto forzado y 0/50/150 ms:* se cumple con 0 ms (Cubic 0,99×, BBR
  sin retransmisiones). Con 50 y 150 ms el criterio no alcanza: el
  reordenador hace su trabajo, pero el reparto fijo hunde el goodput por
  el control de congestión interior, algo que el criterio no preveía.
  QUIC no lo cumple en ningún caso. De ahí la guarda G4.
- *0 descartes OLD:* se cumple (`too_old` 0 en todas las corridas, en
  las dos puntas).
- *Desborde con IMIX sin descartes:* se cumple en el contenedor (sin
  netem; enlace a 200 ms por un relé, 24 000 pps de 300 B: la ventana
  desbordó 9158 veces de bajada y 37 837 de subida, y cada paquete llegó
  una vez).
- *Reinicio:* los tests unitarios del salto de 2^20 y `restart.sh` con
  `split = force` y `split = on` pasan.
- *Redundancia +0 ms y ≤ +5 % de CPU:* se cumple (abajo).

#### El reordenador en redundancia [medido]

Fuente: `bond.yml` por `workflow_dispatch`, commit `c0a75b4`, 10 corridas
por celda: run 38189482615 (`equal` y `reorder150`, 10 s) y run
38189484583 (`starlink15`, 31 s), sin `BOND_NETEM_INFLIGHT`. `redund` son
los valores por omisión (reordenador encendido, `reorder_dup_wait_ms` 0),
`redund-ro0` es `reorder = no` y `redund-dw20` espera hasta 20 ms en
tráfico duplicado.

- **Con los valores por omisión, el reordenador no cambia nada medible:**
  contra `redund-ro0`, goodput 1,00× en las 18 celdas (3 casos, 3
  controles de congestión, 2 sentidos) y CPU 0,93–1,02× en el cliente y el
  servidor. El RTT p50 y el ping de 1200 B quedan dentro de ±1 ms, salvo
  en celdas con dos modos (`reorder150` con Cubic de subida: +26 ms porque
  6 de 10 corridas cayeron en el modo de 105 ms frente a 4 de 10; p =
  0,36). No retuvo ningún paquete (`held` 0). Cumple el criterio «+0 ms y
  ≤ +5 % de CPU».
- **Lo que registran los contadores sin esperar:** en `equal` y
  `reorder150`, los huecos son las pérdidas en todos los enlaces
  (coinciden con `lost_all`). En `starlink15` suelta ~800–1600 huecos por
  corrida y ve 60–480 llegadas tarde: el desorden de la vuelta tras cada
  corte.
- **Esperar 20 ms en duplicado empeora, no mejora:**
  - QUIC declara 4,2 veces más pérdidas en `equal` (espurias de 0 a ~200)
    y 19–24 veces más en `reorder150`; en `starlink15` sus espurias suben
    de 282 a 640 de subida y de 312 a 429 de bajada, cuando el criterio
    pedía que bajaran a la mitad;
  - Cubic retransmite 9–12 % más en `starlink15` (p ≤ 0,007);
  - BBR de bajada en `starlink15` cae en el modo alto en las 10 corridas
    (75,6 Mbit/s, 3136 retransmisiones, +24 ms de RTT p50);
  - en el resto, goodput y CPU iguales. Por qué empeora QUIC no está medido
    [MEDIR: la espera retiene, detrás de cada pérdida en todos los enlaces,
    los paquetes que la siguen].
  - Decisión: `reorder_dup_wait_ms` queda en 0.

#### La espiral de las guardas, en la Pi [medido en campo]

Fuente: dos tandas de 7 perfiles el 2026-10-13 de madrugada (UTC-3), en el
orden P0 a P4 y de vuelta, con un speedtest por perfil y `cengarde-rec`
cada 200 ms en el router y en el VPS (las grabaciones crudas tienen las IP
de los módems y del VPS: no van al repo). Cuatro enlaces 5G (CLARO, ENTEL,
MOVISTAR, WOM) por VLAN, pesos 1, reparto en los dos sentidos y
`reorder_max_wait_ms` de 200 a 500. Las velocidades son el pico del túnel
en ventanas de 2 s según el motor, no el resultado del speedtest; n = 2 por
perfil, y cada celda da el rango de las dos.

| perfil | guardas | subida pico | bajada pico | envíos perdidos (router) | atraso máx. de subida | subida duplicando |
| --- | --- | --- | --- | --- | --- | --- |
| P0 | copia, sin reparto | 63–68 | 126–129 | 2200–5491 | 2,4 s | — |
| P1 | spread 30, mute 150, mín. 2 | 85–101 | 99–105 | 113–36 483 | 0,4–1,9 s | 0–30 % |
| P6 | spread 30, sin mute, mín. 1 | 72–102 | 90–92 | 3957–52 882 | 2,6 s | 15–18 % |
| P2 | spread 100, mute 300, mín. 2 | 105 | 98–105 | 0–15 268 | 0,07–1,9 s | 0 % |
| P5 | spread 250, mute 300, mín. 2 | 117–124 | 93–102 | 30–16 444 | 0,15–2,6 s | 0 % |
| P3 | spread 250, sin mute, mín. 1 | 116–126 | 107–114 | 0–190 | 0,15–0,23 s | 0 % |
| P4 | sin guardas | 115–121 | 95–107 | 0 | 0,17–0,34 s | 0 % |

- **Con `split_spread_ms` 30 (P1, P6) las guardas entran en espiral:** en
  tres de cuatro corridas la subida pasó del 15 al 30 % del tiempo
  duplicando, los sockets del router rechazaron miles de envíos
  (`tx_drops`: `sendmmsg` con EAGAIN o ENOBUFS) y un enlace llegó a 2,6 s
  de atraso. Con 250 o sin guarda (P3, P4), casi ningún envío perdido y
  ningún enlace pasó de 340 ms. El mecanismo [razonado]: un enlace se
  atrasa porque su cola se llena; G4 lo saca del reparto y le da copia de
  todo, y con uno solo en el reparto G2 manda todo por todos. El flujo
  entero cae justo sobre las colas llenas.
- **Repartida y permisiva, la subida llega al doble de pico que copiando**
  (P3 y P4: 115–126 frente a 63–68 Mbit/s).
- **La bajada repartida queda por debajo de la copiada** (90–114 frente a
  126–129). Copiando, cada enlace perdió ~41 % de la bajada en la
  grabación completa, pero solo un 5–8 % se perdió en todos (`lost_all`):
  la copia suma capacidad sin quererlo, porque las pérdidas de los enlaces
  casi no coinciden, a costa de 4 veces el egreso del VPS [razonado]. Es la
  idea de k de N (FEC entre enlaces) del paso 4, y un dato para ella.
- **La copia también pierde envíos en el router** (P0: 2200–5491): no solo
  el reparto llena algo del lado de la Pi. Qué lo llena (el socket, la cola
  de la interfaz, la del trunk o el puerto del CPE) está sin medir [MEDIR:
  `tc -s qdisc`, `SndbufErrors` de `/proc/net/snmp`, la velocidad de cada
  puerto, iperf3 por enlace dentro y fuera del túnel].
- **El laboratorio no reproduce el mecanismo:** el caso `bloat` de `bond`
  (tres enlaces de 30, 20 y 8 Mbit/s detrás de colas de 1, 1 y 2 s) no
  perdió ningún envío; las colas de netem son independientes y las copias
  salen gratis. Ahí sacar las copias ayuda a BBR con spread 30 y hunde a
  QUIC (tabla abajo, «`split_spread_copy` en el laboratorio»).
- **Decisión** (con el dueño, 2026-10-13): las copias de G4 y el duplicado
  de G2 pasan a ser una perilla, `split_spread_copy`, en los dos extremos.
  Por omisión `yes`, como hasta la 0.6.2; con `no`, el enlace que queda
  fuera lleva solo lo que va por todos y el que queda solo lleva todo. La
  próxima prueba de campo compara P7 (P6 sin copias) y P8 (P3 sin copias)
  con P6 y P3.

#### `split_spread_copy` en el laboratorio [medido]

Fuente: `bond.yml` por `workflow_dispatch`, commit `0ee36ed`, run
38399231367: caso `bloat` (tres enlaces de 20–22 ms y 30, 20 y 8 Mbit/s,
detrás de colas de 1, 1 y 2 s), 3 corridas de 10 s por celda, `split =
force` en los dos extremos. Mediana en Mbit/s; «envíos por paquete» es lo
que el extremo que envía mandó por paquete (3 = copia en los tres enlaces).

| variante | sentido | Cubic | BBR | QUIC | envíos por paquete |
| --- | --- | ---: | ---: | ---: | --- |
| `redund` | subida / bajada | 26,4 / 26,4 | 26,2 / 26,4 | 25,7 / 25,7 | 2,0–2,3 |
| `split-sp30` (copias) | subida / bajada | 27,0 / 26,9 | 26,2 / 26,4 | 26,0 / 26,0 | 2,8–3,0 |
| `split-sp30-nc` | subida / bajada | 27,0 / 27,2 | 29,6 / 31,4 | 6,3 / 6,1 | 1,0–2,3 |
| `split-sp250` (copias) | subida / bajada | 31,8 / 27,8 | 25,1 / 26,4 | 26,0 / 26,0 | 2,0–3,0 |
| `split-sp250-nc` | subida / bajada | 26,7 / 26,3 | 21,4 / 21,1 | 6,5 / 6,1 | 1,0–1,2 |

- **Con copias, el reparto en `bloat` es la redundancia:** casi todo va por
  los tres enlaces (2,8–3,0 envíos por paquete) y da lo mismo que `redund`.
- **Sin copias, BBR con spread 30 gana un 13–19 %** (29,6 y 31,4 frente a
  26,2 y 26,4): manda por paquete 1,0–1,2 veces.
- **Sin copias, QUIC se hunde a 6 Mbit/s en los dos spreads** (0,23–0,25×).
  Declara perdidos el 4–7 % de los paquetes, y casi todos son espurios
  (525 de 539 de subida con spread 30): es desorden, no pérdida. Es el
  pendiente «por qué QUIC declara pérdidas espurias con el reparto». Las
  copias lo esconden porque un paquete llega también por los enlaces
  rápidos.
- **Sin copias y con spread 250, Cubic retransmite 3–4 %** y BBR baja a 21
  (0,80–0,85×).
- **Ningún envío perdido en ninguna celda:** las colas de netem son
  independientes y no hay socket que se llene. Lo que pasó en la Pi no se
  reproduce aquí.
- Con eso, `yes` sigue por omisión. El A/B de campo (P7 y P8) dice si en la
  Pi pesa más la espiral o el costo para QUIC.

#### El banco en la casa: la bajada la limitaba la NIC USB de la Pi [medido en banco]

Fuente: 2026-10-14 de madrugada, el banco montado en la mesa (la Pi, los
tres CPEs y el VPS de siempre, y un PC en el trunk del Omada con una VLAN
por enlace, historia 004). `bench/fieldperf.sh` con iperf3 a 4 flujos,
10 s por prueba, fuera del túnel: cada enlace solo y los cuatro a la vez,
desde la Pi y desde el PC por las mismas VLAN; `bench/radiorec.py` leyendo
la radio de los cuatro módems cada segundo. Una corrida de cada cosa
(n = 1): las cifras son del banco, no del campo, y la radio cambia entre
corridas.

| bajada (Mbit/s) | Pi, NIC USB | Pi, NIC integrada | PC |
| --- | ---: | ---: | ---: |
| Claro solo / con los cuatro | 36 / 27 | 92 / 104 | 100 / 93 |
| Entel solo / con los cuatro | 70 / 66 | 100 / 105 | 119 / 100 |
| Movistar solo / con los cuatro | 74 / 7 | 39 / 77 | 49 / 46 |
| WOM solo / con los cuatro | 57 / 28 | 56 / 53 | 263 / 243 |
| suma solo / con los cuatro | 237 / 128 | 287 / 338 | 532 / 483 |

- **La NIC USB gigabit de los enlaces enganchaba a 480 Mbit/s (USB 2.0)**,
  en un puerto azul de la Pi y también en uno USB 3 del PC: el adaptador
  (TP-Link `2357:0601`) o su cable no hacen enlace USB 3.0. El 5G entrega
  la bajada a ráfagas que el switch pasa a 1 Gbit/s, y el adaptador las
  vacía por USB 2.0: se desborda antes de que Linux vea los paquetes
  [razonado; `rx_dropped` y `softnet_stat` limpios, pero el VPS retransmitió
  7000–12 000 segmentos por bajada desde la Pi y 2–6 desde el PC].
- **Con los enlaces en la NIC integrada** (`eth0`, `bcmgenet`) y la LAN en el
  adaptador, la bajada con los cuatro a la vez pasó de 128 a 338 Mbit/s,
  Claro y Entel rinden como desde el PC y Movistar ya no colapsa. Es un
  arreglo transitorio: la LAN queda en USB 2.0 hasta un adaptador nuevo.
- **WOM no era la Pi:** en las bajadas del PC (263 y 243 Mbit/s) tenía la
  pata NR n78 en todos los segundos; en las de la Pi, en 3 de 13 y después
  en ninguno. Es la red sumando o no la 5G, y solo se ve con la radio al
  lado.
- **La subida es de los módems:** 113–132 Mbit/s en total desde la Pi y
  desde el PC, y el mismo bufferbloat en los dos (Entel, 300–700 ms de ping
  con su subida cargada).
- **El estado de `AT+QENG` no sirve para saber si un módem transmite:**
  dijo `NOCONN` incluso con WOM bajando 263 Mbit/s.
- **La primera prueba de campo (2026-10-13) corrió con este cuello**: sus
  bajadas repartidas y copiadas estaban limitadas por la Pi, no solo por las
  guardas. Las conclusiones sobre la espiral de la subida se mantienen (la
  subida no cambió); las de la bajada hay que repetirlas.

#### `split_spread_copy` por el túnel, en el banco [medido en banco]

Fuente: el banco de la casa, 2026-10-14, con los enlaces ya en la NIC
integrada de la Pi (arriba). Cada perfil en los dos extremos, y el túnel
cargado con `fieldperf -m tunnel` desde la Pi (iperf3 a 10.79.0.1, 4
flujos, 10 s por sentido); dos rondas en orden inverso (n = 2), con los
tres registradores encendidos. Mbit/s y ping por el túnel bajo carga (p50).

| perfil | subida | ping subida | bajada | ping bajada | subida repartida |
| --- | --- | --- | --- | --- | --- |
| P0 copia | 33,6 / 33,1 | 173 / 240 ms | 143 / 125 | 131 / 90 ms | 0 % |
| P6 spread 30, copias | 27,8 / 56,6 | 38 / 36 ms | 135 / 117 | 82 / 82 ms | 99,8 / 91,5 % |
| P7 spread 30, sin copias | 56,9 / 37,4 | 40 / 35 ms | 103 / 167 | 42 / 64 ms | 99,9 / 99,9 % |
| P3 spread 250, copias | 35,0 / 38,9 | 34 / 35 ms | 160 / 154 | 41 / 40 ms | 99,8 / 99,9 % |
| P8 spread 250, sin copias | 22,4 / 36,7 | 37 / 37 ms | 129 / 179 | 38 / 50 ms | 99,7 / 99,8 % |

- **Las copias contra su ausencia no se distinguen con n = 2:** las dos
  rondas de un mismo perfil difieren hasta en un 60 % (P6, P7 y P8 en
  subida), más que cualquier diferencia entre perfiles.
- **Ningún envío perdido en la Pi** (22 en una sola corrida), contra los
  miles de la prueba de campo del 2026-10-13. La guarda de 30 ms sacó a un
  enlace en casi todas las corridas, y la espiral apareció una sola vez
  (P6-2 terminó duplicando, con el 91,5 % repartido). Hipótesis [MEDIR en
  campo]: los envíos perdidos del campo venían en parte de la NIC USB a
  USB 2.0, que también llevaba la subida (cuatro copias de cada paquete);
  con los enlaces en la NIC integrada ya no aparecen.
- **Repartir baja mucho la latencia bajo carga de subida:** 34–40 ms de
  ping por el túnel contra 173–240 ms copiando, con una subida parecida.
  Copiando, el túnel va al paso de la cola más llena de las cuatro.
- **Pendiente:** repetir con n ≥ 5 y en el campo, ahora sin el cuello de
  la NIC, antes de cambiar el valor por omisión de `split_spread_copy`.
- **Con más fierro, lo mismo** [medido en banco, n = 2]: el PC del banco
  (i9, NIC PCIe) como segundo router del mismo VPS, con las mismas cuatro
  VLAN y el mismo motor, dio por el túnel 44/41 de subida y 105/118 de
  bajada copiando, y 38–56 de subida y 123–152 de bajada repartiendo (P3 y
  P8): lo mismo que la Pi. El techo del túnel no es la CPU ni la NIC de la
  Pi (ya en la integrada), sino el reparto: copiando rinde lo del mejor
  enlace, y repartiendo por turnos con pesos iguales un ~30 % de los
  480 Mbit/s de bajada y un ~40 % de los ~120 de subida que los cuatro
  enlaces dan juntos fuera del túnel. Es el trabajo de los pasos 2 y 3
  (pesos por capacidad, planificador por llegada esperada).

#### El A/B desde el PC, con n = 5–7 y la radio cruzada [medido en banco]

Fuente: el PC del banco como router (CPU de sobra), los perfiles P0, P6,
P7, P3 y P8 en los dos extremos, el túnel cargado con `fieldperf -m tunnel`
en 5 rondas de orden alternado (más 2 rondas previas de P0, P3 y P8:
n = 7, 5, 5, 7 y 7), con el estado del motor cada 200 ms y `radiorec`
cruzados por la hora. Informe con gráficos: `~/campo/2026-10-14/
informe-ab-pc.html` en el PC del banco (sin IP; el script, al lado).

| perfil | subida Mbit/s | ping subida | bajada Mbit/s | ping bajada |
| --- | --- | --- | --- | --- |
| P0 copia | 35 (30–44) | 154 ms | 125 (105–179) | 83 ms |
| P6 spread 30, copias | 50 (43–53) | 34 ms | 107 (89–149) | 81 ms |
| P7 spread 30, sin copias | 43 (37–56) | 34 ms | 134 (104–137) | 55 ms |
| P3 spread 250, copias | 47 (26–56) | 35 ms | 128 (119–183) | 38 ms |
| P8 spread 250, sin copias | 34 (19–56) | 34 ms | 143 (118–164) | 39 ms |

Medianas, (mín–máx) y ping p50 por el túnel bajo carga.

- **Repartir baja la latencia bajo carga**, con significancia: con la
  subida cargada, 154 ms copiando contra 34–35 con cualquier reparto
  (Mann-Whitney p ≈ 0,001); con la bajada cargada, 83 contra 38 con spread
  250 (p ≈ 0,002). Spread 30 no mejora la bajada: con los enlaces en la NIC
  integrada, la guarda de 30 ms saca enlaces de más.
- **Con o sin copias no cambia nada medible** (ningún p < 0,05; lo más
  cercano, la subida de P3 frente a P8, 47 contra 34, p = 0,07): el valor
  por omisión se queda.
- **Ningún envío perdido en 31 corridas.**
- **El reparto ignora la capacidad:** con pesos iguales cada enlace lleva
  el 23–27 % de la subida; Claro y Entel acumulan cola (atraso p95 de 119
  y 154 ms) y Movistar y WOM no (55 ms). En la bajada, el RTT p95 por
  enlace llega a 1,6 s en WOM y Movistar.
- **La radio adelanta la cola de bajada:** SINR y CQI bajos van con RTT
  altos en ese enlace (ρ de Spearman −0,47 y −0,39, p < 0,001, n = 98).
  En la subida, Claro y WOM llegan a 21–22 dBm de potencia, casi el máximo
  de 23 dBm: subida limitada por potencia; la correlación de la potencia
  con el atraso es débil (ρ = +0,22, p = 0,05). El RM502Q de Movistar no da
  CQI ni potencia. La pata NR de WOM no apareció en ninguna prueba.
- **Para el gobernador (pasos 2–3):** pesos por la capacidad medida de cada
  enlace y sentido; SINR y CQI como señal adelantada de la bajada; la
  potencia en el techo como aviso de subida saturada.

#### El gobernador base, en el laboratorio [medido en laboratorio]

`govern.h` (paso 2 del plan, experimental, `governor = yes` en cada
extremo): cada extremo pesa los enlaces de lo que envía por su cola, el
retraso de envío de cada enlace menos su mínimo de los últimos 10 s. La
primera regla (bajar el peso de un enlace con más de 20 ms de cola, subirlo
con menos de 10) falló en la traza de un BBR de subida: cuando el flujo
llena todo, todos los enlaces pasan el objetivo y bajan juntos, y como los
pesos solo cuentan como proporción, el reparto no cambia (los tres llegaron
al piso a los 10 s). La regla que quedó compara cada cola con la más corta
de los enlaces del reparto: baja 1/8 por tick el peso de un enlace con más
de 20 ms de cola por encima de esa, sube el que queda a menos de 10. Iguala
los retrasos, y saturados los pesos quedan como las capacidades (en la
traza: 30, 20 y 8 Mbit/s → pesos altos, medios y bajos).

Fuente: `bench/lab.sh bond` en el PC del banco (WireGuard y netem reales),
2026-10-14, n = 3 corridas de 10 s, medianas en Mbit/s subida / bajada:

| caso | variante | Cubic | BBR | QUIC |
| --- | --- | ---: | ---: | ---: |
| `bloat` | `redund` | 26,4 / 26,5 | 26,3 / 26,5 | 25,7 / 25,7 |
| | `split-sp250` | 28,0 / 27,1 | 26,4 / 26,4 | 26,0 / 26,0 |
| | `split-sp250-gov` | 34,4 / 31,1 | 26,3 / 26,5 | 26,0 / 26,0 |
| `het` | `redund` | 52,7 / 52,8 | 52,7 / 52,9 | 52,1 / 52,1 |
| | `split-sp250` | 21,4 / 21,3 | 25,1 / 25,3 | 1,6 / 2,0 |
| | `split-sp250-gov` | 38,0 / 37,2 | 28,7 / 28,7 | 1,9 / 2,2 |
| `reorder50` | `redund` | 44,0 / 44,2 | 43,9 / 44,1 | 43,4 / 43,5 |
| | `split-sp250` | 57,3 / 48,3 | 68,3 / 67,6 | 3,9 / 3,7 |
| | `split-sp250-gov` | 57,2 / 48,8 | 69,0 / 69,1 | 3,6 / 3,8 |

- Ayuda donde los enlaces son distintos: Cubic +23 % en `bloat` (con
  ~1 % de retransmisiones) y de 21 a 38 en `het`; BBR en `het` de 25 a
  29 de mediana, con corridas de 42–45: en 10 s apenas converge (unos
  5 s).
- No empeora donde son iguales (`reorder50`).
- Todavía no le gana a la redundancia en `het` (38 contra 53).
- QUIC se hunde con cualquier reparto, con o sin gobernador (2–4 Mbit/s,
  20–30 % de pérdidas espurias): no son los pesos, es el desorden; es el
  pendiente más grande del reparto.

#### El gobernador en el banco: el reparto no lo dejan ver los enlaces que entran y salen [medido en banco]

Fuente: el PC del banco como router, 2026-10-14 de mañana, P3 (spread 250)
con y sin `governor` en el cliente (solo la subida; el VPS sin gobernador),
4 rondas alternadas de 20 s por sentido, con el estado del motor cada
200 ms.

- Subida sin gobernador: 40,5 / 41,0 / 38,8 / 41,7 Mbit/s; con
  gobernador: 59,1 / 36,1 / 24,9 / 38,8. El ping por el túnel, ~40 ms en
  las dos. La bajada (control, sin gobernador en las dos): 90–193.
- Lo que explica la dispersión no son los pesos: **los dos módems del CPE
  doble (Entel y WOM) salen del reparto como no elegibles (guarda G3:
  ninguna respuesta verificada del VPS por ese enlace en tres intervalos
  de sonda)**. WOM pasó toda la corrida B-4 fuera (con peso 1000 del
  gobernador), y los dos, cerca de la mitad de A-2 y A-3. Movistar y
  Claro, cada uno en su CPE, casi nunca.
- **El CPE doble se reinicia** (medido el mismo día): arrancó a las
  10:40:57, en medio de B-4, y durante las pruebas el registrador de radio
  lo perdió por minutos (09:25–09:43 y 10:35–10:41). Con una carga solo
  por ese CPE (iperf3 por las VLAN 10 y 20, fuera del túnel), la bajada
  (45 + 233 Mbit/s, 20 s) pasa con su ping de gestión en 0,3–1 ms y sin
  cortes. A los 13 s de la subida por los dos módems a la vez (~20 + ~40
  Mbit/s), el CPE se apaga de golpe: el ping seguía bajo 1 ms en la
  respuesta anterior y volvió a arrancar. No es la CPU (la bajada, con
  cinco veces más paquetes, no la mueve) ni la temperatura (55 °C, corte
  a 115–125 °C). Lo que cambia con la subida es la potencia de
  transmisión de los dos módems, que en ese CPE llegan a 21–22 dBm
  Los módems informan 3,80–3,82 V en reposo (`AT+CBC`). **Era la
  fuente** (confirmado el mismo día): la del CPE daba 1,5 A medidos a
  12 V; con una de 2,5 A, la misma prueba pasa entera (bajada 96 + 201,
  subida por los dos módems los 20 s) sin reinicio y con el ping de
  gestión sin una sola pérdida en 75 s. Un CPE con dos módems 5G
  necesita una fuente de al menos 2,5 A a 12 V.
- La única corrida con los cuatro enlaces dentro todo el tiempo (B-1) dio
  59 Mbit/s, con pesos que siguen las colas (Entel, la más cargada, 327;
  WOM 892).
- Con el CPE doble reiniciándose, el gobernador no se pudo juzgar en el
  banco (ni ninguna prueba de subida anterior con los cuatro enlaces); en
  el laboratorio sí ayuda (arriba).
- **Repetido con la fuente nueva** (el mismo día, 11:00–11:17, el CPE
  doble sin reinicios): subida sin gobernador 35,5 / 32,8 / 40,9 / 28,7
  Mbit/s (media 34,5); con gobernador 33,8 / 35,0 / 31,3 / 18,7 (media
  29,7). La bajada (control) 145–207. **El gobernador no mejora la
  subida en el banco** (n = 4, sin diferencia significativa). En dos
  corridas con gobernador un enlace acumuló colas de 0,6–1 s (Entel en
  B-1, con peso 11 y fuera del reparto, pero con el 42 % de los bytes
  enviados: las copias de `split_spread_copy` a los enlaces atrasados
  siguen llenando su cola), y en B-4 los pesos quedaron casi iguales y la
  subida cayó a 18,7. Sin gobernador, los cuatro enlaces quedan dentro
  del reparto casi todo el tiempo [hipótesis: con el gobernador, las
  copias a los enlaces fuera del reparto anulan la reducción de su peso;
  PROBAR con `split_spread_copy = off`].
- **Con `split_spread_copy = off`** en A y en B (11:30–11:47): sin
  gobernador 33,3 / 20,5 / 36,0 / 39,6 Mbit/s (media 32,4); con
  gobernador 38,6 / 40,7 / 32,3 / 38,9 (media 37,6; Mann-Whitney
  unilateral p = 0,24). Sin las copias, el enlace al que el gobernador
  baja el peso sí se descarga (Entel, 7–20 % de los bytes con colas de
  30–100 ms). Pero Claro se cayó desde las 11:37 (su watchdog subió del
  `ifup` al reinicio del módem, y el módem volvió sin sesión de datos):
  cinco de las ocho corridas fueron con tres enlaces.
- **Con los cuatro enlaces y los watchdogs de los CPE en pausa**
  (12:00–12:16; los watchdogs confunden la cola de nuestra carga con un
  enlace sin tráfico y lo bajan con `ifup`, que fue lo que dejó caído a
  Claro): sin gobernador 27,9 / 52,0 / 43,9 / 43,8 Mbit/s (media 41,9);
  con gobernador 56,6 / 35,7 / 56,3 / 26,7 (media 43,8; p = 0,89). Los
  cuatro enlaces quedan dentro del reparto el 95–100 % del tiempo en las
  ocho corridas. **El caudal no cambia; el atraso entre enlaces, sí:** el
  peor atraso de reparto entre los cuatro (`split_behind_ms`, cada
  200 ms) tiene mediana 16 / 47 / 54 / 90 ms sin gobernador y
  49 / 15 / 19 / 22 con él, y p90 58–182 frente a 36–132. Es lo que el
  gobernador busca (igualar las colas), pero el ping por el túnel no lo
  refleja (37–48 ms en las dos) y el caudal de subida varía más entre
  corridas (27–57) que entre A y B. Conclusión provisional: el gobernador,
  con `split_spread_copy = off`, no estorba y empareja los enlaces; para
  ver si sube el caudal hacen falta corridas más largas y n mayor.
- **Tanda larga** (12:30–13:00, n = 6 por brazo, 60 s de subida, copias
  apagadas en los dos, watchdogs en pausa): sin gobernador 51,7 / 52,9 /
  54,2 / 53,0 / 50,5 / 30,0 Mbit/s (mediana 52,3); con gobernador 49,5 /
  51,8 / 52,6 / 50,4 / 47,8 / 48,5 (mediana 50,0; p = 0,24). El peor
  atraso de reparto entre los cuatro enlaces baja con el gobernador
  (mediana por corrida 95 → 66 ms, p = 0,12 unilateral; p90 214 → 193,
  p = 0,09), el ping por el túnel no cambia (39–47 ms). Con 60 s las dos
  ramas dan ~50 en vez de ~40: TCP tarda en llenar el túnel.
  **Conclusión: el gobernador por cola empareja algo los enlaces pero no
  sube el caudal; repartir según la cola no es lo que limita.** El paso
  siguiente es estimar la capacidad de cada enlace (lo que el otro
  extremo dice haber recibido por él) y dosificar el envío por enlace
  bajo esa capacidad, para que la cola quede en el router y no en el
  módem. Con el gobernador, `split_spread_copy` pasa a `no` por omisión
  (0.6.4): con copias, el gobernador se anula (arriba).

#### El gobernador en la bajada y `cake` sobre el túnel, en el banco [medido en banco]

Fuente: el PC del banco como router, 2026-10-14 de 12:40 a 13:00; el VPS
con la build de la rama (0.6.4).

- **Gobernador de bajada** (P3 en los dos extremos, copias apagadas, el
  gobernador solo en el VPS; 5 rondas alternadas de 30 s de bajada,
  watchdogs de los CPE en pausa): sin gobernador 162,9 / 129,5 / 66,9 /
  74,1 / 58,2 Mbit/s; con gobernador 152,6 / 158,0 / 134,0 / 154,0 /
  100,6. A-1 no vale: el VPS aún corría la configuración anterior (copias
  y spread 30; su ping fue 166 ms frente a 37–43 en las demás). En las
  otras cuatro rondas, la corrida con gobernador gana a la de su ronda
  las cuatro veces (+22 % a +108 %; signos 4/4, p = 0,06), aunque Claro
  se cayó en B-5. Las bajadas de los enlaces son muy desiguales (WOM
  188–233 Mbit/s solo, Movistar ~20): con pesos iguales WOM lleva el
  32–40 % de los bytes, y con gobernador el 39–54 % (81 % en B-5, sin
  Claro), mientras Entel baja del 28 % al 12–19 %. En la subida, con
  enlaces de 20–30 Mbit/s cada uno, no había nada que redistribuir; en la
  bajada sí. Movistar queda fuera del reparto por atraso (`spread`) la
  mayor parte del tiempo en las dos ramas (cola de 0,2–0,7 s).
- **Claro y el USB del CPE:** a las 12:52, con la bajada cargada, el
  kernel de `cpe3` (MT7621) registró `xhci-mtk: ERROR unknown event type
  37` y luego `qmi_wwan wwan0: NETDEV WATCHDOG: transmit queue 0 timed
  out`: el módem (RM520N-GL, fw A03) dejó de transmitir por USB con la
  interfaz arriba. No fue el watchdog del CPE (estaba en pausa)
  [hipótesis: el controlador xHCI del MT7621 con el módem a 5 Gbit/s;
  PROBAR con el firmware nuevo del módem o con el puerto a USB 2.0].
- **`cake` sobre el túnel** (la interfaz WireGuard del PC: 40 Mbit/s de
  subida, 130 de bajada con IFB; P3 sin gobernador; 2 corridas con y 2
  sin, 30 s por sentido; con tres enlaces, Claro caído): el ping por el
  túnel durante la subida, p90 169 y 92 ms sin `cake`, 53 y 46 con él;
  p99 281 y 392 sin `cake`, 61 y 52 con él; el caudal igual (33–37
  Mbit/s). En la bajada el ping ya era bueno sin `cake` (p90 48–50) y
  no cambia. Sin `cake` todo el túnel es un solo flujo UDP hacia el VPS
  y el ping espera detrás de la carga en la cola del enlace; `cake` en
  la interfaz del túnel ve los flujos de adentro y les da turno. Muestra
  chica (32 pings por corrida, uno por segundo).

#### El gobernador de bajada y `cake`, en la Pi [medido en la Pi, red de pruebas]

Fuente: la Pi con 0.6.4 (paquete del CI de `master`), 2026-10-14 de 13:10 a
13:28; P3 en la Pi (`uci`), el VPS con y sin gobernador; 4 rondas de 30 s
de bajada y luego 4 corridas con y sin `cake` en `wgcg` con el gobernador
del VPS encendido; watchdogs de los CPE en pausa.

- **La primera ronda no vale, aquí ni en el PC:** el `cengarde-nat check`
  del VPS (cada 5 min) vio que `fieldperf.sh vps on` había cambiado los
  puertos reservados y regeneró la configuración del motor
  (`cengarde-vps-setup refresh`), lo que borró el perfil del A/B a mitad
  de la corrida (13:13:04). Desde ahora `vps on` y `vps off` corren ese
  check en el momento. (La Pi pidió la bajada repartida en el segundo
  siguiente a su recarga: una primera lectura que decía 2,5 min miraba
  su sesión anterior, que el VPS lista hasta que caduca. B-1 vale: 133,7
  Mbit/s con gobernador.) Desde las 13:35 la red de pruebas queda así: la
  Pi con P3 por UCI y el VPS con P3 y el gobernador en
  `/etc/cengarde/engine.local` (nuevo: `cengarde-vps-setup` lo copia en la
  configuración que genera).
- **Gobernador de bajada, rondas 2–4:** sin gobernador 94,4 / 122,9 /
  113,2 Mbit/s; con gobernador 137,4 / 157,0 / 155,6: gana las tres
  (+24 % a +46 %), como en el PC. El ping por el túnel durante la bajada
  sube un poco con él (p90 67–69 ms frente a 48–64).
- **`cake` en `wgcg`** (40 Mbit/s de subida, 130 de bajada con IFB): en la
  subida el ping ya era bueno sin `cake` en la Pi (p90 42–51 ms; en el PC
  92–169) y con `cake` queda en 40–42; el caudal igual (34–38). En la
  bajada, `cake` baja el caudal de 139–161 a 87–88 Mbit/s (el tope era
  130) y el ping solo de p90 54–69 a 46–47. En la Pi, `cake` de bajada
  con IFB cuesta demasiado [hipótesis: la CPU que atiende IFB, `cake`,
  WireGuard y el motor; PROBAR la bajada en la salida de `br-lan`, sin
  IFB, o solo `cake` de subida].
- **`cake` de bajada en la salida de `br-lan`** (sin IFB; la subida en la
  salida de `wgcg`), medido desde el PC como equipo de la LAN detrás de
  la Pi (13:45, 2 corridas con y 2 sin, 30 s por sentido, la red de
  pruebas ya con P3 y el gobernador del VPS): bajada 127 y 173 Mbit/s sin
  `cake`, 97 y 84 con él (tope 130); ping p90 70–80 ms sin `cake`, 52–55
  con él; p99 101–111 frente a 68–71. Cuesta menos que con IFB pero
  todavía ~35 % del caudal: con enlaces que entregan entre 85 y 230
  Mbit/s según la hora, un tope fijo o deja caudal o no controla la
  cola. Es el argumento para que el tope lo fije el propio cengarde con
  su estimación de capacidad (el paso siguiente del plan), en vez de un
  número a mano. La subida desde la LAN dio solo 14 Mbit/s en las cuatro
  corridas (`cake` de 40 no llegó a actuar), frente a 24 desde la Pi
  misma en ese momento, con retransmisiones en los dos [PENDIENTE: por
  qué la subida reenviada rinde menos que la de la Pi].

## Qué hacemos con esto

### Orden de la Fase 5 y criterios de salida

Los umbrales son propuestas [razonado] y se miden en `bench/`.

| Paso | Qué | Criterio de salida medible |
| --- | --- | --- |
| 0 | Laboratorio: WireGuard del kernel + iperf3 (Cubic/BBR) + un cliente QUIC, sobre netem por enlace; mismo build, n ≥ 10 con dispersión. Instrumentos pasivos: tardanza contra plazo absoluto, pérdida en todos los enlaces, histograma de tamaños y tipos | Corren en el CI `equal`, `het`, `reorder50`/`reorder150`, `linkdeath`, `starlink15` [supuesto], `ltespike` y `deepq` (hecho, con QUIC; n = 10 por `workflow_dispatch`). Instrumentos en el motor (hecho, 0.5.5). Hay una semana de datos de la Pi con 4×5G + Starlink, con las métricas de RFC 8869 (pérdida, retardo, estabilidad de la tasa) |
| 1 | Reordenador por la secuencia de cengarde (~~8192~~ 4096 ranuras, liberación por enlace, nunca descarta) y formato v4 de DATA (marca de reparto, bits ECN, secuencia de control aparte) | Con reparto forzado y 0/50/150 ms de spread, las retransmisiones del TCP interior quedan ≤ 1,2× las de un solo enlace. 0 descartes OLD. Desborde con IMIX sin descartes. Reinicio correcto ~~tras CG_RXV_RESET~~ tras el salto de 2^20 de un servidor reiniciado (restart.sh). En redundancia pura, +0 ms y ≤ +5 % de CPU |
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
- ~~**Formato v4.** Incluye la marca de reparto, los bits de ECN, la secuencia de control aparte (toca epoch.h), el informe por enlace y, si se quiere una prueba activa de MTU, una sonda rellenada. Puede ir con el v4 del multicliente o en la versión siguiente.~~ Hecho en el v4 del multicliente (PR 3d2a, 2026-10-10, historia 010): la marca de reparto (0x04) y los bits de ECN (0x03) en los flags de DATA, que hoy se envían en 0, y la secuencia de control aparte, con su ventana. La carga de las sondas puede crecer, y el receptor ignora lo que no conoce: ahí van el informe por enlace y la sonda rellenada, sin otra versión.
- **Corregir los textos.** ROADMAP.md:582-584 («reordenar usando el contador de WireGuard») y el contexto del estudio («dedup exacta por índice receptor + contador + tag») tienen que pasar a la secuencia de cengarde.

## Pendiente

- Paso 0, lo que falta:
  - ~~un cliente QUIC dentro del túnel~~ (hecho: `bench/quicperf.py`, aioquic 1.2.0);
  - ~~n ≥ 10 con dispersión~~ (hecho: la tabla da mediana y p25–p75; `workflow_dispatch` con `repetitions`, y la corrida de 10 queda en la sección «Medidas con dispersión»);
  - ~~los instrumentos pasivos: tardanza contra plazo, histograma de tamaños~~ (hecho, abajo);
  - ~~comprobar por qué `starlink15` retransmite tanto~~ (hecho: pérdida en los cortes, arriba);
  - una semana de la Pi con `cengarde-rec` (guía de OpenWrt) y su informe de `bench/fieldrec.py`, que ahora también da la tardanza conjunta y, con `--rf`, cruza los enlaces con las métricas de radio que `cell-monitor` (SmoothStream) deja en InfluxDB: si los cortes y silenciados coinciden con cambios de RAT, banda o celda más que al azar, y si el SINR cae antes;
  - `lost_all` por corrida en `bond.sh`: el contador va ~8200 secuencias detrás de la ventana, así que en corridas cortas le pasa a la siguiente las pérdidas del final; sirve en los intervalos largos del registrador.
- Medir lo marcado [MEDIR] y [supuesto] antes de fijar umbrales: el patrón de 15 s de Starlink en Chile, la correlación entre operadores en Santiago, si el TOS del datagrama de WireGuard llega a cengarde por loopback, y la tolerancia al desorden de iOS/macOS.
- Definir los campos del informe por enlace, al final de la carga de las sondas v4 (el formato ya les deja lugar).
- Paso 1, lo que falta:
  - ~~el A/B de `reorder_dup_wait_ms`~~ (hecho: queda en 0); el de `reorder_bypass` (0/128/256: `lab.d/reorder.sh`, `reorder_hol`), que no corrió en el CI;
  - G3 medido: cuánto pierde el reparto cuando muere un enlace (`linkdeath` con `split`);
  - por qué QUIC declara pérdidas espurias con el reparto entre enlaces iguales;
  - `cengarde-vps-setup` no expone `split_spread_ms` (el VPS usa 30 ms);
  - probar `split = on` en la Pi con pesos por enlace, cuando el dueño lo decida.
  - la espiral de las guardas con congestión: ~~falta la corrida con repeticiones y los perfiles que separan G4 del silenciado~~ (hecha: «La espiral de las guardas, en la Pi»). Falta el A/B de `split_spread_copy` en campo (P7 y P8 frente a P6 y P3, con el motor 0.6.3 en los dos extremos), saber qué rechaza los envíos en la Pi (para eso `bench/fieldperf.sh`: iperf3 por enlace fuera del túnel, todos a la vez y por el túnel, con los contadores de UDP, de cada interfaz y del motor antes y después), y después el gobernador (pasos 2–3). La primera prueba (n = 1, la bajada sin repartir porque el reordenador del router estaba apagado) dio la misma forma: 37,5 Mbit/s de subida copiando, 52,0 y 37,1 con las guardas estrictas y medias, y 81,2 con `split_spread_ms` 250.

## Cambios

- 2026-10-10: creada con el estudio de bonding.
- 2026-10-10: el formato v4 (PR 3d2a) trae lo reservado para el bonding.
- 2026-10-10: paso 0 en parte. `bench/lab.sh bond` y `bond.yml`, con la línea base de arriba. El registrador de campo: `cengarde-rec` en el paquete de OpenWrt, `bench/fieldrec.py` con su prueba y `lab.d/fieldrec.sh`, y `time_ms` en el estado del motor.
- 2026-10-11: paso 0 hecho salvo los datos de campo (PR 6, paquetes 0.5.5): QUIC y dispersión en `bond`, los instrumentos pasivos del motor y su análisis en `fieldrec.py`. Corregida la explicación de `starlink15` (era [razonado]): son pérdidas en los cortes, no desorden.
- 2026-10-11: la corrida de 10 repeticiones («Medidas con dispersión»: dos modos en `reorder150` con Cubic y en `starlink15` con BBR) y `fieldrec.py --rf`, con las métricas de radio de `cell-monitor`.
- 2026-10-12: paso 1 (PR #24, paquetes 0.6.0): el reordenador, el reparto experimental, la guarda `split_spread_ms` y sus medidas; el A/B del reordenador en redundancia (sin costo; `reorder_dup_wait_ms` queda en 0). Corregidas las referencias de código que el v4 dejó viejas (`CG_RXV_RESET`, la secuencia densa) y la ventana (4096, no 8192).
- 2026-10-13: primera prueba de perfiles en la Pi (en Pendiente: la espiral de las guardas).
- 2026-10-13: segunda prueba en la Pi, con dos tandas y la bajada repartida («La espiral de las guardas, en la Pi»), y el caso `bloat` en `bond`. `split_spread_copy` (paquetes 0.6.3): las copias de G4 y el duplicado de G2 con un solo enlace pasan a ser una perilla, por omisión como hasta la 0.6.2; `bond` suma el sufijo `-nc` y la tabla, los envíos por paquete y los envíos perdidos. `bench/fieldperf.sh` y `fieldperf.py`: iperf3 en campo, por enlace y por el túnel, con los contadores que dicen dónde se pierde un envío. `cengarde-rec` espera con el `sleep()` de `ucode` donde el `sleep` de busybox no acepta fracciones (el de OpenWrt): antes grababa una vez por segundo aunque el motor escribiera cada 200 ms.
- 2026-10-14: `bench/radiorec.py`: la radio de cada módem cada segundo desde el PC del banco (celda servidora, portadoras agregadas, la pata NR), en el formato de `cell-monitor`, para cruzarla con `fieldrec.py --rf`. Los cuatro módems del banco anclan en LTE y WOM suma NR n78 en NSA, así que la falla conjunta de «los NSA caen juntos y el SA no» no se puede probar aquí; sí la de dos operadoras en la misma banda de anclaje (Entel y Movistar, B7). Historia 004.
- 2026-10-14: el banco en la casa («El banco en la casa»): la bajada de la Pi la limitaba su NIC USB, enganchada a USB 2.0; con los enlaces en la NIC integrada la bajada con los cuatro a la vez pasó de 128 a 338 Mbit/s. WOM rindió 263 Mbit/s solo con la pata NR encima.
- 2026-10-14: el A/B de `split_spread_copy` por el túnel en el banco («`split_spread_copy` por el túnel, en el banco»): sin diferencia visible con n = 2, ningún envío perdido con los enlaces en la NIC integrada, y repartir baja el ping bajo carga de subida de ~200 a ~37 ms.
- 2026-10-14: el PC del banco como segundo router (`pc`): por el túnel rinde lo mismo que la Pi, así que el techo es el reparto, no el fierro de la Pi.
- 2026-10-14: el A/B desde el PC con n = 5–7 y la radio cruzada («El A/B desde el PC»): repartir baja el ping bajo carga de 154 a 34 ms en subida (p ≈ 0,001); con o sin copias no cambia nada medible; la radio (SINR, CQI) adelanta la cola de bajada.
- 2026-10-14: el gobernador base (`govern.h`, `governor`), experimental y apagado por omisión, con sus medidas de laboratorio («El gobernador base, en el laboratorio»).
