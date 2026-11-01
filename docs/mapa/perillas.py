#!/usr/bin/env python3
"""The knob table of the map: docs/mapa/Perillas/*.md and Perillas.md are
generated from T below by mapa.py (perillas, check), which also checks T
against every key engine/src/config.c parses and its numeric defaults
against the parser itself."""
import os
import re

OUT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(OUT, "..", "..", "engine", "src", "config.c")

# piece note names
SONDAS = "Sondas y cola de bonding (proto.h)"
SALUD = "Salud y silenciado (health.h)"
COLA = "Cola por enlace (cg_gov_base)"
CAP = "Capacidad (capacity.h)"
CAKE = "Tope para cake (capacity.h)"
SLOT = "Ranuras de Starlink (slot.h)"
ARR = "Llegadas (arrival.h)"
MODEL = "Modelo de enlace (linkmodel.h)"
GOV = "Gobernador (govern.h)"
SPLIT = "Reparto (split.h)"
G3 = "G3 enlace sin respuesta (split.h)"
G4 = "G4 guarda de dispersión (split.h)"
SDUP = "Copias bajo estrés (split.h)"
ETA = "ETA (eta.h)"
COPIAS = "Copias a tiempo (eta.h)"
TICKLE = "Cosquilla (tickle.h)"
DOM = "Enlace dominante (dominant.h)"
PACE = "Dosificación (pace.h)"
TCPF = "Liberación TCP (tcpfree.h)"
TCPA = "Retransmisión según la evidencia (tcpack.h)"
SRV = "Elección de servidor (srvpick.h)"
NAMES = "Nombres del servidor (resolve.h)"
RO = "Reordenador (reorder.h)"
ACT = "Enlaces activos (active.h)"
LANES = "Colas del servidor (steer.h)"
RCVB = "Presupuesto de recepción (rcvbudget.h)"
THR = "Hilos por enlace (thrplan.h)"
RESC = "Rescate (rescue.h)"
FEC = "FEC (fec.h)"
F3 = "FEC v3 (fec3.h)"
CLIENTS = "Varios clientes (clients.h)"
FWD = "Tabla de reenvío (fwdtable.h)"
TUNR = "Rutas del túnel nativo (tunroute.h)"
DEC = "Bitácora de decisiones (decision.h)"
CONF = "Configuración (config.c)"
SPARE = "Lo que sobra (spare.h)"
CODES = "Códigos de borrado (rs.h, rlc.h)"
HARQ = "Reparación coherente (harq.h)"
PCTL = "Protocontrolador (protoctl.c, stream.h, streamcc.h)"
SUBF = "Subflujo (subflow.h)"
LCC = "Ritmo por enlace (linkcc.h)"

H = {
    "002": "002-wireguard-para-cengarde", "005": "005-motor-c-v1", "006": "006-salud-de-enlaces",
    "008": "008-luci-uci-y-emparejamiento", "009": "009-recarga-control-e-ip-pass",
    "010": "010-ipv6-varias-ip-multicliente-nombres", "011": "011-hilos", "012": "012-bonding",
    "013": "013-capacidad-y-dosificacion", "014": "014-tunel-nativo", "015": "015-modelo-de-enlace", "016": "016-celda-virtual",
    "017": "017-terreno-nostalgica", "019": "019-perdida-con-cola", "020": "020-copias-parciales-y-paridad", "021": "021-fec-v2",
    "023": "023-reparacion-coherente", "025": "025-protocontrolador", "026": "026-fec-v3",
    "027": "027-adios-wireguard", "029": "029-mpgrit",
    "audit": "audit-pending",
}

G, L, K = "global", "`[link NOMBRE]`", "`[client NOMBRE]`"
BOTH, CLI, SRVS = "los dos", "cliente", "servidor"

