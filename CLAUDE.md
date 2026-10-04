# CLAUDE.md — cengarde

Contexto mínimo: reglas, comandos e índice; lo investigado vive en
`docs/historias/`. **Mantenerlo por debajo de ~80 líneas.**

## Proyecto

Fork de porech/engarde (Go). engarde duplica cada datagrama de WireGuard por
todos los enlaces del cliente (p. ej. una Raspberry Pi con varios módems) hacia
un servidor (VPS), y el otro extremo se queda con la primera copia. Objetivo
del fork: motor en C (Pi, OpenWrt, VPS), eBPF opcional y OpenWrt con LuCI.
Plan: `ROADMAP.md`. Estado: Fases 1 y 2 hechas; Fase 3: OpenWrt (UCI, LuCI,
imágenes, un solo secreto, `cengarde ctl`, recarga sin cortar, IP pass desde
el router) probado en QEMU; falta hardware real.

## Mapa del repo

- `engine/`: motor C (`src/`, `tests/`, `examples/`, `README.md`).
- `openwrt/`: paquetes `cengarde` y `luci-app-cengarde`, `test/` (VMs QEMU
  y Playwright) y guía; `contrib/`: VPS (cloud-init, NAT) y systemd.
- El engarde Go original salió del árbol; sigue en el historial (`3492df9`).
- `bench/`: laboratorio netns/veth con `udpgen` (WireGuard falso), ver su README.
- `docs/historias/`: investigación y decisiones (índice abajo).

## Comandos

- Motor: `make -C engine test`, `make -C engine SANITIZE=1 test` y, en
  cruzado, `make CC=aarch64-linux-gnu-gcc`.
- Laboratorio (root): `bench/lab.sh build`, `ci` (smoke, health, control y
  `lab.d/`) y `latency`; `ENGINE=go bench/lab.sh build` suma el Go del
  historial, para `compare` (Go frente a C) y `suite` (línea base Go).
- OpenWrt de punta a punta: `openwrt/test/e2e.sh IMAGEN` (`openwrt/README.md`).
- Contenedor cloud: `apt-get update` y luego `iproute2 strace` (laboratorio)
  o `qemu-system-x86 openssh-client` (e2e); sin IPv6, WireGuard ni netem.
- CI: `engine.yml` (gcc/clang con `-Werror`, sanitizers, qemu en aarch64,
  armhf y MIPS big-endian, laboratorio en netns), `openwrt.yml` (paquetes
  25.12/24.10, imágenes, e2e con KVM y release con tags `v*`) y `vps.yml`
  (reglas de seguridad de `cengarde-nat` en netns).

## Reglas de trabajo

- Docs y conversación en español; código, comentarios y commits en inglés
  (`tipo: resumen`, como upstream).
- PRs hacia `master`: el usuario autoriza crearlos y pushearlos sin preguntar.
- Toda cifra de rendimiento sale de `bench/` o se marca como estimación.
- **Protocolo propio (v3):** cliente y servidor son siempre cengarde, sin
  compatibilidad con engarde Go. Cualquier cambio de formato sube
  `CG_PROTO_VERSION` (historias 005, 006 y 009).
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
- Basta con leer el TL;DR de cada historia; el resto, solo si la tarea lo pide.
- Si el índice pasa de ~15 entradas, las historias cerradas se mueven al
  archivo de `docs/historias/README.md`.

## Índice de historias

| # | Tema | Léela cuando… |
| --- | --- | --- |
| [001](docs/historias/001-diagnostico-engarde-go.md) | Diagnóstico medido del engarde Go | quieras saber qué no repetir o comparar con la línea base |
| [002](docs/historias/002-wireguard-para-cengarde.md) | WireGuard: formato, índices, anti-replay | toques el MTU, la detección de WireGuard o las sesiones |
| [003](docs/historias/003-librist-gestion-de-enlaces.md) | libRIST: silenciado de enlaces, WRR, ARQ | diseñes la salud de los enlaces, el reparto, el bonding o la recuperación |
| [004](docs/historias/004-entorno-real.md) | Entorno real: Pi 4 (destino OpenWrt limpio), 4 enlaces 5G en VLAN y Starlink, VPS Vultr | fijes objetivos de rendimiento o empaquetado |
| [005](docs/historias/005-motor-c-v1.md) | Motor C v1: protocolo, arquitectura y medidas | toques `engine/` |
| [006](docs/historias/006-salud-de-enlaces.md) | Salud de enlaces (silenciado, mudo), protocolo v2, baja latencia | toques las sondas, el reparto o las perillas de latencia |
| [007](docs/historias/007-openwrt-y-vps.md) | Paquete OpenWrt, plantilla del VPS, SmoothWAN como referencia | empaquetes o instales en OpenWrt o el VPS |
| [008](docs/historias/008-luci-uci-y-emparejamiento.md) | UCI, LuCI, un solo secreto (BLAKE2s), imágenes, prueba en QEMU | toques `openwrt/`, `cengarde keys` o el cloud-config |
| [009](docs/historias/009-recarga-control-e-ip-pass.md) | Recarga sin cortar, `cengarde ctl`, pausa de enlaces, IP pass desde el router (v3) | toques la recarga, el socket de control, procd o el IP pass |
| [010](docs/historias/010-ipv6-varias-ip-multicliente-nombres.md) | Plan de IPv6, respuesta desde la dirección de llegada, varios routers por VPS (v4) y nombres; PR 1: reinicio del servidor y metadatos del VPS | toques IPv6, las direcciones del VPS, el multicliente, `cengarde-nat` o los nombres |
