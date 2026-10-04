# 008 — UCI, LuCI, emparejamiento con un secreto, imágenes y prueba en QEMU

- **Fecha:** 2026-10-03 (actualizada el 2026-10-04 con la historia 009)
- **Estado:** vigente
- **Fuentes:**
  - código:
    - `engine/src/blake2s.c`, `pair.c` y `main.c` (`cengarde keys`);
    - `openwrt/cengarde/files/`, `openwrt/luci-app-cengarde/` y
      `openwrt/test/`;
    - `contrib/vps/` y `.github/workflows/openwrt.yml`;
  - de OpenWrt:
    - `luci.mk`, `form.js` y `ui.js` del feed LuCI que fijan los SDK 25.12.5
      (commit `128a781`) y 24.10.8 (`cac97ed`);
    - `wireguard.sh` de netifd (25.12 `f0a60ee`, `v24.10.8`);
    - `miniupnpd.init` del feed packages (`5caa62e`);
  - RFC 7693 (BLAKE2).

## TL;DR

- **Un secreto por router:** 32 bytes aleatorios, generados en el primer
  arranque. De él salen todas las claves, con BLAKE2s-256 con clave y una
  etiqueta por clave:
  - la del enlace de cengarde;
  - las privadas de WireGuard de los dos extremos;
  - la PSK.

  `cengarde keys` las imprime, y el VPS solo recibe el secreto, dentro de su
  cloud-config.
- **UCI y `cengarde-setup`:** `/etc/config/cengarde` más un script
  idempotente que mantiene en la configuración del router:
  - la interfaz `wgcg`;
  - las rutas `0.0.0.0/1` y `128.0.0.0/1`;
  - una métrica por enlace, `peerdns` y la zona `wan`;
  - UPnP para el IP pass.

  Desactivar o desinstalar lo deshace.
- **luci-app-cengarde:**
  - *Estado*: cada 3 s, con avisos;
  - *Configuración*: General, VPS (secreto y cloud-config listo para copiar),
    Túnel (IP pass) y Avanzado.

  En español con `luci-i18n-cengarde-es`. Compila en ~15 s con el SDK,
  trayendo del feed de LuCI solo `luci-base`.
- **Imágenes de 25.12.5** con el ImageBuilder: Pi 4 (~17 MB) y x86-64
  (~14 MB), con LuCI, cengarde, WireGuard, UPnP y drivers USB Ethernet. Un
  tag `v*` las publica.
- **Prueba de punta a punta en QEMU** (`openwrt/test/e2e.sh`): router y
  «VPS» OpenWrt con tres enlaces. La configuración se hace solo desde LuCI,
  con Playwright.
  - **Resultado:** verde, y sin pérdidas con un enlace caído.
  - **Encontró:** tres bugs propios (ya corregidos), un límite del
    servidor en VPS con varias IP y uno de miniupnpd con IP reservadas.
- **Falta** la Pi 4 y un VPS reales.

## Contexto

El usuario pidió «la UCI y la pestaña LuCI y todo lo necesario para que sea
aún más amigable, instalar y probar». Es el pendiente de la historia 007,
inspirado en SmoothWAN:
- emparejar con un secreto;
- túnel automático;
- elegir los enlaces;
- IP pass sí/no.

## Decisiones

### Emparejamiento: un secreto, todas las claves

**Derivación:**

```
clave(etiqueta) = BLAKE2s-256(clave = secreto, "cengarde pairing v1: " + etiqueta)
```

- Etiquetas: `link`, `wg-server`, `wg-client` y `wg-psk`.
- Las privadas de WireGuard salen recortadas («clamped»), como las de
  `wg genkey`.

**Por qué BLAKE2s:**
- es la función hash del propio WireGuard;
- ~130 líneas sin dependencias;
- el modo con clave es un PRF;
- vectores verificados:
  - RFC 7693 («abc»);
  - el KAT con clave de la implementación de referencia (clave `00..1f`);
  - `hashlib` para el resto;
  - pasa en big-endian (MIPS bajo qemu).

**Frente a SmoothWAN** (8 caracteres → sha256, historia 007): un secreto
de 256 bits no se puede atacar por fuerza bruta a partir de un handshake
capturado.

**Qué implica:**
- Quien tenga el secreto puede hacerse pasar por cualquiera de los dos
  extremos, como ya pasaba con la clave compartida de cengarde.
- WireGuard conserva el secreto hacia adelante gracias a sus claves
  efímeras.
- El secreto viaja en el user data de Vultr, que se ve en su panel y desde
  el servicio de metadatos del VPS.
- Para rotarlo: *Generar* en LuCI y luego `/etc/cengarde/secret` más
  `cengarde-vps-setup` en el VPS.

**Uso:** `cengarde keys` lee el secreto por la entrada estándar, para que no
salga en `ps`, e imprime asignaciones de shell. Lo usan `cengarde-setup` y
`cengarde-vps-setup`.

