# CLAUDE.md — cengarde

Contexto mínimo: reglas, comandos e índice; lo investigado vive en
`docs/historias/`. **Mantenerlo por debajo de ~80 líneas.**

## Proyecto

Fork de porech/engarde (Go). engarde duplica cada datagrama de WireGuard por
todos los enlaces del cliente (p. ej. una Raspberry Pi con varios módems) hacia
un servidor (VPS), y el otro extremo se queda con la primera copia. Objetivo
del fork: motor en C (Pi, OpenWrt, VPS), eBPF opcional y OpenWrt con LuCI.
Plan: `ROADMAP.md`. Estado: Fases 1 y 2 hechas; Fase 3: OpenWrt (UCI, LuCI,
imágenes, un solo secreto, `cengarde ctl`, recarga, IP pass, IPv6, varios
routers por VPS, nombres con DNS dinámico) en QEMU; falta hardware real.

## Mapa del repo

- `engine/`: motor C (`src/`, `tests/`, `examples/`, `README.md`).
- `openwrt/`: paquetes `cengarde` y `luci-app-cengarde`, `test/` (VMs QEMU
  y Playwright) y guía; `contrib/`: VPS (cloud-init, NAT) y systemd.
- `bench/`: laboratorio netns/veth con `udpgen` (WireGuard falso); `docs/historias/`: investigación (índice abajo).

## Comandos

- Motor: `make -C engine test`, `SANITIZE=1 test` (ASan), `SANITIZE=thread
  test` (TSAN) y, en cruzado, `make CC=aarch64-linux-gnu-gcc`.
- Laboratorio (root): `bench/lab.sh build`, `ci` (smoke, health, control y
  `lab.d/`) y `latency`; `ENGINE=go bench/lab.sh build` suma el engarde Go,
  que salió del árbol (`3492df9`), para `compare` (Go frente a C) y `suite`.
- OpenWrt de punta a punta: `openwrt/test/e2e.sh IMAGEN` (`openwrt/README.md`).
- Contenedor cloud: `apt-get update`, `iproute2 iptables strace` (laboratorio)
  o `qemu-system-x86 openssh-client` (e2e); sin IPv6, WireGuard, netem, systemd.
- CI: `engine.yml` (gcc/clang `-Werror`, ASan, TSAN, qemu aarch64/armhf/MIPS
  BE, AArch64 real, laboratorio en netns), `openwrt.yml` (paquetes, imágenes,
  e2e con KVM, release con tags `v*`), `vps.yml` (`cengarde-nat`, systemd
  real) y `bond.yml` (`lab.sh bond`: WireGuard y TCP reales sobre netem).

## Reglas de trabajo

- Docs y conversación en español; código, comentarios y commits en inglés
  (`tipo: resumen`, como upstream).
- PRs hacia `master`: el usuario autoriza crearlos y pushearlos sin preguntar.
- Toda cifra de rendimiento sale de `bench/` o se marca como estimación.
- **Protocolo propio (v4):** cliente y servidor son siempre cengarde, sin
  compatibilidad con engarde Go. Cualquier cambio de formato sube
  `CG_PROTO_VERSION` (historias 005, 006, 009 y 010).
- **Plano de datos:** nunca bloquear, nunca `malloc` por paquete, nunca un log
  por paquete; marcar en el anti-replay solo después de verificar el MAC.
- **Lógica de decisión** (dedup, política de envío, silenciado, tablas): en
  funciones puras dentro de cabeceras pequeñas, con tests unitarios aislados
  (patrón de libRIST, historia 003).
- **Compilar:** el motor tiene que compilar sin warnings con gcc y clang.

## Memoria del proyecto: historias

- Este archivo no crece con investigación: lo investigado va a
  `docs/historias/NNN-tema.md` (plantilla en `docs/historias/README.md`), y
  aquí solo se añade una línea al índice.
- Antes de investigar algo, busca si ya hay historia. Si cambian los hechos,
  actualiza la historia existente (fecha y qué cambió) en vez de crear otra.
- Basta con leer el TL;DR de cada historia. Estancado o buscando ideas: lee
  el estudio `012-bonding/ejes.md` y marca ahí lo probado, medido o descartado.
- Con más de ~15 entradas en el índice, las cerradas van al archivo del README.

## Índice de historias

