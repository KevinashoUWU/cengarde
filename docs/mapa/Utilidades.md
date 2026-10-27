---
tags: [mapa, pieza]
archivos:
  - engine/src/siphash.h
  - engine/src/blake2s.h
  - engine/src/util.h
  - engine/src/hist.h
  - engine/src/idmap.h
  - engine/src/ring.h
  - engine/src/sock.h
  - engine/src/netlink.h
  - engine/src/names.h
  - engine/src/dns.h
  - engine/src/tun.h
  - engine/src/ippkt.h
  - engine/src/pair.h
  - engine/src/status.h
  - engine/src/ctl.h
  - engine/src/log.h
  - engine/src/engine.h
  - engine/src/main.c
  - engine/src/os.c
sello: f592651a70209553
---
# Utilidades

**Grupo:** Utilidades — [[Mapa del motor]]

Cabeceras sin lógica de decisión ni medida propia. Una línea cada una.

- `siphash.h` / `siphash.c`: SipHash-2-4, el MAC de cada datagrama y las cookies ([[Cookies y HELLO (cookie.h)]]).
- `blake2s.h` / `blake2s.c`: BLAKE2s; deriva todas las claves de un solo secreto (`pair.h`), fuera del plano de datos ([[008-luci-uci-y-emparejamiento]]).
- `pair.h` / `pair.c`: emparejamiento con un solo secreto: clave de cengarde, claves de WireGuard y direcciones del túnel.
- `util.h` / `util.c`: utilidades generales (listas, base64, direcciones, relojes).
- `hist.h`: histogramas log2 de duraciones chicas (los saltos entre hilos del router, [[Hilos por enlace (thrplan.h)]]).
- `idmap.h`: mapa de id de sesión a índice ([[Varios clientes (clients.h)]]).
- `ring.h`: el anillo SPSC entre hilos de datos ([[Hilos por enlace (thrplan.h)]]).
- `pump.h` / `pump.c`: las bombas de los enlaces; solo E/S ([[Hilos por enlace (thrplan.h)]]).
- `sock.h` / `sock.c`: sockets UDP no bloqueantes y sus búferes.
- `netlink.h` / `netlink.c`: interfaces y direcciones por rtnetlink ([[Dirección de origen (addrpick.h)]]).
- `names.h` / `names.c`: los sockets de las búsquedas DNS ([[Nombres del servidor (resolve.h)]]).
- `dns.h`: el códec DNS, con lectura acotada de entrada no confiable.
- `tun.h` / `tun.c`, `ippkt.h`: la TUN del túnel nativo y la forma de un paquete IP ([[Rutas del túnel nativo (tunroute.h)]]).
- `status.h` / `status.c`: el JSON de estado, escrito atómico (`sig` por enlace: [[Observaciones del enlace (linksig.h)]]; `tcp_e2e`: [[Liberación TCP (tcpfree.h)]]; `rule` y `tcp_rule`: [[Retransmisión según la evidencia (tcpack.h)]]; `active`: [[Enlaces activos (active.h)]]).
- `ctl.h` / `ctl.c`: el socket de control (`cengarde ctl`).
- `log.h` / `log.c`: el registro.
- `engine.h`: piezas comunes de los bucles; traduce la config a `cg_hcfg` ([[Salud y silenciado (health.h)]]) y `cg_ro_cfg` ([[Reordenador (reorder.h)]]); las etiquetas de epoll (`CG_EV_PCTL`: los sockets de [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]).
- `os.c`, `main.c`, `proto.c`: arranque, CPU y prioridad, y el códec del protocolo ([[Sondas y cola de bonding (proto.h)]]).
