# cengarde en OpenWrt (y su VPS)

Un router OpenWrt (la Pi 4, un PC x86) que suma varios enlaces y un VPS
que recibe. Todo se configura desde LuCI, en **Servicios → cengarde**, y
ambos extremos comparten un solo secreto, del que salen todas las claves.

**Estado:** probado de punta a punta en QEMU con dos VMs OpenWrt (router y
un «VPS»):
- configuración solo desde LuCI, con Playwright;
- túnel arriba por tres enlaces, sin pérdidas al caer uno;
- cambios aplicados sin reiniciar el motor ni perder pings, e IP pass
  encendido y apagado desde el router;
- pausar y reanudar un enlace desde la página de estado;
- desactivar deja el router como estaba.

El CI repite la prueba en cada cambio (`test/e2e.sh`). Falta probarlo en la
Pi 4 y en un VPS reales. Decisiones en las historias
[007](../docs/historias/007-openwrt-y-vps.md),
[008](../docs/historias/008-luci-uci-y-emparejamiento.md) y
[009](../docs/historias/009-recarga-control-e-ip-pass.md).

```
LAN ── router OpenWrt ══ enlace 1 ══╗
       WireGuard wgcg   ══ enlace 2 ══╬══ VPS: cengarde → WireGuard wg0 → Internet
       └→ cengarde      ══ enlace 3 ══╝   (cada paquete por todos; gana la primera copia)
```

## En cinco pasos

1. **Instala** la imagen lista o los paquetes (sección 1).
2. **Crea una interfaz por enlace** (módem o VLAN), con DHCP (sección 2).
3. **Crea el VPS** pegando el cloud-config que muestra LuCI (sección 3).
4. **Activa:** en LuCI, la IP del VPS, los enlaces y *Activado*, y luego
   *Guardar y aplicar* (sección 4).
5. **Mira** *Servicios → cengarde → Estado* (sección 5).

## 1. Instalar

### Imagen lista (lo más fácil)

En GitHub: *Releases* (cuando haya un tag), o *Actions → OpenWrt* → la
ejecución más reciente → *Artifacts*.

| Artefacto | Para | Archivo |
| --- | --- | --- |
| `cengarde-firmware-25.12.5-rpi-4` | Raspberry Pi 4 / 400 / CM4 | `…-rpi-4-ext4-factory.img.gz` a la microSD |
| `cengarde-firmware-25.12.5-generic` | PC x86-64 (p. ej. el J4005) | `…-combined-efi.img.gz` (UEFI) o `…-combined.img.gz` (BIOS) |

Es OpenWrt 25.12.5 oficial más:
- cengarde y su página de LuCI;
- LuCI en español;
- WireGuard;
- UPnP (miniupnpd y su página);
- drivers de adaptadores Ethernet USB: Realtek r8152/r8153/r8156, ASIX
  AX88179 y CDC Ethernet (módems HiLink, tethering).

**Instalar la imagen:**
- **Grabar:** descomprímela y grábala con balenaEtcher o
  `gunzip -c … | dd of=/dev/sdX bs=4M`.
- **Entrar:** a `http://192.168.1.1`, usuario `root` y sin contraseña.
  Ponle una de inmediato: *Sistema → Administración*.
- **Desde un OpenWrt que ya tienes:** usa la imagen `-sysupgrade` en
  *Sistema → Copia de seguridad / Grabar firmware*, que conserva la
  configuración.

Cada router genera su propio secreto en el primer arranque.

### Paquetes en un OpenWrt que ya tienes

El artefacto `cengarde-openwrt-<versión>-<arquitectura>` trae tres
paquetes:
- `cengarde`;
- `luci-app-cengarde`;
- `luci-i18n-cengarde-es`.

| Arquitectura | Equipo |
| --- | --- |
| `aarch64_cortex-a72` | Pi 4 |
| `x86_64` | PC |

Cópialos al router (`scp` a `/tmp`) e instálalos:

```sh
apk update && apk add --allow-untrusted /tmp/*cengarde*.apk   # OpenWrt 25.12
opkg update && opkg install /tmp/*cengarde*.ipk               # OpenWrt 24.10
apk add miniupnpd-nftables luci-app-upnp                      # para IP pass (24.10: opkg install)
```

Las dependencias salen de los repositorios oficiales: WireGuard
(`wireguard-tools`, `kmod-wireguard`) y `luci-proto-wireguard`.
`--allow-untrusted` hace falta porque los paquetes no vienen de un feed
firmado (pendiente).

