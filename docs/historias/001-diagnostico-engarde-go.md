# 001 — Diagnóstico medido del engarde Go

- **Fecha:** 2026-10-03 (actualizada el mismo día con datos de campo)
- **Estado:** vigente
- **Fuentes:** código Go en `cmd/` (commit 3492df9); laboratorio `bench/`
  (`sudo bench/lab.sh suite` y las demos); resumen en `ROADMAP.md` §2.

## TL;DR

- **Cliente, bajada:** el coste por paquete crece más que el número de
  enlaces: 23 → 60 → 91–103 µs de CPU con 1 → 2 → 3 enlaces. Causa: N
  goroutines escriben en el mismo socket de WireGuard y el runtime aparca y
  despierta goroutines en cada paquete.
- **Cliente, subida:** escribe en serie y de forma bloqueante en cada enlace.
  Un enlace lento con cola local hace que su socket se destruya y se recree
  constantemente o, con `writeTimeout: -1`, frena a todos los demás.
- **Servidor:** un socket y una goroutine para todos los caminos. Satura un
  núcleo (~35–37k pps únicos con 3 enlaces) y, al atascarse, pierde las N
  copias del mismo paquete a la vez.
- **Seguridad y robustez:** un solo paquete desde cualquier origen basta para
  recibir toda la bajada durante 30 s (reflexión); `panic` si el puerto web
  está ocupado; 15–16 data races en el cliente y 6 en el servidor.
- **Prototipo C** (`bench/protoclient.c`): ~4× menos CPU en bajada con 3
  enlaces, ~6× deduplicando.
- **Confirmado en campo** (historia 004): la Pi 4B con 4 enlaces se queda en
  20–30 Mbit/s por CPU; un Celeron J4005 con la misma configuración llega a
  ~90 Mbit/s.

## Contexto

Antes de reescribir en C hacía falta saber qué limita al engarde actual y qué
no hay que repetir. Todo se midió en una VM con Xeon a 2,1 GHz y 4 vCPU, con
paquetes de 1400 B. Al repetir, las cifras varían en torno a ±10 %. "µs/pkt"
es la CPU del proceso (usuario + kernel en su contexto) por paquete WireGuard;
incluye trabajo del kernel que es igual para todas las variantes, así que
sirve para comparar, no como coste absoluto. Limitaciones: el "WireGuard" del
laboratorio no cifra y no hay `netem` (sin retardo, jitter ni pérdida en los
enlaces).

## Hallazgos

### Causas raíz en el código

| Lugar | Qué hace | Consecuencia |
| --- | --- | --- |
| `cmd/engarde-client/main.go:157-203` `updateAvailableInterfaces` | sondea las interfaces cada 1 s (`:201`) y recorre `sendingChannels` sin lock (`:165`) | un enlace nuevo tarda hasta 1 s; carrera con los borrados; ~7 consultas netlink/s |
| `main.go:94-111` `isAddressAllowed` | rechaza cualquier dirección con `:` (`TODO` en `:95`) | sin IPv6 |
| `main.go:237-257` `wgWriteBack` | una goroutine por enlace; buffer de 1500 (`:238`); `LastRec` sin sincronizar (`:251`); escribe a `*wgAddr`, que es nil hasta que WG envía algo | contención en el socket de WG; truncado si el MTU de WG > 1468; un log por paquete tras reiniciar |
| `main.go:259-297` `receiveFromWireguard` | `*sourceAddr = srcAddr` sin sincronizar (`:273`); `SetWriteDeadline` + `WriteToUDP` bloqueantes y en serie por enlace (`:276-282`); ante error, `terminateRoutine` y borrado (`:283-295`); `Lock()` en cada paquete (`:290`) | bloqueo en cabeza de línea; sockets que entran y salen sin parar |
| `main.go:147-155` `terminateRoutine` | escribe `IsClosing` sin sincronizar (`:148`) | carrera |
| `main.go:345-347` | `writeTimeout` por defecto de 10 ms | ver "enlace lento" |
| `cmd/engarde-client/webserver.go:97` `webGetList` | lee `sendingChannels` sin lock | carrera; posible `fatal error: concurrent map read and map write` |
| `webserver.go:141-153` `webResetExclusions` | GET con efectos; reemplaza `exclusionSwaps` sin lock (`:142`) | CSRF; carrera |
| `webserver.go:256-272` `webserver()` | si `ListenAndServe` falla, vuelve a registrar las rutas en `DefaultServeMux` (`:260`) | `panic: http: multiple registrations for /` que tumba el túnel |
| `cmd/engarde-server/main.go:127-166` `receiveFromClient` | buffer de 1500 (`:128`); clave string por paquete con asignaciones (`:145`); cualquier origen nuevo pasa a ser cliente (`:157-158`); `client.Last` sin lock (`:150`); reenvía todas las copias a WG (`:161`) | reflexión; WG descifra N copias; presión de GC |
| `cmd/engarde-server/main.go:168-210` `receiveFromWireguard` | envía cada paquete a todos los "clientes" vivos (`:184`); `Lock()` por paquete (`:203`) | reflexión; con varios clientes en un puerto, cada uno recibe el tráfico de los demás |
| `cmd/engarde-server/main.go:50` `getClientByAddr` | no se usa | código muerto |
| `cmd/engarde-server/webserver.go:61`, `:95` | lee `clients` sin lock; mismo panic al reintentar | carrera; caída |
| `cmd/engarde-client/udpconn_bindtodevice.go` | socket `AF_INET` + `SO_REUSEADDR` + `SO_BINDTODEVICE` | solo IPv4 |

