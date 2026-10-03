# CLAUDE.md — cengarde

Contexto mínimo para trabajar en este repo. Aquí solo van reglas, comandos e
índice; lo investigado vive en `docs/historias/`. **Mantenerlo por debajo de
~80 líneas.**

## Proyecto

Fork de porech/engarde (Go). engarde duplica cada datagrama de WireGuard por
todos los enlaces del cliente (p. ej. una Raspberry Pi con varios módems) hacia
un servidor (VPS), y el otro extremo se queda con la primera copia. Objetivo
del fork: motor en C (Linux primero: Pi, OpenWrt, VPS), luego eBPF opcional,
empaquetado para OpenWrt y una web de administración nueva. Plan:
`ROADMAP.md`. Estado: Fases 1 y 2 hechas en lo esencial (`engine/`, salud de
enlaces); siguiente, binario para OpenWrt limpio (no SmoothWAN) y la Pi.

## Mapa del repo

- `engine/`: motor C (`src/`, `tests/`, `examples/`, `README.md`).
- `cmd/engarde-{client,server}/`: engarde Go de referencia, solo para medir.
- `webmanager/` → `internal/assets/browser/`: UI Angular del Go (se sustituirá).
- `bench/`: laboratorio netns/veth con `udpgen` (WireGuard falso). Ver
  `bench/README.md`.
- `docs/historias/`: investigación y decisiones (índice abajo).

## Comandos

- Motor: `make -C engine` y `make -C engine test`. También
  `make -C engine SANITIZE=1 test` y, en cruzado,
  `make CC=aarch64-linux-gnu-gcc`.
- Laboratorio (root):
  - `bench/lab.sh build`, `smoke` y `health` (los que corre el CI),
    `compare` (Go frente a C), `latency` y `suite` (línea base Go);
  - `ENGINE=c` usa cengarde en ambos extremos.

  En el contenedor cloud hace falta `apt-get install -y iproute2 strace`
  (`apt-get update` antes); su kernel no tiene IPv6, WireGuard ni
  `sch_netem`.
- CI: `.github/workflows/engine.yml` (gcc/clang con `-Werror`, sanitizers,
  qemu en aarch64/armhf/MIPS big-endian, humo y salud de enlaces en netns).

## Reglas de trabajo

- Docs y conversación en español; código, comentarios y commits en inglés
  (`tipo: resumen`, como upstream).
- PRs hacia `master`: el usuario autoriza crearlos y pushearlos sin preguntar.
- Toda cifra de rendimiento sale de `bench/` o se marca como estimación.
- **Protocolo propio (v2):** cliente y servidor son siempre cengarde, sin
  compatibilidad con engarde Go. Cualquier cambio de formato sube
  `CG_PROTO_VERSION` (historias 005 y 006).
- **Plano de datos:** nunca bloquear, nunca `malloc` por paquete, nunca un log
  por paquete; marcar en el anti-replay solo después de verificar el MAC.
- **Lógica de decisión** (dedup, política de envío, silenciado, tablas): en
  funciones puras dentro de cabeceras pequeñas, con tests unitarios aislados
  (patrón de libRIST, historia 003).
- **Compilar:** el motor tiene que compilar sin warnings con gcc y clang.

## Memoria del proyecto: historias

- Este archivo no crece con investigación. Lo investigado va a
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
| [004](docs/historias/004-entorno-real.md) | Entorno real: Pi 4 (destino OpenWrt limpio), 4 enlaces 5G en VLAN, VPS Vultr | fijes objetivos de rendimiento o empaquetado |
| [005](docs/historias/005-motor-c-v1.md) | Motor C v1: protocolo, arquitectura y medidas | toques `engine/` |
| [006](docs/historias/006-salud-de-enlaces.md) | Salud de enlaces (silenciado, mudo), protocolo v2, baja latencia | toques las sondas, el reparto o las perillas de latencia |
