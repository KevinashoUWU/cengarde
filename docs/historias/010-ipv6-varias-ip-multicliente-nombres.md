# 010 — IPv6, dirección de llegada, varios routers por VPS y nombres

- **Fecha:** 2026-10-04
- **Estado:** vigente. PR 1 y PR 2 hechos (paquetes 0.4.1 y 0.4.2,
  protocolo v3 sin cambios); del PR 3, hecho el 3d1 (herramientas del VPS,
  aún v3); el resto del PR 3 y los PR 4 y 5, pendientes.
- **Fuentes:**
  - código del PR 1 (`git log ebe570b..823e4dd`):
    - `engine/src/epoch.h`; `client.c:442` (`ms_since_new`), `:456`
      (`started_over`), `:420` y `:596` (anillo de ecos);
    - `engine/src/config.c:244` (`get_addrs`); `util.c:176`
      (`cg_addr_unmap`) y `:189` (`cg_addr_unfit_peer`);
    - `engine/src/pair.c:46-78` y `pair.h`; `main.c:110` (`keys`);
    - `contrib/vps/cengarde-nat:76` (`base`) y `:101` (`base6`);
      `contrib/vps/cengarde-vps.conf`; `contrib/vps/install.sh`;
    - `openwrt/cengarde/files/cengarde-setup:193` (`upnp_ip`), `:203`
      (`fix_stun`) y `:399` (`upnp_no_ip`); `config.js` y `status.js`;
    - antes del PR 1, en `ebe570b`: `config.c:257` (direcciones de más
      descartadas en silencio) y `cengarde-nat:73` (DROP solo en `PUB_IF`);
  - código del PR 2 (`git log 1c27532..`, paquetes 0.4.2):
    - `engine/src/pktinfo.h`; `server.c` (`path_update`, `flush_replies`);
      `sock.c` (`cg_udp_bind_opts`);
    - `engine/src/srvpick.h` y `addrpick.h`; `client.c` (`link_open`,
      `path_check`); `netlink.c` (`on_addr`, `cg_addr_slot`);
    - `openwrt/cengarde/files/cengarde-setup` (`companions`,
      `fix_companions`, `keep_off`, `leaked_prefix`, `engine_problems`),
      `cengarde.defaults` (migración a `list server`), `config.js` y
      `status.js`;
    - `contrib/vps/test/systemd.sh` y el trabajo `systemd` de `vps.yml`;
  - commits del PR 1: `7ae94a8` (reinicio del servidor), `b7da949` y `4f2f6d8`
    (metadatos y WireGuard), `a69fa64` y `79b4d9f` (validación), `0f13648`
    y `9296855` (LuCI y STUN), `bfe114e` (emparejamiento), `d99d5ec`,
    `7fcd131`, `1f6b666`, `b8eff95`, `c202376`, `53f3b83`, `d0ce648` y
    `09ea6e7` (pruebas), `823e4dd` (paquetes 0.4.1);
  - kernel Linux v5.10 y v6.12, leído: `net/socket.c` (`____sys_sendmsg`,
    búfer de control en la pila de `sizeof(struct cmsghdr) + 20` bytes),
    `IP_PKTINFO`/`IPV6_PKTINFO`, `sendmmsg`, `ip6_route_get_saddr`;
  - OpenWrt, leído el 2026-10-04:
    - master de netifd `06d06c8` (`interface.c:105-106`, `ip6class` y
      `delegate`), odhcpd `68f3826` (`config.c:173`, `ra_default`),
      odhcp6c `10a5222` y firewall4 `c2ae8c8` (25.12.5 empaqueta netifd
      `cbb83a1`, odhcpd `5d7be43` y firewall4 `b6e5157`);
    - de las ramas `openwrt-24.10` y `openwrt-25.12`: `dhcpv6.sh`
      (`delegate`), `wireguard.sh` (IPv6) y el `validation.js` de LuCI
      (tipo `host`); `board.d/02_network` de la imagen 25.12.5 de la Pi 4;
      miniupnpd 2.3.9 (STUN);
    - de 25.12.5: `alias.c` de netifd, `dhcp.sh`, `ppp.sh` y `ppp6-up`,
      `qmi.sh`, `mbim.sh` y `ncm.sh` (`delegate`);
  - systemd: `systemd.service(5)` (`RemainAfterExit`), `LoadCredential=`
    (v249, v255 y v260), `IPAddressDeny=`; unidades `.path` en la
    historia 009;
  - Vultr, consultado el 2026-10-04 en <https://docs.vultr.com/>:
    `products/network/reserved-ips` (y `…/management/convert-existing-ip`),
    `support/products/network/are-reserved-ips-free`,
    `how-to-configure-multiple-public-ipv4-addresses-on-a-vultr-compute-instance`
    y `products/compute/instances/cloud-compute/networking/ipv6`; y
    `netplan generate` 1.1.2;
  - respuestas del usuario, 2026-10-04;
  - medidas: `sudo bench/lab.sh restart [REF]`, `multiip`, `fallback`,
    `down` y `up` (antes y después), `sudo bench/lab.sh ci`,
    `sudo sh contrib/vps/test/security.sh` (y `vps.yml`) y
    `openwrt/test/e2e.sh` con 25.12.5.

## TL;DR

- **Cuatro pedidos, cinco PRs y un solo cambio de protocolo:**
  1. arreglos, sin cambio de protocolo (**hecho**, 0.4.1);
  2. respuesta desde la dirección de llegada, IPv6 por fuera del túnel y
     cierre de la fuga de IPv6 de la LAN (**hecho**, 0.4.2);
  3. varios routers por VPS, con el protocolo v4 y un panel de reenvío de
     puertos;
  4. IPv6 dentro del túnel, apagado por defecto;
  5. nombres en vez de IP, pensados para DNS dinámico.
- **Lo que cerró el PR 1:**
  - un reinicio del servidor ya no deja la bajada atascada: antes pasaba en
    7 de 10 reinicios; después, en 0 de 54, y vuelve en 2,0–2,1 s cuando
    hay que rehacer la ventana (`bench/lab.sh restart`);
  - la LAN ya no lee por el túnel los metadatos del VPS, donde está el
    secreto (probado en netns), y el motor tampoco (`IPAddressDeny`, sin
    probar en un systemd real);
  - el puerto de WireGuard del VPS solo contesta en local: ya no se llega
    a él desde el túnel (probado en netns) ni por IPv6 (solo comprobado que
    las reglas existen);
  - configuración más estricta, MTU de LuCI hasta 1416, STUN solo con un
    servidor explícito, y direcciones de túnel y pista de cliente
    derivadas del secreto (aún sin usar).
