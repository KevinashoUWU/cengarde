# 005 — Motor C v1: protocolo y arquitectura

- **Fecha:** 2026-10-03 (actualizada el mismo día con la Fase 2, y el
  2026-10-04 con las historias 009 y 010)
- **Estado:** vigente; la historia 006 sustituye la política de envío, las
  sondas y el formato de `cg_probe_info` (protocolo v2), y la 009 suma las
  banderas de IP pass (protocolo v3)
- **Fuentes:** `engine/` (código y tests), `sudo bench/lab.sh compare` y
  `smoke`, historias 001–004.

## TL;DR

- **Un binario, `cengarde`:** cliente o servidor según la configuración.
  - C11 sin dependencias, un hilo con epoll, lotes de 64 con
    `recvmmsg`/`sendmmsg` y ningún `malloc` por paquete.
  - Ocupa 55–70 KB sin símbolos (x86_64, aarch64, MIPS); unos 97 KB desde
    la historia 009 (OpenWrt x86_64).
- **Protocolo propio v1:** cliente y servidor son siempre cengarde (decisión
  del usuario, historia 004).
  - Cabecera de 24 B con sesión, secuencia, marca de tiempo y MAC SipHash-2-4
    sobre cabecera y payload, con una clave por sentido.
  - El byte de enlace queda fuera del MAC, así que el MAC se calcula una sola
    vez por paquete.
- **Deduplicación por secuencia autenticada:** se comprueba antes del MAC y se
  marca después. Los duplicados se descartan sin pagar el MAC, y una copia
  falsificada nunca tapa a la buena.
- **Servidor:**
  - solo crea sesiones a partir de paquetes autenticados;
  - cada sesión tiene su propio socket hacia WireGuard (multi-cliente en un
    puerto);
  - los caminos solo cambian de dirección con paquetes autenticados.

  Esto cierra la reflexión de la historia 001.
- **Medido (3 enlaces, VM Xeon):**
  - Bajada a 10k pps: el cliente pasa de 94 a 14 µs/pkt.
  - Subida a 60k pps: Go pierde un 34–38 % y cengarde un 0 %; a 100k pps
    (1,1 Gbit/s), un 0,33 %.
  - Enlace lento: 0 % de pérdida y ningún socket recreado.
- **Compromiso:** unos 25 µs más de latencia por salto que Go en esta VM,
  porque C duerme en epoll en vez de sondear en activo (Go gasta el doble de
  CPU).

## Decisiones

### Protocolo (`src/proto.h`)

| Bytes | Campo | Notas |
| --- | --- | --- |
| 0 | versión (4 bits) y tipo (4 bits) | v1; tipos DATA = 1, PROBE = 2, PROBE_REPLY = 3 |
| 1 | flags | 0 |
| 2 | reservado | debe ser 0 |
| 3 | id de enlace | **no autenticado**; el emisor lo cambia por enlace sin recalcular el MAC |
| 4–7 | sesión | aleatoria, la elige el cliente |
| 8–11 | secuencia | por sesión y sentido, compartida por todos los tipos |
| 12–15 | marca de tiempo | µs monotónicos del emisor (32 bits, se da la vuelta) |
| 16–23 | MAC | SipHash-2-4 sobre los bytes 0–15 (con el byte 3 a 0) y el payload |

- **El MAC cubre el payload.** Si solo cubriera la cabecera, un atacante en
  uno de los enlaces podría reenviar una cabecera válida con el payload
  corrupto y ganar la carrera a las copias buenas, tumbando el túnel. Como la
  ventana solo se marca tras verificar el MAC, la copia corrupta se descarta y
  la buena pasa.
- **Coste:** un SipHash por paquete único en cada extremo. Verificado contra
  OpenSSL y bajo qemu en MIPS big-endian.
- **Claves:** el secreto de 32 B se parte en clave cliente→servidor (bytes
  0–15) y servidor→cliente (16–31), así que reflejar un paquete hacia su
  emisor no lo valida.
- **Sondas:** una por enlace y segundo (`probe_interval_ms`). Sirven para
  medir el RTT (EWMA 1/8), mantener el NAT abierto y que el servidor aprenda
  el camino. Su payload, `cg_probe_info`, lleva lo que cada lado mide en su
  sentido de recepción (rx, primeras copias, retraso), así que cada extremo ve
  los dos sentidos. En la Fase 2 pasan a ser adaptativas (100 ms con
  tráfico, 1 s en reposo) y llevan el retraso de ida: protocolo v2,
  historia 006.
- **Formato:** cualquier cambio sube `CG_PROTO_VERSION`.

### Recepción y deduplicación

- **Ventana anti-replay** de 8192 bits (8128 secuencias efectivas, como
  WireGuard), en `src/replay.h`.
