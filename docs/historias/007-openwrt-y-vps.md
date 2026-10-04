# 007 — Paquete para OpenWrt, plantilla del VPS y SmoothWAN como referencia

- **Fecha:** 2026-10-03 (actualizada el 2026-10-04 con la historia 010)
- **Estado:** vigente
- **Fuentes:**
  - `openwrt/cengarde/`, `openwrt/README.md`,
    `.github/workflows/openwrt.yml`, `contrib/vps/`, `contrib/systemd/`;
  - los SDK oficiales 24.10.8 y 25.12.5 de `downloads.openwrt.org`;
  - el cloud-config de SmoothWAN para Ubuntu 22.04 en Vultr (aportado por el
    usuario) y smoothwan.com (`engarde/`, `features/`, `tinyfec/`).

## TL;DR

- **Paquete `cengarde` 0.2.0** (0.2 = protocolo v2), compilado con los SDK
  oficiales:
  - versiones: 25.12.5 (`.apk`, gcc 14.3) y 24.10.8 (`.ipk`, gcc 13.3);
  - destinos: la Pi 4 (`aarch64_cortex-a72`) y x86_64;
  - ~32 KB comprimido, binario de 65 KB, solo depende de `libc`;
  - trae servicio procd y una plantilla de cliente.
- **Validado con la libc musl de OpenWrt:**
  - 208.249 checks en aarch64 (bajo qemu) y en x86_64;
  - `smoke` y `health` del laboratorio con el binario x86_64 de OpenWrt, con
    las mismas cifras que con glibc;
  - el CI lo repite y publica los paquetes como artefactos.
- **VPS:** plantilla cloud-init para Ubuntu (Vultr).
  - Compila cengarde, deja WireGuard detrás de él, lo ejecuta con un servicio
    systemd sin privilegios y monta el NAT.
  - Reenvía los puertos 1024–65000 al router (el «IP pass» de SmoothWAN).
  - Las reglas de NAT y reenvío están probadas en namespaces.
- **Sin probar todavía** en un router ni en un VPS reales. La UCI, la LuCI
  y el emparejamiento con un secreto llegaron después: historia 008.
- **SmoothWAN marca qué tiene que hacer la LuCI:**
  - emparejar con el VPS desde el router;
  - IP pass con UPnP;
  - elegir las interfaces activas.

  Su propia documentación dice que engarde Go necesita CPU de 3 GHz en router
  y servidor para pasar de 100 Mbit/s.

## Contexto

Tras la Fase 2, el usuario pidió compilar para probar en un OpenWrt limpio:
no SmoothWAN, abandonado y con kernel 5.x (historia 004). Después compartió
el cloud-config que SmoothWAN usa para el VPS y pidió dos cosas:
- reenviar puertos (DNAT) con UPnP en el router, para tener la IP pública
  del VPS en terreno y apuntar servicios a él;
- anotar una pestaña de LuCI que lo configure todo: IP pass sí/no, creación
  automática del túnel de WireGuard y conexión de interfaces.

## Decisiones

### Paquete (`openwrt/cengarde/`)

- **Fuentes:** el `Makefile` del paquete copia `../../engine` en vez de bajar
  un tarball. Se enlaza con un symlink en `package/` del SDK, y como `make`
  resuelve el directorio real, la ruta relativa funciona.
- **Versión:** `PKG_VERSION 0.2.0` sigue al protocolo; el binario muestra
  `0.2.0-r1-g<commit>`.
- **Flags:** se usan las del SDK, incluido su hardening; se anulan las del
  motor con `HARDEN=`.
- **Configuración:** INI, sin UCI, con la plantilla
  `engine/examples/client.conf` como conffile 0600 y el estado en
  `/var/run/cengarde.json` (tmpfs).
  - Desde la versión 0.3 es UCI (`/etc/config/cengarde`) y el INI se genera
    (historia 008).
- **Servicio procd:**
  - se reinicia cada 5 s para siempre;
  - no arranca mientras la clave sea la de la plantilla;
  - en `server` va mejor la IP del VPS que un nombre, porque al arrancar no
    hay DNS: va por el túnel.
- **Prioridad:** primero 25.12 con `apk`, porque es la estable; 24.10 con
  `opkg` sigue soportada.

### Validación sin hardware

- **aarch64:** `qemu-aarch64 -L <toolchain del SDK>` ejecuta el binario del
  paquete y los tests compilados con ese toolchain.
- **x86_64:** se ejecuta a través de su cargador:
  `ld-musl-x86_64.so.1 --library-path <toolchain>/lib`.
- **Laboratorio:** acepta cualquier binario con `CENGARDE_BIN`, como un
  envoltorio de ese cargador.

### VPS (`contrib/vps/`, `contrib/systemd/`)

- **Persistente:** usa servicios de systemd y `wg-quick`, no un `runcmd` que
  se repite en cada arranque como hace SmoothWAN.
- **Puertos:**
  - cengarde en 65500/UDP;
  - WireGuard en 65501/UDP, accesible solo desde la propia máquina (se
    descarta desde fuera);
    - *Corrección 2026-10-04:* el DROP solo cubría la interfaz pública y
      solo IPv4: desde el túnel, y por IPv6 si el VPS la tiene, se llegaba.
      Desde 0.4.1 se descarta en todo lo que no sea `lo`, en IPv4 e IPv6
      (historia 010);
  - los dos quedan fuera del rango reenviado, 1024–65000.
