# Motor cengarde (C)

Reescritura en C de engarde: duplica cada datagrama de WireGuard por todos los
enlaces del cliente hacia el servidor, y en el otro extremo entrega a
WireGuard solo la primera copia. Cliente y servidor son el mismo binario; el
modo lo decide la configuración. El diseño y sus razones están en
[`docs/historias/005-motor-c-v1.md`](../docs/historias/005-motor-c-v1.md).

**No es compatible en el cable con engarde Go:** usa su propia cabecera
autenticada, así que los dos extremos tienen que ser cengarde.

## Compilar

```sh
make -C engine            # binario engine/cengarde
make -C engine test       # tests unitarios
make -C engine SANITIZE=1 test   # con ASan y UBSan
```

Solo necesita un compilador C11 y las cabeceras de Linux, sin más
dependencias. Respeta `CC`, `CFLAGS` y `LDFLAGS` para compilar en cruzado (por
ejemplo `make CC=aarch64-linux-gnu-gcc`) y dentro del SDK de OpenWrt.

## Puesta en marcha

1. **Clave compartida** (la misma en los dos extremos):

   ```sh
   cengarde genkey
   ```

2. **Servidor (VPS):** copia `examples/server.conf`, pon la clave y la
   dirección de escucha de WireGuard (`wireguard = 127.0.0.1:51820`).
   Compruébala con `cengarde -t -c server.conf` y arranca con
   `cengarde -c server.conf`. Abre en el cortafuegos el puerto UDP de `listen`
   (59402 por defecto).

3. **Cliente:** copia `examples/client.conf`. Pon la clave, `server = <VPS>:59402`
   y los enlaces: patrones en `interfaces` o secciones `[link NOMBRE]`.

4. **WireGuard del cliente:** el `Endpoint` del peer pasa a ser la dirección
   `listen` del cliente (`127.0.0.1:59401`). El servidor WireGuard no cambia.

5. **MTU de WireGuard:** cengarde añade 24 bytes por paquete. Con una ruta de
   1500 bytes el máximo es 1416 sobre IPv4 y 1396 sobre IPv6
   (1500 − IP − 8 UDP − 24 cengarde − 32 WireGuard). En redes móviles conviene
   ir más bajo, por ejemplo `MTU = 1380`, o menos si algún operador tiene un
   MTU de camino menor.

## Qué hace cada extremo

- **Cliente:**
  - abre un socket por enlace (`SO_BINDTODEVICE` + bind a la IP del enlace);
  - sigue los cambios de interfaces y direcciones por netlink;
  - manda cada paquete por todos los enlaces vivos, sin bloquear: un enlace
    lleno pierde solo sus copias;
  - entrega a WireGuard la primera copia de cada paquete del servidor;
  - envía una sonda por enlace cada segundo para medir RTT y mantener el NAT.
- **Servidor:**
  - crea una sesión por cliente solo a partir de un paquete autenticado;
  - cada sesión tiene su propio socket hacia WireGuard, así que varios
    clientes comparten un puerto;
  - aprende los caminos (sesión + enlace) solo de paquetes autenticados,
    incluidos los cambios de NAT;
  - reparte la bajada por todos los caminos vivos.

## Estado

`status_file` escribe cada segundo un JSON con contadores globales y, por
enlace:
- estado (`live`, `stalled` o `down`) y RTT;
- paquetes y bytes enviados, descartados y errores;
- `rx_first`: cuántas veces llegó primero;
- `rx_lag_ms`: retraso medio frente a la primera copia;
- `rx_missed`: paquetes que no trajo;
- lo mismo visto desde el otro extremo (`server_view` / `client_view`).

## Limitaciones conocidas

- **Servidor con varias IPs públicas:** responde desde la IP que elija el
  kernel. Usa `listen = <IP pública>:59402`.
- **Puerto de WireGuard en el cliente:** el cliente lo aprende del primer
  paquete que WireGuard le envía, como engarde.
- **Aún no** baja privilegios, no recarga la configuración y el servidor es de
  un solo hilo. La salud de los enlaces se limita a sacar de la rotación los
  que se quedan mudos; el silenciado por retraso es la Fase 2.
