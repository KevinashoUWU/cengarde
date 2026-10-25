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
- `bench/`: laboratorio netns/veth con `udpgen` (WireGuard falso); `docs/historias/`: investigación (índice abajo);
  `docs/mapa/`: mapa del motor (una nota por pieza y por perilla; bóveda Obsidian `~/Obsidian/cengarde`).

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
- **Mapa (`docs/mapa`), al día con HEAD:** léelo antes de tocar una pieza. Un PR que la cambia
  actualiza su nota y la re-sella (`docs/mapa/mapa.py sello "NOTA.md"`); una perilla nueva va en
  `perillas.py` (`mapa.py perillas`). El CI (`mapa.py check`) falla si el mapa queda atrás.

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
| [002](docs/historias/002-wireguard-para-cengarde.md) | WireGuard: formato, índices, anti-replay, el reloj del router | toques el MTU, la detección de WireGuard, las sesiones o los handshakes |
| [003](docs/historias/003-librist-gestion-de-enlaces.md) | libRIST: silenciado de enlaces, WRR, ARQ | diseñes la salud de los enlaces, el reparto, el bonding o la recuperación |
| [004](docs/historias/004-entorno-real.md) | Entorno real: Pi 4 (destino OpenWrt limpio), 4 enlaces 5G en VLAN y Starlink, VPS Vultr | fijes objetivos de rendimiento o empaquetado |
| [006](docs/historias/006-salud-de-enlaces.md) | Salud de enlaces (silenciado, mudo), protocolo v2, baja latencia | toques las sondas, el reparto o las perillas de latencia |
| [007](docs/historias/007-openwrt-y-vps.md) | Paquete OpenWrt, plantilla del VPS, SmoothWAN como referencia | empaquetes o instales en OpenWrt o el VPS |
| [008](docs/historias/008-luci-uci-y-emparejamiento.md) | UCI, LuCI, un solo secreto (BLAKE2s), imágenes, prueba en QEMU | toques `openwrt/`, `cengarde keys` o el cloud-config |
| [009](docs/historias/009-recarga-control-e-ip-pass.md) | Recarga sin cortar, `cengarde ctl`, pausa de enlaces, IP pass desde el router (v3) | toques la recarga, el socket de control, procd o el IP pass |
| [010](docs/historias/010-ipv6-varias-ip-multicliente-nombres.md) | Plan de IPv6, varios routers por VPS y nombres; hechos el PR 1 (reinicio del servidor, metadatos del VPS), el PR 2 (dirección de llegada, IPv6 por fuera, fuga de IPv6) y el PR 3: 3d1 (herramientas del VPS), 3d2a (protocolo v4: pista, HELLO y cookies), 3d2b (varios routers en el servidor, tabla de reenvío) y 3d2c (`cengarde-vps-setup` con varios routers y `forward add`, dirección del túnel derivada en OpenWrt), el PR 4 (IPv6 dentro del túnel: ULA, NAT66, `TUNNEL_IPV6`) y el PR 5 (nombres: resolvedor propio por los enlaces, `dns.h`, `resolve.h`, `names.c`) | toques el protocolo, IPv6, las direcciones del VPS o del túnel, el multicliente, `cengarde-vps-setup`, `cengarde-nat`, los nombres o el DNS |
| [011](docs/historias/011-hilos.md) | Hilos: colas del servidor (PR 3a, `lanes`, `steer.h`, `rcvbudget.h`, `udp_mem`), la puerta S1 y un hilo por enlace en el router (PR 3b, `link_threads`, anillo SPSC) | toques `lanes`, el programa BPF, el presupuesto de recepción, `udp_mem`, `link_threads`, `ring.h`, `pump.c` o los hilos de cualquier extremo |
| [012](docs/historias/012-bonding.md) | Estudio de bonding (60 fuentes, Haiku/Sonnet/Opus) y el plan de la Fase 5: laboratorio, reordenador por la secuencia de cengarde, exclusión rápida, control de cola; paso 0: `lab.sh bond` (TCP y QUIC), los instrumentos pasivos (`joint`, `lost_all`, `sizes`) y el registrador de campo; paso 1: reordenador y reparto experimental con guardas (`split_spread_ms`, `split_spread_copy`), medidos en laboratorio y en la Pi; paso 2: el gobernador base (`governor`) | diseñes el bonding, k-de-N, la FEC, el ARQ o el reparto entre enlaces, o toques `bond`, `arrival.h`, `reorder.h`, `split.h`, `govern.h`, `cengarde-rec`, `fieldrec.py` o `fieldperf` |
| [013](docs/historias/013-capacidad-y-dosificacion.md) | Paso 4: el reordenador que se rinde antes de tiempo, la capacidad por enlace desde las sondas, pesos según ella y el tope para `cake` | toques `reorder.h`, el plazo de un hueco, la capacidad, los pesos del reparto o `cake` en el túnel |
| [014](docs/historias/014-tunel-nativo.md) | Túnel nativo: salir de WireGuard (TUN, MAC, cifrado opcional), por qué (las retransmisiones de TCP bloqueadas), investigación, medidas en la Pi, fase 1 (PR #61), `tcp_aware` y por qué una retransmisión no se adelanta, y el plan por flujo | toques `tunnel`, la TUN, `flow.h`, `tunroute.h`, `tcpfree.h`, el cifrado o el reordenamiento por flujo |
| [015](docs/historias/015-modelo-de-enlace.md) | Modelo de enlace: `type` por enlace (`auto`, `cellular`, `starlink`, `fiber`, `ethernet`, `radio`), su envío al VPS en la cola de bonding (bits 20–22, sin versión nueva), el preset de Starlink (retención con costo, copias a tiempo en redundancia), las correcciones mixtas de la auditoría probadas por tipo, las ranuras de Starlink medidas (`starslot.py`, `starfield.py`), el enlace dominante (`dominant.h`, caso `dominant`), los engranes de Starlink (pérdida por secuencia, reloj de ranuras `slot.h`, copias en el borde) y lo que falta | toques `linkmodel.h`, `dominant.h`, `slot.h`, `starsat`, `starslot.py`, `type` en `[link]`, `uplink_type`, `eta_buffer`, `capacity_loss_pct`, `exp_link_types`, las perillas de la auditoría o el reparto con Starlink |
| [016](docs/historias/016-celda-virtual.md) | Celda LTE virtual (srsRAN 4G + GNU Radio, `bench/celda/`): el acantilado del SNR (con ~1 dB, RTT de segundos aunque la celda esté vacía), la carga de otros usuarios, Entel en B2 frente a B28, cengarde encima, `mute_queue` / `capacity_expire_ms` (apagadas) y la cola honda de operador con perfiles calibrados (`entel_b2`, `wom`) | estudies enlaces celulares por dentro (SNR, MCS, HARQ, scheduler, colas), toques `bench/celda/` o sus perfiles, el silenciado por cola o la caducidad de la capacidad |
| [017](docs/historias/017-terreno-nostalgica.md) | Terreno en Nostálgica FM 20–22-10: copias recortadas por `pace_peers_ms` 60 (WOM 36 %), la perilla a 250 (subida 0,29 → 0,002 %), Entel B2 → B28, Starlink pierde sin cola y más allá del plato, rescate casi mudo (arreglado en 023) | toques `pace_peers_ms`, las copias en redundancia, el rescate, o prepares terreno con Starlink y celulares |
| [018](docs/historias/018-ns3-verdad-conocida.md) | ns-3 con verdad conocida (`~/sdr/ns3`): capacidad sobreestimada +510 % con búfer corto, C5 medido, prototipos `lossq` y `stand`, reglas «cola mía o de otros» (88 %) y firma de pérdida (89 %), ya en el estado (`linksig.h`), receta de cola honda | toques `capacity.h`, `linksig.h`, el arranque rápido, el silenciado, o quieras detectar el tipo de enlace |
| [019](docs/historias/019-perdida-con-cola.md) | Capacidad con verdad de ns-3: pérdida con cola a cualquier carga (`capacity_loss_queue_ms`) y la cola permanente que deshacía el arranque rápido (`capacity_stand`, choque C5), medidas en ns-3, la celda calibrada y el laboratorio | toques `capacity.h`, los cortes de la capacidad, el arranque rápido o un búfer corto de operador |
| [020](docs/historias/020-copias-parciales-y-paridad.md) | Copias parciales y paridad por el enlace ahogado (`redund_spare`: `copies`, `parity`, `mix`; `spare.h`): la paridad en lo que sobra baja la pérdida suelta 4–10 veces y sube el TCP 70 % en la celda (WOM, Entel B2), pero en un microcorte las copias completas cubren más; apagada, `mix` para terreno; la FEC v7 no actúa en redundancia | toques `spare.h`, `redund_spare`, la paridad en redundancia, las copias a un enlace ahogado, `bench/celda/proteger.py` o los casos `protect` del laboratorio |
| [021](docs/historias/021-fec-v2.md) | FEC v2 en lo que sobra (`redund_spare`: `rs`, `rlc`): Reed-Solomon sobre Leopard-RS vendorizado (sin libstdc++; error del camino portable corregido) y RLC de ventana deslizante, protocolo 8 (`CG_T_REPAIR`), plan de k y m según la pérdida; costo por paquete, laboratorio y celda contra `mix`, `parity` y las copias de 250 ms | toques `rs.h`, `rs.c`, `rlc.h`, `gf256.h`, `repair.h`, `third_party/leopard`, `cg_spare_plan`, `redund_spare_code_ms` o las reparaciones del servidor |
| [022](docs/historias/022-multistarlink.md) | Varios Starlink (Manto Verde): `multisl.py` y los casos `msl*`, correlación entre platos (un plato más solo tapa lo no compartido: el flujo queda en ρ × p; bordes alineados), el motor con varios `starlink` (`cg_spare_mask`, pares del dominante, copias en el borde por cada plato), escenarios con celulares, la receta de VLAN en el NUC y el plan para medir ρ con dos platos | uses varios platos, toques `dominant.h`, `slot.h`, `spare.h` con varios enlaces del mismo tipo, o prepares el montaje de Manto Verde |
| [023](docs/historias/023-reparacion-coherente.md) | Reparación coherente: por qué el rescate estaba mudo (la pista `CG_F_PREV` antes de las copias, nada retenido en redundancia), la paridad pedida (PARQ, protocolo 9: déficit de rango de RLC, índices nuevos de RS), el orden código → PARQ → NACK, TCP de adentro (no competirle, pérdidas más allá del túnel) y la retransmisión según la evidencia (`tcp_ack_rule`: ACK que vuelven y la marca `CG_F_TUNHOLE`) | toques el rescate, `harq.h`, `CG_T_PARQ`, `rescue_code_ms`, `parq`, `rescue_tcp_race`, `tcp_retx_one`, `tcp_ack_rule`, `tcpack.h`, la pista de las copias o el estado `tcp_e2e` |
| [024](docs/historias/024-pep-tcp-partido.md) | PEP modo A: TCP partido en el router y el VPS con piezas de Debian (`ss-redir`/`ss-server`, kcptun) contra el túnel transparente: gana por BBR en el tramo lejano (pérdida más allá del VPS: 1,1 → 44 Mbit/s, vivo 80 → 0 % perdido), con copias duplica el TCP; costos (ACK roto, búferes de MB) y las cifras que el PEP B debe batir | diseñes el PEP B (protocontrolador), toques `BOND_PEP`, `livesim.py`, `pepstat.py` o `peptab.py`, o pienses en proxies o BBR en el VPS |