## 2. Los enlaces

Una interfaz por módem o VLAN, con DHCP: *Red → Interfaces → Añadir
interfaz*. Si el módem entrega la conexión por Ethernet en una VLAN, el
dispositivo es `eth1.<vid>`.

Por consola, para cuatro VLAN en un adaptador USB (ajusta nombres e IDs):

```sh
for spec in wom:10 entel:20 claro:30 movistar:40; do   # nombre:vlan
	uci set network.${spec%%:*}=interface
	uci set network.${spec%%:*}.proto='dhcp'
	uci set network.${spec%%:*}.device="eth1.${spec#*:}"
done
uci commit network && service network reload
```

- **No hace falta tocar nada más:** cengarde pone a cada enlace una métrica
  propia, apaga los DNS de la operadora y los mete en la zona `wan` (ver
  sección 4).
- **Si el adaptador no aparece** en `ip link`, instala su driver; la imagen
  ya trae los comunes.
- **IPv6 de los enlaces:** cengarde agrega a cada enlace DHCP o estático
  una interfaz DHCPv6, `<enlace>6` (por ejemplo `starlink6`), sobre el
  mismo dispositivo y en la zona del enlace. Con ella el motor ve la IPv6
  del módem y puede llegar al VPS por IPv6; su prefijo nunca llega a la LAN
  (`delegate 0`) ni sus DNS al router (`peerdns 0`).
  - **La fuga de IPv6 de la LAN:** el túnel lleva solo IPv4 por ahora; si
    una interfaz delega un prefijo a la LAN, la IPv6 de la LAN sale por
    fuera del túnel. Por eso, con «enrutar todo el tráfico por el
    túnel», cengarde también pone `delegate 0` y `peerdns 0` en las
    interfaces IPv6 de los enlaces que no creó, como el `wan6` de fábrica
    de los equipos con puerto WAN (Starlink delega un prefijo), y
    `delegate 0` en los enlaces PPP o de módem (qmi, mbim, ncm), que se lo
    pasan a la `<enlace>_6` que crean. Desactivar cengarde lo deshace.
  - **Apagarlo:** *IPv6 en los enlaces* (pestaña *Avanzado*,
    `uplink_ipv6 '0'`) quita solo las compañeras `<enlace>6`; la fuga se
    sigue cerrando mientras todo vaya por el túnel.
  - **Avisos:** si el nombre `<enlace>6` ya existe o pasa de 15
    caracteres, cengarde no toca esa interfaz y el estado muestra
    `ipv6_companion_conflict`: el motor no verá la IPv6 de ese enlace. Si
    la LAN aún tiene un prefijo público, muestra `ipv6_leak`.

## 3. El VPS

1. **Copia el cloud-config:** en LuCI, *Servicios → cengarde →
   Configuración → VPS*, usa *Copiar* o *Descargar*.
   - Lleva el secreto y fija el mismo commit de cengarde que tiene el router,
     para que ambos hablen el mismo protocolo.
   - Sigue lo que marques en el formulario aunque no hayas guardado; por
     ejemplo, *IP pass*.
2. **Crea el VPS:** en Vultr, *Deploy → Cloud Compute*. Para el modo de
   servicio de un solo hilo conviene la CPU más rápida: *CPU Optimized* o
   *High Frequency*.
   - Elige **Ubuntu 24.04** o 22.04.
   - Pega el texto en *Cloud-Init User-Data*.
   - Si usas un *Firewall Group* de Vultr, abre UDP 65500, y también los
     puertos que reenvíes con IP pass.
   - **IP fija:** la IPv4 que trae la instancia se pierde si la destruyes.
     Una *Reserved IP* (US$3 al mes) sobrevive a eso: convierte en reservada
     la IP actual de la instancia, o despliégala con una. Queda como su IP
     principal y llega por DHCP: no hay nada que configurar en el VPS.
   - **IPv6 (opcional):** márcala al crearla, o después en *Settings → IPv6
     → Assign IPv6 Network* y reinicia desde el panel. Pon su IPv6 en la
     lista de direcciones del router (paso 4): los enlaces con IPv6, como
     Starlink, llegan por ella, y los demás por la IPv4.
3. **Espera unos minutos:** compila cengarde. Luego, por SSH:

   ```sh
   systemctl status cengarde wg-quick@wg0
   tail /var/log/cloud-init-output.log
   cengarde ctl status        # o: cengarde ctl links
   ```

