# 014 — Túnel nativo: cengarde sin WireGuard

- **Fecha:** 2026-10-15
- **Estado:** vigente; fase 1 en el PR #61
- **Fuentes:** corridas RTMP de la red de pruebas (`~/campo/2026-10-15/rtmp-dbg/`, `fec-field/2124/`), la captura `cgpc.pcap` de la corrida 4, la investigación `research-014.md` y las medidas `bench014/RESULTS.md` (scratch de la sesión), el estudio `012-bonding/ejes.md`, `sudo bench/lab.sh native`.

## TL;DR

- **El problema:** cengarde va por fuera de WireGuard, así que no ve los flujos.
  - Un hueco retiene a todos los flujos del túnel.
  - Las retransmisiones de TCP quedan detrás del hueco que vienen a reparar. En la corrida 4, TCP reparó en ~30 ms y su retransmisión esperó 500 ms (`reorder_max_wait_ms`): RTT de 470–600 ms y los cuatro enlaces en cero.
- **La decisión:** cengarde pasa a ser el túnel. Usa una interfaz TUN, el MAC y el anti-replay que ya tiene, y el cifrado como opción posterior. WireGuard sigue disponible como modo, y es el de omisión mientras el nativo no lo iguale.
- **Medido en la Pi 4, por núcleo y con paquetes de 1400 B:**
  - escribir en la TUN: 5,2 µs (2,1 Gbit/s);
  - leer de la TUN: 6,0 µs (1,9 Gbit/s);
  - SipHash: 1,3 µs (8,4 Gbit/s);
  - ChaCha20-Poly1305 con OpenSSL y NEON: 2,4 Gbit/s; en C portable, 1,0 Gbit/s;
  - AES-GCM: 0,5 Gbit/s.

  A 400 Mbit/s, la TUN con el MAC cuesta ~25 % de un núcleo por sentido, y con ChaCha ~40 %.
