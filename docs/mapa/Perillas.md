---
tags: [mapa, hub, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# Perillas

Las 128 claves que `engine/src/config.c` lee (comprobado al generar esta nota: ni falta ni sobra ninguna).
Por omisión y rango salen de `cg_config_parse`; el significado, de los comentarios de `config.h` y de [[engine-README]].
Las perillas marcadas como experimento están apagadas por omisión.

Secciones: «global» es la parte sin sección; `[link NOMBRE]` es del cliente; `[client NOMBRE]` del servidor.

## Básicas y sistema

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/mode\|mode]] | (obligatoria) | [[Configuración (config.c)]] | [[005-motor-c-v1]] |
| [[Mapa/Perillas/key\|key]] | (obligatoria; con secciones `[client]`, una en cada una y ninguna global) | [[Configuración (config.c)]], [[Varios clientes (clients.h)]] | [[008-luci-uci-y-emparejamiento]] |
| [[Mapa/Perillas/description\|description]] | vacía | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/listen\|listen]] | cliente `127.0.0.1:59401`; servidor `*:59402` | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/log_level\|log_level]] | `info` | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/status_file\|status_file]] | vacía (no escribe) | [[Configuración (config.c)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/status_interval_ms\|status_interval_ms]] | 1000 | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/control_socket\|control_socket]] | vacía (sin socket) | [[Configuración (config.c)]] | [[009-recarga-control-e-ip-pass]] |
| [[Mapa/Perillas/rcvbuf\|rcvbuf]] | 4194304 (4 MiB) | [[Presupuesto de recepción (rcvbudget.h)]] | [[011-hilos]] |
| [[Mapa/Perillas/sndbuf\|sndbuf]] | 0 (el del kernel) | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/cpu\|cpu]] | -1 (cualquiera) | [[Hilos por enlace (thrplan.h)]] | [[006-salud-de-enlaces]], [[011-hilos]] |
| [[Mapa/Perillas/busy_poll_us\|busy_poll_us]] | 0 | [[Hilos por enlace (thrplan.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/rt_priority\|rt_priority]] | 0 (normal) | [[Hilos por enlace (thrplan.h)]] | [[006-salud-de-enlaces]] |

## Túnel nativo

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/tunnel\|tunnel]] | `wireguard` | [[Rutas del túnel nativo (tunroute.h)]], [[Liberación TCP (tcpfree.h)]] | [[014-tunel-nativo]] |
| [[Mapa/Perillas/tun\|tun]] | `cg0` | [[Rutas del túnel nativo (tunroute.h)]] | [[014-tunel-nativo]] |
| [[Mapa/Perillas/tun_addr\|tun_addr]] | ninguna (las pone el sistema) | [[Rutas del túnel nativo (tunroute.h)]] | [[014-tunel-nativo]] |
| [[Mapa/Perillas/tun_mtu\|tun_mtu]] | 1420 | [[Rutas del túnel nativo (tunroute.h)]] | [[014-tunel-nativo]] |
| [[Mapa/Perillas/tun_txqueuelen\|tun_txqueuelen]] | 0 (el del kernel, 500) | [[Rutas del túnel nativo (tunroute.h)]] | [[audit-pending]] |
| [[Mapa/Perillas/tun_route\|tun_route]] | un cliente sin ella: `0.0.0.0/0` y `::/0` | [[Rutas del túnel nativo (tunroute.h)]] | [[014-tunel-nativo]] |

