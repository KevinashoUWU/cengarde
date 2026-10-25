---
tags: [mapa, glosario]
archivos:
  - engine/src/health.h
  - engine/src/split.h
  - engine/src/capacity.h
  - engine/src/reorder.h
sello: 7a55fff237c62027
---
# Glosario

Términos del proyecto, en español, con el nombre del código entre paréntesis. «Cosquilla», «presunto», «bomba» y «carril» son traducciones de este mapa; el código dice *tickle*, *presumed*, *pump* y *lane*.

## Enlaces y salud
- **Atraso (`mute_queue`):** lo que compara el silenciado con esa perilla: el mayor entre lo que va detrás, la cola propia sobre la del más rápido y las sondas sin respuesta. → [[Salud y silenciado (health.h)]]
- **Enlace / camino:** un enlace es una subida del router; en el servidor, un camino es sesión + enlace. → [[Sondas y cola de bonding (proto.h)]]
- **Sonda / respuesta:** mensaje de control por enlace que trae las medidas del otro extremo. → [[Sondas y cola de bonding (proto.h)]]
- **Cola de bonding (bonding tail):** 4 bytes al final de cada sonda: peso, si reordena, si pide reparto, rescate, lleno, tipo. → [[Sondas y cola de bonding (proto.h)]]
- **OWD (retardo de ida):** reloj del receptor menos el ts del emisor; solo las diferencias entre enlaces significan algo. → [[Salud y silenciado (health.h)]]
- **Silenciado (muted), G1:** enlace muy atrasado que conserva socket y sondas pero no lleva datos. → [[Salud y silenciado (health.h)]]
- **Mudo (stall):** enlace que dejó de responder; no lleva nada hasta que vuelve, salvo que siga trayendo paquetes (lo que sube lo usa unos RTT más; el DATA del protocontrolador, un segundo). → [[Salud y silenciado (health.h)]]
- **Goteo (trickle):** un paquete de cada N por un enlace silenciado. → [[Mapa/Perillas/mute_trickle|mute_trickle]]
- **Portador (carrier):** enlace que lleva todo lo que se duplica. → [[Salud y silenciado (health.h)]]
- **Modelo de enlace / tipo / preset:** clase del enlace (`auto`, `cellular`, `starlink`…) y sus parámetros. → [[Modelo de enlace (linkmodel.h)]]

## Reparto
- **Redundancia:** cada paquete por todos los enlaces; el receptor se queda con la primera copia. → [[Anti-replay (replay.h)]]
- **Primer enlace / lo que sobra (`redund_spare`):** en redundancia, el enlace protegido (el de menor retardo sin cola parada) y la asignación de cada otro enlace, la tasa que le cabe sin llenarle la cola, donde van copias parciales o paridad. → [[Lo que sobra (spare.h)]]
- **Microcorte:** 20–40 s en que Starlink pierde 50–88 % de la subida (historia 017); un XOR no rehace ahí. → [[Lo que sobra (spare.h)]]
- **Reparto (split) / bonding:** cada paquete por un solo enlace. → [[Reparto (split.h)]]
- **Elegible / candidato:** enlace que puede recibir parte del reparto. → [[Reparto (split.h)]]
- **Peso / peso 0 (dup0):** la parte de un enlace; con 0, copia de todo. → [[Reparto (split.h)]]
- **G2:** con menos de dos elegibles, todo va por todos. → [[Reparto (split.h)]]
- **G3:** enlace sin nada verificado del otro extremo en 3 intervalos de sonda. → [[G3 enlace sin respuesta (split.h)]]
- **G4 / dispersión (spread):** solo comparten el reparto los enlaces a `split_spread_ms` del más rápido. → [[G4 guarda de dispersión (split.h)]]
- **Estrés (stress):** el más rápido es uno que salió y todos los de dentro van lejos detrás. → [[G4 guarda de dispersión (split.h)]]
- **Recuperación (recover):** vuelta de un enlace que pasó a ser el más rápido. → [[G4 guarda de dispersión (split.h)]]
- **Copias bajo estrés (sdup):** copias con presupuesto mientras dura el estrés. → [[Copias bajo estrés (split.h)]]
- **Gobernador:** pesos desde la cola de cada enlace. → [[Gobernador (govern.h)]]
- **ETA (llegada esperada):** cada paquete por donde llegaría antes. → [[ETA (eta.h)]]
- **Backlog:** la cola virtual de cada enlace en el ETA. → [[ETA (eta.h)]]
- **Cosquilla (tickle):** carga breve a un enlace ocioso para volver a medirlo. → [[Cosquilla (tickle.h)]]
- **Dominante:** enlace que entrega mucho más que los lentos juntos; el reparto se hace a un lado. → [[Enlace dominante (dominant.h)]]
- **Presunto (presumed):** dominante supuesto en reposo, por su historia. → [[Enlace dominante (dominant.h)]]
- **Copias a tiempo:** copia solo si llegaría cerca de la primera (`pace_ms`, `pace_peers_ms`). → [[Copias a tiempo (eta.h)]]