- **Lo que cerró el PR 2:**
  - un VPS con varias IP, o con IPv6, ya funciona: el servidor contesta
    desde la dirección a la que llegó cada paquete (en el laboratorio, con
    el motor anterior, los enlaces que iban a una dirección secundaria
    nunca quedaban vivos);
  - el router acepta hasta 8 direcciones del VPS, IPv4 e IPv6; cada enlace
    usa la primera que puede y pasa a la siguiente tras 10 s sin
    respuesta, y elige su IPv6 de origen como RFC 6724 (nunca la ULA si hay
    una global);
  - la IPv6 de los módems llega al motor con una interfaz DHCPv6 por
    enlace, y la LAN ya no recibe por fuera del túnel el prefijo que
    delega un enlace como Starlink (probado en QEMU con un prefijo
    delegado);
  - aviso cuando el MTU del camino de un enlace no deja pasar el túnel.
- **Decisiones del usuario que cambian el diseño:**
  - la IPv4 reservada, si la hay, es la IP principal del VPS: nada de IP
    extra ni de IP propia 1:1 por router;
  - el IP pass con varios routers es un panel de reenvío de puertos;
  - IPv6 dentro del túnel apagado por defecto;
  - solo Starlink da IPv6 hoy, con un prefijo delegado: la IPv6 de la LAN
    podía salir por fuera del túnel si el enlace Starlink tenía una
    interfaz DHCPv6 que lo delegaba; desde el PR 2, cengarde le pone
    `delegate '0'` mientras todo va por el túnel;
  - el "VPS" puede ser un Debian o Ubuntu casero con IP dinámica, quizá
    detrás de un router, encontrado por DNS dinámico.
- **Lo que cerró el PR 3d1** (solo VPS, protocolo v3): `cengarde-nat`
  declarativo, sin nombres de interfaz y con hairpin; un archivo por router
  en `/etc/cengarde/clients` con su `cg-NAME`; el secreto del cloud-config
  importado una sola vez; usuario fijo; `install.sh` que sobrevive a una
  sesión SSH cortada; vuelta a la 0.4.2 con `purge`. Un router 0.4 sigue
  funcionando sin tocarlo (sección «PR 3d1»).
- **Siguiente:** el PR 3d2, protocolo v4 y varios routers por VPS.

## Contexto

El usuario pidió IPv6, un servidor con varias IP (una IPv4 o IPv6 reservada
de Vultr), varios routers en un mismo VPS y nombres en vez de IP. La
investigación dio un diseño (29 decisiones, cinco PRs) y seis preguntas.
Aquí queda lo que sigue en pie tras las respuestas, sus hechos y el PR 1.

**Fallos que encontró la investigación** (reproducidos en netns o en QEMU,
o leídos en el código):
- **VPS con varias IP:** el servidor contesta desde la dirección que elige
  el kernel y el router descarta la respuesta (historia 008, punto 4).
- **Origen IPv6:** con una ULA listada antes que la GUA, el cliente sale
  hacia un VPS global desde la ULA, que no tiene ruta.
- **Reinicio del servidor:** la bajada quedaba atascada hasta reiniciar el
  cliente (6 de 10 y 3 de 8 reinicios en la investigación).
- **IP pass:** una sonda capturada y reenviada tras un reinicio del
  servidor crea una sesión que, por ser la más nueva, decide el IP pass
  (corrección en la historia 009). Sigue abierto hasta el PR 3.
- **Metadatos del VPS:** el VPS reenviaba hacia ellos el tráfico de la
  LAN; en netns, con un servicio falso, la LAN leía el secreto.

## Decisiones del usuario (2026-10-04)

1. **IPv4 reservada:** la opción recomendada. Si el usuario reserva una,
   pasa a ser la IP principal del VPS; si no, se usa la de la instancia. No
   hacen falta herramientas para IP extra (`ip add`, 1:1 por sitio).
2. **IP pass con varios routers:** no el 1:1, sino un panel de reenvío de
   puertos como el de cualquier router: el VPS tiene su IP principal, y
   cada regla dice protocolo, puerto o rango del VPS, router destino y
   puerto destino (por ejemplo, `IP_VPS:9000 → router 1, puerto 9000`).
3. **IPv6 dentro del túnel:** apagado por defecto en 0.5; se enciende
   después de probarlo en la Pi.
4. **IPv6 en los enlaces:** hoy solo Starlink, con IPv6 nativa y un prefijo
   delegado; su IPv4 va detrás de CGNAT. No se sabe si el VPS tiene IPv6
   (también en la historia 004).
5. **Nombres:** por todas las razones, incluido alojar el "VPS" en casa o
   en un operador con IP dinámica, encontrado por DNS dinámico. Importa
   volver a resolver cuando cambia la IP, y el servidor puede ser cualquier
   Debian o Ubuntu, quizá detrás de un router con un reenvío de puerto.

Sin respuesta: cuántos routers por VPS. Se mantiene el límite del diseño,
32 (puertos de WireGuard 65501–65532).

## Hallazgos: hechos en los que se apoya el plan

### Kernel (leído en v5.10 y v6.12)

- **Un socket comodín responde desde la dirección que elige la ruta,** no
  desde la de llegada. `IP_PKTINFO` (IPv4, `ipi_spec_dst`) e
  `IPV6_PKTINFO` (con ifindex 0) fijan el origen por mensaje.
- **Un socket `[::]` de doble pila** con `IPV6_RECVPKTINFO` informa
  también las llegadas IPv4, como direcciones v4-mapped. Solo leído: el
  contenedor no tiene IPv6.
- **Búfer de control en la pila:** `____sys_sendmsg` copia hasta
  `sizeof(struct cmsghdr) + 20` bytes de control sin `kmalloc`. Con
  `msg_controllen = CMSG_LEN(20)` (36 B en 64 bits) el `IPV6_PKTINFO` cabe.
- **`sendmmsg` se detiene en el primer error:** una dirección local que
  desaparece cortaría las respuestas del resto del lote. Hace falta un
  bucle que salte solo el mensaje que falla.
- **Linux da alcance global a las ULA:** netlink no las distingue, así que
  el rango se decide por el prefijo (fc00::/7), como en RFC 6724.
- **Origen sin especificar en IPv6:** `ip6_route_get_saddr` elige entre
  todas las interfaces. Como odhcp6c instala rutas por defecto por origen,
  una consulta DNS por un enlace tiene que atarse a su dirección.
- **WireGuard del kernel escucha en `0.0.0.0` y en `[::]`:** solo un DROP
  deja su puerto fuera del alcance de los demás.

### Vultr

- **IP reservada como principal:** llega por DHCP y sobrevive a
  reconstruir la instancia. Se convierte la actual o se despliega con ella.
  Cuesta US$3 al mes.
