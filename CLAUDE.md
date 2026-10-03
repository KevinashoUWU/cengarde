# CLAUDE.md — cengarde

Contexto mínimo para trabajar en este repo. Aquí solo van reglas, comandos e
índice; lo investigado vive en `docs/historias/`. **Mantenerlo por debajo de
~80 líneas.**

## Proyecto

Fork de porech/engarde (Go). engarde duplica cada datagrama de WireGuard por
todos los enlaces del cliente (p. ej. una Raspberry Pi con varios módems) hacia
un servidor (VPS), y WireGuard descarta los duplicados. Objetivo del fork:
reescribirlo en C (Linux primero: Pi, OpenWrt, VPS), luego eBPF opcional,
empaquetado para OpenWrt y una web de administración nueva. Plan y criterios
de salida: `ROADMAP.md`. Estado: Fase 0 (banco de pruebas) hecha; siguiente,
Fase 1a (cliente C compatible en el cable con engarde Go).

## Mapa del repo

- `cmd/engarde-{client,server}/`: implementación Go de referencia (upstream).
  Se porta su comportamiento, no su diseño.
- `webmanager/` → `internal/assets/browser/`: UI Angular embebida (se sustituirá).
- `bench/`: laboratorio netns/veth, `udpgen` (WireGuard falso) y
  `protoclient.c` (prototipo de cliente en C). Ver `bench/README.md`.
- `docs/historias/`: investigación y decisiones (índice abajo).

## Comandos

- Go: `go build ./cmd/...` (necesita `internal/assets/browser/`: `make
  frontend` o un `index.html` de relleno).
- Laboratorio (root): `bench/lab.sh build`, `bench/lab.sh suite`, demos
  `demo_stranger`, `demo_webpanic` y `demo_races`. En el contenedor cloud
  hace falta antes `apt-get install -y iproute2 strace`; su kernel no tiene
  WireGuard ni `sch_netem`.
- Todavía no hay tests unitarios.

## Reglas de trabajo

- Docs y conversación en español; código, comentarios y commits en inglés
  (`tipo: resumen`, como upstream).
- PRs hacia `master`: el usuario autoriza crearlos y pushearlos sin preguntar.
- Toda cifra de rendimiento sale de `bench/` o se marca como estimación.
- Compatibilidad en el cable con engarde Go hasta la Fase 5 (protocolo v0:
  datagramas WireGuard tal cual).
- Plano de datos: nunca bloquear, nunca `malloc` por paquete, nunca un log por
  paquete.
- La lógica de decisión (silenciado de enlaces, dedup, admisión de caminos) va
  en funciones puras dentro de cabeceras pequeñas, con tests unitarios
  aislados (patrón de libRIST, historia 003).

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
| [001](docs/historias/001-diagnostico-engarde-go.md) | Diagnóstico medido del engarde Go | toques el plano de datos, el servidor o la web; para no repetir sus bugs |
| [002](docs/historias/002-wireguard-para-cengarde.md) | WireGuard: formato, índices, anti-replay | implementes dedup, sesiones, admisión de caminos o MTU |
| [003](docs/historias/003-librist-gestion-de-enlaces.md) | libRIST: silenciado de enlaces, WRR, ARQ | diseñes la salud de los enlaces, el reparto, el bonding o la recuperación |