**Qué deja montado:**
- cengarde como servicio de systemd sin privilegios;
- WireGuard solo para cengarde (`127.0.0.1:65501`): su puerto solo contesta
  en la propia máquina; no desde Internet ni desde el túnel, tampoco por
  IPv6;
- el túnel no llega al servicio de metadatos del proveedor
  (169.254.0.0/16), que guarda el secreto en el user data; el motor
  tampoco;
- NAT y, con IP pass, el reenvío de puertos;
- `cengarde-passthrough.path`, que abre o cierra el IP pass cuando lo pide
  el router.

No desactiva SSH ni su cortafuegos. Detalle en
[`contrib/vps/`](../contrib/vps/): `install.sh` instala,
`cengarde-vps-setup` deriva las claves del secreto y `cengarde-nat` hace el
NAT.

## 4. Activar

En *Servicios → cengarde → Configuración*, pestaña *General*:
- **Activado;**
- **Direcciones del VPS:** sus IP públicas, IPv4 e IPv6, en orden de
  preferencia (hasta 8). Cada enlace usa la primera de una familia que
  tenga, y pasa a la siguiente que pueda usar tras 10 s sin respuesta
  (`server_failover_ms`, pestaña *Avanzado*). Tienen que ser IP, no
  nombres: con todo el tráfico por el túnel, un nombre no se podría
  resolver;
- **Enlaces:** los del paso 2.

Luego *Guardar y aplicar*. Desde ese momento cengarde mantiene en la
configuración del router:

| Dónde | Qué | ¿Se deshace al desactivar? |
| --- | --- | --- |
| Red | interfaz `wgcg` (WireGuard, `10.79.0.2/30`) con las claves del secreto; su par `cengarde_vps` apunta a cengarde en `127.0.0.1` | sí |
| Rutas | `0.0.0.0/1` y `128.0.0.0/1` por `wgcg`: le ganan a cualquier ruta por defecto sin borrarlas, y cada enlace conserva la suya para cengarde | sí |
| Enlaces | una métrica propia a cada uno (con la misma métrica, netifd deja una sola ruta por defecto) | no: no estorba |
| Enlaces | `peerdns 0`: los DNS de la operadora irían por el VPS y suelen rechazarlo; se usan los IPv4 de la pestaña *Túnel* | sí |
| Enlaces | `<enlace>6`: DHCPv6 sobre el dispositivo del enlace, con `delegate 0` y `peerdns 0`, en la zona del enlace | sí |
| Enlaces | con «enrutar todo»: `delegate 0` y `peerdns 0` en las interfaces IPv6 que no creó (`wan6`), y `delegate 0` en los enlaces PPP o de módem | sí |
| Cortafuegos | `wgcg`, y los enlaces que no tengan zona, en la zona `wan` | `wgcg` sí, los enlaces no |
| UPnP | con IP pass: miniupnpd sobre `wgcg`, con la IP del VPS como externa (o la que dé STUN) | sí |

**Otros detalles:**
- **Los cambios posteriores no cortan el túnel:** el motor los aplica en
  marcha, con la misma sesión. Solo el secreto, el puerto y las perillas de
  CPU lo reinician, en el lugar y en menos de un segundo.
- **Sin dirección del VPS o sin enlaces** no se crea el túnel, para no
  dejar el router sin salida.
- **MTU del túnel:** 1380 por defecto; como mucho 1416 sobre IPv4 y 1396
  si hay una dirección IPv6 del VPS (en caminos de 1500 bytes). Si el
  camino de un enlace es más chico, el estado lo avisa.
- **DNS de la pestaña Túnel:** solo los IPv4 van por el túnel mientras IPv6
  no pase por él; los IPv6 se dejan fuera.
- **Desinstalar** el paquete también lo deshace todo.
- **Pestaña Avanzado:** las perillas del motor (silenciado de enlaces
  lentos, sondas, sondeo activo); valen los valores por defecto.
- **Hilos por enlace** (pestaña *Avanzado*): *Desactivado (el código de
  siempre)*, el valor por omisión hasta medirlos en la Pi; *Activado (un
  hilo por enlace)*, para que un enlace o un hilo atascado no frene a los
  demás, a cambio de más CPU con poco tráfico; *Un hilo (estructura nueva)*,
  para distinguir un fallo de los hilos de uno del código nuevo. Cambiarlo
  reinicia el motor (unos segundos sin túnel). Si un hilo se queda parado
  más de 5 s con paquetes esperando, el estado lo avisa
  (`link_thread_stalled`).