| # | Tema | Léela cuando… |
| --- | --- | --- |
| [001](docs/historias/001-diagnostico-engarde-go.md) | Diagnóstico medido del engarde Go | quieras saber qué no repetir o comparar con la línea base |
| [002](docs/historias/002-wireguard-para-cengarde.md) | WireGuard: formato, índices, anti-replay, el reloj del router | toques el MTU, la detección de WireGuard, las sesiones o los handshakes |
| [003](docs/historias/003-librist-gestion-de-enlaces.md) | libRIST: silenciado de enlaces, WRR, ARQ | diseñes la salud de los enlaces, el reparto, el bonding o la recuperación |
| [004](docs/historias/004-entorno-real.md) | Entorno real: Pi 4 (destino OpenWrt limpio), 4 enlaces 5G en VLAN y Starlink, VPS Vultr | fijes objetivos de rendimiento o empaquetado |
| [005](docs/historias/005-motor-c-v1.md) | Motor C v1: protocolo, arquitectura y medidas | toques `engine/` |
| [006](docs/historias/006-salud-de-enlaces.md) | Salud de enlaces (silenciado, mudo), protocolo v2, baja latencia | toques las sondas, el reparto o las perillas de latencia |
| [007](docs/historias/007-openwrt-y-vps.md) | Paquete OpenWrt, plantilla del VPS, SmoothWAN como referencia | empaquetes o instales en OpenWrt o el VPS |
| [008](docs/historias/008-luci-uci-y-emparejamiento.md) | UCI, LuCI, un solo secreto (BLAKE2s), imágenes, prueba en QEMU | toques `openwrt/`, `cengarde keys` o el cloud-config |
| [009](docs/historias/009-recarga-control-e-ip-pass.md) | Recarga sin cortar, `cengarde ctl`, pausa de enlaces, IP pass desde el router (v3) | toques la recarga, el socket de control, procd o el IP pass |
| [010](docs/historias/010-ipv6-varias-ip-multicliente-nombres.md) | Plan de IPv6, varios routers por VPS y nombres; hechos el PR 1 (reinicio del servidor, metadatos del VPS), el PR 2 (dirección de llegada, IPv6 por fuera, fuga de IPv6) y el PR 3: 3d1 (herramientas del VPS), 3d2a (protocolo v4: pista, HELLO y cookies), 3d2b (varios routers en el servidor, tabla de reenvío) y 3d2c (`cengarde-vps-setup` con varios routers y `forward add`, dirección del túnel derivada en OpenWrt), el PR 4 (IPv6 dentro del túnel: ULA, NAT66, `TUNNEL_IPV6`) y el PR 5 (nombres: resolvedor propio por los enlaces, `dns.h`, `resolve.h`, `names.c`) | toques el protocolo, IPv6, las direcciones del VPS o del túnel, el multicliente, `cengarde-vps-setup`, `cengarde-nat`, los nombres o el DNS |
| [011](docs/historias/011-hilos.md) | Hilos: colas del servidor (PR 3a, `lanes`, `steer.h`, `rcvbudget.h`, `udp_mem`), la puerta S1 y un hilo por enlace en el router (PR 3b, `link_threads`, anillo SPSC) | toques `lanes`, el programa BPF, el presupuesto de recepción, `udp_mem`, `link_threads`, `ring.h`, `pump.c` o los hilos de cualquier extremo |
| [012](docs/historias/012-bonding.md) | Estudio de bonding (60 fuentes, Haiku/Sonnet/Opus) y el plan de la Fase 5: laboratorio, reordenador por la secuencia de cengarde, exclusión rápida, control de cola; paso 0: `lab.sh bond` (TCP y QUIC), los instrumentos pasivos (`joint`, `lost_all`, `sizes`) y el registrador de campo; paso 1: reordenador y reparto experimental con guardas (`split_spread_ms`, `split_spread_copy`), medidos en laboratorio y en la Pi; paso 2: el gobernador base (`governor`) | diseñes el bonding, k-de-N, la FEC, el ARQ o el reparto entre enlaces, o toques `bond`, `arrival.h`, `reorder.h`, `split.h`, `govern.h`, `cengarde-rec`, `fieldrec.py` o `fieldperf` |
| [013](docs/historias/013-capacidad-y-dosificacion.md) | Paso 4: el reordenador que se rinde antes de tiempo, la capacidad por enlace desde las sondas, pesos según ella, el tope para `cake` y la vuelta tras un interferente (`split_predict`) | toques `reorder.h`, el plazo de un hueco, la capacidad, los pesos del reparto o `cake` en el túnel |
| [014](docs/historias/014-tunel-nativo.md) | Túnel nativo: salir de WireGuard (TUN, MAC, cifrado opcional), por qué (las retransmisiones de TCP bloqueadas), investigación, medidas en la Pi, fase 1 (PR #61), `tcp_aware` y por qué una retransmisión no se adelanta, y el plan por flujo | toques `tunnel`, la TUN, `flow.h`, `tunroute.h`, `tcpfree.h`, el cifrado o el reordenamiento por flujo |
