# 009 — Recarga en caliente, socket de control e IP pass desde el router

- **Fecha:** 2026-10-04
- **Estado:** vigente
- **Fuentes:**
  - código:
    - `engine/src/ctl.h` y `ctl.c`;
    - `config.c` (`cg_config_restart_needed`);
    - `os.c` (cargador y `cg_reexec`);
    - `client.c` y `server.c`;
    - `proto.h` (protocolo v3);
    - `openwrt/cengarde/files/` y `openwrt/luci-app-cengarde/`;
    - `contrib/vps/`;
  - engarde Go en `3492df9`: `cmd/engarde-client/webserver.go`
    (`get-list`, `include`, `exclude`, `swap-exclusion`, `reset-exclusions`)
    y `main.go:61-83` (`exclusionSwaps`);
  - procd `5670ff9`, `service/instance.c`:
    - :1244, `instance_restart` manda `reload_signal` en lugar de reiniciar;
    - :1300 y :1334, un cambio en el md5 de `file` o en `reload_signal`
      cuenta como cambio de la instancia;
  - rpcd `d99f703`:
    - `session.c:146`, permisos con `fnmatch` sin `FNM_PATHNAME`;
    - `file.c:1065-1086`, `exec` comprueba el ejecutable y después la línea
      «ejecutable arg1 arg2…»;
  - systemd v249, `src/core/path.c`: `path_spec_watch`,
    `path_spec_fd_event` y `path_spec_check_good`;
  - medidas: `sudo bench/lab.sh control`, `openwrt/test/e2e.sh` y la prueba de
    `cengarde-nat` en netns.

## TL;DR

- **Paridad con el engarde Go.** El gestor web de engarde Go permitía
  excluir o incluir enlaces en marcha y listarlos. cengarde lo hace con un
  socket Unix y `cengarde ctl`:
  - `status` y `links`;
  - `link NOMBRE off|on|auto` y `reset`;
  - `reload`.

  En LuCI, cada enlace de la página de estado tiene un botón Pausar/Reanudar.
- **Recarga en caliente** con `SIGHUP` o `ctl reload`:
  - se aplica en el lugar y con la misma sesión casi todo: enlaces,
    servidores, salud, sondas, buffers, log, estado e IP pass;
  - el proceso se reinicia solo (mismo PID, sesión nueva) si cambian `mode`,
  `key`, `listen`, `control_socket`, las perillas de CPU, `wireguard` o
  `max_sessions`;
  - un archivo con errores no se aplica: el motor sigue con el anterior y
    publica el error (`config_error`).
- **Ni un paquete perdido** en el laboratorio (2000 pps, 12 s): 23 999 de
  23 999 con un enlace pausado, dos recargas y el enlace reanudado, en la
  misma sesión.
- **IP pass gobernado desde el router** (protocolo v3):
  - las sondas llevan `CG_F_PASS_SET`/`CG_F_PASS`, dentro de la cabecera
    autenticada;
  - el servidor escribe `on` u `off` en `passthrough_file`, según lo que
    pida la sesión más nueva, y lo confirma en sus respuestas;
  - en el VPS, una unidad `.path` de systemd ejecuta `cengarde-nat sync`,
    que guarda la última palabra del router para los reinicios.
- **OpenWrt:**
  - procd manda `SIGHUP` (`reload_signal`) en vez de reiniciar;
  - la línea de comando es siempre `-c /var/etc/cengarde.conf` (con un
    enlace simbólico si hay `config_file`), porque procd también manda la
    señal cuando cambia el comando.

## Contexto

Después de quitar el código Go (historia 001), quedaban dos cosas del
engarde Go que cengarde no tenía:

- **Tocar los enlaces en marcha.** El gestor web del Go listaba las
  interfaces (`get-list`) y permitía excluirlas o incluirlas sin reiniciar.
  Las exclusiones eran temporales: `exclusionSwaps` invierte lo que dice la
  configuración y se pierde al reiniciar.
- **Cambiar la configuración sin cortar:**
  - en cengarde, un cambio en LuCI reiniciaba el motor: sesión nueva y
    unos paquetes perdidos;
  - cada evento de una interfaz de subida podía regenerar la configuración.