- **Orden:** comprobar → si es DUP u OLD, descartar sin MAC → si es NEW,
  verificar el MAC → marcar. Un duplicado no refresca la vida del camino,
  porque no está autenticado; las sondas sí.
  - *Corrección 2026-10-04:* desde 0.4.1, en el cliente, una respuesta de
    sonda OLD sí paga el MAC, para ver si el servidor empezó de cero
    (`epoch.h`, historia 010).
- **Estadísticas por enlace** (`src/arrival.h`): cuántas veces llegó primero,
  duplicados, tardíos, paquetes que no trajo y retraso suavizado frente a la
  primera copia. Son las señales para el silenciado de la Fase 2 (historia
  003). Como cuentan duplicados sin verificar, son informativas.

### Política de envío (patrón libRIST)

La versión 1 (`src/policy.h`) dejaba sin payload un enlace sin nada verificado
durante `stall_ms` (3 s) y enviaba por todos si todos estaban mudos. La Fase 2
la sustituye por `src/health.h` (mudo en los dos sentidos y silenciado por
retraso; historia 006). Se mantiene que el socket nunca se destruye por
errores transitorios: un EAGAIN descarta la copia de ese enlace y nada más.

### Cliente (`src/client.c`)

- **Enlaces por netlink** (`src/netlink.c`): volcado inicial y luego eventos,
  con resincronización si el kernel avisa de ENOBUFS. Reintenta cada 5 s.
- **Qué interfaces usa:** una sección `[link X]` manda sobre los patrones
  `interfaces`/`exclude`. `lo`, `wg*`, `docker*` y similares quedan siempre
  excluidas.
- **Socket por enlace:** `SO_BINDTODEVICE`, bind a la IP del enlace y
  `connect` al servidor (ruta cacheada; solo entran respuestas del servidor).
  Se recrea solo si cambia la IP o el destino.
- **Puerto local de WireGuard:** se aprende solo de datagramas con forma de
  WireGuard (`cg_looks_like_wg`), para que cualquier proceso local no pueda
  secuestrar la bajada.

### Servidor (`src/server.c`)

- **Tabla de sesiones** (`src/idmap.h`): hash abierto con borrado por
  desplazamiento hacia atrás, con tests que fuerzan colisiones y la vuelta del
  array.
- **Sesión:** se crea con el primer paquete autenticado. Tiene su socket
  conectado a WireGuard, así que WireGuard ve un endpoint por cliente.
  Caduca a los 180 s sin tráfico (`session_timeout_ms`) y hay un máximo de
  `max_sessions`.
- **Caminos** (sesión, id de enlace): una dirección nueva o migrada solo se
  acepta con el MAC verificado. Caducan a los 30 s (`path_timeout_ms`).

### Bucle y robustez

- **Equidad:** como mucho 8 lotes por socket y vuelta (`CG_MAX_ROUNDS`).
  epoll en modo nivel vuelve a por el resto.
- **Logs con límite de frecuencia:** nunca uno por paquete. Se registran los
  paquetes malformados o sin autenticar (útil si un cliente engarde Go apunta
  a un servidor cengarde).
- **IPv6 ausente:** si el kernel arrancó sin IPv6, `listen = *` cae a IPv4.
- **Estado:** JSON atómico (temporal + `rename`) en `status_file`, cada
  segundo; desde la Fase 2, escrito por un hilo aparte (historia 006).
- **Makefile:** los flags imprescindibles van en `CG_CFLAGS`; `CFLAGS` y
  `LDFLAGS` son del que compila (OpenWrt los pasa por línea de comandos).

### MTU

- 24 B de cabecera. Con una ruta de 1500, el MTU de WireGuard tiene que ser
  ≤ 1416 sobre IPv4 y ≤ 1396 sobre IPv6.
- Buffers de 2048: el datagrama de WireGuard más grande admitido es de
  2024 B; uno mayor se descarta y se avisa en el log.

## Medidas

`sudo bench/lab.sh compare`, misma VM que la historia 001 (Xeon 2,1 GHz,
4 vCPU), paquetes de 1400 B, 3 enlaces, cada motor en ambos extremos.
Formato: CPU en µs/pkt, cliente / servidor.

Rango de dos pasadas completas:

| Prueba | engarde Go | cengarde | Pérdida Go → C |
| --- | --- | --- | --- |
| bajada 10k pps | 91–94 / 29–30 | 13,4–14,2 / 17–19 | 0–0,14 % → 0 |
| bajada 20k pps | 58–61 / 20–22 | 9,8–10,3 / 12–13 | 0–0,06 % → 0 |
| bajada 40k pps | 33 / 16–17 | 9,0–9,4 / 9,8–10,2 | 1,3–1,7 % → 0 |
| subida 10k pps | 31–33 / 35–37 | 17,2–17,4 / 13,2–13,4 | 0,1–1,1 % → 0 |
| subida 30k pps | 16 / 26–28 | 10,0–12,8 / 7,7–9,0 | 1,9–7,3 % → 0 |
| subida 60k pps | 13–14 / 17 | 8,3–8,6 / 7,4–7,8 | 34–38 % → 0 |