- **Una IPv4 extra** ya no hace falta: se configura a mano, y la plantilla
  de netplan de Vultr choca con la de cloud-init (`netplan generate` 1.1.2).
- **IPv6:** *Settings → IPv6 → Assign IPv6 Network* y reiniciar desde el
  panel. Vultr no documenta un prefijo enrutado hacia la instancia, de ahí
  ULA más NAT66 para la LAN.
- **El user data, con el secreto,** se ve en el panel y desde el servicio de
  metadatos, 169.254.169.254 (historia 008).
- **Sin verificar:** si el servicio de metadatos contesta a pedidos
  reenviados desde el túnel (el reenvío sí se probó en netns), y si una IP
  reservada añadida en caliente pide reiniciar.

### OpenWrt (24.10 y 25.12)

- **netifd:** una interfaz DHCPv6 compañera por enlace (`proto dhcpv6`)
  le da al motor la IPv6 del módem. `delegate 0` no pasa su prefijo a la
  LAN y `peerdns 0` no usa sus DNS. Las mismas opciones existen en las dos
  ramas.
- **Dispositivo de la compañera:** el del enlace, como el `wan6` de
  fábrica; `@enlace` solo si el enlace no tiene uno propio (un cliente
  wifi). Un alias `@enlace` existe solo mientras el enlace está arriba
  (`alias.c`, `alias_check_state`): un fallo de DHCPv4 se llevaría también
  su IPv6.
- **Interfaces IPv6 dinámicas:** PPP, qmi, mbim y ncm crean `<iface>_6` y
  le copian `delegate 0` solo desde la interfaz madre (`ppp6-up`,
  `qmi.sh`, `mbim.sh`, `ncm.sh`). Por eso, mientras todo va por el túnel,
  cengarde pone `delegate 0` en los enlaces que no son DHCP; en los DHCP
  no, porque `dhcp.sh` declara `delegate` (solo para 6rd) y netifd
  reiniciaría su cliente DHCP. Las compañeras de cengarde no usan ese
  sufijo, y `ipv6_leak` queda como aviso para otras fuentes.
- **La Pi 4 no trae `wan6`:** de fábrica solo tiene `lan`
  (`board.d/02_network`). La fuga de IPv6 aparece solo si alguien crea
  una DHCPv6 que delega sobre el enlace Starlink, o si Starlink va al
  puerto WAN de un equipo con `wan6` de fábrica, que delega.
- **fw4:** acepta includes de nftables (NAT66 hacia `wgcg`). No está
  verificado si aplica la zona de una interfaz por compartir dispositivo:
  la zona se añade explícita.
- **odhcpd:** `ra_default 1` anuncia ruta por defecto con solo una ULA.
- **`wireguard.sh`** maneja direcciones y `allowed_ips` IPv6; LuCI tiene
  el tipo de dato `host` (nombre o IP).
- **miniupnpd 2.3.9:** no arranca con `use_stun` y sin `ext_stun_host`, y
  rechaza un `ext_ip` privado o reservado (historia 008).

### systemd

- **`LoadCredential=`** copia el archivo al arrancar. Refrescarlo en una
  recarga solo existe desde v260; Ubuntu 22.04 trae 249 y 24.04 trae 255.
  Por eso hoy el VPS reinicia en vez de recargar (historia 009), y el PR 3
  pasa a un usuario fijo con el archivo en 0640 root:cengarde.
- **`IPAddressDeny=`** existe en 249 y 255 (usa BPF de cgroups).
- **Un `Type=oneshot` sin `RemainAfterExit=yes`** nunca queda activo, y
  systemd corre `ExecStop` justo después de `ExecStart`: borraría las
  reglas de NAT en cada arranque.
- **Unidad `.path`:** dispara con el reemplazo atómico (historia 009). Sin
  probar en un systemd real; el contenedor no lo tiene.

## Lo que cambian las respuestas

Solo lo que las respuestas cambiaron o ya está hecho; el resto del diseño
sigue igual (ver «Pendiente, por PR»).

| Decisión | PR | Tras las respuestas |
| --- | --- | --- |
| Arreglo inmediato del reinicio (anillo de ecos) | 1 | Hecho. |
| Emparejamiento: dirección de túnel, ULA y pista derivadas | 1 | Hecho; se usan en el PR 3. |
| MTU: 1380, LuCI 1280–1416, tope 1396 con un literal IPv6, `path_mtu` | 1–2 | Hecho. |
| Responder desde la dirección de llegada: un socket `[::]` con pktinfo | 2 | Hecho. Hacía falta igual: IPv4 e IPv6 en el VPS (SLAAC, temporales) y un servidor casero con varias direcciones. |
| Compañeras DHCPv6 (`delegate 0`, `peerdns 0`) y cierre de la fuga aunque cengarde no las haya creado | 2 | Hecho; pasó a ser lo urgente: Starlink delega un prefijo. |
| `cengarde-nat` declarativo con su unidad (`RemainAfterExit`) | 2 | **Movido al PR 3,** junto al panel de puertos que lo reescribe; el PR 2 prueba en un systemd real el `cengarde-nat` de hoy. |
| IP extra en el VPS e IPv4 propia 1:1 por router (`--public-ip`, `pass_ip`, SNAT fijo, despacho con `-g`) | 2–3 | **Sustituida** por la decisión 1: la reservada es la IP principal. Fuera también `extra-ips` y el netplan para IP extra. |
| IP pass con varios routers: la IP principal para uno y 1:1 para los demás | 3 | **Sustituida** por el panel de reenvío de puertos (abajo). |
| IPv6 dentro del túnel: ULA y NAT66 en los dos extremos | 4 | Apagado por defecto. El lado VPS no puede suponer netplan. |
| Nombres: resolvedor propio por los enlaces, TTL entre 30 s y 15 min, última respuesta buena, nueva consulta si todos los enlaces del nombre quedan mudos | 5 | **Pesa más:** DNS dinámico y servidor casero. |
| IP externa de UPnP: `pass_ip`, primer IPv4 literal, IPv4 resuelto, STUN con servidor | 1, 2, 5 | Sin `pass_ip`. Con IP dinámica, el IPv4 resuelto envejece: STUN sigue los cambios. |

**Panel de reenvío de puertos** (propuesta, se cierra en el PR 3):
- **Regla:** protocolo (tcp, udp o ambos), puerto o rango del VPS, router
  destino y puerto destino. DNAT hacia la dirección de túnel de ese router;
  dentro, el router reparte con su cortafuegos o con UPnP, como hoy.
- **Lo que sobrevive del IP pass:** el rango completo 1024–65000 para un
  router (el de hoy). Las reglas explícitas van antes, así que ganan.
