# cengarde en OpenWrt (y su VPS)

Guía para la primera prueba en un OpenWrt limpio (25.12 o 24.10) con un VPS
Ubuntu. El paquete se compila con los SDK oficiales de OpenWrt y ya está
validado con su libc (tests unitarios y laboratorio). **Todavía no se ha
probado en un router ni en un VPS reales**, y no hay interfaz LuCI: la
configuración es el archivo INI. Decisiones y verificaciones en la
[historia 007](../docs/historias/007-openwrt-y-vps.md).

Esquema: el WireGuard del router envía a cengarde en `127.0.0.1:59401`.
cengarde duplica cada paquete por todos los uplinks hacia el VPS
(UDP 65500), y allí otro cengarde entrega la primera copia al WireGuard del
VPS. El túnel usa `10.79.0.1` (VPS) y `10.79.0.2` (router).

## 1. El paquete

**Del CI:** en GitHub, Actions → *OpenWrt* → la ejecución más reciente →
*Artifacts*:

| Artefacto | Para | Formato |
| --- | --- | --- |
| `cengarde-openwrt-25.12.5-aarch64_cortex-a72` | Raspberry Pi 4 con OpenWrt 25.12 | `.apk` |
| `cengarde-openwrt-25.12.5-x86_64` | PC x86 (p. ej. el J4005) con 25.12 | `.apk` |
| `cengarde-openwrt-24.10.8-*` | lo mismo con OpenWrt 24.10 | `.ipk` |

**O compilado con el SDK** (de
`https://downloads.openwrt.org/releases/<versión>/targets/<target>/`; la Pi 4
es `bcm27xx/bcm2711` y un PC es `x86/64`):

```sh
tar --zstd -xf openwrt-sdk-*.tar.zst && cd openwrt-sdk-*/
ln -s /ruta/a/cengarde/openwrt/cengarde package/cengarde
make defconfig && make package/cengarde/compile
ls bin/packages/*/base/cengarde*
```

**Instalación** (copia el archivo al router, por ejemplo con `scp` a `/tmp`):

```sh
apk add --allow-untrusted /tmp/cengarde-0.2.0-r1.apk      # OpenWrt 25.12
opkg install /tmp/cengarde_0.2.0-r1_*.ipk                 # OpenWrt 24.10
```

El paquete instala:
- `/usr/sbin/cengarde`;
- el servicio `/etc/init.d/cengarde` (procd: reinicia solo y manda los logs
  a `logread`). No arranca mientras la clave siga sin poner;
- la configuración `/etc/cengarde/cengarde.conf`: una plantilla de cliente,
  con permisos 0600 porque lleva la clave, y el estado en
  `/var/run/cengarde.json`.

## 2. Claves (en el router)

Todas las claves se generan en el router, son aleatorias, y cuatro de ellas
van luego a la plantilla del VPS:

```sh
apk update && apk add kmod-wireguard wireguard-tools   # 24.10: opkg update && opkg install ...
mkdir -p /etc/cengarde && cd /etc/cengarde && umask 077
cengarde genkey > cengarde.key
wg genkey | tee wg-client.key | wg pubkey > wg-client.pub
wg genkey | tee wg-server.key | wg pubkey > wg-server.pub
wg genpsk > wg.psk
echo "REPLACE_CENGARDE_KEY          $(cat cengarde.key)"
echo "REPLACE_WG_SERVER_PRIVATE_KEY $(cat wg-server.key)"
echo "REPLACE_WG_CLIENT_PUBLIC_KEY  $(cat wg-client.pub)"
echo "REPLACE_WG_PRESHARED_KEY      $(cat wg.psk)"
```

## 3. El VPS

1. **Plantilla:** abre
   [`contrib/vps/cloud-config.yaml`](../contrib/vps/cloud-config.yaml) y
   cambia los cuatro `REPLACE_` por los valores del paso 2.
   - El reenvío de puertos (sección 7) viene activado: `PASSTHROUGH=yes`.
2. **Crear el servidor:** en Vultr, Ubuntu 22.04 o 24.04; pega la plantilla
   en *Cloud-Init User-Data*.
   - El servicio usa un solo hilo, así que importa la frecuencia de la CPU,
     no el número de núcleos.
3. **Comprobar:** a los pocos minutos (compila cengarde desde el código), por
   SSH:

   ```sh
   systemctl status cengarde wg-quick@wg0
   cat /run/cengarde/status.json
   ```

Qué deja montado:
- cengarde como servicio de systemd con usuario sin privilegios
  ([`contrib/systemd/cengarde.service`](../contrib/systemd/cengarde.service));
- WireGuard escuchando solo para cengarde;
- NAT para el túnel;
- `sysctl` (`ip_forward` y buffers).

A diferencia de la plantilla de SmoothWAN, no desactiva SSH ni el
cortafuegos: añade sus propias reglas por delante.

## 4. Los uplinks (en el router)

Si el adaptador USB no aparece (`ip link`), instala su driver primero:
- `kmod-usb-net-rtl8152` para los Realtek;
- `kmod-usb-net-asix-ax88179` para los ASIX.

