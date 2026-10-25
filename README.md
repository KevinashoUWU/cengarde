# cengarde

*Redundant multi-link transport for WireGuard: a C rewrite of engarde for
OpenWrt routers. Documentation in Spanish.*

**WireGuard por varios enlaces a la vez, sin perder un paquete.**
- cengarde manda cada paquete de un túnel WireGuard por todos tus enlaces a
  la vez (módems 4G/5G, fibra, satelital…) hacia un VPS.
- En el otro extremo gana la primera copia que llega. Si un enlace se corta
  o se atrasa, la conexión sigue como si nada.

Nació como fork de [engarde](https://github.com/porech/engarde) y hoy es
un motor nuevo en C con protocolo propio, pensado para routers OpenWrt (una Raspberry Pi 4, un PC x86) y configurable
entera desde LuCI.

```
                         ┌══ enlace 1 (5G) ══┐
LAN ── router OpenWrt ───┼══ enlace 2 (5G) ══┼─── VPS ─── Internet
       WireGuard         └══ enlace 3 (…)  ══┘    cengarde → WireGuard
       └→ cengarde            cada paquete por todos; gana la primera copia
```

## Qué hace

- **Redundancia sin cortes:** cada paquete sale por todos los enlaces
  activos, en los dos sentidos. Un enlace caído no hace perder nada mientras
  quede otro.
- **Aparta los enlaces atrasados:**
  - un enlace que queda más de 150 ms por detrás del más rápido (por
    ejemplo, un módem con la cola llena) deja de llevar datos hasta que se
    recupera;
  - sigue sondeándose y no corta el túnel;
  - siempre quedan al menos dos enlaces activos.
- **Reordena lo que recibe:** cada extremo pone en secuencia los paquetes
  antes de entregarlos a WireGuard (activado), con ventanas que cubren
  ~600 ms a ~27.000 paquetes por segundo. Con redundancia pura no retiene
  nada.
- **Experimental, apagado por omisión:**
  - **repartir** los paquetes grandes entre los enlaces en vez de copiarlos
    por todos (`split`);
  - **el rescate:** si el reordenador va a dar un paquete por perdido, lo
    pide al otro extremo (un NACK, por los dos enlaces más rápidos, con
    reintentos), que lo reenvía por su camino más rápido, en la bajada y en
    la subida. No tapa la congestión (esa pérdida es la que hace frenar a
    TCP) y sirve sobre todo con tráfico de tasa fija (`rescue_wait_ms`,
    `rescue_kb`; ver [`engine/README.md`](engine/README.md));
  - plazos del reordenador por enlace (`reorder_by_link`) y adaptativos
    (`reorder_adaptive`).
- **Arranque rápido de la capacidad:** cada enlace recuerda su mejor
  capacidad de los últimos 5 min y, tras 10 s sin llenarse, vuelve al menos
  a la mitad de ella, para que un enlace en reposo recupere su parte sin
  esperar segundos.
- **Seguro:**
  - protocolo propio autenticado (SipHash-2-4) con ventana anti-replay;
  - el servidor solo atiende paquetes autenticados;
  - WireGuard cifra el contenido.
- **Liviano:** C11 sin dependencias, un hilo con epoll y lotes
  (`recvmmsg`/`sendmmsg`). El binario ocupa unos 110 KB.
- **OpenWrt de punta a punta:**
  - paquetes para 25.12 y 24.10, e imágenes listas para la Pi 4 y x86-64;
  - una pestaña de LuCI en español;
  - el túnel WireGuard se crea solo, a partir de un único secreto compartido
    con el VPS;
  - **IP pass**: la IP pública del VPS en tu red, con UPnP. Se enciende y se
    apaga desde el router, y el VPS lo sigue solo.
- **El VPS en un paso:** LuCI entrega el cloud-config listo para pegar al
  crear el VPS (Ubuntu, por ejemplo en Vultr).
- **Estado en vivo:** RTT, atraso y qué enlace entrega primero cada paquete,
  en LuCI, en JSON o con `cengarde ctl`.
- **Cambios sin cortar:** la configuración se aplica en marcha, con la misma
  sesión, y un enlace se puede pausar y reanudar desde LuCI o con
  `cengarde ctl link NOMBRE off`.
- **IPv4 e IPv6 hasta el VPS:** el router acepta varias direcciones del VPS
  y cada enlace usa la primera que puede (por ejemplo, la IPv6 por
  Starlink), con failover a la siguiente. El VPS contesta desde la
  dirección a la que llegó cada paquete, así que sirve cualquier IP suya, y
  el prefijo IPv6 de un enlace no se escapa a la LAN por fuera del túnel.

## Medidas

En el laboratorio de [`bench/`](bench/): 3 enlaces, paquetes de 1400 B,
VM Xeon de 2,1 GHz, cada motor en ambos extremos (historias
[005](docs/historias/005-motor-c-v1.md) y
[006](docs/historias/006-salud-de-enlaces.md)).

| Prueba | engarde (Go) | cengarde |
| --- | --- | --- |
| CPU del cliente, bajada a 10.000 paquetes/s | 91–94 µs por paquete | 13–14 µs por paquete |
| Pérdida en subida a 60.000 paquetes/s (~670 Mbit/s) | 34–38 % | 0 % |
| Pérdida con un enlace lento (5 Mbit/s) bajo carga | hasta 77 % | 0 % |

- Un enlace con 500 ms de cola se aparta en 3–3,6 s, sin que el túnel pierda
  nada.
- **El rescate, en el laboratorio** (3 enlaces con 1 % de pérdida al azar
  cada uno, 4 flujos Cubic de bajada, 3 rondas): de 10–11 a ~50 Mbit/s; donde
  la pérdida es de un enlace lleno, cuesta ~15 %. En la red de pruebas, con
  16 flujos de bajada saturando, no mejoró (86,4 contra 91,0 Mbit/s): la
  pérdida bajo carga es casi toda de colas llenas, no de radio (historia
  [013](docs/historias/013-capacidad-y-dosificacion.md)).
- **Falta medir en la Pi 4.** Con engarde, el usuario la vio quedarse en
  20–30 Mbit/s con 4 enlaces, por CPU
  ([historia 004](docs/historias/004-entorno-real.md)).

## Empezar

### En OpenWrt (lo recomendado)

1. **Instala:** graba la imagen (Pi 4 o x86-64) o instala los paquetes en tu
   OpenWrt.
2. **Crea los enlaces:** una interfaz por enlace (DHCP en cada VLAN o
   módem).
3. **Crea el VPS:** en **Servicios → cengarde → VPS**, copia el cloud-config
   y pégalo como *user data* al crear el VPS.
4. **Activa:** pon la IP del VPS (o varias, IPv4 e IPv6), elige los
   enlaces y activa.
5. **Comprueba:** mira la página **Estado**.

Guía completa, con las imágenes y los paquetes: [`openwrt/README.md`](openwrt/README.md).

### En otro Linux, a mano

```sh
make -C engine && sudo make -C engine install   # /usr/sbin/cengarde
cengarde genkey                                  # el secreto compartido
```

Las configuraciones de cliente y servidor se explican en
[`engine/README.md`](engine/README.md), y el VPS en [`contrib/vps/`](contrib/vps/).

## Estado

- **Motor:** listo lo esencial:
  - redundancia;
  - protocolo propio v5;
  - salud de los enlaces;
  - perillas de baja latencia;
  - recarga en caliente y socket de control (`cengarde ctl`);
  - FEC en lo que sobra (`redund_spare = rlc | rs`, historia 021), con lo
    rehecho revisado antes de entregarlo;
  - el protocontrolador, TCP partido en el router y el VPS (historia 025):
    A, un TCP simple en el medio; B, el protocolo GRIT (experimental).
- **OpenWrt:** paquetes, LuCI, imágenes y emparejamiento con un solo secreto.
  Se prueba de punta a punta en QEMU en cada cambio: dos VMs, configuradas
  desde LuCI.
- **Falta probarlo en hardware real:** la Pi 4 y un VPS en Vultr.
- **Compatibilidad:** los dos extremos tienen que ser cengarde con la misma
  versión del protocolo; no habla con engarde.

## Hoja de ruta

Lo que viene; el detalle está en [ROADMAP.md](ROADMAP.md):

- **IPv6, varios routers por VPS y nombres** (historia
  [010](docs/historias/010-ipv6-varias-ip-multicliente-nombres.md)): hechos
  los cinco PRs (los arreglos, la respuesta desde la dirección de llegada
  con IPv6 por fuera del túnel, varios routers por VPS con un panel de
  reenvío de puertos, IPv6 dentro del túnel apagado por defecto, y nombres
  con DNS dinámico, también para un servidor casero); falta probarlos en
  la Pi, en Vultr y con un servidor en casa.
- **Servidor:** varios hilos.
- **Distribución:** un feed de OpenWrt firmado, binarios estáticos y paquetes
  para Debian y Raspberry Pi OS.
- **eBPF/XDP opcional** para bajar la CPU en el VPS (Fase 4).
- **Más allá de la redundancia** (Fase 5):
  - usar solo los k mejores enlaces;
  - sumar enlaces (bonding);
  - FEC (las retransmisiones ya están, como el rescate experimental).

## Estructura

| Carpeta | Qué hay |
| --- | --- |
| [`engine/`](engine/) | el motor en C: fuentes, tests y ejemplos de configuración |
| [`openwrt/`](openwrt/) | los paquetes `cengarde` y `luci-app-cengarde`, la guía y la prueba en QEMU |
| [`contrib/`](contrib/) | el VPS: cloud-config, NAT y la unidad de systemd |
| [`bench/`](bench/) | el laboratorio de medidas (network namespaces) |
| [`docs/historias/`](docs/historias/) | investigación y decisiones |

## Desarrollo

```sh
make -C engine test                                    # tests unitarios
make -C engine SANITIZE=1 test                         # con ASan y UBSan
sudo bench/lab.sh build && sudo bench/lab.sh smoke     # laboratorio
openwrt/test/e2e.sh <imagen-x86-64>                    # OpenWrt de punta a punta, en QEMU
```

**El CI:**
- compila con gcc y clang (`-Werror`), y prueba en aarch64, armhf y MIPS
  big-endian;
- arma los paquetes y las imágenes de OpenWrt;
- corre la prueba de punta a punta;
- con un tag `v*`, publica todo como release.

## Créditos y licencia

cengarde nace de [engarde](https://github.com/porech/engarde), de
Alessandro Rinaldi. De engarde vienen la idea de duplicar sobre WireGuard y
años de uso real transmitiendo radio en vivo desde terreno.

El código Go original sigue en el historial de este repositorio (commit
`3492df9`), y el laboratorio lo compila desde ahí cuando hace falta
compararse.

Licencia: GPL-3.0 o posterior (`GPL-3.0-or-later`), [LICENSE.txt](LICENSE.txt).
Hasta el cambio de licencia, cengarde se publicó bajo GPL-2.0, como
engarde. Del código de engarde no queda nada en el árbol: el motor, el
protocolo, los paquetes de OpenWrt y LuCI, el laboratorio y la
documentación son trabajo nuevo de este proyecto. La historia de git
conserva los commits de engarde con su licencia original.
