# 012 — Bonding para cengarde: qué dice la evidencia y el plan de la Fase 5

- **Fecha:** 2026-10-10 (actualizada el 2026-10-12 con el paso 1)
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
- **Paso 1, medido** [2026-10-12]: con el reordenador, el reparto de dos enlaces iguales da 1,98× uno solo con Cubic y BBR (QUIC 1,2–1,4×). Pero un reparto por turnos con 50–150 ms de dispersión se hunde (BBR a 1 Mbit/s, QUIC a 0,4) con el reordenador limpio: el receptor entrega al ritmo del enlace lento. La guarda `split_spread_ms` (30 ms) deja al lento con copias; repartir bien entre retardos distintos es el paso 3.
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
    elegibles, duplica;
  - G3: un enlace sin nada verificado del otro extremo durante 3
    intervalos de sonda deja de recibir parte (la ventana de un enlace
    muerto pasa de ~1,3 s a ~300 ms [estimación; MEDIR en `linkdeath`]);
  - G4, `split_spread_ms` (30 ms; 0 = sin límite; decidido como perilla
    por el dueño): un enlace que va más de eso detrás del más rápido en su
    retardo de envío lleva copias en vez de parte, y vuelve tras quedarse
    dentro de 3/4 de esa diferencia durante `mute_settle_ms`.
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
  enlace solo (1,00× en todas las celdas). Cuesta la ganancia que TCP
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
- *Redundancia +0 ms y ≤ +5 % de CPU:* sin retener nada (`held_total` 0 en redundancia, en el contenedor) [medido]; el A/B con 10 repeticiones en `equal`, `reorder150` y `starlink15` (`redund` frente a `redund-ro0` y `redund-dw20`) está corriendo en el CI.

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
  - el A/B de `reorder_dup_wait_ms` (0 frente a 20 ms) en `starlink15` y con `link_threads` legacy y off, y el de `reorder_bypass` (0/128/256: `lab.d/reorder.sh`, `reorder_hol`), que no corrió en el CI;
  - G3 medido: cuánto pierde el reparto cuando muere un enlace (`linkdeath` con `split`);
  - por qué QUIC declara pérdidas espurias con el reparto entre enlaces iguales;
  - `cengarde-vps-setup` no expone `split_spread_ms` (el VPS usa 30 ms);
  - probar `split = on` en la Pi con pesos por enlace, cuando el dueño lo decida.

## Cambios

- 2026-10-10: creada con el estudio de bonding.
- 2026-10-10: el formato v4 (PR 3d2a) trae lo reservado para el bonding.
- 2026-10-10: paso 0 en parte. `bench/lab.sh bond` y `bond.yml`, con la línea base de arriba. El registrador de campo: `cengarde-rec` en el paquete de OpenWrt, `bench/fieldrec.py` con su prueba y `lab.d/fieldrec.sh`, y `time_ms` en el estado del motor.
- 2026-10-11: paso 0 hecho salvo los datos de campo (PR 6, paquetes 0.5.5): QUIC y dispersión en `bond`, los instrumentos pasivos del motor y su análisis en `fieldrec.py`. Corregida la explicación de `starlink15` (era [razonado]): son pérdidas en los cortes, no desorden.
- 2026-10-11: la corrida de 10 repeticiones («Medidas con dispersión»: dos modos en `reorder150` con Cubic y en `starlink15` con BBR) y `fieldrec.py --rf`, con las métricas de radio de `cell-monitor`.
- 2026-10-12: paso 1 (PR #24, paquetes 0.6.0): el reordenador, el reparto experimental, la guarda `split_spread_ms` y sus medidas. Corregidas las referencias de código que el v4 dejó viejas (`CG_RXV_RESET`, la secuencia densa) y la ventana (4096, no 8192).
