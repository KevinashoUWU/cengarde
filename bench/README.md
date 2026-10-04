# bench — laboratorio de rendimiento

Laboratorio reproducible para medir el motor cengarde en C (`engine/`, y en
el futuro eBPF), y compararlo con el engarde Go original, en una sola máquina
Linux, sin hardware. Dos network
namespaces unidos por tres pares veth que hacen de enlaces: `cli` hace de
Raspberry Pi y `srv` de VPS. WireGuard se sustituye por `udpgen`, un
emisor/receptor UDP que numera y marca con la hora cada paquete, así que se
miden pérdidas reales (paquetes sin ninguna copia), duplicados, reordenación y
latencia de un sentido.

```
netns cli (la Pi)                            netns srv (el VPS)
WG falso: udpgen 127.0.0.1:50000             servidor :59402 (engarde o cengarde)
cliente 127.0.0.1:59401                      WG falso: udpgen 127.0.0.1:59301
l1 10.0.1.1 ──────────────────────────────── s1 10.0.1.2
l2 10.0.2.1 ──────────────────────────────── s2 10.0.2.2
l3 10.0.3.1 ──────────────────────────────── s3 10.0.3.2
```

Las cifras citadas en [`ROADMAP.md`](../ROADMAP.md) y en las historias 001,
005 y 006 salen de aquí.

## Requisitos

root, iproute2 (`ip`, `tc` con `sch_tbf`), gcc, make y python3. Go y git solo
hacen falta para la línea base del engarde Go (`ENGINE=go`), y curl para sus
demos.

## Uso

```sh
sudo bench/lab.sh build    # udpgen, protoclient y cengarde en bench/bin/
sudo bench/lab.sh ci       # lo que corre el CI: smoke, health, control y los escenarios de lab.d con LAB_CI=1
sudo bench/lab.sh smoke    # prueba de humo de cengarde
sudo bench/lab.sh health   # salud de enlaces: un enlace con 500 ms de cola, subida y bajada (historia 006)
sudo bench/lab.sh control  # con tráfico: pausar un enlace, recargar dos veces, IP pass on/off; sin pérdidas (historia 009)
sudo bench/lab.sh latency  # latencia y CPU con busy_poll_us 0, 50 y 200, y el Go si está compilado (historia 006)

sudo ENGINE=go bench/lab.sh build  # además, el engarde Go (normal y -race)
sudo bench/lab.sh suite    # línea base del engarde Go (historia 001, ~5 min)
sudo bench/lab.sh compare  # engarde Go frente a cengarde, cada uno en ambos extremos (historia 005)
```

`ci` corre cada escenario en una subshell y con el laboratorio limpio, sigue
aunque uno falle y acaba con `ci: ok (...)` o `ci: FAILED: ...`; borra los
namespaces al salir, también si se interrumpe. El CI (`engine.yml`) solo llama
a `build` y a `ci`.

### Escenarios en `lab.d/`

Cada escenario nuevo va en su propio archivo, `bench/lab.d/NOMBRE.sh`, que
`lab.sh` carga al arrancar: define la función `NOMBRE` (se corre con
`sudo bench/lab.sh NOMBRE`), usa las funciones y variables de `lab.sh`
(`setup`, `start`, `teardown`, `jget`, `$RUN`, `$BIN`…) y pone `LAB_CI=1` si
el CI tiene que correrlo. Así, cambios en paralelo añaden escenarios sin tocar
`lab.sh`. Reglas:

- autocontenido: monta su laboratorio y lo desmonta con `teardown` al acabar,
  también si falla;
- termina con `NOMBRE: ok` o `NOMBRE: FAILED`, con una línea `FAIL: ...` que
  diga qué falló, y devuelve distinto de 0 si falló.

Paso a paso (pasa las mismas variables a `setup` y a `start`):

```sh
sudo NLINKS=3 bench/lab.sh setup     # netns, enlaces y configs en bench/run/
sudo NLINKS=3 bench/lab.sh start     # cengarde en ambos extremos (ENGINE=go: engarde)
sudo bench/lab.sh down 10000 5       # 10.000 pps de bajada (VPS -> Pi) durante 5 s
sudo bench/lab.sh up 10000 5         # subida (Pi -> VPS)
sudo bench/lab.sh shape l3 5mbit     # enlace lento con cola local (módem USB, WiFi)
sudo bench/lab.sh teardown
```