# name: (sections, side, default, range, pieces, stories, description)
T = {
 # --- basic ---
 "mode": ([G], BOTH, "(obligatoria)", "`client` o `server`", [CONF], ["005"], "Qué extremo es este proceso."),
 "key": ([G, K], BOTH, "(obligatoria; con secciones `[client]`, una en cada una y ninguna global)", "base64 de 32 bytes", [CONF, CLIENTS], ["008"], "El secreto del par: de él salen las claves del MAC (`pair.h`). En `[client NOMBRE]`, la clave de ese router."),
 "description": ([G], BOTH, "vacía", "hasta 127 caracteres", [CONF], [], "Texto libre que va al estado."),
 "status_file": ([G], BOTH, "vacía (no escribe)", "ruta", [CONF], ["006"], "Dónde se escribe el JSON de estado (`status.h`), atómico, desde un hilo aparte."),
 "status_interval_ms": ([G], BOTH, "1000", "100–3600000", [CONF], [], "Cada cuánto se escribe el estado."),
 "control_socket": ([G], BOTH, "vacía (sin socket)", "ruta absoluta", [CONF], ["009"], "El socket Unix de `cengarde ctl` (`ctl.h`)."),
 "rcvbuf": ([G], BOTH, "4194304 (4 MiB)", "0–2^30 bytes", [RCVB], ["011"], "El búfer de recepción de los sockets. En el servidor, el presupuesto de recepción lo recorta según `udp_mem`."),
 "sndbuf": ([G], CLI, "0 (el del kernel)", "0–2^30 bytes", [CONF], [], "El búfer de envío de los sockets de los enlaces."),
 "log_level": ([G], BOTH, "`info`", "`error`, `warn`, `info`, `debug`", [CONF], [], "Nivel del registro."),
 "listen": ([G], SRVS, "`*:59402`", "una dirección", [CONF], ["027"], "Dónde escucha el servidor a los enlaces. En el cliente es un error desde 0.12 (era la dirección para el WireGuard local)."),
 "cpu": ([G, L], BOTH, "-1 (cualquiera)", "0–1023", [THR], ["006", "011"], "Fija el proceso (global) o el hilo de un enlace (`[link]`, con `link_threads = on`) a una CPU."),
 "busy_poll_us": ([G], BOTH, "0", "0–1000000", [THR], ["006"], "Tras tráfico, sondea sin dormir este tiempo. `thrplan.h` lo limita si los hilos no caben en las CPU."),
 "rt_priority": ([G], BOTH, "0 (normal)", "0–99", [THR], ["006"], "Prioridad `SCHED_FIFO` de los hilos de datos."),
 # --- tunnel ---
 "tunnel": ([G], BOTH, "`native`", "`native` (`wireguard` es un error desde 0.12)", [TUNR, TCPF], ["014", "027"], "Qué lleva el túnel: paquetes IP de una TUN propia (autenticados, sin cifrar), el único modo desde 0.12."),
 "tun": ([G], BOTH, "`cg0`", "nombre de interfaz", [TUNR], ["014"], "Nombre de la TUN del túnel."),
 "tun_addr": ([G], BOTH, "ninguna (las pone el sistema)", "hasta 4 prefijos", [TUNR], ["014"], "Direcciones que cengarde pone en la TUN."),
 "tun_mtu": ([G], BOTH, "1420", "576–2024", [TUNR], ["014"], "MTU de la TUN."),
 "tun_txqueuelen": ([G], BOTH, "0 (el del kernel, 500)", "0–100000 paquetes", [TUNR], ["audit"], "Cola de transmisión de la TUN."),
 "tun_route": ([G, K], SRVS, "un cliente sin ella: `0.0.0.0/0` y `::/0`", "hasta los prefijos de `CG_TUN_ROUTES`", [TUNR], ["014"], "Prefijos de cada cliente en la TUN del servidor; también comprueba el origen de lo que manda (como AllowedIPs)."),
 # --- health ---
 "mute_behind_ms": ([G], BOTH, "150", "0–60000 (0: sin silenciado)", [SALUD], ["006"], "Atraso frente al enlace más rápido que silencia un enlace tras `mute_settle_ms`."),
 "unmute_behind_ms": ([G], BOTH, "4/5 de `mute_behind_ms` (120)", "0–`mute_behind_ms`", [SALUD], ["006"], "Atraso bajo el cual un enlace silenciado vuelve (tras 2 × `mute_settle_ms`)."),
 "mute_settle_ms": ([G], BOTH, "2000", "100–600000", [SALUD, G4], ["006", "012"], "Cuánto tiene que durar la condición para silenciar (y el doble para volver). G4 lo usa para la vuelta al reparto."),
 "mute_trickle": ([G], BOTH, "0", "0–1000000", [SALUD], ["006"], "Un enlace silenciado lleva un paquete duplicado de cada tantos (0: ninguno)."),
 "min_active_links": ([G], BOTH, "2", "1–16", [SALUD], ["006"], "Enlaces vivos que siempre llevan todo; el silenciado se detiene ahí."),

 # --- probes / servers (client) ---
 "probe_interval_ms": ([G], CLI, "100", "100–60000", [SONDAS, SALUD, G3], ["006"], "Intervalo de sondas con tráfico. G3 y el mudo cuentan en intervalos de sonda."),
 "probe_idle_ms": ([G], CLI, "1000", "`probe_interval_ms`–600000", [SONDAS], ["006"], "Intervalo de sondas en reposo."),
 "server": ([G, L], CLI, "(obligatoria, global o por enlace)", "hasta 8 entradas: IPv4, `[IPv6]` o nombres", [SRV, NAMES], ["010"], "Lista ordenada de direcciones del servidor; un nombre lo resuelve el cliente por los enlaces."),
 "resolver": ([G, L], CLI, "global: `1.1.1.1 9.9.9.9 [2606:4700:4700::1111] [2620:fe::fe]`; enlace: ninguno", "hasta 8 direcciones", [NAMES], ["010"], "Servidores DNS para los nombres de `server`; los del enlace van primero."),
 "server_failover_ms": ([G], CLI, "el mayor de 10000 y 3 × `probe_idle_ms`", "0 (nunca) o ≥ 3 × `probe_idle_ms`, hasta 3600000", [SRV], ["010"], "Sin respuesta verificada este tiempo, el enlace pasa a la siguiente dirección."),
 "interfaces": ([G], CLI, "`*`", "patrones", [CONF], ["009"], "Interfaces que pueden llevar el túnel."),
 "exclude": ([G], CLI, "vacía (más las de fábrica: `lo wg* docker* veth* br-* virbr* tun* tap* ifb* dummy*`)", "patrones", [CONF], ["009"], "Interfaces que nunca llevan el túnel."),
 "passthrough": ([G, K], BOTH, "cliente: sin pedir (-1); `[client]`: `yes`", "`yes` o `no`", [FWD], ["009", "010"], "Cliente: pide IP pass al servidor. `[client NOMBRE]`: si ese router puede tenerlo."),
 "link_threads": ([G], CLI, "`legacy`", "`auto`, `on`, `off`, `legacy`", [THR], ["011"], "Cómo lee el cliente sus enlaces: el bucle de 0.4, un hilo por enlace o la estructura nueva en un hilo."),
 "io_queue": ([G], CLI, "256", "64–1024, potencia de dos", [THR], ["011"], "Entradas de cada anillo hub → bomba."),
 # --- link section ---
 "label": ([L, K], BOTH, "vacía", "hasta 63 caracteres", [CONF], [], "Nombre visible del enlace o del cliente."),
 "enabled": ([L, K], BOTH, "`yes`", "`yes` o `no`", [CONF, CLIENTS], [], "`[link]`: si el enlace se usa. `[client]`: con `no`, el servidor rechaza sus sondas."),
 "weight": ([L], CLI, "1", "0–1000", [SPLIT, GOV], ["012"], "Parte del reparto con `split = on` (subida, y bajada vía sondas). 0: copia de todo."),
 "type": ([L], CLI, "`auto`", "`auto`, `cellular`, `starlink`, `fiber`, `ethernet`, `radio`", [MODEL], ["015"], "Tipo del enlace: elige el preset de los lazos; viaja al VPS en la cola de bonding."),
 # --- server ---
 "max_sessions": ([G], SRVS, "64 (o 4 por cliente si es más)", "1–4096", [CLIENTS], ["010"], "Límite de sesiones."),
 "session_timeout_ms": ([G], SRVS, "180000", "1000–86400000", [CLIENTS], ["010"], "Una sesión sin tráfico este tiempo se cierra."),
 "path_timeout_ms": ([G], SRVS, "30000", "1000–3600000", [CLIENTS], ["010"], "Un camino sin tráfico este tiempo se olvida."),
 "passthrough_file": ([G], SRVS, "vacía", "ruta (solo con un cliente)", [FWD], ["009"], "Dónde va el IP pass que pide el cliente."),
 "forward_file": ([G], SRVS, "vacía", "ruta (solo con `[client]`)", [FWD], ["010"], "La tabla de reenvío que lee `cengarde-nat`."),
 "forward": ([K], SRVS, "ninguna", "reglas `tcp:9000=22 udp:5000-5010`", [FWD], ["010"], "Puertos públicos que van a ese router."),
 "lanes": ([G], SRVS, "`auto` (8)", "`auto`, 1, 2, 4, 8, 16", [LANES, PACE], ["011"], "Sockets de escucha guiados por número de enlace (cBPF); 1 es el socket de antes."),
 # --- reorder ---
 "reorder": ([G], BOTH, "`yes`", "`yes` o `no`", [RO], ["012"], "Pone en secuencia los DATA del otro extremo antes de entregarlos. Se anuncia en la cola de bonding."),
 "reorder_max_wait_ms": ([G], BOTH, "200", "0–2000", [RO], ["012", "013"], "Lo más que un hueco retiene lo que viene detrás, cuando hay reparto."),
 "reorder_dup_wait_ms": ([G], BOTH, "0", "0–`reorder_max_wait_ms`", [RO], ["012"], "La misma espera donde nada se reparte (redundancia). 0: no retiene."),
 "reorder_margin_ms": ([G], BOTH, "10", "0–1000", [RO], ["013"], "Se suma al retardo del enlace más lento al esperar un hueco."),
 "reorder_bypass": ([G], BOTH, "128", "0–2048 bytes", [RO], ["012"], "Paquetes de hasta este tamaño se entregan sin esperar."),
 "reorder_by_link": ([G], BOTH, "`no`", "`yes` o `no`", [RO], ["013"], "Un hueco espera solo al enlace por el que salió (`CG_F_PREV`). Experimental."),
 "reorder_adaptive": ([G], BOTH, "`no`", "`yes` o `no`", [RO], ["013"], "El margen de cada enlace aprende de sus tardíos. Experimental."),
 "reorder_pool_kb": ([G], BOTH, "8192", "256–131070", [RO], ["012", "013"], "Memoria para lo retenido."),
 "reorder_active": ([G], BOTH, "`yes`", "`yes` o `no`", [RO, ACT], ["013"], "Un hueco espera solo a los enlaces por los que el otro extremo llevó datos hace poco (la máscara de sus sondas, C2); `no`: a todos los no silenciados, como antes."),


 "tcp_aware": ([G], BOTH, "`no`", "`yes` o `no`", [TCPF, RO], ["014"], "Marca libres los segmentos sin datos y las retransmisiones de TCP del túnel."),
 # --- rescue / fec ---
 "rescue_kb": ([G], BOTH, "0 (apagado)", "0–262144", [RESC], ["013"], "Anillo de lo enviado hace poco, para los NACK (servidor: bajada; cliente: subida)."),
 "rescue_pct": ([G], BOTH, "5", "1–50", [RESC], ["013"], "Presupuesto del rescate, en % de lo enviado."),
 "rescue_wait_ms": ([G], BOTH, "0 (no pide)", "0–1000", [RESC, RO, HARQ], ["013", "023"], "Cuánto espera un hueco pedido. Con `fec`, al menos 2 × `fec_wait_ms` + 20; con un código protegiendo, más `rescue_code_ms`; con TCP adentro, nunca más que su presupuesto (`rescue_tcp_race`)."),
 "rescue_tries": ([G], BOTH, "2", "1–4", [RESC, RO], ["013"], "Cuántas veces se pide un hueco dentro de `rescue_wait_ms`."),
 "rescue_code_ms": ([G], SRVS, "100", "0–1000", [HARQ, RESC], ["023"], "Con un código protegiendo (sus reparaciones o paridades llegan), cuánto espera un hueco a que lo rehaga antes de pedir; se suma a `rescue_wait_ms`. En la práctica decide el sello: una reparación cuya base pasó el hueco."),
 "parq": ([G], SRVS, "`yes`", "`yes` o `no`", [HARQ, CODES], ["023"], "Un hueco que el código no rehízo pide su déficit de rango en paridad (`CG_T_PARQ`: combinaciones nuevas de RLC o índices nuevos del bloque RS), no los paquetes. Con `no`, el paquete (NACK)."),
 "rescue_tcp_race": ([G], BOTH, "`yes`", "`yes` o `no`", [HARQ, TCPF], ["023"], "Con TCP adentro (túnel nativo), ningún hueco se retiene ni se pide más allá del presupuesto de TCP (su RTT más un cuarto, RACK): lo que no llega antes queda para la retransmisión de TCP."),
 "tcp_retx_one": ([G], BOTH, "`yes`", "`yes` o `no`", [TCPF, HARQ, TCPA], ["023"], "Con `tcp_aware`, en redundancia la primera retransmisión de un segmento va por un solo enlace (el más rápido), no copiada; una segunda del mismo, copiada. Con `no`, toda retransmisión va copiada (y `tcp_ack_rule` no cuenta)."),
 "tcp_ack_rule": ([G], BOTH, "`yes`", "`yes` o `no`", [TCPA, TCPF], ["023"], "Con `tcp_retx_one`, los enlaces de una retransmisión según lo que se sabe de su original: ya ACK o SACK, por un enlace (también repetida); perdida por el túnel en todos los enlaces, copiada (también la primera); entregada por el túnel, en camino o nada sabido, como `tcp_retx_one`. Lo dicen los ACK que vuelven y la marca `CG_F_TUNHOLE` del otro extremo."),
 "rescue_target_ms": ([G], BOTH, "20 (el de `governor_target_ms`; 0 lo sigue)", "0–1000", [RESC, CAP], ["audit"], "Cola donde empieza a bajar el peso del rescate (cero a 4 veces). Auditoría C12."),

 "rescue_full_hyst": ([G], BOTH, "0 (apagado)", "0–2", [RESC, CAP, MODEL], ["015", "audit"], "Histéresis del bit `full` del rescate (C11). Experimento, campo del modelo."),
 "fec": ([G], BOTH, "`no`", "`yes` o `no`", [FEC, F3, CODES], ["013", "021", "026"], "Paridad XOR de cada grupo de DATA repartido, o con `fec_code = rlc` la ventana deslizante de la FEC v3. En el servidor, también la memoria con la que usa las paridades y las reparaciones de `redund_spare` (`parity`, `mix`, `rs`, `rlc`) y de la FEC v3 (el anillo de 2048 DATA y las ecuaciones: ~5 MB por sesión). En el router, con un servidor que manda reparaciones en la bajada (`fec_code = rlc` allí), el mismo anillo y las ecuaciones para rehacerlas."),
 "fec_code": ([G], BOTH, "`xor`", "`xor` o `rlc`", [F3, FEC, CODES], ["026"], "Lo que manda `fec = yes`: `xor`, la paridad espacial de siempre (un paquete por enlace por grupo, rehace una pérdida por grupo); `rlc`, la FEC v3: reparaciones de una ventana deslizante sobre todo DATA, una parte fija de los bytes repartida por los enlaces, con reparto o con un solo enlace (protocolo 11; el servidor con `fec = yes`). En el servidor, las mismas reparaciones sobre la bajada de cada sesión (~2 MB más por sesión; `cengarde-vps-setup fec rlc`), que el router rehace con `fec = yes`. Se cambia sin reiniciar."),
 "fec_pct": ([G], BOTH, "20", "1–50", [F3], ["026"], "FEC v3: las reparaciones, % de los bytes de DATA: la parte entera tras una ráfaga, menos con pérdida al azar sola (`fec_pct_min`). Un enlace de parte s detenido T se rehace unos s·T / (r·(1 − s)) después de empezar. Medido con cuatro enlaces: 20 % rinde en la pérdida al azar y en detenciones de 50 ms, 33 % en las de 100–200 ms; con BBR se paga la parte entera."),
 "fec_pct_min": ([G], BOTH, "0", "0–50", [F3, "Observaciones del enlace (linksig.h)"], ["026"], "FEC v3: la parte de las reparaciones cuando no hay pérdida que la pida (`cg_f3_pct`). Lo que la pide, en muestras **sin cola** (la más alta de la muestra y de la anterior, 10 ms sobre el mínimo del minuto): una **ráfaga** (5 % o más en la muestra: una detención, un corte) da `fec_pct` entero por 30 s; la pérdida **al azar**, además **bajo la capacidad del enlace**, esto más 25 veces la pérdida (pesada por los bytes de cada enlace) hasta `fec_pct`, entera 30 s y luego a la mitad cada 10 s (se desvanece, no se corta: cortarla de golpe hacía ping-pong con cubic). Los primeros 10 s, `fec_pct` entero (nada se sabe aún). Los bytes del protocontrolador (B, y A con BBR en el medio) no la piden con un solo enlace; con varios sí (el reordenador espera cada hueco). La pérdida de un enlace con cola o lleno (TCP que desborda; BBR hace 15–70 ms de cola) no cuenta: las reparaciones sobre ella pedían más reparaciones. Igual a `fec_pct` o más: la parte fija."),
 "fec_window_ms": ([G], BOTH, "300", "10–1000", [F3, CODES], ["026"], "FEC v3: hasta dónde alcanza una reparación (una de cada dos cubre su último cuarto); como mucho `CG_RLCW_SPAN` (1024) secuencias, y lo útil queda dentro de lo que el receptor espera un hueco (su plazo más `rescue_code_ms`)."),
 "fec_single": ([G], BOTH, "`auto`", "`auto`, `yes` o `no`", [F3, "Observaciones del enlace (linksig.h)"], ["026"], "FEC v3 con un solo enlace: `auto` repara mientras pierde al azar (sin cola, como Starlink) y no con pérdida por congestión ni en un corte (pérdida sobre la mitad de la tasa); `yes` siempre, `no` nunca."),
 "fec_wait_ms": ([G], BOTH, "10", "1–200", [FEC, RESC], ["013"], "Plazo máximo de un grupo antes de mandar su paridad."),
 "fec_share_max": ([G], BOTH, "0 (sin tope)", "0–100", [FEC, SPLIT], ["012", "013"], "Con `fec`, ningún enlace lleva más de este % de lo repartido reciente."),
 # --- split ---
 "split": ([G], BOTH, "cliente `off`; servidor `client`", "cliente: `off`, `on`, `force`; servidor: `off`, `client`, `force`", [SPLIT, SONDAS], ["012"], "Si se reparte lo que se envía en vez de copiarlo por todos."),
 "split_small": ([G], BOTH, "256", "0–2048 bytes", [SPLIT], ["012"], "Paquetes de hasta este tamaño nunca se reparten."),
 "split_spread_ms": ([G], BOTH, "30", "0–2000 (0: sin guarda)", [G4], ["012"], "Solo comparten el reparto los enlaces a esta distancia del más rápido (G4)."),
 "split_spread_copy": ([G], BOTH, "`yes` salvo con `governor`, `capacity_weights` o `split_eta`", "`yes` o `no`", [G4, SPLIT], ["012"], "Los enlaces fuera del reparto llevan copia de todo (y G2 duplica)."),
 "split_stress_dup": ([G], BOTH, "50", "0–400", [SDUP], ["012"], "Bajo el estrés de G4, las copias pueden sumar este % de los bytes."),
 "split_stress_rejoin": ([G], BOTH, "`yes`", "`yes` o `no`", [G4], ["012"], "El enlace más rápido que salió vuelve al instante, no tras `mute_settle_ms`."),

 "split_dwell_ms": ([G], BOTH, "0 (apagado)", "0–5000", [G4, MODEL], ["015", "audit"], "C3: G4 retiene un enlace que volvió por recuperación este tiempo."),
 "split_dwell_oq_ms": ([G], BOTH, "0 (cualquiera)", "0–5000", [G4, MODEL], ["015", "audit"], "C3: lo anterior solo mientras su propia cola es como mucho esto."),
 "governor": ([G], BOTH, "`no`", "`yes` o `no`", [GOV], ["012", "013"], "Los pesos del reparto salen de la cola de cada enlace."),
 "governor_target_ms": ([G], BOTH, "20", "1–1000", [GOV, CAP, RESC], ["012", "audit"], "La cola que un enlace puede tener sobre la más corta. Por omisión también fija `capacity_sat_ms` y `rescue_target_ms`."),
 "governor_window_ms": ([G], BOTH, "10000", "1000–120000", [COLA, GOV, CAP], ["012"], "Ventana del mínimo de retardo de cada enlace (la base de la cola)."),
 "capacity_weights": ([G], BOTH, "`no`", "`yes` o `no`", [CAP, SPLIT], ["013"], "Los pesos del reparto salen de la capacidad medida."),
 "capacity_sat_ms": ([G], BOTH, "20 (el de `governor_target_ms`; 0 lo sigue)", "0–1000", [CAP, CAKE, ETA], ["audit"], "La cola que dice que un enlace está lleno (C12)."),
 "capacity_loss_pct": ([G], BOTH, "0 (apagado)", "0–50", [CAP, MODEL], ["015"], "EXPERIMENTO: un enlace cargado que pierde más de esto está lleno."),
 "capacity_seq_lag": ([G], BOTH, "`yes`", "`yes` o `no`", [CAP], ["015"], "La pérdida de una muestra se cuenta por secuencia (`rx_top`), no 250 ms antes del informe."),



 "cap_g4_episode": ([G], BOTH, "0 (apagado)", "0–3", [CAP, G4, MODEL], ["015", "audit"], "C5: una salida de G4 por cola cuenta para el arranque rápido."),
 "split_eta": ([G], BOTH, "`no`", "`yes` o `no`", [ETA], ["013"], "Reparto por llegada esperada."),
 "split_tickle_ms": ([G], BOTH, "0 (nunca)", "0–60000", [TICKLE], ["013"], "Con `split_eta`, cada este tiempo se le da carga a un enlace ocioso."),
 "split_stale_max": ([G], BOTH, "1 (como se midió)", "1–64", [CAP, ETA], ["013"], "La capacidad de un enlace ocioso cuenta hasta este múltiplo para el ETA."),
 "eta_buffer": ([G], BOTH, "`no`", "`yes` o `no`", [MODEL, ETA, CAP], ["015"], "EXPERIMENTO: retiene un enlace que pierde en la cola más alta que mostró, y no lo sondea."),
 "split_qpred_ms": ([G], BOTH, "0 (apagado)", "0–2000", [ETA, CAP, MODEL], ["015", "audit"], "#33: el ETA toma la cola donde la pone su tendencia este tiempo adelante."),
 "split_qpred_mode": ([G], BOTH, "0", "0–2", [ETA, MODEL], ["015"], "0 ambos sentidos, 1 solo colas que suben, 2 como 1 y a lo más el doble."),
 "eta_keep": ([G], BOTH, "0 (apagado)", "0–3", [ETA, MODEL], ["015", "audit"], "C9: qué guarda el backlog del ETA sobre una cola vieja."),
 "eta_keep_ms": ([G], BOTH, "400", "0–2000", [ETA, MODEL], ["015"], "Con `eta_keep = 2`, cuán vieja es la cola medida."),
 "redund_pace_ms": ([G], BOTH, "0 (apagado)", "0–2000", [COPIAS, MODEL], ["015"], "Una copia va por un enlace solo si llega a menos de esto de la primera."),
 "pace_peers_ms": ([G], BOTH, "-1 (el del preset: 60 ms en `starlink`)", "0–2000 (0: apagado)", [COPIAS, MODEL], ["015", "017"], "La ventana de las copias hacia los demás enlaces de una sesión con Starlink, sobre los 60 ms del preset; el cliente para la subida, el servidor para la bajada."),
 "pacing": ([G], BOTH, "`no`", "`yes` o `no`", [PACE], ["013"], "Dosifica el socket de cada enlace (o cada carril del servidor) al doble de su capacidad."),
 "split_dominant_pct": ([G], BOTH, "0 (el del preset: 250 en `starlink`)", "0–65535 (grande: apagado)", [DOM, MODEL], ["015"], "Umbral del enlace dominante, solo donde el preset lo tiene."),
 "split_dominant_hold": ([G], BOTH, "0 (el del preset: apagado)", "0–2 (1 sí, 2 no)", [DOM, MODEL], ["015"], "Mantener el modo dominante mientras no pierde, y presumirlo en reposo."),
 "starlink_slot_clock": ([G], BOTH, "0 (el del preset: encendido en `starlink`)", "0–2 (1 sí, 2 no)", [SLOT, CAP, MODEL], ["015"], "El reloj de ranuras de Starlink en los enlaces `starlink`."),
 "starlink_edge_ms": ([G], BOTH, "12000 (:12, :27, :42, :57)", "0–14999", [SLOT], ["015"], "Dónde cae el borde dentro de cada período de 15 s del reloj de pared."),
 "starlink_edge_pre_ms": ([G], BOTH, "100", "0–5000", [SLOT], ["015"], "La ventana empieza este tiempo antes del borde."),
 "starlink_edge_post_ms": ([G], BOTH, "50", "0–5000", [SLOT], ["015"], "La ventana termina este tiempo después del borde."),

 "redund_spare": ([G], CLI, "`no`", "`no`, `copies`, `parity`, `mix`, `rs` o `rlc`", [SPARE, COPIAS, FEC, CODES], ["020", "021"], "EXPERIMENTO: en redundancia, lo que llevan los enlaces que no son el primero, en lo que les sobra (su cola bajo `redund_spare_ms`): copias parciales, paridad XOR sobre k paquetes del primero (el servidor necesita `fec = yes`), la mezcla (copias si cabe todo o en un microcorte, paridad si no), o como la mezcla con un código de borrado en vez del XOR: bloques Reed-Solomon (`rs`, Leopard) o una ventana deslizante (`rlc`), con k y m según la pérdida del primero (protocolo 8; el servidor necesita `fec = yes`)."),
 "redund_spare_code_ms": ([G], CLI, "30", "1–500", [CODES, SPARE], ["021"], "Con `redund_spare = rs` o `rlc`, cuánto abarca un bloque (o la ventana) de paquetes fuente: k son los paquetes de ese tiempo (de 4 a 64), y un bloque que no se llena cierra a ese plazo (la ventana manda una reparación)."),
 "redund_spare_ms": ([G], CLI, "50", "1–2000", [SPARE], ["020"], "Con `redund_spare`, la cola (sobre el mínimo de 60 s) en la que cada enlace que no es el primero sostiene lo que lleva."),
 "redund_spare_first": ([G], CLI, "vacío (el de menor retardo sin cola)", "el nombre de un `[link NAME]` del archivo", [SPARE], ["020"], "Con `redund_spare`, el primer enlace (el que los demás protegen) fijado: ese mientras esté vivo y llevando, medido ahora y sin ahogarse; si se ahoga como primero queda vetado 15 s y manda la elección automática, y vuelve cuando se despeja; mientras los demás copian toda la subida, solo tras 2 s seguidos de ahogo (un salto de retardo más corto no lo saca). El estado dice por qué no es el primero (`upload.spare.pin`)."),
 "exp_link_types": ([G], BOTH, "vacía (todos los tipos)", "nombres de tipo", [MODEL], ["015"], "Limita las perillas globales del modelo a los enlaces de esos tipos."),
 # --- the protocontroller (story 025) ---
 "protocontroller": ([G], BOTH, "`no`", "`no`, `a` o `b`", [PCTL], ["025"], "El protocontrolador (solo con `tunnel = native`): en el router, con `a` o `b`, el motor toma el TCP que el firewall redirige a `protocontroller_port` y lo lleva al VPS como un TCP simple por el túnel (`a`) o como flujo fiable propio (`b`); en el VPS, `a` o `b` da lo mismo: abre al destino el TCP que pidan los routers con cualquiera de las dos. Con `no`, un mensaje de flujo que llega se cuenta y se descarta."),
 "protocontroller_port": ([G], BOTH, "7801", "1–65535", [PCTL], ["025"], "Puerto donde escucha el router (IPv4 e IPv6) lo que el firewall redirige (`REDIRECT`); en el VPS, el de la variante A en su dirección del túnel."),
 "protocontroller_vps": ([G], CLI, "vacío (la primera dirección del prefijo de `tun_addr`)", "una dirección IP", [PCTL], ["025"], "Variante A: la dirección del VPS en el túnel, a la que el router abre el TCP del medio."),
 "protocontroller_mid_cc": ([G], BOTH, "`bbr`", "un control de congestión del kernel, o vacío (el del kernel)", [PCTL], ["025"], "Variante A: el control de congestión del TCP del medio, en cada extremo para lo que envía (el router la subida, el VPS la bajada). Con cubic, la pérdida que el túnel no repara a tiempo lo frena (42 contra 54 Mbit/s en `protect`; 7 contra 50 de bajada); si el kernel no lo tiene, el suyo (un aviso)."),
 "protocontroller_buf_kb": ([G], BOTH, "1024 (con `subflow`, 8176)", "64–16368", [PCTL], ["025", "029"], "Búfer de cada conexión por sentido, como tope: lo leído del socket y no confirmado (al enviar), lo llegado y no escrito (al recibir: su ventana); en la variante A, lo leído de un lado y no escrito en el otro. Con el ritmo por enlace (`link_cc`) es el techo de la bajada: lo enviado queda retenido hasta su acuse (~150 ms con las colas de los enlaces), y en terreno 2 MB dieron 51–87 Mbit/s, 4 MB 72–108 y 8 MB 104–163 (+49 % en cuatro rondas de cuatro; OMR, con 16 MB, 103–184; historia 029)."),
 "protocontroller_mem_mb": ([G], BOTH, "0 (un octavo de la RAM, 64–1024)", "0–4096", [PCTL], ["025", "029"], "Memoria de todas las conexiones juntas (trozos de 16 KB). Cada conexión toma como mucho su parte pareja; lleno, lo que llega en orden cabe olvidando lo que está fuera de orden; sin trozos libres no se aceptan conexiones nuevas ni se lee más."),
 "protocontroller_max_flows": ([G], BOTH, "256", "1–65535", [PCTL], ["025"], "Conexiones a la vez; la siguiente se rehúsa con RST."),
 "protocontroller_queue_ms": ([G], BOTH, "5", "1–1000", [PCTL], ["025"], "Variante B: la cola que el tramo del medio puede armar sobre su menor ida y vuelta: más, y el ritmo baja a lo entregado (`streamcc.h`)."),
 "protocontroller_backlog_ms": ([G], BOTH, "200", "10–10000", [PCTL], ["025"], "Variante B: lo que un flujo guarda sin enviar o sin escribir, en tiempo a su ritmo (al menos 64 KB): la contrapresión hacia el emisor."),
 "protocontroller_dead_s": ([G], BOTH, "60", "0–3600", [PCTL], ["025"], "Variante B: un flujo sin avance este tiempo, con algo pendiente (el túnel caído), se cierra con RST en los dos lados; cuenta desde el último mensaje del otro lado o desde que el flujo vuelve a tener algo en vuelo; 0: nunca."),
 "protocontroller_reo_ms": ([G], BOTH, "10", "0–1000", [PCTL], ["025"], "Variante B: el piso de la ventana de RACK sobre un cuarto de la ida y vuelta (la reparación del túnel va primero); un duplicado la agranda 4 ms, hasta 300 (el presupuesto de latencia, historia 029), y vuelve sola; con un código o el rescate, al menos el horizonte de reparación del motor (historia 028)."),
 "protocontroller_nack": ([G], BOTH, "`yes`", "`yes`, `no`", [PCTL], ["029"], "Variante B: el NACK del túnel de un mensaje de GRIT es una pérdida que se envía otra vez (`yes`) o solo una pista y decide el RACK de GRIT (`no`); en terreno lo que el túnel dio por perdido llegó tarde (historia 029). Con `subflow` no cuenta: el NACK solo es siempre una pista y decide la palabra del enlace."),
 "subflow": ([G], BOTH, "`no`", "`yes`, `no`", [SUBF, PCTL], ["029"], "MPGRIT (paso U1): cada extremo numera lo que pone en cada enlace (`sseq`, protocolo 12) y sigue la secuencia de cada enlace que recibe; lo que un enlace perdió dentro de sí (un `sseq` posterior del mismo enlace llegó hace más que su ventana) va de vuelta en un `SFLOSS`; un mensaje de GRIT que fue solo por ese enlace se envía otra vez cuando nada lo reconstruye o el receptor también lo pidió. El NACK solo del túnel pasa a ser una pista (`protocontroller_nack` no cuenta), el RACK de GRIT sobre la mezcla se apaga y su temporizador espera al menos 300 ms más. En terreno: GRIT reenvía 0,6–1 % en vez de 10 %. Si la configuración no los dice, cambia los objetivos que ganaron en terreno (paso 2): `protocontroller_queue_ms` 50, `governor_target_ms` 60, `capacity_loss_pct` 5 y `protocontroller_buf_kb` 2048. Tienen que tenerla los dos extremos."),
 "protocontroller_offer": ([G], BOTH, "`yes`", "`yes`, `no`", [PCTL], ["029"], "Con `subflow`, GRIT ofrece la capacidad del motor y un cuarto mientras no estén llenos todos los enlaces que llevan los datos (`cg_scc_offer`): cada enlace recibe más que su capacidad y lo que entrega la sube donde hay espacio (un módem da más cuando su cola crece). Experimento del paso 2 (historia 029)."),
 "link_cc": ([G], BOTH, "`no`", "`yes`, `no`", [LCC, SUBF, PCTL], ["029"], "MPGRIT (paso 2, experimento): con `subflow`, un BBR por enlace ([[Ritmo por enlace (linkcc.h)]]) alimentado por los `SFACK` decide el reparto por llegada (su ritmo, su retardo sin cola, su cola en vuelo; pasado `governor_target_ms` de cola, último recurso) y la oferta de GRIT es la suma de los ritmos de los enlaces, como OpenMPTCProuter corre un BBR por subflujo. Sin `subflow` no hace nada. Tienen que tenerla los dos extremos."),


 "protocontroller_cc": ([G], SRVS, "`auto`", "`auto`, un control de congestión del kernel, o vacío", [PCTL], ["025"], "Control de congestión del TCP del VPS hacia el destino; `auto`: el del kernel (cubic) y BBR en cuanto ese tramo pierde (sin el ProbeRTT de BBR para el vídeo en vivo); si el kernel no lo tiene, el suyo (un aviso)."),
}

