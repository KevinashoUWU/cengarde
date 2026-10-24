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
SRV = "Elección de servidor (srvpick.h)"
NAMES = "Nombres del servidor (resolve.h)"
RO = "Reordenador (reorder.h)"
LANES = "Colas del servidor (steer.h)"
RCVB = "Presupuesto de recepción (rcvbudget.h)"
THR = "Hilos por enlace (thrplan.h)"
RESC = "Rescate (rescue.h)"
FEC = "FEC (fec.h)"
WGW = "Reloj atrasado de WireGuard (wgwatch.h)"
CLIENTS = "Varios clientes (clients.h)"
FWD = "Tabla de reenvío (fwdtable.h)"
TUNR = "Rutas del túnel nativo (tunroute.h)"
DEC = "Bitácora de decisiones (decision.h)"
CONF = "Configuración (config.c)"
SPARE = "Lo que sobra (spare.h)"
CODES = "Códigos de borrado (rs.h, rlc.h)"

H = {
    "002": "002-wireguard-para-cengarde", "005": "005-motor-c-v1", "006": "006-salud-de-enlaces",
    "008": "008-luci-uci-y-emparejamiento", "009": "009-recarga-control-e-ip-pass",
    "010": "010-ipv6-varias-ip-multicliente-nombres", "011": "011-hilos", "012": "012-bonding",
    "013": "013-capacidad-y-dosificacion", "014": "014-tunel-nativo", "015": "015-modelo-de-enlace", "016": "016-celda-virtual",
    "017": "017-terreno-nostalgica", "019": "019-perdida-con-cola", "020": "020-copias-parciales-y-paridad", "021": "021-fec-v2",
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
 "listen": ([G], BOTH, "cliente `127.0.0.1:59401`; servidor `*:59402`", "una dirección", [CONF], [], "Dónde escucha: el cliente, a WireGuard local; el servidor, a los enlaces."),
 "cpu": ([G, L], BOTH, "-1 (cualquiera)", "0–1023", [THR], ["006", "011"], "Fija el proceso (global) o el hilo de un enlace (`[link]`, con `link_threads = on`) a una CPU."),
 "busy_poll_us": ([G], BOTH, "0", "0–1000000", [THR], ["006"], "Tras tráfico, sondea sin dormir este tiempo. `thrplan.h` lo limita si los hilos no caben en las CPU."),
 "rt_priority": ([G], BOTH, "0 (normal)", "0–99", [THR], ["006"], "Prioridad `SCHED_FIFO` de los hilos de datos."),
 # --- tunnel ---
 "tunnel": ([G], BOTH, "`wireguard`", "`wireguard` o `native`", [TUNR, TCPF], ["014"], "Qué lleva el túnel: datagramas de WireGuard o paquetes IP de una TUN propia (autenticados, sin cifrar)."),
 "tun": ([G], BOTH, "`cg0`", "nombre de interfaz", [TUNR], ["014"], "Nombre de la TUN con `tunnel = native`."),
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
 "mute_queue": ([G], BOTH, "`no`", "`yes` o `no`", [SALUD, COLA], ["016"], "EXPERIMENTO: el silenciado compara el atraso, la cola propia sobre la del más rápido y las sondas sin respuesta (el mayor); un informe viejo de un enlace callado no reinicia la espera."),
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
 "wireguard": ([G, K], SRVS, "(obligatoria con un cliente y `tunnel = wireguard`)", "una dirección", [CONF, CLIENTS], ["005"], "El WireGuard local del VPS."),
 "wireguard_poke": ([G, K], SRVS, "`10.79.0.2:9`", "una dirección o `none`", [WGW], ["002", "010"], "Dirección del router en el túnel: un datagrama ahí hace que WireGuard inicie un handshake."),
 "wireguard_port_file": ([G], SRVS, "vacía", "ruta", [WGW], ["010"], "Guarda el puerto que WireGuard conoce de cada cliente, a través de reinicios (`wgport.h`)."),
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
 "reorder_bypass": ([G], BOTH, "128", "0–2048 bytes", [RO], ["012"], "Datos de hasta este tamaño se entregan sin esperar."),
 "reorder_by_link": ([G], BOTH, "`no`", "`yes` o `no`", [RO], ["013"], "Un hueco espera solo al enlace por el que salió (`CG_F_PREV`). Experimental."),
 "reorder_adaptive": ([G], BOTH, "`no`", "`yes` o `no`", [RO], ["013"], "El margen de cada enlace aprende de sus tardíos. Experimental."),
 "reorder_pool_kb": ([G], BOTH, "8192", "256–131070", [RO], ["012", "013"], "Memoria para lo retenido."),
 "reorder_idle_ms": ([G], BOTH, "0 (apagado)", "0–60000", [RO, ARR], ["audit"], "EXPERIMENTO (C2): el reordenador deja de esperar a un enlace que no trajo datos este tiempo."),
 "reorder_retx_release_ms": ([G], BOTH, "0 (apagado)", "0–2000", [RO, TCPF], ["014"], "Una retransmisión de TCP suelta los huecos retenidos desde este tiempo antes de ella."),
 "tcp_aware": ([G], BOTH, "`no`", "`yes` o `no`", [TCPF, RO], ["014"], "Con `tunnel = native`, marca libres los segmentos sin datos y las retransmisiones de TCP."),
 # --- rescue / fec ---
 "rescue_kb": ([G], BOTH, "0 (apagado)", "0–262144", [RESC], ["013"], "Anillo de lo enviado hace poco, para los NACK (servidor: bajada; cliente: subida)."),
 "rescue_pct": ([G], BOTH, "5", "1–50", [RESC], ["013"], "Presupuesto del rescate, en % de lo enviado."),
 "rescue_wait_ms": ([G], BOTH, "0 (no pide)", "0–1000", [RESC, RO], ["013"], "Cuánto espera un hueco pedido. Con `fec`, al menos 2 × `fec_wait_ms` + 20."),
 "rescue_tries": ([G], BOTH, "2", "1–4", [RESC, RO], ["013"], "Cuántas veces se pide un hueco dentro de `rescue_wait_ms`."),
 "rescue_target_ms": ([G], BOTH, "20 (el de `governor_target_ms`; 0 lo sigue)", "0–1000", [RESC, CAP], ["audit"], "Cola donde empieza a bajar el peso del rescate (cero a 4 veces). Auditoría C12."),
 "rescue_by_owd": ([G], CLI, "`no`", "`yes` o `no`", [RESC], ["015", "audit"], "El rescate del cliente por el enlace de menor retardo de subida, no de RTT (C10). Experimento."),
 "rescue_full_hyst": ([G], BOTH, "0 (apagado)", "0–2", [RESC, CAP, MODEL], ["015", "audit"], "Histéresis del bit `full` del rescate (C11). Experimento, campo del modelo."),
 "fec": ([G], BOTH, "`no`", "`yes` o `no`", [FEC, CODES], ["013", "021"], "Paridad XOR de cada grupo de DATA repartido. En el servidor, también la memoria con la que usa las paridades y las reparaciones de `redund_spare` (`parity`, `mix`, `rs`, `rlc`)."),
 "fec_wait_ms": ([G], BOTH, "10", "1–200", [FEC, RESC], ["013"], "Plazo máximo de un grupo antes de mandar su paridad."),
 "fec_share_max": ([G], BOTH, "0 (sin tope)", "0–100", [FEC, SPLIT], ["012", "013"], "Con `fec`, ningún enlace lleva más de este % de lo repartido reciente."),
 # --- split ---
 "split": ([G], BOTH, "cliente `off`; servidor `client`", "cliente: `off`, `on`, `force`; servidor: `off`, `client`, `force`", [SPLIT, SONDAS], ["012"], "Si se reparte lo que se envía en vez de copiarlo por todos."),
 "split_small": ([G], BOTH, "256", "0–2048 bytes", [SPLIT], ["012"], "Datagramas de hasta este tamaño nunca se reparten."),
 "split_spread_ms": ([G], BOTH, "30", "0–2000 (0: sin guarda)", [G4], ["012"], "Solo comparten el reparto los enlaces a esta distancia del más rápido (G4)."),
 "split_spread_copy": ([G], BOTH, "`yes` salvo con `governor`, `capacity_weights` o `split_eta`", "`yes` o `no`", [G4, SPLIT], ["012"], "Los enlaces fuera del reparto llevan copia de todo (y G2 duplica)."),
 "split_stress_dup": ([G], BOTH, "50", "0–400", [SDUP], ["012"], "Bajo el estrés de G4, las copias pueden sumar este % de los bytes."),
 "split_stress_rejoin": ([G], BOTH, "`yes`", "`yes` o `no`", [G4], ["012"], "El enlace más rápido que salió vuelve al instante, no tras `mute_settle_ms`."),
 "split_stress_single": ([G], BOTH, "`no`", "`yes` o `no`", [G4, SDUP], ["audit"], "EXPERIMENTO: bajo estrés, el reparto usa solo el enlace más rápido."),
 "split_dwell_ms": ([G], BOTH, "0 (apagado)", "0–5000", [G4, MODEL], ["015", "audit"], "C3: G4 retiene un enlace que volvió por recuperación este tiempo."),
 "split_dwell_oq_ms": ([G], BOTH, "0 (cualquiera)", "0–5000", [G4, MODEL], ["015", "audit"], "C3: lo anterior solo mientras su propia cola es como mucho esto."),
 "governor": ([G], BOTH, "`no`", "`yes` o `no`", [GOV], ["012", "013"], "Los pesos del reparto salen de la cola de cada enlace."),
 "governor_target_ms": ([G], BOTH, "20", "1–1000", [GOV, CAP, RESC], ["012", "audit"], "La cola que un enlace puede tener sobre la más corta. Por omisión también fija `capacity_sat_ms` y `rescue_target_ms`."),
 "governor_window_ms": ([G], BOTH, "10000", "1000–120000", [COLA, GOV, CAP], ["012"], "Ventana del mínimo de retardo de cada enlace (la base de la cola)."),
 "capacity_weights": ([G], BOTH, "`no`", "`yes` o `no`", [CAP, SPLIT], ["013"], "Los pesos del reparto salen de la capacidad medida."),
 "capacity_sat_ms": ([G], BOTH, "20 (el de `governor_target_ms`; 0 lo sigue)", "0–1000", [CAP, CAKE, ETA], ["audit"], "La cola que dice que un enlace está lleno (C12)."),
 "capacity_loss_pct": ([G], BOTH, "0 (apagado)", "0–50", [CAP, MODEL], ["015"], "EXPERIMENTO: un enlace cargado que pierde más de esto está lleno."),
 "capacity_seq_lag": ([G], BOTH, "`yes`", "`yes` o `no`", [CAP], ["015"], "La pérdida de una muestra se cuenta por secuencia (`rx_top`), no 250 ms antes del informe."),
 "capacity_expire_ms": ([G], BOTH, "0 (apagado)", "0–600000", [CAP], ["016"], "EXPERIMENTO: un enlace fuera del envío (silenciado, mudo o fuera del reparto) tanto tiempo sin medirse ve su capacidad a la mitad, y otra vez cada tanto, hasta 1/16 de su mejor."),
 "capacity_loss_queue_ms": ([G], BOTH, "0 (apagado)", "0–1000", [CAP, COLA, MODEL], ["019"], "EXPERIMENTO: un enlace que pierde al menos 1 % de lo enviado, mandándole más de lo que entrega, con la cola sobre su mínimo de 60 s en al menos esto, dos muestras seguidas, está lleno a cualquier carga (con el preset de Starlink, solo cargado). Sugerido: 10."),
 "capacity_stand": ([G], BOTH, "0 (apagado)", "0–2", [CAP, COLA], ["019"], "EXPERIMENTO: 1, una cola permanente (sobre el mínimo de 60 s, sobre `capacity_sat_ms`, 4 muestras) frena el arranque rápido; 2, además el arranque rápido va antes de 10 s si el enlace entrega 9/8 de su capacidad sin pérdida ni cola, dos muestras."),
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
 "starlink_edge_copies": ([G], BOTH, "`no`", "`yes` o `no`", [SLOT, COPIAS], ["015"], "En redundancia, copias a los demás enlaces solo dentro de la ventana del borde."),
 "redund_spare": ([G], CLI, "`no`", "`no`, `copies`, `parity`, `mix`, `rs` o `rlc`", [SPARE, COPIAS, FEC, CODES], ["020", "021"], "EXPERIMENTO: en redundancia, lo que llevan los enlaces que no son el primero, en lo que les sobra (su cola bajo `redund_spare_ms`): copias parciales, paridad XOR sobre k paquetes del primero (el servidor necesita `fec = yes`), la mezcla (copias si cabe todo o en un microcorte, paridad si no), o como la mezcla con un código de borrado en vez del XOR: bloques Reed-Solomon (`rs`, Leopard) o una ventana deslizante (`rlc`), con k y m según la pérdida del primero (protocolo 8; el servidor necesita `fec = yes`)."),
 "redund_spare_code_ms": ([G], CLI, "30", "1–500", [CODES, SPARE], ["021"], "Con `redund_spare = rs` o `rlc`, cuánto abarca un bloque (o la ventana) de paquetes fuente: k son los paquetes de ese tiempo (de 4 a 64), y un bloque que no se llena cierra a ese plazo (la ventana manda una reparación)."),
 "redund_spare_ms": ([G], CLI, "50", "1–2000", [SPARE], ["020"], "Con `redund_spare`, la cola (sobre el mínimo de 60 s) en la que cada enlace que no es el primero sostiene lo que lleva."),
 "redund_spare_first": ([G], CLI, "vacío (el de menor retardo sin cola)", "el nombre de un `[link NAME]` del archivo", [SPARE], ["020"], "Con `redund_spare`, el primer enlace (el que los demás protegen) fijado: ese mientras esté vivo y llevando, medido ahora y sin ahogarse; si se ahoga como primero queda vetado 15 s y manda la elección automática, y vuelve cuando se despeja; mientras los demás copian toda la subida, solo tras 2 s seguidos de ahogo (un salto de retardo más corto no lo saca). El estado dice por qué no es el primero (`upload.spare.pin`)."),
 "exp_link_types": ([G], BOTH, "vacía (todos los tipos)", "nombres de tipo", [MODEL], ["015"], "Limita las perillas globales del modelo a los enlaces de esos tipos."),
}