- **Siempre fuera:** el puerto de cengarde, el rango de WireGuard, el SSH, y
  un puerto pedido por dos routers.
- **UPnP:** solo tiene sentido en el router con el rango completo; en los
  demás anunciaría puertos que nunca llegan.
- **Por decidir:** dónde vive el panel. En el VPS
  (`cengarde-vps-setup forward add|del|list`) ve todos los routers y los
  choques. En la LuCI de cada router se parece más a lo pedido, pero
  necesita llevar las reglas por el protocolo, que cambia en el mismo PR.

**Servidor casero con IP dinámica** (consecuencias, sin probar):
- `contrib/vps` no puede suponer Vultr, cloud-init ni netplan: hace falta
  una instalación sin cloud-config (`install.sh` más `add` con el secreto
  pegado) y, en el PR 4, `accept_ra` por sysctl en vez de netplan.
- **Detrás de un router casero:** este reenvía al servidor el UDP 65500 y,
  con IP pass, los puertos reenviados. La respuesta desde la dirección de
  llegada (PR 2) sale con la privada y el router casero la traduce.
- **Cuando cambia la IP:** los enlaces quedan mudos, el resolvedor vuelve a
  consultar y se mudan. El corte dura la detección más una consulta: hay
  que medirlo en el escenario `names` del PR 5.
- **UPnP:** STUN con servidor explícito da la IP pública del router casero
  y sigue sus cambios.
- **El servidor necesita una IPv4 pública** (sin CGNAT), con el UDP 65500
  reenviado si está detrás de un router: detrás de CGNAT no sirven ni el
  DNS dinámico ni el reenvío. Un servidor solo IPv6 sigue fuera.

## PR 1: arreglos sin cambio de protocolo (hecho)

Paquetes 0.4.1, protocolo v3: un router 0.4.0 sigue hablando con un VPS
0.4.1 y al revés. El arreglo del reinicio va en el cliente (router) y el de
los metadatos en el VPS.

### Reinicio del servidor (`epoch.h`, `client.c`)

- **El fallo:** un servidor reiniciado abre la sesión con una secuencia
  aleatoria. Más o menos la mitad de las veces cae más de 8128 paquetes por
  detrás de lo que marcó la ventana del cliente, y toda la bajada, sondas
  incluidas, queda "demasiado vieja" para siempre.
- **Por qué no basta "respuesta vieja, rehacer":** las respuestas de sonda
  comparten la secuencia con los datos. Un enlace más de 8128 paquetes por
  detrás (una cola de 800 ms a más de 10 kpps) da respuestas viejas mientras
  los demás traen paquetes nuevos.
- **La regla** (`cg_restart_reply`, pura): se rehace la ventana con una
  respuesta de sonda vieja solo si:
  1. su MAC verifica;
  2. su eco consume una entrada, de 10 s como mucho, del anillo de las
     últimas 64 sondas de ese enlace;
  3. no llegó nada nuevo verificado por ningún enlace en
     2 × `probe_idle_ms` (2 s por omisión).

  Un paquete de datos nunca rehace la ventana.
- **Por qué 64 sondas y no 4:** con tráfico sale una sonda cada 100 ms;
  4 cubrían 400 ms y un enlace con 800 ms de cola nunca casaba. 64 cubren
  6,4 s.
- **Por qué el ts exacto:** prueba la frescura mejor que un rango, porque
  el ts de 32 bits viaja en claro y da la vuelta cada 71,6 min. Las
  respuestas normales también consumen su eco: una copia reenviada después
  no cuenta.
- **Coste:** solo las respuestas de sonda viejas pagan ahora un MAC en el
  cliente; los datos viejos siguen sin pagarlo. El anillo ocupa 528 B por
  enlace en x86_64, dentro de `struct link`, sin `malloc`.
- **Al reabrir el socket de un enlace,** su último paquete nuevo pasa a
  `new_before_open_ms`, para que la condición 3 no se engañe.
- **Estado:** `download.window_resets` y el log, limitado, "the server
  started over, download window reset".
- **Riesgo que queda:** al rehacer la ventana pueden volver a entrar
  paquetes viejos que caigan dentro de ella; WireGuard los descarta con su
  propio anti-replay. La solución de raíz es la continuidad de secuencia del
  v4 (PR 3), con la que esta red no debería saltar nunca.

### VPS: metadatos y puerto de WireGuard (`cengarde-nat`, `install.sh`)

- **Metadatos:** REJECT (`icmp-net-prohibited`) de `wg0` hacia
  169.254.0.0/16, por encima del ACCEPT del túnel. Cada `up` lo vuelve a
  poner en orden si falta uno de los dos (un `down` cortado, o borrado a
  mano).
- **El motor tampoco llega:** drop-in
  `/etc/systemd/system/cengarde.service.d/vps.conf` con
  `IPAddressDeny=169.254.0.0/16`, solo en el VPS. En la unidad genérica
  estorbaría a un cliente en otra nube con un DNS de enlace local como
  169.254.169.253. Sin probar: lo hará el trabajo con systemd del PR 2.
- **WireGuard:** el DROP de su puerto pasa de `-i PUB_IF` a `! -i lo`:
  antes se llegaba desde el túnel. Con IPv6, `ip6tables` abre el puerto de
  cengarde y cierra el de WireGuard, que escucha también en `[::]`.
- **`down`** lo quita todo, también la forma 0.4 del DROP.
- **Sin cambiar:** la entrada desde el túnel al propio VPS sigue la
  política del sistema (ufw). Restringirla cortaría el SSH desde la LAN,
  que el PR 3 necesita para agregar routers.
- **Queda:** cualquier proceso root del VPS puede leer el user data. Los
  secretos de los demás routers (PR 3) nunca van en él.

### Configuración, LuCI y UPnP

- **Motor** (`config.c`, `util.c`):
  - `listen` y `wireguard` con más de una dirección son un error;
  - `server` admite como mucho 4 entradas, y un nombre que resuelve a
    varias direcciones nunca empuja fuera a las siguientes;
  - el comodín (`*`, 0.0.0.0, `::`) y las multicast no valen como servidor;
  - una v4-mapped pasa a IPv4, para salir por un socket IPv4.
- **LuCI:** `server` y `dns` con `ipaddr(1)` (sin máscara); MTU entre 1280
  y 1416, el máximo sobre IPv4 con una ruta de 1500.
- **UPnP** (`upnp_ip`): IP del VPS si es IPv4; si no, STUN solo con
  `stun_host` (y `stun_port`, nuevos en la pestaña *Túnel*); si no hay
  servidor, nada, y el problema `upnp_no_ip` en el estado.

