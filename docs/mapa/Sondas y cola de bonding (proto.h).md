---
tags: [mapa, pieza]
archivos:
  - engine/src/proto.h
  - engine/src/proto.c
  - engine/tests/test_proto.c
sello: 994a5b4395a36300
---
# Sondas y cola de bonding (proto.h)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
Es la fuente de casi todas las medidas. El cliente manda una sonda por enlace; el servidor responde por el mismo enlace. Cada sonda o respuesta lleva lo que el otro extremo midió: el retardo de ida (`owd`), los paquetes verificados recibidos por ese camino (`rx`), la secuencia DATA más nueva recibida (`rx_top`) y el intervalo hasta la próxima sonda. Al final va la «cola de bonding» (`cg_probe_bond`), 4 bytes con el peso del enlace, si el extremo reordena, si pide reparto, si guarda para el rescate, si la ruta está llena, el tipo del enlace y si marca sus ACK de TCP (`CG_BOND_TCPMARK`, bit 23); y tras ella la máscara de enlaces activos (`cg_probe_act`, protocolo 11): 16 bits con los enlaces por los que el emisor llevó datos dentro de su retención antes del ts, y la retención en ms ([[Enlaces activos (active.h)]]). La respuesta la lleva solo si la sonda la traía.

## Lee
- El reloj monotónico del emisor (timestamp de la cabecera) y lo recibido por cada camino.
- [[Salud y silenciado (health.h)]] marca `CG_F_MUTED`; [[Rescate (rescue.h)]] fija `CG_BOND_RESCUE` y `CG_BOND_FULL`; [[Modelo de enlace (linkmodel.h)]] da `CG_BOND_TYPE` (bits 20–22).

## Decide / escribe
- `owd` → [[Salud y silenciado (health.h)]] (filtro EWMA 1/4), y desde ahí [[Cola por enlace (cg_gov_base)]], [[G4 guarda de dispersión (split.h)]], [[Gobernador (govern.h)]], [[Capacidad (capacity.h)]], [[ETA (eta.h)]].
- `rx` y `rx_top` → [[Capacidad (capacity.h)]] (lo entregado y la pérdida por secuencia).
- La cola de bonding → `cg_split_active` en [[Reparto (split.h)]] (vale solo la del mensaje de control más nuevo, `cg_split_tail_newer`).
- La máscara de enlaces activos de la más nueva → el `rset` del [[Reordenador (reorder.h)]] ([[Enlaces activos (active.h)]]).
- Respuestas verificadas → [[G3 enlace sin respuesta (split.h)]] (`last_reply_ms`) y el retardo de cada enlace para el [[Reordenador (reorder.h)]] (`cg_ro_owd`).

## Perillas
- [[Mapa/Perillas/probe_interval_ms|probe_interval_ms]]: 100; 100–60000 (con tráfico).
- [[Mapa/Perillas/probe_idle_ms|probe_idle_ms]]: 1000; hasta 600000 (en reposo).
- [[Mapa/Perillas/split|split]], [[Mapa/Perillas/reorder|reorder]], [[Mapa/Perillas/weight|weight]], [[Mapa/Perillas/type|type]]: lo que viaja en la cola.

## Dónde se midió
- [[006-salud-de-enlaces]]: el retardo de ida relativo como señal (protocolo v2), sondeo cada 100 ms con tráfico y cada segundo en reposo.
- [[012-bonding]]: la cola de 4 B de las sondas, sin subir la versión.
- [[015-modelo-de-enlace]]: el tipo en los bits 20–22, sin versión nueva.
- [[021-fec-v2]]: protocolo 8, el tipo `CG_T_REPAIR` (7) de los códigos de borrado (`repair.h`, [[Códigos de borrado (rs.h, rlc.h)]]): cuenta en la secuencia de control, como la paridad; `cg_hdr_parse` exige su cabecera y un byte.
- [[023-reparacion-coherente]]: protocolo 9, el tipo `CG_T_PARQ` (8, del servidor al cliente): un recuento y hasta 8 pedidos de 20 bytes (`harq.h`); `cg_hdr_parse` exige el largo exacto. La versión sube también porque `CG_F_PREV` se escribe ahora una vez decididas las copias.
- [[025-protocontrolador]]: protocolo 10, los mensajes del protocontrolador (variante B) dentro de DATA (primer nibble `0xF`, nunca IPv4 ni IPv6; `protoctl.h`): ningún tipo nuevo de cabecera, pero un servidor 9 los escribiría a la TUN y la conexión quedaría colgada sin aviso → [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]].
- [[013-capacidad-y-dosificacion]] (2026-10-26): la máscara de enlaces activos (`cg_probe_act`, `CG_PROBE_ACT_LEN` = 48), en el mismo protocolo 11.
- [[026-fec-v3]]: protocolo 11, las reparaciones de ventana ancha (`CG_REP_RLCW` en `CG_T_REPAIR`: base, largo, densidad y semilla en vez del mapa) y `CG_T_REPAIR`/`CG_T_PARQ` en los dos sentidos: un servidor 10 tomaría una reparación ancha por mal formada (contada, sin daño), pero un cliente con la FEC v3 contra él protegería en vano → [[FEC v3 (fec3.h)]]. `cengarde-vps-setup` acepta 4 a 12.
- [[029-mpgrit]]: protocolo 12, la secuencia de cada enlace (`sseq`, 4 bytes en el desplazamiento 24: la cabecera pasa de 24 a 28 bytes), fuera del MAC como el byte del enlace (una copia por otro enlace lleva el suyo, `cg_hdr_set_sseq`), 0 sin numerar; en una sonda o su respuesta, el siguiente `sseq` del enlace (la sonda de cola del subflujo); y el tipo `CG_T_SFLOSS` (9, de control, en los dos sentidos): un byte con el enlace y los rangos de un NACK (`cg_hdr_parse` exige ese largo) → [[Subflujo (subflow.h)]].
- [[023-reparacion-coherente]] (2026-10-24): `CG_F_TUNHOLE` (0x02 de DATA, reservado hasta entonces) y `CG_BOND_TCPMARK` (bit 23 de la cola, también en la respuesta), sin versión nueva, como el tipo: un par anterior escribe 0 y los ignora → [[Retransmisión según la evidencia (tcpack.h)]].

## Choques conocidos
- Un solo sensor alimenta siete lazos: el OWD llega tarde (el del sentido contrario, más una sonda, más ~300–400 ms de EWMA) y todos actúan sobre la misma máscara (auditoría de relojería, `~/campo/2026-10-17/relojeria/analysis.md`; ver [[audit-pending]]).
- C2 (resuelto el 2026-10-26): la máscara de enlaces activos viaja tras la cola de bonding, dentro del protocolo 11 aún no publicado (sin versión nueva): [[Enlaces activos (active.h)]].
- El comentario de cabecera dice «version 5» y `CG_PROTO_VERSION` vale 7 (ver [[Mapa del motor#Contradicciones]]).

## Archivos
- `engine/src/proto.h` (`struct cg_probe_info`, `CG_BOND_*`, `CG_F_*`), `proto.c`.
- Tests: `engine/tests/test_proto.c`.
- `client.c`: `send_probe`, `on_probe_reply`, `on_hello`. `server.c`: `on_probe`, `queue_reply`, `queue_hello`.
