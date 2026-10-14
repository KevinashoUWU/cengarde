# 004 — Entorno real de despliegue (datos de campo)

- **Fecha:** 2026-10-03 (actualizada el mismo día: OpenWrt limpio; y el
  2026-10-04: Starlink e IPv6)
- **Estado:** vigente
- **Fuentes:** datos aportados por el usuario (2026-10-03, 2026-10-04 y
  2026-10-11, sobre una transmisión del 2026-10-10).

## TL;DR

- **Cliente:** Raspberry Pi 4B con overclock, con SmoothWAN (distro OpenWrt
  que trae engarde Go). Cuatro uplinks 5G (WOM, Entel, Claro y Movistar),
  cada uno en una VLAN, todas en trunk sobre **un único adaptador USB
  Ethernet gigabit**; la LAN sale por la NIC integrada.
- **Medido con engarde Go:** la Pi se queda en **20–30 Mbit/s con 4
  enlaces** y el cuello es la CPU (CPU starvation). Un x86 Celeron J4005 con la
  misma configuración llega a **~90 Mbit/s**. Esto confirma la estimación de
  la historia 001 (35–50 Mbit/s con 3 enlaces en una Pi 4, menos con 4).
- **Servidor:** VPS Vultr en Santiago, el plan "CPU Optimized" más barato
  (vCPU dedicadas; número exacto de vCPU por confirmar). Pocas vCPU, así que el
  servidor también tiene que ser eficiente; la redundancia multiplica por N el
  tráfico de salida del VPS.
- **Decisión del usuario:** como es una reconstrucción, cliente y servidor
  serán cengarde desde el primer día. No hace falta compatibilidad en el cable
  con engarde Go (ver historia 005).
- **IPv6 y Starlink** (2026-10-04): hoy solo un enlace Starlink da IPv6,
  nativa y con un prefijo delegado; su IPv4 va detrás de CGNAT. Los módems
  5G no dan IPv6, y no se sabe si el VPS la tiene. El "VPS" podría ser
  también un Debian o Ubuntu casero con IP dinámica, quizá detrás de un
  router, encontrado por DNS dinámico (historia 010).
- **Destino del cliente:** OpenWrt limpio, no SmoothWAN. Según el usuario,
  SmoothWAN quedó abandonado hace años y se quedó en kernel 5.x. Las medidas
  de arriba se hicieron sobre SmoothWAN.

- **Primer uso real** (2026-10-10, observado por el usuario, sin
  registro): transmisión con cengarde en el anfiteatro de Tierra Amarilla,
  un pozo de señal donde antes, con engarde Go y la misma redundancia, se
  sufría. Con peores condiciones de radio que entonces, salieron hasta
  8 Mbit/s de video AV1.
  - **Claro y WOM llevaron el tráfico.** Claro, en LTE B5 (5 MHz, banda
    baja y despejada porque los demás operadores se van a bandas altas),
    con la subida sólida y RSRQ de hasta −5, se llevó casi todo el tráfico y
    las primeras copias. WOM alcanzaba bandas altas y por momentos 5G.
  - **Movistar y Entel quedaban en reserva** («rtt mute», historia 006):
    Movistar por congestión y Entel por errores grandes de capa 1, señal
    intermitente y cambios de RAT caóticos.
  - Es lo que el diseño busca: los enlaces que se atrasan dejan de
    retrasar el túnel sin sacarlos del todo. Falta medirlo con el
    registrador de campo y las métricas de radio (Pendiente).

## Hallazgos

- **Un solo adaptador USB para los 4 enlaces:** las N copias de cada paquete
  pasan por la misma NIC USB, la misma cola y el mismo bus. El coste por paquete
  del driver USB (`r8152` o `ax88179`, según el adaptador) y del etiquetado VLAN
  se multiplica por N. La deduplicación y los lotes de cengarde ahorran en
  espacio de usuario y en WireGuard, pero las N copias en el driver son
  inherentes a la redundancia (de ahí la Fase 5: k-de-N y por tamaño).
