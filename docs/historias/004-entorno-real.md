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