RESTART = {"mode", "key", "listen", "control_socket", "busy_poll_us", "cpu", "rt_priority", "reorder_pool_kb",
           "rescue_kb", "fec", "link_threads", "io_queue", "lanes", "tunnel", "tun", "tun_addr", "tun_mtu",
           "tun_txqueuelen", "protocontroller", "protocontroller_port", "protocontroller_vps", "protocontroller_mid_cc",
           "protocontroller_buf_kb", "protocontroller_mem_mb", "protocontroller_max_flows", "protocontroller_queue_ms",
           "protocontroller_backlog_ms", "protocontroller_dead_s", "protocontroller_reo_ms", "protocontroller_nack", "protocontroller_offer", "protocontroller_cc"}

def parsed_keys():
    s = open(SRC).read()
    keys = set(re.findall(r'get_(?:u32|bool|str|addrs|pfxs)\(&?ini, (?:""|sec), "([a-z_0-9]+)"', s))
    keys |= set(re.findall(r'cg_ini_get\(&?ini, (?:""|sec), "([a-z_0-9]+)"', s))
    keys |= set(re.findall(r'cg_ini_get\(ini, sec, "([a-z_0-9]+)"', s))
    keys |= {"server", "resolver"}  # get_servers / get_resolvers
    return keys

def link(p):
    return f"[[{p}]]"