| Variable | Valor por defecto | Uso |
| --- | --- | --- |
| `NLINKS` | 3 | enlaces activos (1–3) |
| `SIZE` | 1400 | tamaño de paquete en bytes |
| `ENGINE` | `c` | `c`: cengarde (`engine/`) en ambos extremos, con configs INI en `bench/run/`; `go`: el engarde Go |
| `GO_REF` / `GO_REPO` | `3492df9…` / porech/engarde | de qué commit sale el engarde Go: del historial de este repositorio o, si no está (clon superficial), de `GO_REPO` |
| `WRITE_TIMEOUT` | 10 | `writeTimeout` del cliente Go, en ms (`-1` lo desactiva) |
| `PROTO` | vacío | con `ENGINE=go`, `c` o `dedup`: usa `protoclient` (C) en vez del cliente Go |
| `CLIENT_BIN` / `SERVER_BIN` | `bench/bin/engarde-*` | probar otros binarios de engarde |
| `CLIENT_EXTRA` / `SERVER_EXTRA` | vacío | ajustes extra de cengarde, `clave = valor` separados por `;` (p. ej. `busy_poll_us = 50`) |
| `CENGARDE_BIN` | `bench/bin/cengarde` | otro binario de cengarde, p. ej. un envoltorio que ejecuta la compilación de OpenWrt con su musl (historia 007) |
| `RUN` | `bench/run` | configs, logs, JSON de estado y sockets de control. La ruta de un socket Unix no pasa de 107 bytes: si el repositorio está muy hondo, `control` falla con `socket path too long` y hay que usar un `RUN` más corto |

Demos de los problemas del engarde Go descritos en el roadmap (necesitan
`ENGINE=go bench/lab.sh build`):
- `demo_stranger`: el servidor envía el tráfico del túnel a cualquiera que le
  mande un paquete. Con el valor por defecto, `ENGINE=c`, muestra en cambio
  que cengarde no responde a nadie sin autenticar.
- `demo_webpanic`: el cliente se cae si el puerto web está ocupado.
- `demo_races`: detector de carreras de Go, con tráfico y uso normal de la
  web.

## Cómo leer la salida

```
pps=10000 (112 Mbit/s) sent=49997 uniq=49997 loss=0.00% dup=99994 p50=110us p99=912us p99.9=2969us cpu client=93.0% server=30.8% | us/pkt client=93.0 server=30.8
```

- `loss`: paquetes de los que no llegó **ninguna** copia.
- `dup`: copias extra entregadas al "WireGuard". Con engarde Go son N−1 por
  paquete, y el WireGuard real las descifra todas antes de descartarlas;
  cengarde deduplica y entrega 0.
- `us/pkt`: CPU (usuario + kernel en el contexto del proceso) por paquete
  WireGuard. Incluye el trabajo del kernel al enviar y, en veth/loopback, parte
  de la recepción del otro extremo; es igual para todas las variantes, así que
  sirve para compararlas, no como coste absoluto.

## Limitaciones

- No hay cifrado: el coste de WireGuard (que con engarde Go descifra N copias
  de cada paquete) no aparece en las cifras.
- veth no es un módem: sin `sch_netem` no hay retardo, jitter ni pérdida en los
  enlaces; `tbf` solo emula un enlace lento con la cola en la propia máquina.
  Esa cola la limita también el `sndbuf` del socket (~190 ms a 5 Mbit/s por
  defecto), así que `health` lo sube a 4 MiB para tener 500 ms.
- Los valores absolutos dependen de la CPU (una VM x86 no es una Pi). Compara
  variantes en la misma máquina y, si puedes, ejecútalo también en la Pi.
- `protoclient.c` es el prototipo de ~200 líneas que sirvió para la primera
  medida, antes del motor; queda como referencia. El motor real está en
  `engine/`.
- `udpgen` envía paquetes con forma de mensaje de datos de WireGuard (tipo 4),
  porque cengarde solo aprende el puerto local de WireGuard de datagramas con
  esa forma.