### Router: `/etc/config/cengarde` y `cengarde-setup`

**Esquema propio**, no el de `openwrt-engarde` como decía el ROADMAP: el
protocolo v2 no es compatible y no hay configuración que migrar.

**Opciones** (`files/cengarde.config`):
- básico: `enabled`, `secret`, `server`, `port` y la lista `uplink` (nombres
  de interfaces lógicas);
- túnel: `tunnel`, `route_all`, `mtu`, `dns` e `ip_pass`;
- las perillas del motor, y `config_file` para usar un INI propio.

**El init** (procd): en cada `start` o `reload` ejecuta
`cengarde-setup apply`, genera `/var/etc/cengarde.conf` y registra la
instancia con `procd_set_param file`. Si esa configuración cambió, procd
manda `SIGHUP` y el motor la aplica sin cortar (historia 009; antes la
reiniciaba).
- **Disparadores:** `cengarde` (lo que aplica LuCI) y `interface.*` de cada
  enlace. El dispositivo de un módem (`wwan0`) solo se conoce cuando la
  interfaz está arriba.

**Rutas:**
- **`0.0.0.0/1` y `128.0.0.0/1` por `wgcg`, no `0.0.0.0/0`:** son más
  específicas que cualquier ruta por defecto, así que ganan sin borrarlas.
- **Los enlaces conservan sus rutas por defecto:** los sockets de cengarde
  van atados a cada enlace (`SO_BINDTODEVICE` más `connect`). El kernel se
  salta las rutas /1, que salen por otra interfaz, y cae en la ruta por
  defecto del enlace.
- **Verificado en la VM:** las tres rutas por defecto (métricas 10/20/30) y
  las dos /1 conviven, y cada enlace llega al VPS.

**Métrica única por enlace:**
- **Observado en la VM:** con tres enlaces DHCP de métrica 0, netifd deja
  una sola ruta por defecto, la del último que sube; los otros quedan sin
  camino al VPS.
- **Regla:** los que no tienen métrica, tienen 0 o repiten la de otra
  interfaz reciben la primera libre desde 10, de diez en diez.
- **No se deshace:** no estorba.

**DNS:**
- `peerdns 0` en los enlaces, y `dns` (1.1.1.1 y 9.9.9.9) en `wgcg`;
- solo mientras el túnel está arriba;
- una marca `cengarde_peerdns` permite deshacer exactamente lo que se cambió.

**Guardas:**
- **Sin `server` o sin enlaces, no se crea el túnel:** sus rutas se llevarían
  todo el tráfico a ninguna parte.
- **El servidor tiene que ser una IP:** con todo el tráfico por el túnel, un
  nombre no se puede resolver antes de que el túnel exista. LuCI lo valida
  como `ipaddr`.

**Cortafuegos:** `wgcg`, y los enlaces que no tengan zona, en `wan`.

**IP pass en el router:**
- miniupnpd con `external_iface wgcg` y `external_ip` igual a la IP del VPS;
- si no, con STUN;
- miniupnpd acepta una IP declarada aunque la interfaz tenga una privada.

**Al quitar el paquete:** el `prerm` ejecuta `cengarde-setup disable`.

### LuCI (`luci-app-cengarde`)

**Configuración:**
- un `form.Map` sobre la sección `main`, con `widgets.NetworkSelect` para los
  enlaces;
- **secreto:** campo con el botón *Generar* (ejecuta `cengarde genkey`; ACL
  de escritura);
- **cloud-config:** se arma en el navegador desde
  `/usr/share/cengarde/cloud-config.yaml` con los valores del formulario,
  aunque no se hayan guardado;
- **Copiar:** con `execCommand`, porque LuCI por `http` no es contexto seguro
  para el portapapeles;
- **Descargar:** como un Blob.

**Estado:** un solo `exec` por sondeo (`cengarde-setup status`), que trae:
- los avisos, como códigos que traduce el JS;
- el último handshake de WireGuard;
- el JSON del motor.

**Compilación:**
- con `luci.mk`, teniendo en el árbol del SDK solo `luci-base`, que aporta
  `po2lmo` y `jsmin`;
- las dependencias de ejecución (`wireguard-tools`, `luci-proto-wireguard`)
  no están en el árbol: entran en *Depends* sin compilarse;
- con el feed completo, `feeds install` arrastraba gettext, libxml2 y
  elfutils.

**Traducción:** `po/es/cengarde.po`. `i18n.py` lo sincroniza con las vistas,
y el CI falla si falta alguna. LuCI elige el idioma según el navegador.

### VPS

- **cloud-config:**
  - solo lleva el secreto y `nat.conf`;
  - baja el commit del paquete (`CENGARDE_REF`, que el paquete rellena al
    compilarse) y ejecuta `contrib/vps/install.sh`.