## Cliente: enlaces, sondas, servidores

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/interfaces\|interfaces]] | `*` | [[Configuración (config.c)]] | [[009-recarga-control-e-ip-pass]] |
| [[Mapa/Perillas/exclude\|exclude]] | vacía (más las de fábrica: `lo wg* docker* veth* br-* virbr* tun* tap* ifb* dummy*`) | [[Configuración (config.c)]] | [[009-recarga-control-e-ip-pass]] |
| [[Mapa/Perillas/server\|server]] | (obligatoria, global o por enlace) | [[Elección de servidor (srvpick.h)]], [[Nombres del servidor (resolve.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/resolver\|resolver]] | global: `1.1.1.1 9.9.9.9 [2606:4700:4700::1111] [2620:fe::fe]`; enlace: ninguno | [[Nombres del servidor (resolve.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/server_failover_ms\|server_failover_ms]] | el mayor de 10000 y 3 × `probe_idle_ms` | [[Elección de servidor (srvpick.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/probe_interval_ms\|probe_interval_ms]] | 100 | [[Sondas y cola de bonding (proto.h)]], [[Salud y silenciado (health.h)]], [[G3 enlace sin respuesta (split.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/probe_idle_ms\|probe_idle_ms]] | 1000 | [[Sondas y cola de bonding (proto.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/passthrough\|passthrough]] | cliente: sin pedir (-1); `[client]`: `yes` | [[Tabla de reenvío (fwdtable.h)]] | [[009-recarga-control-e-ip-pass]], [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/link_threads\|link_threads]] | `legacy` | [[Hilos por enlace (thrplan.h)]] | [[011-hilos]] |
| [[Mapa/Perillas/io_queue\|io_queue]] | 256 | [[Hilos por enlace (thrplan.h)]] | [[011-hilos]] |
| [[Mapa/Perillas/label\|label]] | vacía | [[Configuración (config.c)]] | — |
| [[Mapa/Perillas/enabled\|enabled]] | `yes` | [[Configuración (config.c)]], [[Varios clientes (clients.h)]] | — |
| [[Mapa/Perillas/weight\|weight]] | 1 | [[Reparto (split.h)]], [[Gobernador (govern.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/type\|type]] | `auto` | [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |

## Servidor

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/wireguard\|wireguard]] | (obligatoria con un cliente y `tunnel = wireguard`) | [[Configuración (config.c)]], [[Varios clientes (clients.h)]] | [[005-motor-c-v1]] |
| [[Mapa/Perillas/wireguard_poke\|wireguard_poke]] | `10.79.0.2:9` | [[Reloj atrasado de WireGuard (wgwatch.h)]] | [[002-wireguard-para-cengarde]], [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/wireguard_port_file\|wireguard_port_file]] | vacía | [[Reloj atrasado de WireGuard (wgwatch.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/max_sessions\|max_sessions]] | 64 (o 4 por cliente si es más) | [[Varios clientes (clients.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/session_timeout_ms\|session_timeout_ms]] | 180000 | [[Varios clientes (clients.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/path_timeout_ms\|path_timeout_ms]] | 30000 | [[Varios clientes (clients.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/passthrough_file\|passthrough_file]] | vacía | [[Tabla de reenvío (fwdtable.h)]] | [[009-recarga-control-e-ip-pass]] |
| [[Mapa/Perillas/forward_file\|forward_file]] | vacía | [[Tabla de reenvío (fwdtable.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/forward\|forward]] | ninguna | [[Tabla de reenvío (fwdtable.h)]] | [[010-ipv6-varias-ip-multicliente-nombres]] |
| [[Mapa/Perillas/lanes\|lanes]] | `auto` (8) | [[Colas del servidor (steer.h)]], [[Dosificación (pace.h)]] | [[011-hilos]] |

## Salud

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/mute_behind_ms\|mute_behind_ms]] | 150 | [[Salud y silenciado (health.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/unmute_behind_ms\|unmute_behind_ms]] | 4/5 de `mute_behind_ms` (120) | [[Salud y silenciado (health.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/mute_settle_ms\|mute_settle_ms]] | 2000 | [[Salud y silenciado (health.h)]], [[G4 guarda de dispersión (split.h)]] | [[006-salud-de-enlaces]], [[012-bonding]] |
| [[Mapa/Perillas/mute_trickle\|mute_trickle]] | 0 | [[Salud y silenciado (health.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/min_active_links\|min_active_links]] | 2 | [[Salud y silenciado (health.h)]] | [[006-salud-de-enlaces]] |
| [[Mapa/Perillas/mute_queue\|mute_queue]] | `no` | [[Salud y silenciado (health.h)]], [[Cola por enlace (cg_gov_base)]] | [[016-celda-virtual]] |

## Reordenador

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/reorder\|reorder]] | `yes` | [[Reordenador (reorder.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/reorder_max_wait_ms\|reorder_max_wait_ms]] | 200 | [[Reordenador (reorder.h)]] | [[012-bonding]], [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/reorder_dup_wait_ms\|reorder_dup_wait_ms]] | 0 | [[Reordenador (reorder.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/reorder_margin_ms\|reorder_margin_ms]] | 10 | [[Reordenador (reorder.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/reorder_bypass\|reorder_bypass]] | 128 | [[Reordenador (reorder.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/reorder_by_link\|reorder_by_link]] | `no` | [[Reordenador (reorder.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/reorder_adaptive\|reorder_adaptive]] | `no` | [[Reordenador (reorder.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/reorder_pool_kb\|reorder_pool_kb]] | 8192 | [[Reordenador (reorder.h)]] | [[012-bonding]], [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/reorder_idle_ms\|reorder_idle_ms]] | 0 (apagado) | [[Reordenador (reorder.h)]], [[Llegadas (arrival.h)]] | [[audit-pending]] |
| [[Mapa/Perillas/reorder_retx_release_ms\|reorder_retx_release_ms]] | 0 (apagado) | [[Reordenador (reorder.h)]], [[Liberación TCP (tcpfree.h)]] | [[014-tunel-nativo]] |
| [[Mapa/Perillas/tcp_aware\|tcp_aware]] | `no` | [[Liberación TCP (tcpfree.h)]], [[Reordenador (reorder.h)]] | [[014-tunel-nativo]] |

## Rescate y FEC

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/rescue_kb\|rescue_kb]] | 0 (apagado) | [[Rescate (rescue.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/rescue_pct\|rescue_pct]] | 5 | [[Rescate (rescue.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/rescue_wait_ms\|rescue_wait_ms]] | 0 (no pide) | [[Rescate (rescue.h)]], [[Reordenador (reorder.h)]], [[Reparación coherente (harq.h)]] | [[013-capacidad-y-dosificacion]], [[023-reparacion-coherente]] |
| [[Mapa/Perillas/rescue_tries\|rescue_tries]] | 2 | [[Rescate (rescue.h)]], [[Reordenador (reorder.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/rescue_target_ms\|rescue_target_ms]] | 20 (el de `governor_target_ms`; 0 lo sigue) | [[Rescate (rescue.h)]], [[Capacidad (capacity.h)]] | [[audit-pending]] |
| [[Mapa/Perillas/rescue_by_owd\|rescue_by_owd]] | `no` | [[Rescate (rescue.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |
| [[Mapa/Perillas/rescue_full_hyst\|rescue_full_hyst]] | 0 (apagado) | [[Rescate (rescue.h)]], [[Capacidad (capacity.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |
| [[Mapa/Perillas/rescue_code_ms\|rescue_code_ms]] | 100 | [[Reparación coherente (harq.h)]], [[Rescate (rescue.h)]] | [[023-reparacion-coherente]] |
| [[Mapa/Perillas/parq\|parq]] | `yes` | [[Reparación coherente (harq.h)]], [[Códigos de borrado (rs.h, rlc.h)]] | [[023-reparacion-coherente]] |
| [[Mapa/Perillas/rescue_tcp_race\|rescue_tcp_race]] | `yes` | [[Reparación coherente (harq.h)]], [[Liberación TCP (tcpfree.h)]] | [[023-reparacion-coherente]] |
| [[Mapa/Perillas/tcp_retx_one\|tcp_retx_one]] | `yes` | [[Liberación TCP (tcpfree.h)]], [[Reparación coherente (harq.h)]], [[Retransmisión según la evidencia (tcpack.h)]] | [[023-reparacion-coherente]] |
| [[Mapa/Perillas/tcp_ack_rule\|tcp_ack_rule]] | `yes` | [[Retransmisión según la evidencia (tcpack.h)]], [[Liberación TCP (tcpfree.h)]] | [[023-reparacion-coherente]] |
| [[Mapa/Perillas/fec\|fec]] | `no` | [[FEC (fec.h)]], [[Códigos de borrado (rs.h, rlc.h)]] | [[013-capacidad-y-dosificacion]], [[021-fec-v2]] |
| [[Mapa/Perillas/fec_wait_ms\|fec_wait_ms]] | 10 | [[FEC (fec.h)]], [[Rescate (rescue.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/fec_share_max\|fec_share_max]] | 0 (sin tope) | [[FEC (fec.h)]], [[Reparto (split.h)]] | [[012-bonding]], [[013-capacidad-y-dosificacion]] |

## Reparto y guardas

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/split\|split]] | cliente `off`; servidor `client` | [[Reparto (split.h)]], [[Sondas y cola de bonding (proto.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_small\|split_small]] | 256 | [[Reparto (split.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_spread_ms\|split_spread_ms]] | 30 | [[G4 guarda de dispersión (split.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_spread_copy\|split_spread_copy]] | `yes` salvo con `governor`, `capacity_weights` o `split_eta` | [[G4 guarda de dispersión (split.h)]], [[Reparto (split.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_stress_dup\|split_stress_dup]] | 50 | [[Copias bajo estrés (split.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_stress_rejoin\|split_stress_rejoin]] | `yes` | [[G4 guarda de dispersión (split.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/split_stress_single\|split_stress_single]] | `no` | [[G4 guarda de dispersión (split.h)]], [[Copias bajo estrés (split.h)]] | [[audit-pending]] |
| [[Mapa/Perillas/split_dwell_ms\|split_dwell_ms]] | 0 (apagado) | [[G4 guarda de dispersión (split.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |
| [[Mapa/Perillas/split_dwell_oq_ms\|split_dwell_oq_ms]] | 0 (cualquiera) | [[G4 guarda de dispersión (split.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |

## Gobernador y capacidad

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/governor\|governor]] | `no` | [[Gobernador (govern.h)]] | [[012-bonding]], [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/governor_target_ms\|governor_target_ms]] | 20 | [[Gobernador (govern.h)]], [[Capacidad (capacity.h)]], [[Rescate (rescue.h)]] | [[012-bonding]], [[audit-pending]] |
| [[Mapa/Perillas/governor_window_ms\|governor_window_ms]] | 10000 | [[Cola por enlace (cg_gov_base)]], [[Gobernador (govern.h)]], [[Capacidad (capacity.h)]] | [[012-bonding]] |
| [[Mapa/Perillas/capacity_weights\|capacity_weights]] | `no` | [[Capacidad (capacity.h)]], [[Reparto (split.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/capacity_sat_ms\|capacity_sat_ms]] | 20 (el de `governor_target_ms`; 0 lo sigue) | [[Capacidad (capacity.h)]], [[Tope para cake (capacity.h)]], [[ETA (eta.h)]] | [[audit-pending]] |
| [[Mapa/Perillas/capacity_loss_pct\|capacity_loss_pct]] | 0 (apagado) | [[Capacidad (capacity.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/capacity_seq_lag\|capacity_seq_lag]] | `yes` | [[Capacidad (capacity.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/capacity_expire_ms\|capacity_expire_ms]] | 0 (apagado) | [[Capacidad (capacity.h)]] | [[016-celda-virtual]] |
| [[Mapa/Perillas/capacity_loss_queue_ms\|capacity_loss_queue_ms]] | 0 (apagado) | [[Capacidad (capacity.h)]], [[Cola por enlace (cg_gov_base)]], [[Modelo de enlace (linkmodel.h)]] | [[019-perdida-con-cola]] |
| [[Mapa/Perillas/capacity_stand\|capacity_stand]] | 0 (apagado) | [[Capacidad (capacity.h)]], [[Cola por enlace (cg_gov_base)]] | [[019-perdida-con-cola]] |
| [[Mapa/Perillas/cap_g4_episode\|cap_g4_episode]] | 0 (apagado) | [[Capacidad (capacity.h)]], [[G4 guarda de dispersión (split.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |

## ETA, cosquilla, copias, dosificación

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/split_eta\|split_eta]] | `no` | [[ETA (eta.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/split_tickle_ms\|split_tickle_ms]] | 0 (nunca) | [[Cosquilla (tickle.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/split_stale_max\|split_stale_max]] | 1 (como se midió) | [[Capacidad (capacity.h)]], [[ETA (eta.h)]] | [[013-capacidad-y-dosificacion]] |
| [[Mapa/Perillas/eta_buffer\|eta_buffer]] | `no` | [[Modelo de enlace (linkmodel.h)]], [[ETA (eta.h)]], [[Capacidad (capacity.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/split_qpred_ms\|split_qpred_ms]] | 0 (apagado) | [[ETA (eta.h)]], [[Capacidad (capacity.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |
| [[Mapa/Perillas/split_qpred_mode\|split_qpred_mode]] | 0 | [[ETA (eta.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/eta_keep\|eta_keep]] | 0 (apagado) | [[ETA (eta.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[audit-pending]] |
| [[Mapa/Perillas/eta_keep_ms\|eta_keep_ms]] | 400 | [[ETA (eta.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/redund_pace_ms\|redund_pace_ms]] | 0 (apagado) | [[Copias a tiempo (eta.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/pace_peers_ms\|pace_peers_ms]] | -1 (el del preset: 60 ms en `starlink`) | [[Copias a tiempo (eta.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]], [[017-terreno-nostalgica]] |
| [[Mapa/Perillas/pacing\|pacing]] | `no` | [[Dosificación (pace.h)]] | [[013-capacidad-y-dosificacion]] |

## Modelo de enlace y Starlink

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/exp_link_types\|exp_link_types]] | vacía (todos los tipos) | [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/split_dominant_pct\|split_dominant_pct]] | 0 (el del preset: 250 en `starlink`) | [[Enlace dominante (dominant.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/split_dominant_hold\|split_dominant_hold]] | 0 (el del preset: apagado) | [[Enlace dominante (dominant.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/starlink_slot_clock\|starlink_slot_clock]] | 0 (el del preset: encendido en `starlink`) | [[Ranuras de Starlink (slot.h)]], [[Capacidad (capacity.h)]], [[Modelo de enlace (linkmodel.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/starlink_edge_ms\|starlink_edge_ms]] | 12000 (:12, :27, :42, :57) | [[Ranuras de Starlink (slot.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/starlink_edge_pre_ms\|starlink_edge_pre_ms]] | 100 | [[Ranuras de Starlink (slot.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/starlink_edge_post_ms\|starlink_edge_post_ms]] | 50 | [[Ranuras de Starlink (slot.h)]] | [[015-modelo-de-enlace]] |
| [[Mapa/Perillas/starlink_edge_copies\|starlink_edge_copies]] | `no` | [[Ranuras de Starlink (slot.h)]], [[Copias a tiempo (eta.h)]] | [[015-modelo-de-enlace]] |

## Lo que sobra (redundancia)

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/redund_spare\|redund_spare]] | `no` | [[Lo que sobra (spare.h)]], [[Copias a tiempo (eta.h)]], [[FEC (fec.h)]], [[Códigos de borrado (rs.h, rlc.h)]] | [[020-copias-parciales-y-paridad]], [[021-fec-v2]] |
| [[Mapa/Perillas/redund_spare_ms\|redund_spare_ms]] | 50 | [[Lo que sobra (spare.h)]] | [[020-copias-parciales-y-paridad]] |
| [[Mapa/Perillas/redund_spare_first\|redund_spare_first]] | vacío (el de menor retardo sin cola) | [[Lo que sobra (spare.h)]] | [[020-copias-parciales-y-paridad]] |
| [[Mapa/Perillas/redund_spare_code_ms\|redund_spare_code_ms]] | 30 | [[Códigos de borrado (rs.h, rlc.h)]], [[Lo que sobra (spare.h)]] | [[021-fec-v2]] |

## PEP modo B

| Perilla | Por omisión | Pieza | Historia |
| --- | --- | --- | --- |
| [[Mapa/Perillas/pep\|pep]] | `no` | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_port\|pep_port]] | 7801 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_buf_kb\|pep_buf_kb]] | 1024 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_mem_mb\|pep_mem_mb]] | 64 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_max_flows\|pep_max_flows]] | 256 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_queue_ms\|pep_queue_ms]] | 5 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_backlog_ms\|pep_backlog_ms]] | 200 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_dead_s\|pep_dead_s]] | 60 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_reo_ms\|pep_reo_ms]] | 10 | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |
| [[Mapa/Perillas/pep_cc\|pep_cc]] | `bbr` | [[PEP modo B (pep.c, stream.h, pepcc.h)]] | [[025-protocontrolador]] |

Volver a [[Mapa del motor]].
