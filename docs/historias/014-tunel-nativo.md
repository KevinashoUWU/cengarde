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

## Lo que sigue

1. **Fase 2, reordenar por dependencia** (propuesta de la investigación, razonada):
   - cada dato lleva la secuencia del paquete anterior de su mismo flujo (`prev_seq`), y el receptor solo lo retiene si falta ese predecesor;
   - el emisor reconoce una retransmisión de TCP (secuencia de TCP menor que la máxima de ese flujo) y la manda sin predecesor, así se entrega al llegar;
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