## Medidas
- **Caducidad (`capacity_expire_ms`):** la capacidad de un enlace fuera del envío baja a la mitad cada tanto. → [[Capacidad (capacity.h)]]
- **Cola:** retardo de ida menos su propio mínimo de ventana. → [[Cola por enlace (cg_gov_base)]]
- **Base:** el retardo sin cola (el mínimo). → [[Cola por enlace (cg_gov_base)]]
- **Capacidad:** lo más que entregó un enlace; baja solo lleno. → [[Capacidad (capacity.h)]]
- **Pérdida con cola (`capacity_loss_queue_ms`):** pierde ≥ 1 % con cola sobre el mínimo de 60 s: lleno a cualquier carga. → [[Capacidad (capacity.h)]]
- **Cola permanente (`capacity_stand`):** la cola sobre el mínimo de 60 s que el de 10 s ya tomó de base; frena el arranque rápido. → [[Capacidad (capacity.h)]]
- **Lleno (full, sat):** cola sobre `capacity_sat_ms`, entregas cortas o pérdida con carga. → [[Capacidad (capacity.h)]]
- **Corte / arranque rápido:** bajar la capacidad al llenarse / volver a la mitad de la mejor tras 10 s. → [[Capacidad (capacity.h)]]
- **Sondeo a 5/4 (probe):** ofrecer más de lo medido a un enlace de cola corta. → [[Capacidad (capacity.h)]]
- **Con pérdida (lossy):** pierde más del 0,3 % en 5 s. → [[Capacidad (capacity.h)]]
- **Frontera:** la tasa donde un enlace empieza a perder (planeado para Starlink). → [[Modelo de enlace (linkmodel.h)]]
- **Retención (lossy_hold) / costo de pasarse (hold_over_ms):** el ETA no llena un enlace que pierde más allá de su cola más alta. → [[ETA (eta.h)]]
- **Ranura (slot) / borde (edge) / ventana:** los 15 s de Starlink, el instante del cambio (:12, :27, :42, :57) y el margen alrededor. → [[Ranuras de Starlink (slot.h)]]
- **Tope de cake:** la tasa para un shaper delante del túnel. → [[Tope para cake (capacity.h)]]
- **Tardanza conjunta:** probabilidad de que todos los enlaces de un grupo lleguen tarde. → [[Llegadas (arrival.h)]]

## Recepción y recuperación
- **Reordenador / hueco / plazo / margen:** pone en orden; un hueco es una secuencia que falta; el plazo es cuánto se la espera. → [[Reordenador (reorder.h)]]
- **RACK:** abandonar un hueco cuando todos los enlaces trajeron algo posterior. → [[Reordenador (reorder.h)]]
- **Tardío (late):** llegó después de abandonado su hueco. → [[Reordenador (reorder.h)]]
- **rset:** los enlaces que el reordenador espera. → [[Reordenador (reorder.h)]]
- **Rescate / NACK / GONE:** pedir de nuevo un hueco; el aviso de que no vendrá. → [[Rescate (rescue.h)]]
- **PARQ / sellado / déficit de rango:** pedir paridad en vez de paquetes; un hueco sellado ya no lo cubrirá ninguna reparación nueva del código; el déficit es cuántas reparaciones más lo rehacen. → [[Reparación coherente (harq.h)]]
- **Presupuesto de TCP:** el RTT de los flujos más un cuarto (RACK): lo que tarda TCP en retransmitir un hueco; ninguna reparación del túnel se pide ni se retiene más allá. → [[Reparación coherente (harq.h)]]
- **FEC / paridad / grupo:** XOR de un paquete por enlace, que reconstruye uno perdido. → [[FEC (fec.h)]]
- **Código de borrado / reparación (`rs`, `rlc`):** k paquetes fuente y m reparaciones (Reed-Solomon en bloques, o una ventana deslizante de combinaciones aleatorias) que rehacen hasta m perdidos. → [[Códigos de borrado (rs.h, rlc.h)]]
- **Liberación (CG_F_FREE):** paquetes que el reordenador no retiene. → [[Liberación TCP (tcpfree.h)]]

## Infraestructura
- **Dosificación (pacing):** el kernel limita la tasa del socket de cada enlace. → [[Dosificación (pace.h)]]
- **Carril (lane) / basura:** sockets del servidor por número de enlace; el de lo que no es protocolo. → [[Colas del servidor (steer.h)]]
- **Hub / bomba (pump):** el hilo con la sesión / los hilos que leen los enlaces. → [[Hilos por enlace (thrplan.h)]]
- **Pista de cliente / cookie / HELLO:** cómo el servidor halla la clave y acepta caminos nuevos. → [[Varios clientes (clients.h)]], [[Cookies y HELLO (cookie.h)]]
- **IP pass:** todos los puertos públicos del VPS a un router. → [[Tabla de reenvío (fwdtable.h)]]
- **Túnel nativo:** paquetes IP de una TUN propia en vez de WireGuard. → [[Rutas del túnel nativo (tunroute.h)]]
- **Relojería:** la auditoría de cómo se pisan los lazos (C1–C13). → [[Bitácora de decisiones (decision.h)]], [[audit-pending]]
- **Control nulo:** el mismo código compilado aparte, para medir el ruido de un A/B. → [[015-modelo-de-enlace]]

Volver a [[Mapa del motor]].