- **J4005 frente a Pi 4:** ~3–4× de diferencia con el mismo software, en línea
  con la diferencia de rendimiento por núcleo.
- **Egress del VPS:** con 4 enlaces, cada GB de bajada cuesta 4 GB de salida
  del VPS. A vigilar con la cuota de transferencia del plan.
- **Latencia:** el VPS en Santiago queda cerca de los cuatro operadores
  chilenos (RTT esperado bajo en 5G; medirlo con las sondas de cengarde).

- **Las celdas del banco** (2026-10-14, medido con `AT+QCAINFO`,
  `AT+QENG`, `AT+QRSRP`/`AT+QSINR` y el CSI, más iperf3 por cada VLAN):
  - **Entel:** un monoposte a ~100 m (Villa El Escorial; PCI 151/152, sin
    5G, alterna entre las dos) y una macrocelda en un cerro a ~1 km (PCI 37 en B2 675 y 1125,
    B7 3100 y B28 9485; NSA n78 con PCI 27; 256QAM de subida y 4x4). La PCI
    36 no aparece en esas frecuencias y la 15 es otra macro (la primera que
    llegó con 5G). Bloqueada en la 37, el 5G entra al instante pero el LTE
    llega débil (B7 −96 dBm, SINR negativo): sin bloqueo por ahora, hasta
    instalar 4 LPDA en diamante apuntadas a la macro (historia 013);
  - **Claro:** en hora peak quedó anclado en **B28, PCI 465 (de una minera)**,
    SINR −6/−7: 1,5 Mbit/s de bajada y 5,7 de subida. Tras un reinicio del
    módem (`AT+CFUN=1,1`) enganchó **B2 20 MHz + B4, PCI 90 (planta de
    fundición Paipote, ENAMI)**, SINR 12–14: 95 Mbit/s de bajada y 25 de
    subida. **Fijado al sitio de ENAMI** (eNB 0x19EE:
    `AT+QNWLOCK="common/4g",4,850,90,2050,90,2536,330,9610,330` y reinicio
    de la interfaz): ancla en B2 y agrega B4 (que el módem muestra como
    B66 66536) y B5 (PCI 330); 53 Mbit/s de bajada y 23 de subida esa noche.
    El B7 de Claro que se ve (2900, PCI 142) es de otro sitio y llega muy
    débil (−115 a −119 dBm);
  - **El escaneo (`AT+QSCAN=3,1` en los cuatro módems, 2026-10-14 de
    noche; el identificador de celda dice el sitio: eNB = id >> 8):**

    | Operador | Banda | EARFCN | PCI | eNB | RSRP |
    | --- | --- | --- | --- | --- | --- |
    | Claro | B2 20 MHz | 850 | 90 | 19EE (ENAMI Paipote) | −96 |
    | Claro | B4 10 MHz | 2050 (66536) | 90 | 19EE | −97 |
    | Claro | B5 10 MHz | 2536 (8926) | 330 | 19EE | — |
    | Claro | B28 10 MHz | 9610 | 330 | 19EE | −92 a −99 |
    | Claro | B28 10 MHz | 9610 | 465 | D5E (minera) | −92 a −97 |
    | Claro | B7 20 MHz | 2900 | 142 | D73 | −115 a −119 |
    | Claro | B42 20 MHz | 42990 | 277 | D49A | −104 a −112 |
    | Entel | B7 20 MHz | 3100 | 152 | 864D (monoposte) | −78 a −87 |
    | Entel | B2 15 MHz | 675, 1125 | 152 | 864D | −72 a −81 |
    | Entel | B28 15 MHz | 9485 | 15 | 2C22 | −93 a −105 |
    | Entel | n78 | 620736 | 27 | — | — |
    | WOM | B66 20 MHz | 66936 | 109 | 41C8 | −95 a −99 |
    | WOM | B4 20 MHz | 66736 | 109, 223 | 41C8, 16C38 | −96 a −97 |
    | WOM | B28 10 MHz | 9260 | 314 | 16C38 | −86 a −95 |
    | Movistar | B7 20 MHz | 3300 | 91, 227 | 9B147, 9B54A | −93 a −101 |
    | Movistar | B28 10 MHz | 9360 | 91, 230 | 9B147, 9B54A | −83 a −93 |
    | Movistar | B2 10 MHz | 1000 (8440) | 20 | 9B147 | −87 a −93 |
    | 302-300 | B40 10 MHz | 38800 | 1 | ABAE1 | −109 a −117 |

    Entre paréntesis, el mismo canal con la numeración de otra banda (B66,
    B26, B25). La PCI 37 de la macro de Entel no apareció en este escaneo.
    La B42 de Claro no acepta la SIM (bloqueado ahí, el módem queda en 3G
    sin registro LTE): probablemente reservada para banda ancha hogar
    inalámbrica. La red 302-300 en B40 es una red privada de IoT
    (probablemente de una minera de la zona, vista desde 2021);
  - **WOM:** B66 + NSA n78, cuatro ramas parejas. Sitios, por PCI (datos
    del usuario y del identificador de celda): **109** = eNB 41C8, antena
    de la subida a la población Luis Uribe; **223** = eNB 16C38, Mina
    Tránsito, línea de vista a ~8 km, con 5G (la que usa de noche: B66
    66936 + 66736); **314** (B28 9260) es del mismo eNB 16C38, o sea Mina
    Tránsito, no Las Flores; **24** = calle Las Flores, cerca del estadio de
    Tierra Amarilla, con 5G: aparece solo entre las vecinas, en 66936 y
    66736, a −103 dBm (5–6 dB bajo la 223), y su B28 no se vio. Otras
    vecinas en 66736: 112, 333, 274 (−101 a −106). **Comparadas bloqueadas**
    (2026-10-14, hora peak, WOM solo por la VLAN 20, 15 s):

    | Celda | LTE | 5G | Bajada | Subida | Ping |
    | --- | --- | --- | --- | --- | --- |
    | 223 (Mina Tránsito) | B66 2 × 20 MHz, −98 dBm, SINR −9 a 4 | no enganchó | 43,8 y 29,5 | 14,6 | 40–64 ms |
    | 24 (Las Flores) | B66 20 MHz, −105 dBm, SINR −8 | n78 641664, −93 a −105, SINR 10–14, rank 2 | 32,3 | 17,0 | 63–73 ms |

    La 24 trae 5G pero su ancla LTE llega débil y sin agregación; la 223
    da más bajada y mejor ping. **Queda bloqueado al sitio de Mina
    Tránsito** (`AT+QNWLOCK="common/4g",3,66936,223,66736,223,9260,314`),
    para que no salte de celda a mitad de las medidas; la 24 queda para
    probar de nuevo con las LPDA;
  - **Movistar:** B7, con NR a ratos; RSRQ −20 en las cuatro ramas de LTE
    (celda cargada). Su sitio es la **«palmera falsa» del Hostal Atacama**
    (eNB 9B147): B7 3300 PCI 91, B2 1000 PCI 20 (agregada), B28 9360 PCI 91 y
    NR n78 628334 PCI 125 (NSA). El otro sitio que se ve es el eNB 9B54A
    (PCI 227 en B7 3300, 230 en B28 9360); en B2 1000 también la PCI 194.
  - **Bloqueos para las medidas** (2026-10-14 de noche, para que ningún
    módem salte de celda a mitad de una prueba): Claro al sitio de ENAMI,
    WOM al de Mina Tránsito, Entel al monoposte de Villa El Escorial
    (`AT+QNWLOCK="common/4g",3,3100,152,675,152,1125,152`: B2 + B7 + B2) y
    Movistar a la palmera (`AT+QNWLOCK="common/4g",3,3300,91,1000,20,9360,91`).
    Con el bloqueo de Movistar ancla en B28 y agrega B7, sin B2 ni NR: el
    5G de ese sitio entró cuando el ancla era la B7 [PROBAR sin la B28 en el
    bloqueo, si el 5G importa más que el alcance].