- **Claves:** aleatorias, generadas en el router; cuatro valores se pegan en
  la plantilla.
  - SmoothWAN deriva todo de una contraseña de 8 caracteres
    (sha256 + base64). Es cómodo, pero con un handshake capturado se puede
    atacar por fuerza bruta sin conexión: el `mac1` depende de la clave
    pública del servidor, así que basta probar ~65⁸ ≈ 3·10¹⁴ contraseñas.
  - Plan: un solo secreto de 256 bits (`cengarde genkey`) del que la LuCI
    derive todo. Hecho con BLAKE2s (historia 008).
- **systemd:**
  - `DynamicUser` con la configuración pasada como credencial
    (`LoadCredential`), así que el archivo sigue siendo solo de root;
  - capacidades `NET_ADMIN`, `NET_RAW` y `SYS_NICE`, y sandbox;
  - necesita systemd 248 o posterior; `systemd-analyze verify` no da avisos.
- **No desactiva SSH ni el cortafuegos** (SmoothWAN sí): sus reglas van
  delante de las de ufw.
- **IP pass** (`cengarde-nat`):
  - DNAT de TCP/UDP 1024–65000 al router, conservando la IP de origen;
  - masquerade a la salida;
  - se activa o desactiva en `nat.conf` (desde la historia 009 lo pide el
    router, y `nat.conf` solo fija el valor inicial).

  Probado en namespaces (un veth en lugar de wg0):
  - entra TCP 8080 y UDP 30000 con su origen real, y 65500 se queda en el VPS;
  - el router sale con la IP del VPS;
  - `down` no deja reglas.

### Router (`openwrt/README.md`)

- **Uplinks:** uno por VLAN (`eth1.<vid>`), por DHCP, cada uno con **su
  métrica**. Con dos rutas por defecto de igual métrica, netifd deja una sola
  (observado en la VM de la historia 008: queda la del último en subir), y
  los demás enlaces se quedan sin camino al VPS.
- **DNS:** `peerdns 0` en los uplinks, porque muchas operadoras solo
  contestan a sus clientes, y DNS públicos en wg0.
- **WireGuard:** endpoint `127.0.0.1:59401`, MTU 1380 y
  `allowed_ips 0.0.0.0/0` con ruta.
- **UPnP:** miniupnpd con `external_iface wg0` y `external_ip` (la IP del
  VPS); las opciones están en la documentación oficial de OpenWrt.

## SmoothWAN como referencia

**Qué hace**, según la web y el cloud-config:
- **Cliente:** en LuCI se pone la IP del servidor y una contraseña, y se
  activa; usa las *Active Interfaces*.
- **Servidor:**
  - cloud-init, solo en KVM; aconsejan Vultr *High Frequency* porque engarde
    usa un solo hilo;
  - reenvía los puertos 1024–65000 y configura UPnP;
  - WireGuard en 65532 con MTU 1280, buffers de 25 MB y `netdev_max_backlog`
    de 2048;
  - desactiva SSH, ufw y firewalld, y lo rehace todo en cada arranque.
- **Límites declarados por ellos:**
  - «3.0Ghz Intel/AMD router and server for >100Mbit speeds»;
  - la velocidad es la del WAN más rápido;
  - conmutación sin pérdidas en ~1 ms.

**Otras funciones que conviene copiar:**
- nombres fijos para los dispositivos de red USB, que conservan la cuota y
  las estadísticas de cada módem;
- login de portal cautivo por WAN;
- desviar clientes concretos a un WAN;
- TinyFEC: FEC adaptativo para un solo WAN con pérdidas, idea para la Fase 5.

## Qué hacemos con esto

**Pestaña de LuCI** (Fase 3), lo pedido por el usuario:
- estado por enlace, leído del JSON;
- elegir los uplinks y sus etiquetas;
- emparejar con el VPS con un solo secreto;
- crear y actualizar wg0 automáticamente;
- IP pass sí/no y UPnP sí/no.

**Pregunta:** cómo activa el router el IP pass, que vive en el VPS.
Opciones:
- un mensaje de control autenticado en el protocolo, con el servidor
  aplicando nftables;
- un agente en el VPS;
- dejarlo en el VPS y mostrar la instrucción.

**Decidido** (historia 008): por ahora queda en el VPS. El cloud-config que
entrega LuCI sigue al interruptor; el mensaje de control queda pendiente.

## Pendiente

- **Hardware real:** probar en la Pi 4 con OpenWrt 25.12 y en Vultr, y medir
  CPU por paquete en la Pi con el mismo método que el laboratorio.
- ~~UCI y LuCI~~: hechas (historia 008).
- **Feed firmado**, para instalar con `apk add` sin `--allow-untrusted`.
- **Binarios estáticos** (musl) para Linux sin OpenWrt: Raspberry Pi OS u
  otras distribuciones.
- **DNS del servidor:** volver a resolver su nombre (DNS dinámico) y poder
  arrancar sin DNS.
- **IPv6** en la plantilla del VPS.

## Cambios

- 2026-10-03: creada.
- 2026-10-03: UCI, LuCI y emparejamiento con un secreto (historia 008).
  - La pregunta del IP pass queda decidida.
  - La métrica repetida: netifd deja una sola ruta por defecto, no las
    rechaza.
- 2026-10-04: el IP pass lo gobierna el router y procd manda `SIGHUP` en
  vez de reiniciar el motor (historia 009).
- 2026-10-04: el puerto de WireGuard del VPS no estaba cerrado al túnel ni
  a IPv6 (historia 010).