### Emparejamiento (`pair.c`, `main.c`)

- **Etiquetas nuevas** `tunnel` y `tunnel-ula`, y la pista de cliente, con
  la misma derivación BLAKE2s de la historia 008; fórmulas en
  `engine/README.md` (`cengarde keys`).
- **`cengarde keys`** imprime `CG_TUNNEL_ADDR`, `CG_TUNNEL_ULA` y
  `CG_CLIENT_HINT` después de las cuatro líneas de siempre, así que el
  `eval` de los scripts no cambia. `cengarde version` añade
  "(protocol 3)".
- **Aún sin usar:** el túnel sigue en 10.79.0.2/30 y ningún paquete lleva
  la pista. Al agregar el router k, la dirección derivada choca con
  probabilidad (k − 1)/65533: un 0,05 % en el 32.º, y `add` lo detectará.

### Infraestructura de pruebas

- **Tests unitarios:** el Makefile compila cada `tests/test_*.c`, y cada
  suite es una línea en `tests/suites.h`.
- **Laboratorio:** `bench/lab.sh ci` corre smoke, health, control y cada
  `bench/lab.d/*.sh` con `LAB_CI=1`; `engine.yml` solo llama a `build` y a
  `ci`. `RUN` pasa a ruta absoluta, porque el motor solo acepta un
  `control_socket` absoluto (y la ruta de un socket Unix no pasa de
  107 bytes). `udpgen -l` sigue a su par cuando cambia de puerto, como
  WireGuard.
- **VPS sin VPS:** `contrib/vps/test/security.sh` en netns con iptables
  reales, en el nuevo `vps.yml`, que falla si no hay IPv6. Los scripts
  pasan shellcheck en `openwrt.yml`.

## PR 2: dirección de llegada, IPv6 por fuera y fuga de IPv6 (hecho)

Paquetes 0.4.2, protocolo v3 sin cambios: un router 0.4.1 habla con un VPS
0.4.2 y al revés. Lo que arregla el VPS (contestar desde la dirección de
llegada) pide actualizar el VPS; lo del router (lista, IPv6, fuga), el
router.

### Servidor: respuesta desde la dirección de llegada (`pktinfo.h`, `server.c`)

- **Solo con un `listen` comodín** (`*` o `0.0.0.0`, el de
  `cengarde-vps-setup`): pide `IPV6_RECVPKTINFO` (o `IP_PKTINFO` sin IPv6)
  y responde por cada camino con `IPV6_PKTINFO` (ifindex 0, `CMSG_LEN(20)`)
  o `IP_PKTINFO` (`ipi_spec_dst`). Si el kernel no lo acepta, avisa y sigue
  como antes.
- **Se aprende solo después del MAC y de marcar el anti-replay,** como la
  dirección del cliente. El mensaje de control de cada camino se arma una
  vez y se copia en cada respuesta, sin `malloc` ni log por paquete.
- **Un error no corta a los demás:** las respuestas de sonda salen en un
  bucle que salta solo el mensaje que falla (`sendmmsg` se detiene en el
  primer error), y cada camino envía su lote aparte. Una dirección que
  desaparece suma `local_errors` en su camino, con un log limitado, y el
  camino sigue.
- **Estado:** `listen`, `reply_from_arrival`, `rx.ctrunc` y, por camino,
  `local`, `moves` y `local_errors`; la columna LOCAL en `ctl links`.
- **Hilos:** el servidor sigue con un hilo; el comentario que daba el
  mensaje de control por listo para varios hilos se corrigió. Publicarlo
  bien (dos búferes o un contador de secuencia) queda para el servidor
  multihilo.

### Cliente: lista de direcciones, origen IPv6 y MTU de camino

- **`server` es una lista ordenada** de hasta 8 entradas (global o por
  `[link]`); un nombre da hasta 4 direcciones por familia y se resuelve al
  leer el archivo, como antes (el PR 5 cambia eso).
- **Una familia por enlace** y failover pegajoso (`srvpick.h`): la primera
  entrada de una familia que el enlace tiene; tras `server_failover_ms`
  (10 s; 0 lo apaga; como mínimo 3 × `probe_idle_ms`) sin respuesta
  verificada, la siguiente. Vuelve a la primera solo si:
  - cambia o pierde su dirección local (también si vuelve la misma);
  - gana o pierde una familia con entradas en su lista;
  - una recarga cambia las entradas de su familia (otra familia, o un
    nombre que da las mismas direcciones en otro orden, no cuentan);
  - llega la primera respuesta tras una ronda entera sin respuestas.
- **Origen IPv6** (`addrpick.h`), por rango de RFC 6724: nunca tentativa,
  fallida o de enlace local; la ULA hacia un destino que no es ULA va
  última, luego las obsoletas, las temporales, y primero las estables.
  Entre iguales, la más nueva, como el kernel: así un prefijo nuevo
  (renumeración brusca, RFC 8978) se usa en marcha igual que al arrancar.
- **MTU de camino:** cada 5 s lee `IP_MTU`/`IPV6_MTU` de cada socket y lo
  compara con el datagrama de WireGuard más grande de esos 5 s (un `if` por
  paquete); si no cabe, avisa cada 10 min por enlace con el MTU de
  WireGuard que cabría. Estado: `family`, `candidate`, `candidates`,
  `failovers`, `path_mtu` y `reason` por enlace, y `upload.largest`.
- **Encontrado de paso:** un enlace que perdía su única dirección y
  recuperaba la misma no se notaba hasta la siguiente vuelta de 5 s
  (`netlink.c`); ahora cuenta como cambio al momento.

### OpenWrt: lista, compañeras IPv6 y fuga

- **`list server`** en lugar de `option server`; `cengarde.defaults` lo
  convierte una vez al instalar. UPnP anuncia la primera IPv4 de la lista.
- **Compañeras `<enlace>6`** (DHCPv6, `reqprefix no`, `delegate 0`,
  `peerdns 0`, marca `cengarde_owned`) sobre el dispositivo del enlace, en
  su zona, con su métrica; `uplink_ipv6 '0'` las quita. Si el nombre ya
  existe y no es suyo, no lo toca y avisa (`ipv6_companion_conflict`). Si
  el enlace ya tiene una DHCPv6 propia (como el `wan6` de fábrica), no
  crea otra.
- **Fuga:** con «enrutar todo», `delegate 0` y `peerdns 0`, con marcas, en
  las interfaces IPv6 de los enlaces que no creó, y `delegate 0` en los
  enlaces que no son DHCP (PPP, qmi, mbim, ncm se lo pasan a su
  `<iface>_6`). Desactivar lo deshace. `ipv6_leak` avisa si la LAN aún
  tiene un prefijo que no es ULA.
