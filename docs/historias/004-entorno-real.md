# 004 — Entorno real de despliegue (datos de campo)

- **Fecha:** 2026-10-03
- **Estado:** vigente
- **Fuentes:** datos aportados por el usuario (2026-10-03).

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
- **Empaquetado (Fase 3):** el destino natural es SmoothWAN/OpenWrt en aarch64
  (Pi 4: `aarch64_cortex-a72`) y x86_64.

## Pendiente

- Confirmar el número de vCPU del plan de Vultr y la cuota de transferencia.
- MTU real de cada operador (para fijar el MTU de WireGuard; ver historia 005).
- Modelo del adaptador USB (driver) y si la NIC soporta offloads con VLAN.

## Cambios

- 2026-10-03: creada con los datos de campo del usuario.