El IP pass (historia 008) ya funcionaba, pero para encenderlo o apagarlo
había que entrar al VPS: editar `nat.conf` y reiniciar `wg-quick@wg0`.

## Hallazgos

### Socket de control

- **Stream y no datagrama:** el estado JSON de un servidor con muchas
  sesiones no cabe en un datagrama Unix. Con stream, cada conexión lleva un
  comando de una línea y su respuesta de texto, y el motor la cierra.
- **Sin bloquear el bucle:**
  - lee y escribe con `MSG_DONTWAIT`;
  - hasta 4 conexiones y un plazo de 5 s;
  - mientras espera una respuesta diferida (`reload`), la conexión sale de
    epoll: así ignora a un cliente que cerró su mitad.
- **Solo root:**
  - el socket se crea con `umask 077`;
  - reemplaza un socket viejo, pero nunca uno vivo (lo prueba con
    `connect`) ni un archivo de otro tipo.
- **Sin socket por omisión en el motor:** el laboratorio corre cliente y
  servidor en el mismo sistema de archivos. `cengarde ctl` sí usa
  `/var/run/cengarde/cengarde.sock` por omisión, que es la misma ruta en
  OpenWrt y en Ubuntu (`/var/run` apunta a `/run`).
- **Pausas:**
  - una tabla de hasta 32 nombres, también para interfaces que aún no
    existen, con funciones puras y tests (`ctl.h`);
  - un enlace pausado conserva su hueco para que el estado lo muestre;
  - se mantienen en las recargas y se pierden al reiniciar, como en el Go.

### Recarga

- **El archivo se lee en un hilo:** `getaddrinfo` puede esperar segundos al
  DNS, y los eventos de las interfaces de subida llegan justo cuando la red
  anda mal.
  - Al terminar, el hilo avisa por un `eventfd`.
  - Si llegan más pedidos mientras carga, se juntan en una carga más, y
    quien esperaba recibe el resultado de la última.
- **Qué exige reiniciar** (`cg_config_restart_needed`, con tests): lo que el
  bucle arma una sola vez, es decir sockets, clave, tamaño de la tabla de
  sesiones y planificación.
  - El reinicio es `execv("/proc/self/exe")`, con las señales todavía
    bloqueadas, así que no se pierde ninguna.
  - Antes del `exec` se devuelven la afinidad de CPU y la política
    `SCHED_OTHER`, porque sobreviven al `exec`: si no, quitar `cpu` no
    tendría efecto.
- **Hilos auxiliares en planificación normal:** el escritor de estado y el
  cargador vuelven a la afinidad original y a `SCHED_OTHER`
  (`cg_thread_normal`).

### procd y systemd

- **procd (`instance.c`):**
  - cuando la instancia cambia, `instance_restart` manda `reload_signal` si
    está definido, y si no reinicia (:1244);
  - cuenta como cambio el md5 de cada `file` (:1300), pero también el
    comando;
  - si `config_file` cambiara de ruta, el motor recibiría `SIGHUP` y
    releería la ruta vieja. Por eso el comando nunca cambia.
- **systemd:**
  - la unidad entrega la configuración como credencial, una copia hecha al
    arrancar;
  - un `SIGHUP` leería esa copia, así que en el VPS el camino sigue siendo
    `systemctl restart`. El servidor casi no cambia.
- **Unidad `.path` y reemplazo atómico (systemd v249):**
  - el escritor reemplaza el archivo (temporal + `rename`), y eso dispara
    `PathChanged`;
  - si el archivo no existía, se cumple `path_spec_check_good`, porque
    cambió su existencia;
  - si existía, el inodo vigilado recibe el evento: el `rename` le cambia
    el número de enlaces.
  - No se pudo probar aquí: el contenedor no corre systemd.

### IP pass

- **Tres estados en dos bits:** pedir `on`, pedir `off` o no pedir nada.
  Sin `passthrough` en su configuración, un cliente no apaga el IP pass que
  alguien configuró a mano en el VPS.