## 5. Estado

*Servicios → cengarde → Estado* se actualiza cada 3 s:
- **Servicio:** si está en marcha.
- **Túnel WireGuard:** último handshake.
- **IP pass:** activo en el VPS, desactivado, o esperando que el VPS lo
  confirme.
- **Tráfico** y copias duplicadas descartadas.
- **Por enlace:**
  - estado: activo, silenciado, esperando al VPS, sin respuesta o en pausa;
  - RTT;
  - *atraso frente al más rápido*: pasado el límite, el enlace se silencia;
  - la dirección del VPS que usa y su familia, con «alternativa» si la
    primera que puede usar no contestó;
  - la MTU del camino hasta el VPS;
  - si el VPS lo usa para bajar;
  - qué parte de la bajada llegó primero por él;
  - **Pausar / Reanudar:** saca el enlace sin tocar la configuración (por
    ejemplo, un módem que va a cambiar de plan). La pausa se mantiene hasta
    reanudarlo o hasta que cengarde se reinicie.
- **Avisos:** falta la IP, un enlace caído o solo con IPv6 (sin dirección
  IPv6 del VPS), el VPS que no contesta por ningún enlace, un camino con
  MTU chica para el túnel, la fuga de IPv6 de la LAN, una compañera
  `<enlace>6` que no se pudo crear, el motor detenido, un cambio que no se
  pudo aplicar…

Por consola:

```sh
cengarde-setup status         # lo mismo que la página, en JSON
cengarde ctl links            # cada interfaz y por qué lleva el túnel o no
cengarde ctl link eth3 off    # pausar un enlace; "auto" lo reanuda
logread -e cengarde           # "link eth1.10 (wom) up ..." por enlace
wg show wgcg                  # handshake con el VPS
ping -c 3 10.79.0.1           # el VPS a través del túnel
```

## IP pass (la IP pública del VPS en terreno)

**En el VPS:** el TCP y el UDP de los puertos 1024–65000 que llegan a su IP
se reenvían al router, con la IP de origen intacta. El puerto de cengarde,
el de WireGuard y el SSH nunca se reenvían.

**En el router:**
- miniupnpd entrega esos puertos a los equipos de la LAN que los piden:
  consolas, P2P, cámaras;
- un reenvío fijo se hace en *Red → Cortafuegos → Reenvíos de puertos*,
  desde la zona `wan`;
- **VPS con dirección IPv6:** UPnP necesita la IPv4 pública del VPS. Pon un
  servidor STUN en la pestaña *Túnel* (`stun_host`, y `stun_port` si no es
  3478); sin él, UPnP no pasa puertos y el estado lo avisa (`upnp_no_ip`).

**Activarlo:**
- En LuCI: pestaña *Túnel*, *IP pass*. Requiere «enrutar todo el tráfico
  por el túnel».
- **El VPS sigue al interruptor:** el router se lo pide por cengarde, en
  cada sonda, y el VPS abre o cierra los puertos en menos de un segundo. La
  página de estado muestra cuando el VPS lo confirma.
- **El cloud-config** (`PASSTHROUGH` en `/etc/cengarde/nat.conf`) solo fija
  el valor hasta que el router lo pide por primera vez. Desde entonces el VPS
  recuerda lo último que pidió el router, también tras reiniciarse.

## Por consola (sin LuCI)

```sh
uci add_list cengarde.main.server='203.0.113.10'     # la IP del VPS
uci add_list cengarde.main.server='2001:db8::10'     # opcional: su IPv6
uci add_list cengarde.main.uplink='wom'              # uno por enlace
uci set cengarde.main.enabled='1'
uci commit cengarde && /etc/init.d/cengarde reload
cengarde-setup cloud-config > vps.yaml               # el user data del VPS
```

- **Todas las opciones**, comentadas: `/etc/config/cengarde`.
- **`config_file`:** usa un archivo INI propio en lugar del generado. Sirve,
  por ejemplo, para un servidor OpenWrt.
- **Configuración que se genera:** `/var/etc/cengarde.conf`.

## Problemas conocidos

- **IP pass necesita que la IP del VPS sea pública:** miniupnpd no arranca
  con una privada o reservada. En `logread` aparece «ext_ip contains
  reserved / private address».