- **Fase 1 (PR #61):** `tunnel = native`, sin cifrar y autenticado.
  - **Laboratorio** (2 enlaces de 40 Mbit/s, reparto): 73,5 Mbit/s en cada sentido. Sin límite de enlaces: ~2,5 Gbit/s en el PC.
  - **Varios routers por VPS:** cada uno tiene su `tun_route` y el servidor comprueba el origen de cada paquete (como AllowedIPs).
  - `flow.h` deja listo el siguiente paso.
- **`tcp_aware` (PR #65), corregido el 2026-10-16:** una retransmisión de TCP entregada al instante se adelantaba a lo retenido y TCP lo reenviaba todo (en la red de pruebas, 135 veces las retransmisiones rápidas). Ahora la retransmisión ocupa su lugar en la secuencia y solo los ACK van al instante. En el laboratorio, `on` queda como `off`.
- **La Pi no trae `kmod-tun`.** El paquete de OpenWrt tendrá que depender de él.

## Por qué salir de WireGuard

- **Lo que no se ve:** cengarde recibe datagramas cifrados. Una retransmisión de TCP es para él un paquete nuevo con secuencia nueva, y el reordenador la entrega detrás del hueco original.
  - **Esperar poco:** TCP ve desorden y retransmite sin necesidad.
  - **Esperar mucho:** bloquea la reparación de TCP.
  - Con 500 ms (pensado para MPEG-TS crudo), las pérdidas reales le costaban a TCP medio segundo.
- **Lo que explica:**
  - la ventana grande (#46) bajó el caudal;
  - el rescate no movió el caudal con TCP saturando;
  - los acalambres de RTMP duraban 300–600 ms.
- **Modificar WireGuard, descartado:**
  - el kernel no deja pasar nada del paquete interior, salvo ECN;
  - marcar con nftables no llega a cengarde, porque WireGuard no copia el DSCP y un socket no ve la marca;
  - un fork de wireguard-go o boringtun sería mantener código ajeno y dos capas de cabeceras.
- **El cifrado:**
  - **La autenticación queda siempre** (MAC SipHash más anti-replay): sin ella, el VPS sería un proxy abierto.
  - **El cifrado del contenido es opcional,** porque casi todo ya viaja con TLS, SRT o RIST.
  - **Conviene encendido por omisión** cuando exista: sin él, el operador ve IP, puertos, SNI y el RTMP en claro, y puede estrangular video por DPI.

## Lo que hacen otros (resumen de `research-014.md`)

- **Dos familias:**
  - **Repartir por paquete y reordenar en destino:** Peplink, VeloCloud (para tráfico pesado), Speedify.
  - **Fijar cada flujo a un camino:** MQVPN, el VPN por omisión de OpenMPTCProuter desde v0.64. Fija TCP y reparte UDP, con un reordenador de 50 ms apagado por omisión.
  - **Glorytun / MUD** no reordena: escribe a la TUN en el orden en que llegan los paquetes.
- **El tiempo real no se reparte en ningún producto:** va por un enlace, duplicado o con FEC.
- **Speedify:**
  - **Clasificación:** detecta el tipo de tráfico por cabeceras, puertos, nombres de host y tasa constante.
  - **Prioridad:** usa QoS y activa la redundancia cuando ve pérdida.
  - **Sin umbrales publicados.**
  - **Cifrado:** Speedify y Peplink permiten apagarlo.
- **Sobrecarga:** Peplink, 80 B por paquete; glorytun, 16 B.
- **No hay medidas publicadas** de reordenar por flujo contra reordenar global, ni de liberar retransmisiones de TCP en un intermediario. Las nuestras serían de las pocas.
- **E/S de la TUN:** wireguard-go usa `IFF_VNET_HDR` con TSO/GRO y no usa colas múltiples. Recomendación: partir con una cola no bloqueante y agregar offloads cuando la medida los pida.

## Medidas (`bench014/RESULTS.md`)

| Pi 4, un núcleo, 1400 B | Por paquete | Por núcleo |
| --- | ---: | ---: |
| Escribir en la TUN, 1 cola | 5,2 µs | 2,1 Gbit/s |
| Escribir en la TUN, 4 colas | — | 7,2 Gbit/s en total |
| Leer de la TUN, 1 cola | 6,0 µs | 1,9 Gbit/s |
| Leer de la TUN, 4 colas | — | 3,7 Gbit/s en total |
| SipHash-2-4 (el motor) | 1,3 µs | 8,4 Gbit/s |
| ChaCha20-Poly1305, OpenSSL (NEON) | ~4,7 µs | 2,4 Gbit/s |
| ChaCha20-Poly1305, Monocypher (C portable) | 11 µs | 1,0 Gbit/s |
| AES-128-GCM, OpenSSL (sin instrucciones AES) | — | 0,5 Gbit/s |

- **Cómo se midió:** con cengarde detenido y la Pi en reposo. Se instalaron `kmod-tun` y `openssl-util` de forma temporal y se quitaron después.
- **El kernel de la Pi acepta** `IFF_MULTI_QUEUE`, `IFF_VNET_HDR`, TSO, USO y NAPI.
- **Referencia con WireGuard:** subiendo 74 Mbit/s por el túnel, los 4 núcleos quedaron al 23 % (sistema 12,6 %, interrupciones 7,8 %, usuario 2,5 %).
- **Para el cifrado:** usar OpenSSL o libsodium (con NEON), no C portable. Con duplicación, descartar las copias **antes** de descifrar (por el MAC y la secuencia, como hoy).

## Fase 1 (PR #61)

- **Configuración:** `tunnel = wireguard | native`.
  - En el cliente: `tun`, `tun_addr` y `tun_mtu` (1420 por omisión).
  - En el servidor: una TUN compartida, y cada `[client]` con su `tun_route`. Con un solo cliente vale `0.0.0.0/0 ::/0`.
- **Cliente:** la TUN reemplaza al socket de WireGuard en `wg_read`. El reparto, la duplicación, el rescate y el reordenador no cambian. Para el reparto y el reordenador, cada paquete IP cuenta como dato de WireGuard (tipo 4).
- **Servidor:**
  - **Bajada:** un paquete leído de la TUN va a la sesión más nueva del cliente que tiene su destino (prefijo más largo, `tunroute.h`).
  - **Subida:** solo pasa si su origen es de ese cliente. Si no, se descarta como `spoofed`.
- **Laboratorio (`lab.sh native`):**
  - ping IPv4 e IPv6: 0 % de pérdida;
  - TCP: 73,5 Mbit/s en cada sentido (2 enlaces de 40 Mbit/s, reparto);
  - dos routers con su propio prefijo cada uno;
  - un paquete con el origen de otro router: rechazado.
  - Los casos de WireGuard (`smoke`, `multiclient`, `wgpoke`, `wgport`, `control`, `restart`) siguen pasando.
- **Lo que no cubre todavía:**
  - el cifrado;
  - el reordenamiento por flujo y la liberación de retransmisiones de TCP;
  - OpenWrt y LuCI;
  - `cengarde-vps-setup` y `cengarde-nat` para la TUN.

## `tcp_aware` y las retransmisiones adelantadas (2026-10-16)

**Qué hace `tcp_aware` (PR #65, protocolo v6):** con el túnel nativo, el emisor marca `CG_F_FREE` los segmentos de TCP sin datos y las retransmisiones de TCP (`tcpfree.h`). El reordenador del otro extremo los entregaba al instante, sin esperar huecos.

**Lo medido en la red de pruebas** (Pi ↔ VPS, `iperf3 -P 4 -R` 20 s, `nstat` del kernel del VPS, que es el que envía):

| Contador del VPS | `tcp_aware` no | `tcp_aware` sí |
| --- | ---: | ---: |
| Rendimiento | 324 Mbit/s | 331 Mbit/s |
| `TcpExtTCPFastRetrans` | 69 | 9327 |
| `TcpExtTCPDSACKRecv` (retransmisiones de más) | 19 | 1814 |
| `TcpExtTCPLostRetransmit` | 0 | 437 |
| `TcpExtTCPTSReorder` | 16 | 819 |

El rendimiento se sostiene, pero se gastan enlace y RTT.

**El mecanismo:**
- Una retransmisión lleva una secuencia de cengarde nueva. Entregada al instante, se adelanta a todo lo que el reordenador retiene detrás de un hueco, aunque se envió antes que ella.
- El RACK de TCP (RFC 8985) ve entregado lo último enviado y no lo anterior. Pasado su RTT más la ventana de reordenamiento (~¼ del RTT mínimo), da lo anterior por perdido y lo reenvía. Esos reenvíos también van libres, así que se repite.
- Después llegan los originales retenidos: duplicados (DSACK) y retransmisiones perdidas.
- No hace falta un hueco largo: alcanza con que lo retenido espere más que esa ventana. Con dos enlaces de 20 y 30 ms bastan los ~10 ms de diferencia.

**El arreglo (sin cambiar el protocolo):**
- El receptor distingue la retransmisión del control mirando el segmento: un paquete libre con datos de TCP es una retransmisión (`cg_tcp_data` en `tcpfree.h`). Un fragmento o una cabecera corta cuentan como control.
- La retransmisión ya no se adelanta: ocupa su lugar en la secuencia, como cualquier dato. Los segmentos sin datos siguen yendo al instante.
- El estado cuenta las retransmisiones recibidas en `reorder.retx`.

**Lo que se probó y no se adoptó:** que una retransmisión abandone los huecos anteriores a ella.
- Primero se probó abandonándolos y entregando la retransmisión al instante. Con edad mínima 0 se abandonaron ~900 huecos de reparto por corrida (paquetes en vuelo en el enlace lento) y no bajaron los DSACK.
- Después, reteniéndola en su lugar y abandonando solo huecos cuyo paquete retenido se envió al menos N ms antes:
  - con pérdida aleatoria (escenario 1), 10 ms quedó igual que no abandonar nada;
  - con un enlace cuya cola salta 200 ms (escenario 2), cualquier edad abandona paquetes en vuelo que después llegan duplicados: 34 Mbit/s con 10 ms, 39 con 50 y 43 con 150, contra 45 sin abandonar nada.
- Quedó como perilla experimental apagada: `reorder_retx_release_ms` (0 por omisión), con `lost.retx` y `late.after_retx` en el estado.

**Laboratorio** (`sudo bench/lab.sh tcpaware`: túnel nativo, `split = on`, `reorder_max_wait_ms = 500`, `iperf3 -R -P 4` 20 s, `nstat` en el netns del servidor; mediana de 3 corridas; antes = `master` en `5e44989`):
- **Escenario 1:** 2 enlaces de 50 Mbit/s, de 20 y 30 ms, con 0,5 % de pérdida cada uno.
- **Escenario 2:** 2 enlaces de 50 Mbit/s, de 25 y 30 ms, con 0,1 % de pérdida; el primero suma 200 ms durante 1 s de cada 4 (`TCPA_SPIKE=1:200:1:4`).

| | Esc. 1: `off` | Esc. 1: `on` antes | Esc. 1: `on` ahora | Esc. 2: `off` | Esc. 2: `on` antes | Esc. 2: `on` ahora |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Mbit/s | 16,3 | 15,8 | 15,7 | 43,7 | 38,1 | 45,7 |
| `TCPFastRetrans` | 145 | 673 | 152 | 669 | 1525 | 539 |
| `TCPDSACKRecv` | 0 | 197 | 0 | 339 | 437 | 245 |
| `TCPLostRetransmit` | 0 | 114 | 1 | 115 | 266 | 166 |

- El laboratorio reproduce el defecto: en el escenario 1, `on` tiene 4,6 veces las retransmisiones rápidas de `off` y DSACK donde no había.
- Con el arreglo, `on` queda como `off`. En el escenario 2, `on` queda algo mejor que `off`: los ACK siguen yendo libres.
- El rendimiento del escenario 1 lo limita la pérdida (Cubic con 0,5 %), no el túnel.

**Lo que falta:**
- medirlo en la red de pruebas con la misma corrida de la tabla de arriba;
- la fase 2 por flujo (abajo) choca con lo mismo: una retransmisión entregada al llegar se adelanta a lo retenido de su propio flujo, y RACK reenvía todo eso.

## Lo que sigue

1. **Fase 2, reordenar por dependencia** (propuesta de la investigación, razonada):
   - cada dato lleva la secuencia del paquete anterior de su mismo flujo (`prev_seq`), y el receptor solo lo retiene si falta ese predecesor;
   - el emisor reconoce una retransmisión de TCP (secuencia de TCP menor que la máxima de ese flujo) y la manda sin predecesor, así se entrega al llegar. **Ojo (2026-10-16):** adelantada a lo retenido de su flujo, RACK reenvía todo eso (ver `tcp_aware` arriba);
   - las retransmisiones de QUIC no se pueden reconocer.
2. **Clases y prioridad:**
   - tiempo real por un enlace o duplicado;
   - interactivo y retransmisiones con dos copias;
   - pesado repartido;
   - los flujos chicos fijos a un enlace y solo los grandes repartidos;
   - `cake diffserv4` sobre la TUN, con el tope de la historia 013, para la prioridad.
3. **Cifrado opcional** con ChaCha20-Poly1305 de OpenSSL o libsodium. Necesita época y renovación de claves (el nonce con secuencia de 32 bits da la vuelta). El secreto hacia adelante (Noise) viene después.
4. **OpenWrt** (`kmod-tun`, interfaz y zona para `cg0`, LuCI) y **VPS** (`cengarde-vps-setup` y `cengarde-nat` sobre la TUN).
5. **Medir en la Pi y en terreno:** RTMP y TCP con el reordenamiento global contra el que usa `prev_seq`.

## Cambios

- 2026-10-15: creada. Decisión de salir de WireGuard, la investigación, las medidas en la Pi y la fase 1 (PR #61).
- 2026-10-16: `tcp_aware` (PR #65) medido en la red de pruebas: las retransmisiones adelantadas hacían que TCP reenviara lo retenido. Arreglo: la retransmisión ocupa su lugar en la secuencia, sin cambiar el protocolo; `lab.sh tcpaware`, y `reorder_retx_release_ms`, que se probó y quedó apagado.
- 2026-10-24: historia 023. `tcpfree.h` distingue la primera retransmisión
  de unos bytes de una repetida: en redundancia la primera va por un enlace
  (`tcp_retx_one`); el extremo por donde sale TCP mide el RTT de sus flujos
  (SYN a SYN-ACK) para no competirle con el rescate (`rescue_tcp_race`) y
  cuenta las pérdidas más allá del túnel por SACK (`tcp_e2e`). `flow.h` da
  el hash de la dirección contraria (`cg_flow_of_dir`).
