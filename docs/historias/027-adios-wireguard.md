# 027 — Adiós a WireGuard: el túnel nativo es el único

- **Fecha:** 2026-10-26
- **Estado:** vigente
- **Fuentes:** tarea #72 y decisión del usuario del 2026-10-26 («deprecar a
  wireguard completo, de todo: del código, panel, LuCI, todo»); rama
  `claude/drop-wireguard`, para la 0.12.0; historias
  [002](002-wireguard-para-cengarde.md), [014](014-tunel-nativo.md) y
  [025](025-protocontrolador.md).

## TL;DR

- Desde 0.12 cengarde ya no lleva datagramas de un WireGuard local: el túnel
  es siempre la TUN propia (`cg0`, `tunnel = native`), autenticada por el MAC
  y sin cifrar. Router y VPS se actualizan juntos, así que no hay
  compatibilidad con el modo viejo.
- Por qué: el túnel nativo ([014](014-tunel-nativo.md)) y el protocontrolador
  ([025](025-protocontrolador.md)) solo existen en nativo y son lo que se
  mide y se usa en terreno; WireGuard era un segundo camino que había que
  probar en cada cambio (laboratorio, e2e, LuCI, VPS) sin aportar nada que el
  nativo no tenga, salvo el cifrado (quien lo necesite, una VPN propia por
  dentro del túnel).
- Salieron del motor `wgwatch.h` (el reloj atrasado de la
  [002](002-wireguard-para-cengarde.md)), `wgport.h`, `wgsize.h` (el campo
  `sizes`), `cg_looks_like_wg`, la guarda de WireGuard de `rebuilt.h`, los
  sockets por sesión del servidor y las claves `CG_WG_*` de `cengarde keys`.
- Las claves `wireguard`, `wireguard_poke`, `wireguard_port_file` y, en el
  cliente, `listen` son errores de configuración que nombran esta historia;
  `tunnel = wireguard` también. `tunnel = native` sigue valiendo (y es lo que
  rige sin la clave).
- El formato del protocolo no cambia (`CG_PROTO_VERSION` sigue en 11): nada
  del cable existía solo para WireGuard; lo que cambia es qué decide el
  emisor (ya no duplica ni deja pasar por tipo de mensaje de WireGuard).

## Contexto