RESTART = {"mode", "key", "listen", "control_socket", "busy_poll_us", "cpu", "rt_priority", "reorder_pool_kb",
           "rescue_kb", "fec", "link_threads", "io_queue", "lanes", "tunnel", "tun", "tun_addr", "tun_mtu",
           "tun_txqueuelen"}

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
        ("Túnel nativo", ["tunnel", "tun", "tun_addr", "tun_mtu", "tun_txqueuelen", "tun_route"]),
        ("Cliente: enlaces, sondas, servidores", ["interfaces", "exclude", "server", "resolver", "server_failover_ms",
                               "probe_interval_ms", "probe_idle_ms", "passthrough", "link_threads", "io_queue",
                               "label", "enabled", "weight", "type"]),
        ("Servidor", ["wireguard", "wireguard_poke", "wireguard_port_file", "max_sessions", "session_timeout_ms",
                      "path_timeout_ms", "passthrough_file", "forward_file", "forward", "lanes"]),
        ("Salud", ["mute_behind_ms", "unmute_behind_ms", "mute_settle_ms", "mute_trickle", "min_active_links", "mute_queue"]),
        ("Reordenador", ["reorder", "reorder_max_wait_ms", "reorder_dup_wait_ms", "reorder_margin_ms", "reorder_bypass",
                         "reorder_by_link", "reorder_adaptive", "reorder_pool_kb", "reorder_idle_ms",
                         "reorder_retx_release_ms", "tcp_aware"]),
        ("Rescate y FEC", ["rescue_kb", "rescue_pct", "rescue_wait_ms", "rescue_tries", "rescue_target_ms",
                           "rescue_by_owd", "rescue_full_hyst", "fec", "fec_wait_ms", "fec_share_max"]),
        ("Reparto y guardas", ["split", "split_small", "split_spread_ms", "split_spread_copy", "split_stress_dup",
                               "split_stress_rejoin", "split_stress_single", "split_dwell_ms", "split_dwell_oq_ms"]),
        ("Gobernador y capacidad", ["governor", "governor_target_ms", "governor_window_ms", "capacity_weights",
                                    "capacity_sat_ms", "capacity_loss_pct", "capacity_seq_lag", "capacity_expire_ms",
                                    "capacity_loss_queue_ms", "capacity_stand", "cap_g4_episode"]),
        ("ETA, cosquilla, copias, dosificación", ["split_eta", "split_tickle_ms", "split_stale_max", "eta_buffer",
                               "split_qpred_ms", "split_qpred_mode", "eta_keep", "eta_keep_ms", "redund_pace_ms", "pace_peers_ms", "pacing"]),
        ("Modelo de enlace y Starlink", ["exp_link_types", "split_dominant_pct", "split_dominant_hold",
                               "starlink_slot_clock", "starlink_edge_ms", "starlink_edge_pre_ms",
                               "starlink_edge_post_ms", "starlink_edge_copies"]),
        ("Lo que sobra (redundancia)", ["redund_spare", "redund_spare_ms", "redund_spare_first", "redund_spare_code_ms"]),
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