- **Duplicados entregados a WireGuard:** Go entrega N − 1 por paquete (que
  WireGuard descifra) y cengarde 0.
- **Subida a 100k pps** (1,12 Gbit/s de túnel): 0,33 % de pérdida con ~78 %
  de CPU por extremo.
- **Enlace lento** (l3 a 5 Mbit/s con `tbf`, subida a 2.000 pps):
  - 0 % de pérdida, p99 de 0,7 ms y ningún socket recreado (Go: 9
    recreaciones y hasta 77 % de pérdida);
  - l3 descarta localmente sus propias copias (9.096);
  - el servidor ve en l3 un retraso de 218 ms y 7.563 paquetes no traídos:
    es la señal para silenciarlo en la Fase 2.
- **Latencia a 10k pps con 1 enlace** (sin copias), p50:
  - Go: 90 µs con ~25 % de CPU por extremo;
  - cengarde: 141 µs con ~12 %.

  La diferencia es el despertar desde epoll en la VM: Go sondea en activo y C
  duerme. Escribir el estado en ext4 dentro del bucle sumaba picos de p99.9 de
  12–18 ms; en tmpfs o sin estado, 2,5–5 ms.

## Validaciones (todas pasan)

- **Tests unitarios:** 208.138 checks con gcc, clang y ASan/UBSan, y bajo
  qemu en aarch64, armhf y MIPS big-endian.
- **`bench/lab.sh smoke`:** cada paquete llega exactamente una vez en los dos
  sentidos; un extraño recibe 0 paquetes y queda registrado.
- **Con tráfico de fondo** (15.999/15.999 entregados, 0 duplicados):
  - un paquete de un extraño;
  - caída y vuelta de un enlace (`ip link set down/up`; el servidor migra el
    camino);
  - una interfaz que pierde su IP.
- **Cambio de IP de un enlace:** vuelve al instante con la IP nueva.
- **Reinicio del servidor en caliente:** la sesión se recrea con el primer
  paquete; solo se pierde el segundo que estuvo caído.
  - *Corrección 2026-10-04:* no siempre. La sesión nueva empieza con una
    secuencia aleatoria y, más o menos la mitad de las veces, cae detrás de
    la ventana del cliente: la bajada quedaba atascada hasta reiniciar el
    cliente (7 de 10 y, en otra pasada con
    `sudo bench/lab.sh restart ebe570b`, 4 de 10). Arreglado en 0.4.1: 0 de
    54; vuelta en 2,0–2,1 s cuando hubo que rehacer la ventana (22 de 54) y
    en 0,0–0,1 s si no (historia 010).

## Pendiente

- ~~**Fase 2:** silenciado por retraso; espera activa opcional; escribir el
  estado fuera del bucle~~: hecho (historia 006).
- ~~**Servidor con varias IPs:** responder desde la IP de llegada
  (`IP_PKTINFO`)~~: hecho (historia 010, PR 2).
- **Hilos y privilegios:** servidor multihilo (`SO_REUSEPORT`) si una vCPU se
  queda corta; bajar privilegios (`CAP_NET_RAW`/`CAP_NET_ADMIN`).
- ~~**Operación:** `SIGHUP` para recargar y socket de control~~: hecho
  (historia 009).
- **IPv6:** no probado en el laboratorio (el kernel de la VM no tiene IPv6);
  sí en QEMU desde el PR 2 (historia 010).
- **Hardware real:** medir en la Pi 4 (aarch64, mismo método de CPU por
  paquete) y en el VPS de Vultr.

## Cambios

- 2026-10-03: creada con la primera versión del motor.
- 2026-10-03: la Fase 2 sustituye la política de envío, las sondas y el
  formato de `cg_probe_info` (protocolo v2), y saca el estado del bucle
  (historia 006).
- 2026-10-04: socket de control, recarga en caliente y banderas de IP pass
  (protocolo v3); el binario de OpenWrt x86_64 pasa de 70 a 97 KB
  (historia 009).
- 2026-10-04: el reinicio del servidor podía dejar la bajada atascada, y una
  respuesta de sonda vieja paga ahora el MAC en el cliente (historia 010).
- 2026-10-04: el servidor responde desde la dirección de llegada y el
  cliente acepta una lista de direcciones del servidor; el binario de
  OpenWrt x86_64 pasa de 97 a 110 KB (historia 010, PR 2).