Hasta 0.11 había dos modos: `tunnel = wireguard` (por omisión; cengarde entre
un WireGuard local y los enlaces) y `tunnel = native` (experimental desde la
fase 1 de la [014](014-tunel-nativo.md), PR #61). En terreno, desde la
[014](014-tunel-nativo.md) se corre nativo: las retransmisiones de TCP solo se
pueden ver y liberar sin cifrado (`tcp_aware`, `tcpfree.h`, `tcpack.h`), y el
protocontrolador lleva sus flujos dentro del DATA nativo. El modo WireGuard
quedó como un camino más que mantener: su vigilancia del reloj, el archivo de
puertos, los perfiles de LuCI, `wgcg`, los `wg-quick@cg-NOMBRE` del VPS y las
ramas del laboratorio.

## Hallazgos: qué salió y qué lo reemplaza

**Motor** (`engine/`; unas 2 000 líneas menos, 660 nuevas, sobre todo tests
reescritos):

- `wgwatch.h` y el «pinchazo» (`wireguard_poke`, `wg_poke`,
  `cg_wgw_redirect`): no hay WireGuard que ignore handshakes viejos; el
  túnel nativo no tiene estado de handshake que dependa del reloj.
- `wgport.h` y `wireguard_port_file`: el puerto con que WireGuard conocía a
  cada cliente; sin sockets hacia WireGuard no hay puerto que conservar.
- Los sockets por sesión del servidor (`wg_socket`, `session_rehome`,
  `wg_read`): todo entra y sale por la única TUN (`tun_read`, `tun_write`,
  `flush_tun`). `rcvbudget.h` ya no cuenta un socket por router: 8 carriles
  y el de basura son 9 sockets, no 10.
- El cliente ya no abre `listen` (127.0.0.1:59401) ni recuerda el extremo de
  WireGuard (`wg_peer`, `cg_looks_like_wg`): `wg_fd`, `wg_read`, `wg_send` y
  `wg_msg` pasan a `tun_fd`, `tun_read`, `tun_send` y `tun_msg`; el evento
  `CG_EV_WG` se fue (el cliente usa `CG_EV_TUN`).
- `wgsize.h` y el campo `sizes` del estado: contaba los datagramas por tipo de
  mensaje de WireGuard; en nativo todo caía en `other`, así que ya no medía
  nada. `bench/fieldrec.py` sigue leyendo grabaciones viejas que lo tienen.
- Nombres prestados que se quedan con nombre nativo: el reparto
  (`cg_split_pick`) ya no recibe un tipo de mensaje (en nativo siempre era
  «datos»): duplica por tamaño (`split_small`). El reordenador cambia
  `wgtype` por `nohold` (lo que nunca espera: un mensaje del protocontrolador)
  y `reorder_bypass` cuenta bytes del paquete IP. `cg_rebuilt_check` solo
  conoce la forma IP y los mensajes del protocontrolador. `CG_WG_OVERHEAD`
  salió: el aviso de MTU habla de `tun_mtu` (techo 1448 sobre IPv4 y 1428
  sobre IPv6 en un camino de 1500).
- Estado: `download.wireguard_drops` y el `wireguard_drops` de cada sesión
  pasan a `tun_drops`; se van el extremo `wireguard` del cliente, el
  `wireguard` de cada cliente del servidor, `wireguard_unanswered` y el
  objeto `wireguard` (`pokes`, `redirected_handshakes`). `tunnel` queda
  (siempre `native`).
- `cengarde keys` deja de imprimir `CG_WG_SERVER_KEY`, `CG_WG_CLIENT_KEY` y
  `CG_WG_PSK`; las etiquetas `wg-server`, `wg-client` y `wg-psk` no se
  reusan. `CG_LINK_KEY` y las direcciones del túnel no cambian.
- `cengarde -t` en un servidor con varios routers ya no dice «WireGuard at».
- De paso, un error del túnel nativo que el laboratorio destapó al quedarse
  solo con él: `SIOCSIFADDR` reemplaza la dirección IPv4 de un dispositivo,
  así que de varios `tun_addr` IPv4 solo quedaba el último. Desde la segunda
  van como secundarias con la etiqueta `cg0:1`, `cg0:2`… (`tun.c`).

**OpenWrt** (`openwrt/`): `cengarde-setup` solo arma `cg0` (sin `wgcg`, su
peer, claves ni `tunnel_mode`); el paquete deja de depender de
`wireguard-tools` y LuCI de `luci-proto-wireguard`; LuCI pierde la elección
de modo y la fila de handshakes; el e2e prueba solo nativo y suma un paso que
siembra lo que dejaba una versión con WireGuard y comprueba que `apply` lo
limpia.

**VPS** (`contrib/`): `cengarde-vps-setup` escribe siempre la TUN (`cg0`,
`10.79.0.1/16`, un `tun_route` por router) y sin `wireguard*`; `tunnel
wireguard` se niega; `cengarde-nat` siempre usa `cg0` y pierde las reglas de
los puertos de WireGuard; la unidad de systemd permite `/dev/net/tun` por sí
misma (antes lo hacía el drop-in `native.conf`).

**Laboratorio y CI** (`bench/`, `.github/`): `lab.sh` arma siempre nativo
(`udpgen` entre 10.79.0.2 y 10.79.0.1); salieron `wgpoke`, `wgport`,
`fakewg.py`, `protoclient.c`, `BOND_TUNNEL` y la comparación con el engarde
Go (`ENGINE=go`, `compare`, `suite`): engarde es un relevo de WireGuard y no
corre sobre el túnel nativo; sus números quedan en
[001](001-diagnostico-engarde-go.md) y [005](005-motor-c-v1.md).

## Cómo migra lo que ya está instalado

- **Router:** al instalar 0.12, `uci-defaults` borra `tunnel_mode`,
  `network.wgcg` y su peer `network.cengarde_vps` (idempotente), así netifd
  no se encuentra con un protocolo que ya no tiene. El primer `apply` saca
  `wgcg` de la zona wan, mueve UPnP a `cg0` y crea `cg0` con la misma
  dirección, MTU, DNS y rutas. Un `tunnel_mode` que quede se ignora.
- **VPS:** `install.sh` instala la unidad nueva y corre `cengarde-vps-setup`,
  que escribe la configuración nativa y, con el motor ya arriba con ella,
  `drop_wireguard` borra lo que el propio script había creado: los
  `wg-quick@cg-NOMBRE` cuyo archivo empieza con «# Written by
  cengarde-vps-setup» (un WireGuard del administrador no se toca),
  `/var/lib/cengarde/wg`, el drop-in `native.conf` y `TUNNEL=` /
  `WG_PORT_BASE=` de `nat.conf`. En corridas siguientes no hace nada.
- **Una configuración a mano:** el motor no arranca con una clave de
  WireGuard; el error dice la línea, la clave y que hay que quitarla. El
  cliente necesita `tun_addr` (o que el sistema ponga las direcciones), y
  cada `[client NOMBRE]` del servidor, su `tun_route`.

## Qué hacemos con esto

- Un solo modo que probar: laboratorio, e2e de OpenWrt, `vps.yml` y LuCI
  cubren todos el túnel nativo.
- El cifrado, si alguna vez hace falta, va por dentro (una VPN del usuario
  sobre `cg0`) o como opción del propio túnel nativo
  ([014](014-tunel-nativo.md), «cifrado opcional»), nunca como un segundo
  modo de transporte.

## Pendiente

- El tema EVM de LuCI (`~/Claude/evm-openwrt`, fuera de GitHub) todavía
  muestra «WireGuard» cuando `tunnel` no es `native`
  (`luci-mod-evm/.../view/evm/tunel.js:354`; con 0.12 nunca pasa) y en
  cuatro capturas de `docs/capturas/evm_tunel-*.png`; se arregla en ese
  repositorio.
- Medir en hardware real la migración de un router con WireGuard (la probó
  el e2e en QEMU).

## Cambios

- 2026-10-26: creada.