- **Estado:** un enlace con IPv4 caída e IPv6 arriba cuenta como «solo
  IPv6» (y solo avisa si el VPS no tiene IPv6); `vps_silent` (ningún enlace
  con respuesta tras 10 s); `path_mtu` con la familia y el MTU que cabe,
  sin pedir nunca menos de 1280.
- **LuCI:** lista de direcciones (hasta 8, sin máscara), tope de MTU 1396
  con una IPv6 en la lista, `server_failover_ms` validado contra el
  intervalo de sondas como en el motor, la columna VPS por enlace con su
  familia y una marca de «alternativa», el MTU de camino, y los DNS IPv6
  fuera del túnel mientras IPv6 no vaya por él.
- **procd:** disparadores también para las compañeras; `apply` converge
  (la prueba de QEMU lo comprueba: aplicar otra vez no cambia nada y nada
  se recarga en 60 s).

### VPS en un systemd real (`contrib/vps/test/systemd.sh`)

Nuevo trabajo `systemd` en `vps.yml`, en los runners de GitHub con Ubuntu
24.04 y 22.04 (systemd 255 y 249): `install.sh` desde el checkout, un
router en un netns con el mismo secreto, el IP pass encendido y apagado
por `cengarde-passthrough.path`, reinicios de `cengarde` y de
`wg-quick@wg0` con las mismas reglas, `install.sh` otra vez sin reglas
repetidas, e `IPAddressDeny`: un curl en el cgroup de `cengarde.service` no
llega a un servicio de metadatos falso, y root sí. Solo corre con
`CI=true` o `--yes`: instala de verdad y enciende ufw.

**Lo que encontró en su primera pasada:** en Ubuntu 22.04 (systemd 249)
`cengarde.service` no arrancaba nunca: rechazaba el `%d` de `ExecStart`
("Failed to resolve unit specifiers in %d/cengarde.conf: Invalid slot").
Afectaba a todo VPS con 22.04 desde la plantilla de la historia 007. La
unidad usa ahora `${CREDENTIALS_DIRECTORY}`, que existe desde la 247.

## PR 3d1: herramientas del VPS, aún con el protocolo v3 (hecho)

Sin cambio de protocolo ni del motor: un VPS lo toma bajo un router 0.4.

- **`cengarde-nat` declarativo:** cuatro cadenas propias (`CG_IN`,
  `CG_FWD`, `CG_PRE`, `CG_POST`) restauradas enteras con
  `iptables-restore --noflush` (declarar la cadena la vacía primero:
  comprobado con 1.8.10), y un salto a cada una, el primero de su cadena.
  Ninguna regla nombra una interfaz: el salto de `PREROUTING` usa
  `-m addrtype --dst-type LOCAL` (ni difusión ni tráfico de paso) y el
  `MASQUERADE`, `-s 10.79.0.0/16 ! -o cg-+`; `PUB_IF` queda como opción.
  Hairpin: el `ACCEPT` de `--ctstate DNAT` va antes del `DROP` entre túneles
  y un `SNAT` a 10.79.0.1. `apply`, `sync` (nada mientras está `down`),
  `check` (el temporizador cada 5 min), `down`, `status`, `rules`, `jumps`,
  `reserved`, `purge-legacy`.
- **Conjunto reservado:** SSH, cengarde, los 32 puertos de WireGuard, todo
  lo de debajo de 1024 salvo `FORWARD_ALLOW_LOW`, `FORWARD_RESERVED` y, con
  `FORWARD_AUTO_RESERVE`, lo que escucha el servidor (`ss`, sin loopback ni
  el UDP efímero de un proceso; un socket del kernel, como el de un
  WireGuard propio en 51820, queda reservado aunque caiga en el rango
  efímero) y lo que publica Docker, salvo `FORWARD_UNRESERVE`. Va como
  comentario en `cengarde.conf` (el motor v3 no lo lee; el v4 lo tomará
  como `forward_reserved`).
- **Flujos vivos:** al cambiar el destino de un puerto, `conntrack -D -p P
  --orig-port-dst N -r VIEJO` hasta 256 puertos, o `-r VIEJO` entero si es
  más; probado en netns con conntrack-tools 1.4.8: un flujo UDP a 10 por
  segundo pasa al nuevo router en menos de 2 s, y sin conntrack se queda en
  el viejo (comprobado aparte en el kernel 6.18 con iptables 1.8.10: ni
  `iptables-restore --noflush` ni `iptables -R` mueven un flujo vivo). Un
  puerto UDP que contestaba el propio servidor y pasa a un router (IP pass
  de apagado a encendido, una regla nueva, un `apply` sin estado aplicado
  tras un reinicio): se borran, una a una, las entradas enviadas a una
  dirección del servidor y contestadas desde ella; los flujos que abren el
  servidor o los routers van a un host remoto y no se tocan (probado en
  netns: sin esto, el flujo seguía en el servidor). TCP no lo necesita: un
  puerto sin nadie contesta con un reset. `down` quita las reglas antes de
  borrar los flujos: al revés, un paquete entre los dos volvía a quedar
  atado al router.
- **Entrada:** el archivo de IP pass del motor v3 pasa a
  `/var/lib/cengarde/passthrough` (lo guarda el propio motor, así que
  sobrevive a reinicios; el estado de la 0.4 se migra). La tabla de reenvío
  del v4 (`FORWARD_FILE`) ya se lee y se prueba con fixtures. Ambos se
  rechazan enteros si son un enlace simbólico, no son un archivo regular,
  no son del usuario `cengarde` o pasan de 64 KiB (comprobado sobre el
  descriptor abierto, con `timeout` por si es un FIFO); una línea mala se
  salta con su número y motivo, nunca su contenido.
- **Otros cortafuegos:** ufw como antes; con `nftables.service`, un drop-in
  vuelve a aplicar tras su arranque, recarga y parada (su `flush ruleset`
  borra también las tablas de iptables-nft), y `check` dice qué líneas
  añadir a una cadena ajena con política `drop`; firewalld se rechaza.
  Reglas en los dos backends (legacy y nft): un aviso, según el contenido
  de `/proc/net/ip_tables_names` (los archivos de `/proc` dicen tamaño 0,
  así que `[ -s ]` nunca lo veía).
- **`cengarde-vps-setup`:** `/etc/cengarde/clients/NAME` (0600 root) por
  router y `cg-NAME` en 65501 + ranura; con v3, un solo router, en
  10.79.0.2 (`LEGACY_ADDR=1`). El motor valida la configuración antes que
  WireGuard: si la rechaza, falla la recarga o no contesta en 15 s, todo
  vuelve a como estaba. Sin router, una clave que no sirve a nadie, así el
  motor sigue arriba. IPv4 principal privada: `FORWARD_SKIP_SRC` y qué
  abrir delante; en 100.64.0.0/10, aviso de CGNAT.