## Qué hacemos con esto

- **Objetivos de rendimiento** de la Fase 1/2: la Pi 4 con 4 enlaces tiene que
  dejar de estar limitada por CPU por debajo de lo que dan los enlaces, y el
  J4005 sirve de referencia x86 de gama baja.
- **Pruebas en el hardware real:** `bench/` no reproduce el driver USB ni las
  VLAN. Cuando haya binario para aarch64, medir CPU por paquete en la Pi con el
  mismo método (`/proc/<pid>/stat`) y en el VPS.
- **Empaquetado (Fase 3):** OpenWrt limpio (24.10 o 25.12, kernel 6.x) en
  aarch64 (Pi 4: target `bcm27xx/bcm2711`, arquitectura
  `aarch64_cortex-a72`) y en x86_64. No SmoothWAN.

## Pendiente

- Confirmar el número de vCPU del plan de Vultr y la cuota de transferencia.
- MTU real de cada operador (para fijar el MTU de WireGuard; ver historia 005).
- Modelo del adaptador USB (driver) y si la NIC soporta offloads con VLAN.
- Una semana de campo con `cengarde-rec` y el `cell-monitor` de SmoothStream
  (RSRP, RSRQ, SINR, banda, RAT y celda de cada Quectel a InfluxDB), con los
  relojes sincronizados, para cruzar los cambios de RAT y el SINR con los
  enlaces en reserva y la tardanza conjunta (historia 012).