Cada VLAN es una interfaz DHCP. Cada una necesita **su propia métrica**: si
dos rutas por defecto tienen la misma, el kernel rechaza la segunda y ese
enlace se queda sin camino al VPS. Los DNS de las operadoras se ignoran
(`peerdns 0`), porque muchas solo contestan a sus propios clientes y las
consultas irán por el túnel. Ajusta los nombres y los VLAN ID:

```sh
for spec in wom:10:10 entel:20:20 claro:30:30 movistar:40:40; do  # nombre:vlan:métrica
	name=${spec%%:*}; rest=${spec#*:}; vid=${rest%%:*}; metric=${rest#*:}
	uci set network.$name=interface
	uci set network.$name.proto='dhcp'
	uci set network.$name.device="eth1.$vid"
	uci set network.$name.metric="$metric"
	uci set network.$name.peerdns='0'
done
WAN=$(uci show firewall | sed -n "s/^firewall\.\([^.]*\)\.name='wan'$/\1/p")
for n in wom entel claro movistar wg0; do uci add_list firewall.$WAN.network=$n; done
uci commit firewall
```

cengarde no necesita mwan3: ata cada socket a su interfaz y usa la ruta por
defecto de esa interfaz.

## 5. WireGuard (en el router)

```sh
uci set network.wg0=interface
uci set network.wg0.proto='wireguard'
uci set network.wg0.private_key="$(cat /etc/cengarde/wg-client.key)"
uci add_list network.wg0.addresses='10.79.0.2/30'
uci set network.wg0.mtu='1380'
uci add_list network.wg0.dns='1.1.1.1'
uci add_list network.wg0.dns='9.9.9.9'
uci set network.vps=wireguard_wg0
uci set network.vps.public_key="$(cat /etc/cengarde/wg-server.pub)"
uci set network.vps.preshared_key="$(cat /etc/cengarde/wg.psk)"
uci set network.vps.endpoint_host='127.0.0.1'
uci set network.vps.endpoint_port='59401'
uci set network.vps.persistent_keepalive='25'
uci set network.vps.route_allowed_ips='1'
uci add_list network.vps.allowed_ips='0.0.0.0/0'
uci commit network
```

- **Toda la LAN sale por el túnel:** `allowed_ips 0.0.0.0/0`.
- **MTU 1380:** deja sitio a la cabecera de cengarde (24 B) en redes
  móviles. Si algo se atasca con paquetes grandes, baja a 1280.

## 6. cengarde (en el router)

```sh
cd /etc/cengarde
sed -i "s|^key = .*|key = $(cat cengarde.key)|; s|^server = .*|server = IP_DEL_VPS:65500|" cengarde.conf
vi cengarde.conf      # interfaces = eth1.* y, si quieres, [link eth1.10] label = WOM ...
cengarde -t -c cengarde.conf
/etc/init.d/cengarde enable && /etc/init.d/cengarde start
service network restart && service firewall restart
```

- Pon en `server` la **IP** del VPS, no un nombre: al arrancar, el router
  todavía no tiene DNS (va por el túnel).
- Si aun así no puede arrancar, procd lo reintenta cada 5 s.

**Comprobar:**

```sh
logread -e cengarde        # "link eth1.10 up ..." por cada uplink
cat /var/run/cengarde.json # estado, RTT y salud de cada enlace
wg show                    # "latest handshake" reciente
ping -c 3 10.79.0.1        # el VPS a través del túnel
```

## 7. IP pública en terreno (reenvío de puertos y UPnP)

**En el VPS** (`PASSTHROUGH=yes`, activado por defecto):
- el TCP y el UDP de los puertos 1024–65000 que llegan a su IP pública se
  reenvían al router con la IP de origen intacta;
- 65500 (cengarde), 65501 (WireGuard) y el SSH quedan fuera.

Se cambia en `/etc/cengarde/nat.conf` y se aplica con
`systemctl restart wg-quick@wg0`.

**En el router**, UPnP para que los equipos de la LAN abran sus puertos
solos (consolas, juegos, P2P), anunciando la IP del VPS como pública:

```sh
apk add miniupnpd-nftables luci-app-upnp   # 24.10: opkg install ...
uci set upnpd.config.enabled='1'
uci set upnpd.config.external_iface='wg0'
uci set upnpd.config.external_ip='IP_DEL_VPS'
uci commit upnpd
/etc/init.d/miniupnpd enable && /etc/init.d/miniupnpd restart
```

- **Reenvíos fijos** hacia un equipo concreto (un servidor en terreno): en
  LuCI, Red → Cortafuegos → Reenvíos de puertos, desde la zona `wan`.
- Comprueba con `apk search miniupnpd` el nombre exacto del paquete en tu
  versión.

## Pendiente (LuCI)

Una página de LuCI que haga todo esto sin consola:
- emparejar con el VPS con un solo secreto;
- crear el túnel de WireGuard solo;
- elegir los uplinks;
- activar o desactivar el reenvío de puertos y UPnP;
- ver el estado de cada enlace.

Está en el ROADMAP, Fase 3.