- **`install.sh`:** desde una sesión interactiva o SSH se relanza con
  `systemd-run` como `cengarde-upgrade` (la sesión SSH se reconoce por un
  `sshd` entre sus padres: `sudo` borra `SSH_CONNECTION`, y
  `ssh vps 'sudo sh install.sh'` no tiene terminal); compila antes de tocar
  el `wg0` de la 0.4, así que una compilación fallida deja al router con su
  túnel; para antes el servicio con `DynamicUser` si el usuario estático no
  existe, porque `systemd-sysusers` ve el usuario dinámico vivo (por
  nss-systemd) y no crea el estático (sin reproducir aquí: lo comprueba el
  trabajo `systemd`).
- **La unidad:** `User=cengarde` (`cengarde.sysusers`) y la configuración
  en 0640 root:cengarde leída directamente, sin `LoadCredential`: una
  recarga lee el archivo real, y la línea de órdenes no tiene especificadores
  (el `%d` que rompía Ubuntu 22.04).
- **Pruebas:** `nat-rules.sh` (reglas contra archivos dorados, cargadas con
  `iptables-restore --test`), `netns.sh` (tráfico real), `setup-dryrun.sh`
  (órdenes del sistema falsas), `security.sh`; `systemd.sh` solo en el CI:
  instala la 0.4.2 del historial, actualiza con `sudo sh install.sh` desde
  una terminal que se cuelga (y otra vez sin terminal, como
  `ssh vps 'sudo …'`), `purge` y vuelta, firewalld, nftables y Docker. El trabajo `netns` corre
  también en 22.04 (iptables 1.8.7, conntrack-tools 1.4.6), donde se
  comprueba el DNAT a un rango desplazado.

## Medidas

- **Reinicios del servidor** (`sudo bench/lab.sh restart`, 3 enlaces, 2000
  pps de bajada, l3 detrás de 800 ms de cola llena; contenedor de 4 CPU;
  tiempo desde que arranca el servidor nuevo, consultando el socket de
  control cada 50 ms):

  | Motor | Reinicios con la bajada atascada | Vuelta de todos los enlaces |
  | --- | --- | --- |
  | antes (el código sin arreglo y `restart ebe570b`) | 7 de 10; en otra pasada, con `restart ebe570b`, 4 de 10, y 1 de 2 con l3 sola | nunca, hasta reiniciar el cliente |
  | después, 3 pasadas (10; 10 y 30 más la fase con l3 sola) | 0 de 54; 22 rehicieron la ventana | 2,0–2,1 s si la rehicieron (el umbral es 2 s); 0,0–0,1 s si no |
  | prueba con un anillo de 4 sondas | 0 de 10 con los tres enlaces, pero con l3 sola la bajada se atasca | nunca con l3 sola |

- **`sudo bench/lab.sh ci`:** smoke, health, control y restart pasan.
- **`security.sh`:** sin las reglas, el router lee los metadatos
  falsos (la prueba ve la fuga); con ellas no, y recibe el rechazo al
  instante, sin cortafuegos y detrás de uno como ufw. El puerto de
  WireGuard no contesta desde Internet ni desde el túnel, sí en `lo`. Desde
  las reglas 0.4, `up` y `down` dejan todo como estaba.
  - IPv6: solo que las reglas existen (`ip6tables -C`), en `vps.yml`, que
    pasó; el contenedor no tiene IPv6, así que no se probó que el puerto
    deje de contestar.
- **Tests unitarios:** 209 022 checks sin fallos con gcc en este
  contenedor; `engine.yml` los repite con clang, sanitizers y qemu
  (aarch64, armhf y MIPS big-endian). Nuevos: `test_epoch` (cada
  condición, el enlace rezagado, eco consumido una vez, la vuelta del
  reloj), direcciones en `test_config` y `test_util`, y vectores de
  emparejamiento de `hashlib.blake2s` en `test_blake2s`.
- **QEMU** (`openwrt/test/e2e.sh`, 25.12.5, con el código de `76bf10e`,
  igual a 0.4.1 salvo la versión): pasa entero, LuCI incluido.

**PR 2** (mismo contenedor de 4 CPU, compartido con otros procesos):
- **`multiip`** (`sudo bench/lab.sh multiip`): el servidor con una
  secundaria en `s1` y una /32 en `lo`, con `listen` en `0.0.0.0` y en `*`:
  - con el motor anterior (`3bcf673`), l1 y l2 nunca llegan a vivos;
  - con el nuevo, los tres vivos a los 1,5 s, cada camino con su `local`;
    2000 pps en cada sentido llegan una vez (5999 de 5999 por enlace);
  - al borrar la /32 con 2000 pps de bajada, solo l2 suma `local_errors`
    (unos 2400–2600; l1 y l3, 0) y el túnel no pierde nada; con la /32 de
    vuelta, l2 vive otra vez en 1,8–2,0 s;
  - con `server = 198.51.100.7 10.0.2.2` y 3 s de failover, l2 pasa a la
    segunda 2,8 s después de borrar la /32, y el camino del servidor la
    sigue.
- **CPU del servidor por paquete** (`down` y `up` a 10.000 pps de 1400 B, 3
  enlaces, `listen = *`, 5 rondas alternando el motor anterior y el nuevo;
  resolución 0,2 µs):

  | Sentido | Antes (media / mediana) | Después (media / mediana) |
  | --- | --- | --- |
  | bajada (cada paquete por 3 caminos, ahora con su mensaje de control) | 17,8 / 17,6 µs | 18,3 / 18,4 µs (+3 %; los rangos se solapan) |
  | subida (recibir con pktinfo) | 12,9 / 13,0 µs | 12,8 / 12,8 µs (sin diferencia) |

- **`fallback`** (4 s de failover): con la primera dirección muerta, vivo
  en la segunda a los 4,0–4,1 s, y se queda; una IPv6 primera en un enlace
  solo IPv4 no retrasa nada (0,0 s); tras un corte del lado del servidor de
  al menos 25 s, de vuelta en la primera 0,2–1,2 s después de que vuelve;
  si el enlace pierde su propia dirección y la recupera, en la primera a
  los 0,1 s; con un camino de MTU 1400, `path_mtu` 1400 y el aviso pide un
  MTU de WireGuard de 1316, sin perder paquetes.
- **`sudo bench/lab.sh ci`:** smoke, health, control, fallback, multiip y
  restart pasan.