- **Mismo commit en los dos extremos:** garantiza el mismo protocolo
  (`CG_PROTO_VERSION`).
- **`cengarde-vps-setup`:** deriva las claves y escribe `cengarde.conf` y
  `wg0.conf`. Probado con un secreto de prueba: las claves coinciden con los
  vectores y la configuración pasa `cengarde -t`.
- **`cengarde-nat`:** nunca reenvía el puerto de cengarde, el de WireGuard
  ni el SSH, aunque caigan dentro del rango (reglas `RETURN`). Probado en
  namespaces con el rango 1:65535 y cengarde en el 3000.
- **Quién activa el IP pass del VPS** (la pregunta abierta de la 007): al
  principio, el VPS.
  - El cloud-config sigue al interruptor de LuCI.
  - Desde la historia 009 lo pide el router por cengarde, y `nat.conf` solo
    fija el valor hasta el primer pedido.

### Imágenes

- **ImageBuilder 25.12.5:** perfiles `rpi-4` y `generic`. Los paquetes
  propios van en `packages/`.
- **Lista de paquetes:** en `IMAGE_PACKAGES` del workflow.
- **CI:**
  - publica las imágenes como artefactos, y como release con un tag `v*`;
  - también arma la x86-64 de 24.10.8, solo para la prueba.

### Prueba de punta a punta (`openwrt/test/`)

**`vm.sh`:**
- dos VMs x86-64;
- la LAN de cada una por la red de usuario de QEMU, con LuCI y SSH en
  localhost;
- tres enlaces punto a punto por `-netdev dgram`.

**El «VPS»** es un OpenWrt con:
- los extremos de los enlaces y DHCP;
- `1.2.3.4` en `lo`: miniupnpd rechaza como externa una dirección de un
  rango reservado, como `203.0.113.0/24`;
- WireGuard por UCI;
- el servidor cengarde por `config_file`.

**`luci.mjs`** (Playwright, Chromium en `es-CL`), solo desde la interfaz:
1. pone la IP del VPS, elige los enlaces y marca *Activado*;
2. verifica que el cloud-config lleve el secreto y el commit, y que siga a
   *IP pass* antes de guardar;
3. *Guardar y aplicar*: espera el evento `uci-applied` de LuCI, porque
   navegar antes provoca un rollback;
4. espera en *Estado* «conectado» y tres enlaces «activo»;
5. falla ante cualquier error de JavaScript.

**`e2e.sh`** comprueba además:
- sin dirección del VPS no hay túnel;
- miniupnpd corre con `ext_ifname=wgcg` y la IP del VPS;
- 12 pings sin pérdida mientras un enlace se cae 4 s;
- al desactivar no queda nada del túnel y los DNS de los enlaces vuelven.

**Tiempos:** unos 2,5 min sin KVM y ~7 ms de RTT a través del túnel, en
emulación. En el CI va con KVM.

**Lo que encontró:**
1. `lookupOption` devuelve `null` antes de que el formulario esté en el
   DOM, y la página se rompía.
2. `String.replace` cambiaba el `REPLACE_SECRET` del comentario y no el del
   contenido: el VPS habría quedado sin secreto. Ahora se sustituye la línea
   que lo contiene.
3. El aviso «¡Sin contraseña!» de LuCI comparte la clase de los avisos
   propios.
4. **Límite del motor:** con varias IP en el VPS, el servidor (socket en
   `*:65500`) contesta desde la dirección de la interfaz de salida. El
   router descarta esas respuestas porque su socket está conectado a la IP
   pública.
   - Solución por ahora: `listen` en la IP pública, como ya decía
     `server.conf`.
   - Pendiente: responder desde la dirección de destino (`IP_PKTINFO`).
5. **Límite de miniupnpd 2.3.9:** no arranca si `ext_ip` es privada o
   reservada (`option ext_ip contains reserved / private address`). Con un
   VPS real no pasa, pero un «VPS» de laboratorio necesita una IP pública,
   aunque sea ficticia.

## Pendiente

- **Hardware real:** la Pi 4 con la imagen, un VPS en Vultr con el
  cloud-config, y medir CPU en la Pi.
- **Servidor:** responder desde la dirección por la que llegó el paquete
  (`IP_PKTINFO`/`IPV6_PKTINFO` en `sendmmsg`).
- **IPv6 dentro del túnel.**
- **Feed firmado:** `apk add` sin `--allow-untrusted`.
- **Probar también la imagen de la Pi en QEMU** (`raspi4b`, desde QEMU 9).
- **Etiquetas por enlace en LuCI:** hoy son el nombre de la interfaz.

## Cambios

- 2026-10-03: creada.
- 2026-10-04: el IP pass pedido por el router y el `SIGHUP` de procd quedan
  hechos (historia 009).