### Mediciones

Coste del cliente Go según el número de enlaces, a 10.000 pps:

| Enlaces | Bajada (µs/pkt) | Subida (µs/pkt) |
| ---: | ---: | ---: |
| 1 | 23,0–23,6 | 25,4–26,8 |
| 2 | 60,4 | 29,4 |
| 3 | 91,2–96,4 | 31,8–32,2 |

Go frente al prototipo C, con 3 enlaces y el mismo servidor Go:

| Dirección | pps | Go | C | C + dedup |
| --- | ---: | ---: | ---: | ---: |
| bajada | 10.000 | 93,0–102,8 | 21,6–23,6 | 15,4–15,6 |
| bajada | 20.000 | 58,8–59,4 | 18,2–19,3 | 12,8–13,0 |
| bajada | 40.000 | 32,7–33,4 | 15,8–15,9 | 10,4–10,8 |
| subida | 10.000 | 32,0–33,6 | 15,6–15,8 | 15,4–15,6 |
| subida | 30.000 | 16,6–17,2 | 8,7–9,5 | 8,9–9,7 |

- **Perfil de syscalls del cliente Go** (bajada, 3 enlaces, 3 s con
  `strace -c -f`, que ralentiza el proceso, así que importan las
  proporciones): 25.392 `futex`, 35.947 `epoll_pwait`, 17.181 `sendto` y
  17.144 `recvfrom`. Son ~1,5 `futex` y ~2 `epoll_pwait` por cada envío o
  recepción útil. Con 3 enlaces el proceso no pasa de ~130 % de CPU.
- **Servidor:** con 3 enlaces satura un núcleo a 35–37k pps únicos (~110k
  copias/s). En subida a 20.000 pps pierde un 0,3–0,6 %. Contadores del
  kernel, leídos de `/proc/net/snmp` dentro de cada netns: 880
  `UdpRcvbufErrors` en el socket del servidor, de los que 834 eran copias de
  278 paquetes que perdieron sus 3 copias. Pérdida correlacionada: la
  redundancia no la cubre.
- **Enlace lento** (subida a 2.000 pps; l3 con `tbf` a 5 Mbit/s y cola local
  de 3 MB):
  - Con `writeTimeout` 10: 0–0,97 % de pérdida, p99.9 de 10–30 ms, el socket
    de l3 se recrea 9 veces en 8 s y l3 lleva solo 1.269 de 16.000 paquetes.
  - Con `-1`: 77 % de pérdida y p50 > 200 ms; l1 y l2 quedan frenados al
    ritmo de l3.
  - Mecanismo: el kernel solo vuelve a marcar escribible un socket UDP cuando
    se ha liberado la mitad de su `sk_sndbuf` (212.992 B por defecto). Eso son
    decenas de paquetes, unos 100 ms a 5 Mbit/s, muy por encima del timeout de
    10 ms. Afecta a cualquier enlace de menos de ~50 Mbit/s cuya cola esté en
    la propia máquina.
- **Reflexión** (`demo_stranger`): un único paquete desde 10.0.1.1:7777 →
  9.106–9.116 paquetes recibidos en ~5 s.
- **Panic** (`demo_webpanic`): código de salida 2 ~1 s después de arrancar.
- **Carreras** (`demo_races`, binarios `-race`, ~10 s de tráfico y uso de la
  web): 15–16 en el cliente y 6 en el servidor.
- **Tamaño de binarios** con `-s -w` y UI de relleno: cliente y servidor de
  6,0–6,2 MB en arm64 y 7,0–7,1 MB en mipsle. `protoclient` con `-Os` ocupa
  14 KB (x86_64, glibc dinámica; no es funcionalmente comparable).

### Extrapolación a la Pi (estimación, no medida)

- Por benchmarks de un núcleo, una Pi 4 es ~3–4× más lenta que la VM, una
  Pi 3 ~8× y una Pi 5 parecida.
- Cliente Go con 3 enlaces en una Pi 4: ~95 µs pasan a ~300–400 µs. Con
  ~1,3 núcleos útiles salen 3.000–4.500 pps, unos 35–50 Mbit/s, sin contar
  que WireGuard descifra N copias ni el coste de los drivers USB.
- La pérdida pesa mucho en TCP. Por Mathis (Reno, RTT 50 ms, MSS 1380), un
  0,3 % limita a ~5 Mbit/s por flujo y un 0,01 % a ~27 Mbit/s.

## Qué hacemos con esto

- Se porta el comportamiento (protocolo v0, semántica de la configuración), no
  el diseño de goroutines.
- Los problemas de arriba son pruebas de aceptación de la versión C:
  - `demo_stranger` no recibe nada;
  - `demo_webpanic` no tumba el túnel;
  - el escenario del enlace lento no afecta a los demás;
  - ThreadSanitizer limpio.

  Las tres primeras ya se cumplen (historia 005). La cuarta no aplica: el
  motor es de un solo hilo y no tiene web.

## Pendiente

- ~~Medir en la Pi real~~: confirmado por el usuario (historia 004).
- WireGuard real e iperf3 TCP en el laboratorio; `netem` en una máquina que
  lo tenga.
- Comprobar si algún enlace del usuario da más de 30 Mbit/s por sí solo.

## Cambios

- 2026-10-03: creada a partir de la primera sesión de medidas.
- 2026-10-03: confirmado en campo el límite de CPU en la Pi (historia 004); las
  pruebas de aceptación se cumplen en el motor C (historia 005).