- **Tests unitarios:** 209 808 checks sin fallos con gcc, clang y
  sanitizers, y en qemu (aarch64, armhf y MIPS big-endian). Nuevos:
  `test_pktinfo`, `test_srvpick`, `test_addrpick`, y listas en
  `test_config`.
- **Binario** de OpenWrt x86_64: de 97 a 110 KB.
- **QEMU** (`openwrt/test/e2e.sh`, 25.12.5, paquetes 0.4.2 de `5a1df76`):
  pasa entero, 28 comprobaciones. Lo nuevo:
  - el «VPS» escucha en `*` con 1.2.3.4, 1.2.3.5 y 2001:db8::4, y anuncia
    por RA un prefijo global y una ULA en cada enlace, MTU 1400 en up3 y un
    prefijo delegado en up1, que el router toma con su propia `up1v6`;
  - sin cengarde, la LAN recibe ese prefijo; con todo por el túnel, ya no
    (y `ipv6_leak` no aparece); al desactivar, vuelve, sin marcas;
  - con 2001:db8::4 primera, los tres enlaces van por IPv6 desde su
    dirección global, no la ULA, y el VPS contesta desde 2001:db8::4;
  - `path_mtu` solo en up3, y el motor avisa (un datagrama de 1408 B da
    paquetes de 1480 B; pide MTU 1296);
  - con la IPv6 bloqueada en up2, solo up2 pasa a 1.2.3.4, sin perder
    pings; sin 2001:db8::4 en el VPS, pasan todos;
  - aplicar otra vez no cambia nada, y nada se recarga en 60 s.
- **Visto en QEMU, sin cambiar nada:**
  - al bajar una interfaz, Linux borra sus direcciones IPv6 y netifd no las
    repone; en el «VPS» de la prueba, el enlace afectado se quedó sin IPv6
    y el motor pasó solo a la IPv4, como debía (la prueba ahora usa
    `keep_addr_on_down`);
  - al arrancar, mientras netifd vuelve a poner las direcciones, un enlace
    puede cambiar de origen varias veces en un par de segundos (5 en 2 s en
    up1); luego queda fijo.

## Qué hacemos con esto

- **El PR 2 fue primero,** porque cerraba la fuga de IPv6 de Starlink;
  ya está hecho, y el arreglo a mano de `openwrt/README.md` ya no hace
  falta.
- **Un solo cambio de formato,** v4, solo en el PR 3, con paquetes 0.5.0 en
  los dos extremos.
- **El lado VPS es "un Debian o Ubuntu cualquiera":** Vultr es un caso, no
  el supuesto.
- **Toda cifra de coste** (pktinfo, búsqueda multicliente, HELLO) sale de
  `bench/` (el motor antes y después del cambio) y de un `bench/macbench`
  nuevo (PR 3) antes de publicarse; las del diseño son de un
  microbenchmark aparte (*estimación*).

## Pendiente, por PR

### PR 3: varios routers por VPS (protocolo v4) y panel de puertos

- **Protocolo v4:** pista en el byte 2; sondas de 56 B con `cookie` y
  `rx_top`; HELLO sin estado con época de 30 s; la sesión nueva empieza en
  `rx_top + 2^20`; la ventana de subida nace con todo lo anterior marcado;
  `session_timeout_ms` ≥ 60 s en el servidor; `vps_refusing` en el
  cliente. Cierra el secuestro del IP pass con sondas reenviadas.
- **Servidor multicliente:** `[client NAME]`, admisión por pista sin
  probar todas las claves, sesión activa por cliente, cambios sin reinicio.
- **VPS:** `cengarde-nat` declarativo (`rules`, `apply`, `down`, `sync`,
  `purge-legacy`) con `cengarde-nat.service` (`RemainAfterExit`) y flock,
  movido desde el PR 2; `cengarde-vps-setup add|remove|list` (sin
  `--public-ip`; sirve igual en un servidor casero), `cg-NAME` en 65501–65532, migración desde
  0.4, usuario fijo, `cengarde ctl reload` con respuesta, y el panel de
  reenvío de puertos.
- **OpenWrt:** túnel /32 derivado más ruta a 10.79.0.1, `vps_name`, el
  bloque "este router en un VPS existente" con el comando, sin `pass_ip`.
- **Pruebas:** `bench/cgprobe.c`, `bench/macbench`, escenarios `replay` y
  `multiclient`; en `restart`, `window_resets = 0` con v4. Paquetes 0.5.0.

### PR 4: IPv6 dentro del túnel

- ULA derivada y NAT66 en el router (include de fw4) y en el VPS;
  `ra_default` con marca; interruptor apagado por defecto.
- En el VPS, reenvío IPv6 siempre junto con `accept_ra = 2`, sin depender
  de netplan.

### PR 5: nombres con DNS dinámico

- Resolvedor propio en el motor (`dns.h`, `resolve.h`): consultas por cada
  enlace, atadas a su dirección, primero con los DNS de la operadora.
- TTL entre 30 s y 15 min, última respuesta buena, nueva consulta cuando
  enmudecen todos los enlaces del nombre; medir el corte cuando cambia la
  IP.
- LuCI con el tipo `host`; UPnP con STUN para un servidor de IP dinámica.
- Guía del servidor casero: reenvíos en el router de casa y DNS dinámico.

### Sin PR fijo

- **Paquetes tardíos en enlaces rezagados:** solo si un escenario nuevo,
  `bench/lab.d/lag.sh` (netem, solo en el CI), muestra enlaces que se dan
  por mudos.
- **Hardware real:** en Vultr, la IP reservada, IPv6, y desde la LAN
  `curl -m3 169.254.169.254` (falla) y SSH al VPS (funciona); en la Pi,
  la IPv6 y el MTU IPv6 del enlace Starlink.
- **Fuera de este plan:** servidor multihilo (`SO_REUSEPORT` con cBPF, que
  ve el paquete sin la cabecera UDP: `ld [4]` lee el id de sesión,
  verificado), IP pass por IPv6, prefijo enrutado en la LAN, varios VPS en alta
  disponibilidad, DNSSEC, DoT/DoH y el servidor anunciando sus direcciones.

## Cambios

- 2026-10-04: creada con el plan, las respuestas del usuario y el PR 1.
- 2026-10-04: corregido: las `<iface>_6` de PPP, qmi, mbim y ncm sí copian
  `delegate 0` de su interfaz madre; las compañeras van sobre el
  dispositivo del enlace y no sobre `@enlace`.
- 2026-10-04: PR 2 hecho (0.4.2), con sus medidas; `cengarde-nat`
  declarativo pasa al PR 3.
- 2026-10-04: PR 3d1 hecho (herramientas del VPS con el protocolo v3).