- **Cambiar el secreto:** escribe el nuevo en `/etc/cengarde/secret` del VPS
  y ejecuta:

  ```sh
  cengarde-vps-setup && systemctl restart wg-quick@wg0 cengarde
  ```

- **Actualizar:** router y VPS con el mismo commit; la 0.4 cambió el
  protocolo (v3) y no habla con un VPS anterior. La 0.4.1 y la 0.4.2 no lo
  cambian, pero actualiza igual el VPS: la 0.4.1 cierra al túnel los
  metadatos y el puerto de WireGuard, y la 0.4.2 contesta desde la dirección
  a la que llegó cada paquete (antes, en un VPS con varias IP o con IPv6,
  el router descartaba las respuestas que salían desde otra). En el VPS:

  ```sh
  git -C /opt/cengarde fetch --depth 1 https://github.com/KevinashoUWU/cengarde <commit>
  git -C /opt/cengarde checkout FETCH_HEAD && sh /opt/cengarde/contrib/vps/install.sh
  ```

  La 0.4.2 convierte una vez, al instalarse, `option server` en
  `list server`; una `option server` escrita a mano después se sigue
  leyendo.

## Probar sin hardware

[`test/e2e.sh`](test/e2e.sh) arranca dos VMs con la imagen x86-64 y
comprueba el flujo completo:
- enlaces por DHCP;
- configuración solo desde LuCI;
- túnel arriba, e IP pass confirmado por el VPS;
- compañeras IPv6 `<enlace>6` y `delegate 0` en un enlace PPPoE;
- IPv6 hasta el VPS: cada enlace sale desde su dirección global (no la
  ULA), el VPS contesta desde la suya (`*`), el aviso de MTU de un camino
  de 1400, el paso a la IPv4 cuando un enlace pierde la IPv6 y cuando el
  VPS pierde su dirección IPv6, y la fuga cerrada: un prefijo delegado
  (como el de Starlink) no llega a la LAN mientras todo va por el túnel, y
  vuelve al desactivar;
- pausar y reanudar un enlace desde la página de estado;
- apagar IP pass con pings en curso: sin reiniciar el motor, sin pérdidas,
  y el VPS lo sigue;
- un enlace caído sin perder pings;
- desactivación limpia, y aplicar dos veces no cambia nada.

```sh
sudo apt-get install -y qemu-system-x86 openssh-client
(cd openwrt/test && npm ci && npx playwright install chromium)
openwrt/test/e2e.sh openwrt-25.12.5-cengarde-x86-64-generic-ext4-combined.img.gz
```

- Deja capturas de LuCI y logs en `e2e-out/`.
- Con KVM tarda un minuto; sin KVM, unos tres.
- `test/vm.sh` arranca las VMs para explorar a mano: LuCI del router en
  `http://127.0.0.1:8080`, y SSH en los puertos 2222 (router) y 2223
  (VPS).

## Compilar

**Paquetes, con el SDK** de
`https://downloads.openwrt.org/releases/<versión>/targets/<target>/`
(`bcm27xx/bcm2711` para la Pi 4, `x86/64` para un PC). Del feed de LuCI
basta `luci-base`, por `luci.mk` y sus herramientas `po2lmo` y `jsmin`:

```sh
tar --zstd -xf openwrt-sdk-*.tar.zst && cd openwrt-sdk-*/
./scripts/feeds update luci
mkdir -p package/feeds/luci && ln -s ../../../feeds/luci/modules/luci-base package/feeds/luci/
ln -s /ruta/a/cengarde/openwrt/cengarde /ruta/a/cengarde/openwrt/luci-app-cengarde package/
make defconfig && make package/cengarde/compile package/luci-app-cengarde/compile
ls bin/packages/*/base/*cengarde*
```

**Imagen, con el ImageBuilder** de la misma carpeta: copia los paquetes a
`packages/` y ejecuta

```sh
make image PROFILE=rpi-4 EXTRA_IMAGE_NAME=cengarde PACKAGES="cengarde \
  luci-app-cengarde luci-i18n-cengarde-es luci luci-i18n-base-es \
  miniupnpd-nftables luci-app-upnp luci-i18n-upnp-es kmod-usb-net-asix-ax88179"
```

`PROFILE=generic` para x86-64. La lista completa está en
`.github/workflows/openwrt.yml`.

Las traducciones viven en `luci-app-cengarde/po/es/cengarde.po`;
`python3 openwrt/luci-app-cengarde/i18n.py` las sincroniza con las vistas
(el CI falla si falta alguna).