## Cambios

- 2026-10-03: creada con los datos de campo del usuario.
- 2026-10-03: el destino es OpenWrt limpio, no SmoothWAN (decisión del usuario).
- 2026-10-04: enlace Starlink con IPv6 y prefijo delegado, 5G sin IPv6 y
  servidor casero posible (historia 010).
- 2026-10-11: primer uso real (Tierra Amarilla, 2026-10-10): Claro en B5 y
  WOM llevaron el tráfico, Movistar y Entel en reserva; la semana de campo
  con las métricas de radio pasa a Pendiente.
- 2026-10-14: el banco en la casa, visto por AT (solo consultas). El CPE de
  dos módems lleva Entel (VLAN 10) y WOM (VLAN 20), los dos RM520N-GL con
  firmware A04; el de Movistar (VLAN 30) es un **RM502Q-AE**, no un
  RM520N; el de Claro (VLAN 40), un RM520N-GL con firmware A03. La gestión
  de los tres CPEs va por la VLAN 99 (99.1, 99.2 y 99.3). Los cuatro anclan
  en LTE: Entel en B7 con B2 y B28 agregadas, WOM en B66 con NR n78 (5G
  NSA, no SA: el 13 de `AT+COPS?` es EN-DC), Movistar en B7 sola y Claro en
  B2 con B5 configurada. Entel y Movistar comparten la banda de anclaje.
  `bench/radiorec.py` los registra cada segundo (historia 012).
- 2026-10-14: la NIC USB gigabit de la Pi (TP-Link `2357:0601`) engancha
  a 480 Mbit/s (USB 2.0) en cualquier puerto, también en un PC: limitaba la
  bajada (historia 012). Transitorio: los enlaces (las VLAN 10–40) pasaron
  a la NIC integrada `eth0` y la LAN (`br-lan`, 192.168.50.1) al
  adaptador, hasta reemplazarlo por uno USB 3.0.
- 2026-10-14: las celdas de cada operador en el banco (PCI y bandas), el
  bloqueo de Entel probado y Claro entre la PCI 465 y la 90.
