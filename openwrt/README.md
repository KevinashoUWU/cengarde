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
       WireGuard wgcg   ══ enlace 2 ══╬══ VPS: cengarde → WireGuard cg-router → Internet
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

Para **actualizar** se instalan igual, encima. Desde la 0.6.3-r2, el paquete
reinicia el motor (y el registrador, si estaba grabando) al terminar: el
túnel se corta unos segundos, así que no actualices mientras transmites.
Hasta la 0.6.3-r1 no lo hacía, y el motor viejo seguía en marcha hasta el
próximo reinicio aunque el paquete nuevo estuviera instalado: después de
instalar la r2 sobre una versión anterior, `cengarde ctl status | grep
version` tiene que decir la nueva (si no, `/etc/init.d/cengarde restart`).

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
  - **La fuga de IPv6 de la LAN:** por omisión el túnel lleva solo IPv4
    (la IPv6 por el túnel es opcional: sección «IPv6 por el túnel»); si
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
   sudo cengarde-vps-setup list   # el router, su puerto y cuándo se le oyó
   systemctl status cengarde wg-quick@cg-router cengarde-nat
   tail /var/log/cloud-init-output.log
   sudo cengarde ctl status       # o: cengarde ctl links
   ```

**Qué deja montado:**
- cengarde como servicio de systemd, con un usuario propio (`cengarde`)
  y sin privilegios;
- el router, importado una sola vez del secreto del cloud-config (que
  luego se borra) como `router` (o su `vps_name`): un archivo en
  `/etc/cengarde/clients/` y su WireGuard, `cg-router`, solo para cengarde
  (`127.0.0.1:65501`); los
  puertos de WireGuard (65501–65532, uno por router) solo contestan en la
  propia máquina, no desde Internet ni desde el túnel, tampoco por IPv6;
- el túnel no llega al servicio de metadatos del proveedor
  (169.254.0.0/16), que guarda el secreto en el user data; el motor
  tampoco;
- el cortafuegos de cengarde (`cengarde-nat.service`): NAT y, con IP pass,
  el reenvío de puertos, en cadenas propias que no dependen del nombre de
  ninguna interfaz; cada 5 minutos `cengarde-nat-check.timer` las repone si
  otro cortafuegos las borró;
- `cengarde-passthrough.path`, que aplica la tabla de reenvío del motor
  (el IP pass que pide el router y las reglas de cada uno) cuando cambia.

No desactiva SSH ni su cortafuegos: convive con ufw y con
`nftables.service` (si te falta algo, `sudo cengarde-nat check` dice qué
líneas añadir); firewalld no está soportado todavía y la instalación se
detiene con un mensaje. Detalle en [`contrib/vps/`](../contrib/vps/):
`install.sh` instala, `cengarde-vps-setup` lleva los routers y deriva sus
claves del secreto, y `cengarde-nat` hace el cortafuegos. Sin WireGuard
(experimental, sin cifrar): `sudo cengarde-vps-setup tunnel native`, ver
[Túnel nativo](#túnel-nativo-experimental-sin-cifrar).

### Nombre y dirección de cada router

Un VPS puede atender a varios routers; cada uno tiene su nombre y su dirección
en el túnel (el VPS es la `10.79.0.1`). Los dos van en la pestaña *VPS* de
LuCI:
- **Nombre** (`vps_name`): de 1 a 12 caracteres entre `a-z`, `0-9` y `-`, sin
  empezar por `-` y distinto de `default`; `router` si lo dejas vacío. En el
  VPS es el archivo `/etc/cengarde/clients/NOMBRE` y su WireGuard `cg-NOMBRE`.
- **Dirección** (`tunnel_addr`): `10.79.X.Y`, de `10.79.0.2` a `10.79.255.254`.
  Vacía, sale del secreto (`cengarde keys`), así que dos routers no la
  comparten sin que elijas nada. `wgcg` la lleva como `/32`, y cengarde
  agrega la ruta a `10.79.0.1`, que un `/32` no trae, con o sin «enrutar todo».
- **Un router anterior a la 0.5.2** conserva la `10.79.0.2`, la que su VPS ya
  conoce, y sigue funcionando al actualizar. Pasar a la derivada (vaciar el
  campo) exige que el VPS tenga la nueva: corta el túnel hasta entonces.

El cloud-config de un VPS nuevo ya lleva el nombre y la dirección. Para
sumar este router a **un VPS que ya atiende a otros**, LuCI (pestaña *VPS*,
*Este router en un VPS que ya atiende a otros*) muestra el comando, con el
nombre, la dirección y el protocolo del motor de este router:

```sh
sudo cengarde-vps-setup add casa --tunnel-addr 10.79.131.76 --proto 5
```

- **El VPS pide el secreto:** pega el de la misma pestaña. Nunca va en la
  línea de órdenes.
- **Mismo protocolo:** si el motor del VPS habla otro, `add` se niega y dice
  qué actualizar. El VPS atiende a varios routers desde la 0.5.2.
- **Tras generar un secreto nuevo,** guarda y aplica antes de copiar: LuCI no
  puede calcular la dirección derivada, se la pide al router ya guardado.

### Un servidor propio, sin cloud-init

Sirve cualquier Debian 12 o Ubuntu 22.04 o posterior, también en casa
detrás del router de tu operador, si le llega una IPv4 pública (no detrás
de CGNAT):

```sh
sudo apt-get install build-essential git iptables wireguard-tools conntrack
sudo git clone https://github.com/KevinashoUWU/cengarde /opt/cengarde
sudo git -C /opt/cengarde checkout COMMIT   # el del paquete del router (LuCI lo muestra)
sudo sh /opt/cengarde/contrib/vps/install.sh
sudo cengarde-vps-setup add router       # y pega el secreto del router (LuCI muestra el comando completo)
```

- **Sin secreto en `/etc/cengarde/secret`,** `install.sh` instala y te
  recuerda el `add`; el secreto se pega, nunca va en la línea de órdenes.
- **Con IPv4 privada** (un router de casa, o una nube con NAT 1:1 como
  Oracle Cloud, AWS o GCP): `cengarde-vps-setup` lo detecta, dice qué abrir
  «en tu router de casa o en el cortafuegos del proveedor (security list)»
  (UDP 65500 y los puertos que liste `sudo cengarde-vps-setup forward`) y
  nunca reenvía al router lo que viene de la propia red del servidor
  (`FORWARD_SKIP_SRC` en `/etc/cengarde/nat.conf`).
- **Sus propios servicios siguen siendo suyos:** lo que escucha en el
  servidor y los puertos que publica Docker quedan fuera del reenvío.

### En casa, con IP dinámica (DNS dinámico)

El "VPS" puede ser un equipo en tu casa (o en la oficina) aunque el
operador te cambie la IP: el router lo busca por su nombre y lo sigue.

1. **Una IPv4 pública de verdad:** compara la IP WAN que muestra tu router
   de casa con la que ves en una página como *¿cuál es mi IP?*. Si son
   distintas, o la WAN empieza por `100.64.` a `100.127.`, estás detrás de
   CGNAT y no llega nada desde fuera: ni el reenvío ni el DNS dinámico lo
   arreglan (pide al operador una IP pública). Un servidor solo con IPv6
   todavía no sirve.
2. **El servidor con IP fija en tu red:** una reserva DHCP en el router de
   casa (por ejemplo `192.168.1.50`), e instálalo como en
   [Un servidor propio](#un-servidor-propio-sin-cloud-init).
3. **Reenvío de puertos en el router de casa,** hacia esa IP: UDP 65500 (el
   puerto de cengarde) y, si usas IP pass, los puertos que liste
   `sudo cengarde-vps-setup forward`. `cengarde-vps-setup` ve que el
   servidor tiene una IPv4 privada y te lo recuerda.
4. **Un nombre con DNS dinámico** (DuckDNS, No-IP, dynv6, o el de tu
   dominio) que siga tu IP pública. Lo mejor es el cliente de DNS dinámico
   del propio router de casa, que avisa en cuanto cambia la WAN; si no, uno
   en el servidor (`ddclient`, o un `curl` cada minuto). Conviene un TTL
   corto, de 60 s: los DNS de las operadoras guardan la IP vieja ese tiempo.
5. **En el router con cengarde,** pestaña *General*, en *Direcciones del
   VPS*: el nombre, por ejemplo `micasa.duckdns.org`. Puedes poner después
   la IP de otro servidor como respaldo.

**Qué pasa cuando cambia la IP de casa:** los enlaces dejan de recibir
respuesta; a los 2 s el router vuelve a preguntar el nombre (por cada
enlace, a los DNS de su operadora, nunca por el túnel), y cada 15 s
mientras no haya respuesta; en cuanto el DNS da la IP nueva, todos los
enlaces se mudan. El corte es lo que tarde tu DNS dinámico en publicar la IP
nueva y los DNS en olvidar la vieja (el TTL), más unos segundos: en el
laboratorio, sin cachés, 3,3 s. El estado muestra el nombre, su IP y
hace cuánto respondió el DNS, o el problema si no resuelve.

**IP pass con un nombre:** UPnP necesita saber la IP pública y un nombre no
sirve (su IP cambia): pon un servidor STUN en la pestaña *Túnel* (por
ejemplo `stun.cloudflare.com`, puerto 3478); miniupnpd la descubre a
través del túnel, así que ve la IP de tu casa y sigue sus cambios.

## 4. Activar

En *Servicios → cengarde → Configuración*, pestaña *General*:
- **Activado;**
- **Direcciones del VPS:** sus IP públicas, IPv4 e IPv6, o nombres, en
  orden de preferencia (hasta 8). Cada enlace usa la primera de una
  familia que tenga, y pasa a la siguiente que pueda usar tras 10 s sin
  respuesta (`server_failover_ms`, pestaña *Avanzado*). Un nombre lo busca
  el propio cengarde por cada enlace, primero en los DNS de su operadora y
  nunca por el túnel, y lo sigue si cambia de IP (ver
  [En casa, con IP dinámica](#en-casa-con-ip-dinámica-dns-dinámico));
- **Enlaces:** los del paso 2.

Luego *Guardar y aplicar*. Desde ese momento cengarde mantiene en la
configuración del router:

| Dónde | Qué | ¿Se deshace al desactivar? |
| --- | --- | --- |
| Red | interfaz `wgcg` (WireGuard) con las claves del secreto y la dirección del router en el túnel como `/32` (`tunnel_addr`, o la derivada del secreto); su par `cengarde_vps` apunta a cengarde en `127.0.0.1` | sí |
| Rutas | `10.79.0.1/32` por `wgcg` (la sección `cengarde_vps_route`): el VPS dentro del túnel, también sin «enrutar todo» | sí |
| Rutas | `0.0.0.0/1` y `128.0.0.0/1` por `wgcg`: le ganan a cualquier ruta por defecto sin borrarlas, y cada enlace conserva la suya para cengarde | sí |
| Red | con «IPv6 por el túnel» (apagado por defecto): `wgcg` suma `fdXX:XXXX:XXXX::2/128` (la ULA sale del secreto), el par permite también `::/1` y `8000::/1`, y la ruta a `fdXX:XXXX:XXXX::1/128` por `wgcg` (la sección `cengarde_vps_route6`) | sí |
| Rutas | con «IPv6 por el túnel» y «enrutar todo»: `::/1` y `8000::/1` por `wgcg` | sí |
| Cortafuegos | con «IPv6 por el túnel»: un include de nftables (`cengarde_nat66`, que carga `/usr/share/cengarde/nat66.nft`) al comienzo de la cadena `srcnat`, que enmascara la IPv6 que sale por `wgcg` | sí |
| DHCP | con «IPv6 por el túnel» y «enrutar todo»: `ra_default 1` en la sección `lan` de `/etc/config/dhcp`, marcado con `cengarde_ra_default`, solo si no tenía valor | sí, solo lo que puso cengarde |
| Enlaces | una métrica propia a cada uno (con la misma métrica, netifd deja una sola ruta por defecto) | no: no estorba |
| Enlaces | `peerdns 0`: los DNS de la operadora irían por el VPS y suelen rechazarlo; se usan los IPv4 de la pestaña *Túnel*. El motor sí usa los de cada enlace, por ese enlace, para buscar el nombre del VPS (netifd los guarda como inactivos) | sí |
| Enlaces | `<enlace>6`: DHCPv6 sobre el dispositivo del enlace, con `delegate 0` y `peerdns 0`, en la zona del enlace | sí |
| Enlaces | con «enrutar todo»: `delegate 0` y `peerdns 0` en las interfaces IPv6 que no creó (`wan6`), y `delegate 0` en los enlaces PPP o de módem | sí |
| Cortafuegos | `wgcg`, y los enlaces que no tengan zona, en la zona `wan` | `wgcg` sí, los enlaces no |
| UPnP | con IP pass: miniupnpd sobre `wgcg`, con la IP del VPS como externa (o la que dé STUN) | sí |

**Otros detalles:**
- **Los cambios posteriores no cortan el túnel:** el motor los aplica en
  marcha, con la misma sesión. Solo el secreto, el puerto y las perillas de
  CPU lo reinician, en el lugar y en menos de un segundo. Cambiar la
  dirección del túnel, «enrutar todo» o «IPv6 por el túnel» reinicia además
  la interfaz `wgcg` (netifd no ve esos cambios): un corte breve mientras
  WireGuard rehace el handshake.
- **Sin dirección del VPS o sin enlaces** no se crea el túnel, para no
  dejar el router sin salida.
- **MTU del túnel:** 1380 por defecto; como mucho 1416 sobre IPv4 y 1396
  si hay una dirección IPv6 del VPS (en caminos de 1500 bytes). Si el
  camino de un enlace es más chico, el estado lo avisa.
- **DNS de la pestaña Túnel:** solo los IPv4 van por el túnel; los IPv6 se
  dejan fuera, también con «IPv6 por el túnel» (por ahora).
- **Desinstalar** el paquete también lo deshace todo.
- **Pestaña Avanzado:** las perillas del motor (paso a reserva de los
  enlaces lentos, sondas, sondeo activo); valen los valores por defecto.
- **Hilos por enlace** (pestaña *Avanzado*): *Desactivado (el código de
  siempre)*, el valor por omisión hasta medirlos en la Pi; *Activado (un
  hilo por enlace)*, para que un enlace o un hilo atascado no frene a los
  demás, a cambio de más CPU con poco tráfico; *Un hilo (estructura nueva)*,
  para distinguir un fallo de los hilos de uno del código nuevo. Cambiarlo
  reinicia el motor (unos segundos sin túnel). Si un hilo se queda parado
  más de 5 s con paquetes esperando, el estado lo avisa
  (`link_thread_stalled`).
- **Tipo de cada enlace** (pestaña *General*, un desplegable por enlace
  elegido; historia [015](../docs/historias/015-modelo-de-enlace.md)):
  *Automático* (por omisión), *Celular (LTE/5G)*, *Starlink*, *Fibra*,
  *Cable, DSL o Ethernet* o *Radioenlace*. Se guarda como `list uplink_type
  'ENLACE=TIPO'` y llega al motor como `type = ...` en la sección del
  enlace; el motor se lo dice al VPS, que trata igual la bajada de ese
  enlace. Vale en redundancia y en reparto. Hoy solo *Starlink* cambia algo
  (STARLINK_PENDIENTE); los demás se comportan como *Automático* hasta
  medirlos. Una entrada con un enlace que no es de este router o un tipo
  desconocido no se usa, y queda en `logread`.
- **Reordenamiento y reparto** (pestaña *Avanzado*; historia
  [012](../docs/historias/012-bonding.md); paquetes 0.6.0 o posteriores en
  el router y en el VPS).
  - *Reordenar en el receptor* (`reorder`, activado): el extremo que recibe
    retiene un paquete que llega antes que otro que falta, hasta que llegue
    el que falta o hasta la *espera máxima* (`reorder_max_wait_ms`, 200 ms;
    menos si el enlace más lento ya lo habría entregado), y los entrega a
    WireGuard en orden. El VPS hace lo mismo con lo que recibe. Mientras cada
    paquete va por todos los enlaces no retiene nada, así que el orden de
    entrega es el de siempre. Los handshakes y los paquetes de hasta
    *Paquetes que nunca se retienen* (`reorder_bypass`, 128 bytes:
    keepalives, ACK vacíos) se entregan sin esperar. Sin interfaz quedan
    `reorder_dup_wait_ms`, `reorder_margin_ms` y `reorder_pool_kb` (este
    reinicia el motor al cambiarlo): ver `engine/README.md`.
  - *Repartir los paquetes grandes entre los enlaces (experimental)*
    (`split`, desactivado; necesita *Reordenar en el receptor*): en vez de
    una copia por cada enlace, cada paquete de más de `split_small` bytes
    (256) va por un solo enlace, elegido según su peso. El peso de cada
    enlace es una entrada `ENLACE=PESO` de `split_weight` (el nombre de la
    lista de enlaces; de 0 a 1000; sin entrada, 1; con 0 el enlace lleva
    además una copia de todos los paquetes). Una entrada con un enlace que
    no es de este router o un peso fuera de rango no se usa, y queda en
    `logread`. El VPS sigue al router: reparte la bajada con los mismos
    pesos si el router se lo pide, con su `split = client` de siempre, sin
    tocar nada allí. *Diferencia máxima de retardo para repartir*
    (`split_spread_ms`, 30 ms; 0: sin límite): un enlace que va más de eso
    detrás del más rápido (en su retardo hacia el VPS) deja su parte hasta
    que se mantiene dentro de 3/4 de esa diferencia durante el tiempo de
    asentamiento. *Copias a los enlaces fuera del reparto*
    (`split_spread_copy`, activado como hasta la 0.6.2, salvo con el
    gobernador): mientras está
    fuera lleva una copia de todos los paquetes, y si quedan menos de dos
    en el reparto todo va por todos. Desactivado, lleva solo lo que va por
    todos (handshakes y paquetes chicos) y, si queda uno solo en el
    reparto, ese lleva todo: en terreno las copias llenaban aún más la cola
    del enlace que ya iba atrasado, y en el laboratorio ayudaron a QUIC
    (historia 012), así que está para probar las dos cosas en campo. El VPS
    hace lo mismo con la bajada, con su propio `split_spread_ms` (30) y su
    propio `split_spread_copy` (activado, salvo con su gobernador), y su
    propio `governor`: van en `/etc/cengarde/engine.local` del VPS, que
    `cengarde-vps-setup` copia en el `cengarde.conf` que genera (editar
    `cengarde.conf` no sirve: cada `refresh` lo vuelve a escribir).
    En el laboratorio, 50 ms entre dos enlaces ya se comían casi toda la
    ganancia del reparto y 150 ms la hundían (ver `engine/README.md`).
  - **El rescate y otros ajustes del reordenador (experimentales, solo UCI,
    apagados por omisión; historia
    [013](../docs/historias/013-capacidad-y-dosificacion.md)).** Necesitan
    el protocolo v5 en el router y en el VPS; el detalle está en
    `engine/README.md`.
    - `rescue_wait_ms` (0–1000; 0: no pide): antes de dar por perdido un
      paquete, el reordenador lo pide al otro extremo (un NACK) y espera
      tanto por él. Con `rescue_tries` (2; 1–4) intentos dentro de ese
      tiempo.
    - `rescue_kb` (0: no guarda) y `rescue_pct` (5; 1–50): lo que este
      extremo guarda de lo que envía para reenviarlo cuando se lo piden, y
      el presupuesto, en % de lo enviado. Cambiar `rescue_kb` reinicia el
      motor.
    - Para rescatar la **bajada**: `rescue_wait_ms` en el router y
      `rescue_kb` en el VPS (en su `engine.local`). Para la **subida**,
      al revés: `rescue_kb` en el router y `rescue_wait_ms` en el VPS.
    - No rescata la congestión (una ruta con la cola llena), y sirve sobre
      todo para tráfico de tasa fija (video, radio), no con TCP saturando.
    - `reorder_by_link` y `reorder_adaptive`: el plazo de un hueco según el
      enlace por el que iba, y el margen de cada enlace que aprende de sus
      paquetes tardíos. Ninguno mejoró el rendimiento medido.
    - Por ejemplo: `uci set cengarde.main.rescue_wait_ms=60` y
      `uci set cengarde.main.rescue_kb=32768`.
  - **El reparto protege menos que copiar** (por eso viene apagado),
    aunque con enlaces de retardo parecido suma su velocidad (en el
    laboratorio, dos enlaces iguales llevaron 1,98 veces lo de uno): un
    enlace que muere conserva su parte hasta que el motor lo saca (unos
    300 ms según la estimación; 1,5 s antes de esa protección) y los
    paquetes que le tocaron se pierden; el enlace más lento del reparto fija
    la latencia de todos (como mucho `split_spread_ms` detrás del más
    rápido); y un peso mayor que lo que aguanta un enlace llena la
    cola de su módem. Los pesos son fijos y no hay control de colas.

## 5. Estado

*Servicios → cengarde → Estado* se actualiza cada 3 s:
- **Servicio:** si está en marcha.
- **Túnel WireGuard:** último handshake.
- **IP pass:** activo en el VPS, desactivado, o esperando que el VPS lo
  confirme.
- **Nombre del VPS** (si pusiste uno): su IP y hace cuánto respondió el DNS,
  o «por /etc/hosts».
- **Tráfico** y copias duplicadas descartadas.
- **Por enlace:**
  - estado: activo, esperando al VPS, sin respuesta o en pausa;
  - subida y bajada, cada una «en uso» o «en reserva». Cada sentido se
    decide aparte, en el extremo que envía: es normal subir por un operador
    y bajar por otro. En reserva, el enlace solo lleva sondas hasta que
    alcanza al mejor, y siempre quedan en uso al menos los enlaces activos
    mínimos (pestaña *Avanzado*);
  - RTT;
  - *atraso frente al más rápido* en la subida: pasado el límite, la
    subida de ese enlace pasa a reserva;
  - la dirección del VPS que usa y su familia, con «alternativa» si la
    primera que puede usar no contestó;
  - la MTU del camino hasta el VPS;
  - qué parte de la bajada llegó primero por él;
  - **Pausar / Reanudar:** saca el enlace sin tocar la configuración (por
    ejemplo, un módem que va a cambiar de plan). La pausa se mantiene hasta
    reanudarlo o hasta que cengarde se reinicie.
- **Reordenamiento** (si el motor lo informa; sin él, un motor anterior,
  la caja no aparece): paquetes retenidos ahora (de los que caben), el
  máximo retenido a la vez, los retenidos en total, los faltantes dados por
  perdidos (por el plazo o por falta de lugar) y los que llegaron después de
  darse por perdidos. Si estos últimos son muchos, la espera máxima es corta
  para el enlace más lento. Los contadores cuentan desde que arrancó el
  motor.
- **Avisos:** falta la IP, un enlace caído o solo con IPv6 (sin dirección
  IPv6 del VPS), un nombre del VPS que no resuelve (con el motivo: sin
  respuesta, el DNS falló, no existe), el VPS que no contesta por ningún
  enlace, un camino con
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

### Registrar los enlaces durante días

El registrador de campo (`cengarde-rec`) guarda el estado del motor en
archivos por hora comprimidos con gzip, con un tope de tamaño. Por cada
enlace queda el RTT, las caídas, el paso a reserva y los contadores, en la
resolución de `status_interval_ms`. Con eso, `bench/fieldrec.py` responde a
lo que el bonding necesita del terreno ([historia 012](../docs/historias/012-bonding.md)):

- qué enlaces caen juntos;
- si Starlink corta cada 15 s;
- cuánto se separan los RTT entre enlaces;
- cómo cambia todo eso según la hora.

Con los instrumentos pasivos del motor (desde la 0.5.5) suma la tardanza
conjunta: de los paquetes que salieron por varios enlaces, cuántos llegaron
tarde o no llegaron por todos a la vez (a 50, 100 y 150 ms de la primera
copia), frente a lo que daría la independencia, y qué par conviene duplicar
(el de menor tardanza conjunta, que no siempre es el de menor tardanza
individual). También da la pérdida del túnel (paquetes que no llegaron por
ningún enlace) y los tamaños de los datagramas de WireGuard. Con el
reordenador del receptor (campo `reorder` del estado) suma la sección
*Reordenamiento*: cuántos paquetes retuvo, cuántos faltantes dio por perdidos
(por el plazo o por falta de lugar) y cuántos llegaron después, cuánto esperó
cada paquete retenido, y sus peores intervalos; con `--export`, además, el
`reorder.csv` (una fila por contador, medidor o tramo de histograma y por
intervalo). El estado del servidor (su `status_file`), grabado con el mismo
`cengarde-rec -s ARCHIVO` (es un script de `sh`; el VPS no lo trae, se copia),
da una sección por sesión del VPS. Una grabación de un motor anterior sigue
sirviendo: esas secciones dicen que no hay datos.

Viene apagado. Para una semana con cinco enlaces (unos 120 MB) conviene un
disco USB en vez de `/tmp`:

```sh
uci set cengarde.recorder=recorder
uci set cengarde.recorder.enabled=1
uci set cengarde.recorder.dir=/mnt/usb/cengarde-rec   # o /tmp/cengarde-rec
uci set cengarde.recorder.max_mb=200
uci set cengarde.main.probe_idle_ms=100   # un enlace ocioso también contesta 10 veces por segundo
uci commit cengarde && /etc/init.d/cengarde reload && /etc/init.d/cengarde-rec restart
```

Si el disco no está montado cuando arranca, escribe en la tarjeta SD.
Detenerlo (`/etc/init.d/cengarde-rec stop`) cierra y comprime el archivo en
curso. Para el análisis, en un PC con Python 3 (la hora de Chile es
`--utc-offset -3` en verano y `-4` en invierno):

```sh
scp -O -r root@192.168.1.1:/mnt/usb/cengarde-rec .   # -O: dropbear no trae SFTP
python3 bench/fieldrec.py --utc-offset -3 cengarde-rec > campo.md
```

## IP pass (la IP pública del VPS en terreno)

**En el VPS:** el TCP y el UDP de los puertos 1024–65000 que llegan a su IP
se reenvían al router, con la IP de origen intacta. Nunca se reenvían el
puerto de cengarde, los de WireGuard, el SSH ni lo que escucha el propio
servidor o publica Docker; `sudo cengarde-vps-setup forward` los lista con
su motivo. Los puertos reenviados también funcionan desde la propia LAN,
por la IP del VPS.

**Con varios routers en un VPS:** el rango entero va a uno solo, el primero
que lo pide, que lo conserva mientras lo siga pidiendo; los demás ven en
LuCI que el VPS no lo concede. A cualquiera se le pueden dar puertos sueltos
desde el VPS, que le ganan al rango:

```sh
sudo cengarde-vps-setup forward add casa tcp:8080=80 udp:5000-5010 both:25565
sudo cengarde-vps-setup forward del casa udp:5000-5010   # o: del casa all
sudo cengarde-vps-setup forward                          # quién tiene qué
```

- **Formato:** `tcp`, `udp` o `both`, un puerto o un rango del VPS y, tras
  `=`, el puerto del router si es otro.
- **Se niega** a dar un puerto reservado (dice por qué), uno que ya tiene
  otra regla (dice de quién) o más de 1024 puertos a un router
  (`forward limit NOMBRE N` lo cambia). Bajo 1024, primero
  `forward allow tcp:443`.

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

## IPv6 por el túnel (apagado por defecto)

Por omisión el túnel lleva solo IPv4. Desde la 0.5.3 puede llevar también
IPv6, traducida en los dos extremos (NAT66): con «enrutar todo», la IPv6 del
router y de la LAN sale por el VPS. Viene apagado hasta probarlo en hardware
real.

**En el router** (pestaña *Túnel*, *IPv6 por el túnel*; `tunnel_ipv6 '1'`),
cengarde mantiene:
- en `wgcg`, una dirección ULA, `fdXX:XXXX:XXXX::2/128`; el VPS tiene la
  `::1` del mismo prefijo. El prefijo sale del secreto (`cengarde keys` lo
  imprime como `CG_TUNNEL_ULA`): no hay nada que elegir ni que repetir en el
  VPS;
- la ruta a esa `::1` por `wgcg`, que una `/128` no trae sola;
- `::/1` y `8000::/1` entre las IP permitidas del par, así que con «enrutar
  todo» toda la IPv6 va por `wgcg`;
- un include de firewall4 (`nat66.nft`) que enmascara la IPv6 que sale por
  `wgcg`;
- con «enrutar todo», `ra_default 1` en la sección `lan` de
  `/etc/config/dhcp`, para que odhcpd anuncie una ruta por defecto a una LAN
  que solo tiene su ULA. Solo si no tenía valor: un `ra_default` tuyo se
  respeta, y al apagar cengarde devuelve exactamente lo que puso.

**Por qué NAT66 y una ULA:** el proveedor no enruta un prefijo al VPS (le da
una dirección, o un `/64` en su propia interfaz), así que no hay prefijo que
delegar a la LAN. La LAN solo tiene su ULA, y su IPv6 sale traducida, primero
en el router y luego en el VPS.

**Encenderla:** el VPS tiene que tener IPv6 propia (la que da el
proveedor; ver el paso 2 de la sección 3). Luego, en los dos extremos:
1. **En el VPS:** `sudo cengarde-vps-setup ipv6 on` (se niega si el VPS no
   tiene IPv6; reinicia el WireGuard de cada router, un corte breve). Un VPS
   nuevo no lo necesita: el cloud-config de LuCI sigue el interruptor
   (`TUNNEL_IPV6=yes` cuando el router tiene también «enrutar todo»).
2. **En el router:** LuCI, *Túnel* → *IPv6 por el túnel*, y *Guardar y
   aplicar*. `wgcg` se reinicia (un corte breve). Por consola:
   `uci set cengarde.main.tunnel_ipv6=1 && uci commit cengarde &&
   /etc/init.d/cengarde reload`.

**Conviene saber:**
- **La LAN necesita un prefijo ULA** (*Red → Interfaces → Opciones globales
  de red*, `network.globals.ula_prefix`; OpenWrt crea uno en el primer
  arranque). Sin él, con «enrutar todo» el estado avisa `no_ula_prefix`.
- **Un equipo de la LAN con solo un prefijo ULA prefiere la IPv4** para un
  nombre que tiene las dos (RFC 6724, por la etiqueta de su dirección de
  origen). La IPv6 por el túnel sirve sobre todo a los destinos que solo
  tienen IPv6; no es un fallo.
- **El estado** muestra la dirección IPv6 en el túnel; `cengarde-setup
  status` la da como `tunnel_ula`, y `tunnel_ipv6` dice si está encendido.
- **Apagarla:** en el router, el interruptor: `wgcg` pierde la dirección, el
  cortafuegos el include y la LAN el `ra_default` que cengarde puso. En el
  VPS, `sudo cengarde-vps-setup ipv6 off`.
- **Lo que cubre `test/e2e.sh`** (QEMU): la dirección, la ruta y un ping al
  extremo del VPS por el túnel, la regla de NAT66 en el cortafuegos, el
  `ra_default` con su marca, y que apagarlo lo deja todo como estaba. No
  cubre la salida a Internet por IPv6, que pide hardware real y un VPS con
  IPv6.

## Túnel nativo (experimental, sin cifrar)

Por omisión el túnel es WireGuard (`wgcg`) y cengarde lleva sus datagramas
cifrados. Con el **modo nativo** no hay WireGuard: el motor de cada extremo
abre un dispositivo TUN propio, `cg0`, y lleva los paquetes IP tal cual,
con la autenticación de siempre (MAC y anti-replay con las claves del
secreto). **No cifra:** las operadoras ven el tráfico (IP, puertos, SNI);
solo impide que alguien inyecte o repita paquetes. Sirve para que cengarde
vea los flujos (historia 014); el cifrado llegará después como opción.

**Todo el VPS va en un modo:** el servidor tiene un solo motor, así que
todos los routers de un VPS usan WireGuard o todos el nativo.

**En el router** (pestaña *Túnel*, *Modo del túnel*; `tunnel_mode
'native'`), cengarde mantiene, en lugar de `wgcg` y su par:
- una interfaz `cg0` (`proto static` sobre el dispositivo `cg0`, que el
  motor crea al arrancar) con la misma dirección `/32`, el mismo MTU y los
  mismos DNS. netifd la levanta cuando aparece el dispositivo y la baja
  cuando el motor se detiene; al volver, le pone otra vez direcciones y
  rutas;
- la ruta a `10.79.0.1` por `cg0` y, con «enrutar todo», `0.0.0.0/1` y
  `128.0.0.0/1` (secciones `cengarde_route_all_*`), lo que en WireGuard
  daban las IP permitidas del par;
- con *IPv6 por el túnel*, la ULA `::2/128` en `cg0`, la ruta a `::1` y
  `::/1` y `8000::/1` (`cengarde_route6_all_*`); el NAT66 cubre `cg0`;
- `cg0` en la zona `wan`, y como interfaz externa de UPnP con IP pass;
- en el motor, `tunnel = native`, `tun = cg0`, `tun_addr` (las mismas
  direcciones) y `tun_mtu` (el MTU del túnel).

Necesita `kmod-tun`, que el paquete trae como dependencia (la Pi no lo
tenía instalado).

**En el VPS:** `sudo cengarde-vps-setup tunnel native`:
- escribe `TUNNEL="native"` en `/etc/cengarde/nat.conf`;
- en la configuración del motor, `tunnel = native`, `tun = cg0`,
  `tun_addr = 10.79.0.1/16` (más `fd79::1/8` con `ipv6 on`, que cubre la ULA
  de cada router) y, en cada `[client NOMBRE]`, `tun_route` con la dirección
  del router en el túnel (y su ULA `::2` con IPv6): por ahí le llega lo suyo
  y solo puede mandar desde ahí;
- un drop-in de systemd (`cengarde.service.d/native.conf`) que deja al motor
  abrir `/dev/net/tun` (su unidad tiene `PrivateDevices=yes`) y ningún otro
  dispositivo; por eso **reinicia el motor**: todos los routers ven un corte
  de unos segundos;
- después baja el WireGuard de cada router (`cg-NOMBRE`): su ruta al router
  le ganaría a la del TUN;
- `cengarde-nat` escribe las mismas reglas con `cg0` en lugar de `cg-*`:
  NAT, reenvío de puertos e IP pass a la dirección del router, sin llegar a
  los metadatos ni de un router a otro.

Un VPS nuevo no necesita el comando: el cloud-config de LuCI lleva
`TUNNEL=native` cuando el router está en modo nativo.

**Pasar un router y su VPS al modo nativo:**
1. En el router, actualizar a un paquete con esta versión (0.8.1 o
   posterior), que instala `kmod-tun`.
2. En el VPS, actualizar al mismo commit (`git -C /opt/cengarde fetch
   --depth 1 https://github.com/KevinashoUWU/cengarde COMMIT && git -C
   /opt/cengarde checkout FETCH_HEAD && sudo sh
   /opt/cengarde/contrib/vps/install.sh`) y luego `sudo cengarde-vps-setup
   tunnel native`. Desde aquí los routers en WireGuard quedan sin túnel.
3. En el router, LuCI: *Túnel* → *Modo del túnel* → *Nativo*, *Guardar y
   aplicar*. Por consola: `uci set cengarde.main.tunnel_mode=native && uci
   commit cengarde && /etc/init.d/cengarde reload`.
4. Comprobar: `ifstatus cg0` (`"up": true`), `ip route | grep cg0`, `ping
   -I cg0 10.79.0.1`; en el VPS, `sudo cengarde ctl status` (`"tunnel":
   "native"`) y `sudo cengarde-nat status`.

**Volver a WireGuard:** en el VPS, `sudo cengarde-vps-setup tunnel
wireguard` (vuelven los `cg-NOMBRE`, se va el drop-in, el motor se
reinicia); en el router, *Modo del túnel* → *WireGuard* (o `uci set
cengarde.main.tunnel_mode=wireguard && uci commit cengarde &&
/etc/init.d/cengarde reload`): `cg0` y sus rutas se van y `wgcg` vuelve
como estaba. En el modo WireGuard nada cambia respecto de antes.

**Conviene saber:**
- **El MTU** es el mismo ajuste (*MTU del túnel*, 1380 por omisión). Sin
  WireGuard cada paquete lleva 32 bytes menos, así que cabría más (hasta
  1448 por IPv4 y 1428 por IPv6 en un camino de 1500), pero LuCI sigue
  aceptando hasta 1416.
- **El estado** dice *Túnel nativo (sin cifrar)*: conectado mientras algún
  enlace está vivo (no hay handshakes que mirar); `cengarde-setup status`
  da `"tunnel_mode":"native"`.
- **Lo que cubre `test/e2e.sh`** (QEMU): el paso de los dos extremos a
  nativo, `cg0` con su dirección y rutas, ping sin pérdidas por el TUN, un
  reinicio del motor (netifd devuelve dirección y rutas), IPv6 dentro y la
  vuelta a WireGuard sin restos. `contrib/vps/test` cubre el VPS
  (`setup-dryrun.sh`, `nat-rules.sh`). No está probado todavía en la Pi ni
  en Vultr.

## Por consola (sin LuCI)

```sh
uci add_list cengarde.main.server='203.0.113.10'     # la IP del VPS
uci add_list cengarde.main.server='2001:db8::10'     # opcional: su IPv6
uci add_list cengarde.main.uplink='wom'              # uno por enlace
uci add_list cengarde.main.uplink_type='wom=cellular' # opcional: el tipo de cada enlace
uci set cengarde.main.enabled='1'
uci commit cengarde && /etc/init.d/cengarde reload
cengarde-setup cloud-config > vps.yaml               # el user data del VPS
```

- **Todas las opciones**, comentadas: `/etc/config/cengarde`.
- **Nombre y dirección en el VPS:** `uci set cengarde.main.vps_name=casa`;
  `uci set cengarde.main.tunnel_addr=10.79.5.6` (sin ella, la derivada).
  `cengarde-setup status` dice cuáles usa (`tunnel_addr`, `vps_name`).
- **Reordenamiento y reparto:** `uci set cengarde.main.reorder=0` lo
  apaga; `uci set cengarde.main.split=1` y `uci add_list
  cengarde.main.split_weight=wan=3` (uno por enlace) encienden el reparto
  experimental, `uci set cengarde.main.split_spread_ms=50` cambia la
  diferencia de retardo con la que un enlace sigue recibiendo parte y
  `uci set cengarde.main.split_spread_copy=0` le quita las copias al que
  queda fuera. En el archivo del motor salen como `reorder = no`,
  `split = on`, `split_spread_ms = 50`, `split_spread_copy = no` y
  `weight = 3` en la sección del enlace; `split` nunca sale como `force`
  (es del laboratorio).
- **`config_file`:** usa un archivo INI propio en lugar del generado. Sirve,
  por ejemplo, para un servidor OpenWrt.
- **Configuración que se genera:** `/var/etc/cengarde.conf`.

## Problemas conocidos

- **IP pass necesita que la IP del VPS sea pública:** miniupnpd no arranca
  con una privada o reservada. En `logread` aparece «ext_ip contains
  reserved / private address».
- **Un router sin reloj con batería (Raspberry Pi):** arranca con la hora de
  la imagen, o la del último cambio en `/etc`, hasta que el NTP la corrige
  por el túnel.
  - Por eso el estado puede mostrar un handshake «anterior al ajuste de la
    hora»: el siguiente, en unos dos minutos con tráfico, muestra su
    antigüedad real.
  - Antes de la 0.4.4, en el VPS: si el router se reinicia con la hora
    atrasada, WireGuard ignora sus handshakes (son más viejos que el último
    que aceptó) y el túnel no vuelve («esperando al VPS»). Se destraba con
    *Sistema → Sistema → Sincronizar con el navegador* en LuCI, o en el VPS
    con `systemctl restart wg-quick@wg0`. Desde la 0.4.4, el VPS lo
    destraba solo en menos de un minuto (35–38 s medidos en QEMU; historia
    002).
- **Cambiar el secreto:** en el VPS, `sudo cengarde-vps-setup add NOMBRE
  --replace` y pega el nuevo; el router se corta un momento. Si su dirección
  en el túnel sale del secreto, cambia con él: guarda y aplica en LuCI y usa
  el comando que muestra (lleva la nueva `--tunnel-addr`). La ULA de la IPv6
  por el túnel sale siempre del secreto y cambia también.
- **Actualizar:** router y VPS con el mismo commit. El protocolo es ahora
  el v5 (el enlace anterior en cada paquete de datos y el NACK del rescate):
  un router y un VPS de protocolos distintos no se hablan, así que se
  actualizan juntos, primero el VPS. `cengarde-vps-setup add` se niega si el
  protocolo del router (`--proto`) no es el del motor del VPS. La 0.5.3 no
  cambia el
  protocolo: suma el interruptor de IPv6 por el túnel, apagado. La 0.5.2
  saca la dirección del túnel del secreto (sección «Nombre y dirección de cada
  router»): un router que se actualiza conserva la `10.79.0.2` y sigue
  hablando con su VPS, uno nuevo usa la suya y su VPS tiene que ser 0.5.2. La
  0.5 cambia el protocolo (v4): un router 0.5 no habla con un VPS 0.4 ni al
  revés, así que se actualizan juntos, primero el VPS (desde el router, por
  SSH, el túnel se corta hasta que el router también tenga la 0.5). Trae las
  cookies que impiden que una sonda reenviada cree una sesión, desvíe un
  enlace o cambie el IP pass, y un VPS reiniciado retoma el túnel sin rehacer
  ventanas (historia 010, PR 3d2a). La 0.4 cambió el protocolo (v3); de la
  0.4.1 a la 0.4.7 no lo cambian (un router de una y un VPS de otra se
  entienden en los dos sentidos), pero actualiza igual el VPS:
  - la 0.4.1 cierra al túnel los metadatos y el puerto de WireGuard;
  - la 0.4.2 contesta desde la dirección a la que llegó cada paquete
    (antes, en un VPS con varias IP o con IPv6, el router descartaba las
    respuestas que salían desde otra);
  - la 0.4.3 escucha con una cola por enlace (`lanes`, 8 por omisión), así
    que un parón corto del VPS ya no se lleva todas las copias de un
    paquete; `lanes = 1` en `/etc/cengarde/cengarde.conf` y
    `systemctl restart cengarde` vuelven al socket único de antes;
  - la 0.4.4 destraba un router que vuelve con la hora atrasada: hace que
    WireGuard inicie el handshake (`wireguard_poke`, `none` lo apaga);
  - la 0.4.5 renueva las herramientas del VPS: `cengarde-vps-setup`,
    `cengarde-nat` declarativo, `wg0` pasa a `cg-router` y el servicio a un
    usuario fijo (abajo, qué cambia al actualizar y cómo volver);
  - la 0.4.6 trae al router los hilos por enlace (`link_threads`, pestaña
    *Avanzado* → *Hilos por enlace*): por omisión `legacy`, el mismo bucle
    de antes, y *Activado* (`on`, un hilo por enlace) solo si lo eliges a
    mano, hasta medirlo en la Pi; cambiarlo reinicia el motor. El estado
    avisa de un hilo de enlace parado con paquetes esperando
    (`link_thread_stalled`), y `cengarde ctl threads` lista los hilos en
    los dos extremos. En OpenWrt 24.10, `opkg install` de un paquete con la
    versión ya instalada no hace nada («up to date»): para probar una
    compilación con la misma versión, `opkg install --force-reinstall`; en
    25.12, `apk add` la reemplaza sin más.
  - la 0.4.7 trae el registrador de campo (`cengarde-rec`, apagado: ver
    [Registrar los enlaces durante días](#registrar-los-enlaces-durante-días)),
    `status_interval_ms` en UCI y la hora de pared (`time_ms`) en el estado
    de los dos extremos.

  Un VPS de la 0.4.5 a la 0.5.0 (antes de los varios routers por VPS) no
  pasaba de la 0.6.0 o la 0.6.1: el paso 5/6 de `install.sh` pedía al motor
  viejo, todavía en marcha, que recargara la configuración nueva, y la
  rechazaba («key: expected the base64 of 32 bytes»; la anterior quedaba
  en su lugar y el motor viejo seguía andando). Desde la 0.6.2, `install.sh`
  deja la configuración al reinicio del paso 6/6. Si te pasó, vuelve a
  correr la actualización con la 0.6.2 o posterior.

  En el VPS:

  ```sh
  sudo git -C /opt/cengarde fetch --depth 1 https://github.com/KevinashoUWU/cengarde <commit>
  sudo git -C /opt/cengarde checkout FETCH_HEAD && sudo sh /opt/cengarde/contrib/vps/install.sh
  ```

  La 0.4.2 convierte una vez, al instalarse, `option server` en
  `list server`; una `option server` escrita a mano después se sigue
  leyendo.
  - **Desde una sesión SSH,** `install.sh` sigue como una unidad propia
    (`cengarde-upgrade`): si la sesión se corta, la actualización termina
    igual. Su salida queda en `/var/log/cengarde-upgrade.log`. Si algo la
    interrumpe, ejecútalo otra vez: termina lo que faltaba.
  - **De una 0.4 anterior a la 0.4.5** (sin cambio de protocolo): `wg0`
    pasa a ser `cg-router`, con el mismo puerto y la misma dirección del
    túnel, el servicio pasa a un usuario fijo y el router sigue funcionando
    sin tocarlo.
  - **Cuando cambie el protocolo** (la 0.5), un extremo actualizado no
    habla con el otro, y con «todo por el túnel» tu SSH al VPS va por ese
    túnel. Sigue este orden: baja antes los paquetes nuevos del router;
    desactiva cengarde en LuCI (el router y la LAN salen directo por los
    módems, y el SSH al VPS también); actualiza el VPS; instala los
    paquetes del router; activa cengarde.
- **Volver a la 0.4.2:** en el VPS, `sudo cengarde-vps-setup purge` (quita
  los servicios y reglas nuevos y deja el secreto del router en
  `/etc/cengarde/secret`), luego `git checkout` del commit de la 0.4.2 y su
  `install.sh`. Solo con el `install.sh` viejo no basta: `cg-router` sigue
  ocupando el puerto 65501.


## Probar sin hardware

[`test/e2e.sh`](test/e2e.sh) arranca dos VMs con la imagen x86-64 y
comprueba el flujo completo:
- enlaces por DHCP;
- configuración solo desde LuCI;
- túnel arriba, e IP pass confirmado por el VPS;
- la dirección del router en el túnel, derivada del secreto: `wgcg` con su
  `/32`, el estado y el cloud-config con el nombre y la dirección, una
  `tunnel_addr` inválida rechazada, y la ruta a `10.79.0.1` que sigue
  llevando por `wgcg` sin «enrutar todo»;
- compañeras IPv6 `<enlace>6` y `delegate 0` en un enlace PPPoE;
- IPv6 hasta el VPS: cada enlace sale desde su dirección global (no la
  ULA), el VPS contesta desde la suya (`*`), el aviso de MTU de un camino
  de 1400, el paso a la IPv4 cuando un enlace pierde la IPv6 y cuando el
  VPS pierde su dirección IPv6, y la fuga cerrada: un prefijo delegado
  (como el de Starlink) no llega a la LAN mientras todo va por el túnel, y
  vuelve al desactivar;
- pausar y reanudar un enlace desde la página de estado;
- las opciones de reordenamiento y reparto, y el peso de cada enlace, en el
  archivo del motor (lo inválido se omite y se registra), que el motor
  acepta; y en LuCI, los rangos y la lista de pesos;
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
- Sin VMs ni navegador, `node openwrt/test/views.mjs` corre `config.js` y
  `status.js` contra un LuCI de mentira (opciones y rangos del
  reordenamiento, lista de pesos, caja de *Reordenamiento*, y que cada
  texto tenga su traducción); el CI lo ejecuta en `lint`.

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