def klink(k):
    return f"[[Mapa/Perillas/{k}|{k}]]"

def generate():
    """{path relative to docs/mapa: text} of every generated note, and the
    keys config.c parses that T lacks or T has that config.c does not."""
    keys = parsed_keys()
    missing = sorted(keys - set(T))
    extra = sorted(set(T) - keys)
    files = {}
    for k, (secs, side, dflt, rng, pieces, stories, desc) in T.items():
        afiles = ["engine/src/config.c", "engine/src/config.h"]
        lines = ["---", "tags: [mapa, perilla]", "archivos:"] + [f"  - {f}" for f in afiles] + ["---", "",
                 f"# `{k}`", "", desc, "",
                 f"- **Sección:** {', '.join(secs)}",
                 f"- **Extremo:** {side}",
                 f"- **Por omisión:** {dflt}",
                 f"- **Rango:** {rng}",
                 f"- **Pieza:** {', '.join(link(p) for p in pieces)}",
                 f"- **Historia:** {', '.join(link(H[s]) for s in stories) if stories else '—'}"]
        if k in RESTART:
            lines.append("- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).")
        lines += ["", "Índice: [[Perillas]] · Mapa: [[Mapa del motor]]", ""]
        files[f"Perillas/{k}.md"] = "\n".join(lines)
    # index
    groups = [
        ("Básicas y sistema", ["mode", "key", "description", "listen", "log_level", "status_file", "status_interval_ms",
                               "control_socket", "rcvbuf", "sndbuf", "cpu", "busy_poll_us", "rt_priority"]),
        ("Túnel", ["tunnel", "tun", "tun_addr", "tun_mtu", "tun_txqueuelen", "tun_route"]),
        ("Cliente: enlaces, sondas, servidores", ["interfaces", "exclude", "server", "resolver", "server_failover_ms",
                               "probe_interval_ms", "probe_idle_ms", "passthrough", "link_threads", "io_queue",
                               "label", "enabled", "weight", "type"]),
        ("Servidor", ["max_sessions", "session_timeout_ms",
                      "path_timeout_ms", "passthrough_file", "forward_file", "forward", "lanes"]),
        ("Salud", ["mute_behind_ms", "unmute_behind_ms", "mute_settle_ms", "mute_trickle", "min_active_links"]),
        ("Reordenador", ["reorder", "reorder_max_wait_ms", "reorder_dup_wait_ms", "reorder_margin_ms", "reorder_bypass",
                         "reorder_by_link", "reorder_adaptive", "reorder_pool_kb", "reorder_active", "tcp_aware"]),
        ("Rescate y FEC", ["rescue_kb", "rescue_pct", "rescue_wait_ms", "rescue_tries", "rescue_target_ms", "rescue_full_hyst", "rescue_code_ms", "parq", "rescue_tcp_race",
                           "tcp_retx_one", "tcp_ack_rule", "fec", "fec_wait_ms", "fec_share_max", "fec_code",
                           "fec_pct", "fec_pct_min", "fec_window_ms", "fec_single"]),
        ("Reparto y guardas", ["split", "split_small", "split_spread_ms", "split_spread_copy", "split_stress_dup",
                               "split_stress_rejoin", "split_dwell_ms", "split_dwell_oq_ms"]),
        ("Gobernador y capacidad", ["governor", "governor_target_ms", "governor_window_ms", "capacity_weights",
                                    "capacity_sat_ms", "capacity_loss_pct", "capacity_seq_lag", "cap_g4_episode"]),
        ("ETA, cosquilla, copias, dosificación", ["split_eta", "split_tickle_ms", "split_stale_max", "eta_buffer",
                               "split_qpred_ms", "split_qpred_mode", "eta_keep", "eta_keep_ms", "redund_pace_ms", "pace_peers_ms", "pacing"]),
        ("Modelo de enlace y Starlink", ["exp_link_types", "split_dominant_pct", "split_dominant_hold",
                               "starlink_slot_clock", "starlink_edge_ms", "starlink_edge_pre_ms",
                               "starlink_edge_post_ms"]),
        ("Lo que sobra (redundancia)", ["redund_spare", "redund_spare_ms", "redund_spare_first", "redund_spare_code_ms"]),
        ("Protocontrolador", ["protocontroller", "protocontroller_port", "protocontroller_vps", "protocontroller_mid_cc",
                              "protocontroller_buf_kb", "protocontroller_mem_mb", "protocontroller_max_flows",
                              "protocontroller_queue_ms", "protocontroller_backlog_ms", "protocontroller_dead_s",
                              "protocontroller_reo_ms", "protocontroller_cc"]),
    ]
    seen = [k for _, ks in groups for k in ks]
    unplaced = sorted(set(T) - set(seen))
    if unplaced:
        groups.append(("Sin grupo (agrégalas a un grupo en perillas.py)", unplaced))
    out = ["---", "tags: [mapa, hub, perilla]", "archivos:", "  - engine/src/config.c", "  - engine/src/config.h", "---", "",
           "# Perillas", "",
           f"Las {len(T)} claves que `engine/src/config.c` lee (comprobado al generar esta nota: ni falta ni sobra ninguna).",
           "Por omisión y rango salen de `cg_config_parse`; el significado, de los comentarios de `config.h` y de [[engine-README]].",
           "Las perillas marcadas como experimento están apagadas por omisión.", "",
           "Secciones: «global» es la parte sin sección; `[link NOMBRE]` es del cliente; `[client NOMBRE]` del servidor.", ""]
    for title, ks in groups:
        out += [f"## {title}", "", "| Perilla | Por omisión | Pieza | Historia |", "| --- | --- | --- | --- |"]
        for k in ks:
            secs, side, dflt, rng, pieces, stories, desc = T[k]
            d = dflt.replace("|", "\\|")
            out.append(f"| {klink(k).replace('|', '\\|')} | {d} | {', '.join(link(p) for p in pieces)} | "
                       f"{', '.join(link(H[s]) for s in stories) if stories else '—'} |")
        out.append("")
    out += ["Volver a [[Mapa del motor]].", ""]
    files["Perillas.md"] = "\n".join(out)
    return files, missing, extra