- **Decide la sesión más nueva:**
  - al reiniciarse, un cliente abre una sesión nueva, y la vieja dura
    `session_timeout_ms` (180 s);
  - con «gana cualquiera», un pedido viejo podría ganar;
  - sin sesiones, el estado no cambia: un router apagado no apaga el IP
    pass.
- **El servidor no toca el cortafuegos:** corre sin privilegios
  (`DynamicUser`).
  - Escribe su deseo a un archivo, desde un segundo hilo escritor; si el
    hilo estaba ocupado, reintenta en el siguiente tic.
  - En sus respuestas confirma lo que entregó, y el cliente lo publica
    (`passthrough.server`).
- **Orden de prioridad en `cengarde-nat`:**
  1. lo que pide el router (`/run/cengarde/passthrough`);
  2. lo último que pidió (`/var/lib/cengarde-nat/passthrough`, que
     sobrevive a los reinicios del VPS);
  3. `PASSTHROUGH` de `nat.conf`.

  Así, apagarlo desde el router no se deshace cuando el VPS reinicia.
- **El protocolo sube a v3.** Las banderas nuevas cambian el formato. Un
  router 0.4 no habla con un VPS anterior: hay que actualizar los dos (la
  regla de CLAUDE.md).

### LuCI y rpcd

- **Botones de pausa:** usan `fs.exec` con el permiso
  `"/usr/sbin/cengarde ctl link *"`.
  - rpcd compara con `fnmatch` la línea completa «ejecutable args», y el
    `*` abarca espacios.
  - Los argumentos no pasan por un shell, y `cengarde ctl` valida el nombre
    de la interfaz como el kernel (`dev_valid_name`).

## Medidas

- **Laboratorio** (`sudo bench/lab.sh control`, 3 enlaces, 2000 pps durante
  12 s):
  - 23 999 de 23 999 paquetes, sin duplicados;
  - pausa de `l3`, recarga por `SIGHUP` con IP pass apagado, `ctl reload`
    con IP pass encendido, y `l3` reanudado: la sesión no cambió;
  - el servidor escribió `on`, `off` y `on` en su `passthrough_file`;
  - cambiar `cpu` reinició el cliente en el lugar: mismo PID, sesión nueva,
    y el socket de control volvió.
- **Con ASan y UBSan:**
  - el mismo escenario, sin errores ni fugas;
  - una recarga con un valor inválido devuelve el error y no se aplica;
  - 20 `SIGHUP` seguidos se juntan sin problemas.
- **`cengarde-nat` en netns, con iptables reales:**
  - el reenvío se abre y se cierra con `sync`, sin duplicar reglas;
  - si el servidor se reinicia (ya no hay pedido), se mantiene la última
    palabra del router aunque `nat.conf` diga lo contrario;
  - `down` lo quita todo;
  - un pedido hecho con el túnel caído se aplica en el siguiente `up`.
- **Tamaño:** el binario de OpenWrt x86_64, sin símbolos, pasó de 70 a
  97 KB. Con `-Os`, el texto de `client.c` crece 6 KB, el de `server.c`
  4,6 KB y `ctl.c` suma 4,2 KB.
- **QEMU** (`openwrt/test/e2e.sh`, OpenWrt 25.12.5):
  - LuCI muestra «activo en el VPS»;
  - pausar y reanudar `up3` desde la página de estado;
  - apagar IP pass con pings en curso: 0 % de pérdida, el mismo PID del
    motor, y el VPS escribe `off`.

## Qué hacemos con esto

- Mantener el control en un socket local, sin servidor web propio: LuCI ya
  es la interfaz.
- Cualquier opción nueva del motor se decide en `cg_config_restart_needed`:
  en el lugar si se puede, con reinicio si el bucle la arma una sola vez.

## Pendiente

- **Probar la unidad `.path` en un VPS real:** el contenedor no tiene
  systemd.
- **Servidor:** responder desde la dirección de llegada (`IP_PKTINFO`),
  pendiente desde la historia 008.
- **LuCI:** mostrar las pausas de interfaces que no existen (hoy solo
  `cengarde ctl links` las muestra, como «absent»).

## Cambios

- 2026-10-04: creada.
