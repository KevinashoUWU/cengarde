# 017 — Terreno en Nostálgica FM (20 al 22-10-2026)

- **Fecha:** 2026-10-23
- **Estado:** vigente
- **Fuentes:** registro de cengarde en el NUC (`/srv/cengarde-rec`, bajado a
  `~/campo/2026-10-22/nos-tarde/` y `nos-1500/` del PC del banco), el plato
  (`dish-rec`, `~/campo/2026-10-21/nos/`), el registrador de pings
  (`/srv/cengarde-rec/ping/`), `cell-monitor` del cpe1, el estado del VPS;
  rama `claude/pace-peers-knob` (`7474be9`), análisis en `~/campo/2026-10-20/peers/`
  (`pct.py`), `~/campo/2026-10-22/curvas.py`.

## TL;DR

- Tres días de transmisión real en redundancia con Starlink, Entel y WOM. El
  último día la subida perdió **352 paquetes de 21,1 millones (0,0017 %)** en
  todos los enlaces; Starlink solo habría perdido **129 937 (0,62 %)**.
- Las pérdidas en todos los enlaces venían de **copias recortadas**: la regla
  del preset de Starlink (`pace_peers_ms` 60) dejaba a WOM con 36–60 % de las
  copias. Con `pace_peers_ms = 250` (perilla nueva, desplegada el 22-10 a las
  08:31) pasó a ~90 %, y la subida bajó de 0,29 % a 0,002 %.
- **Entel en banda 2** (RSRQ −18, SINR −6 a −2 por antena) tenía 1–1,5 s de RTT y
  5–19 % de pérdida en la tarde. Forzado a **B28 solo LTE** (22-10, 11:39): RTT
  p50/p90 137/1227 → 45/77 ms, pérdida propia 2,1 → 0,01 %. WOM quedó en
  automático como seguro.
- Los picos de Starlink no eran del satélite ni de la casa: el plato entregó todo
  lo que el NUC mandó y su ping a su PoP siguió limpio. La pérdida ocurre después
  del plato, más hacia Vultr (ping 0,12 %) que hacia Cloudflare (0,05 %).
- Falta: la bajada (0,12 %), que decide el VPS con su regla de 60 ms; el rescate
  casi no actúa; WOM se ahoga con nuestras propias copias sobre su codo de ~5 Mb/s.

## Contexto

Nostálgica FM (Atacama) transmitió en vivo con cengarde en redundancia, túnel
nativo, motor `0d20169` (y `7474be9` desde el 22-10). Enlaces: Starlink (`sl`,
`type = starlink`), Entel y WOM desde el cpe1 (VLAN 10 y 20). El VPS y el NUC
estaban congelados; los cambios se hicieron en ventanas autorizadas.

## Hallazgos

### Por qué se perdía en todos los enlaces

| Día | Subida perdida en todos | Copias de WOM | Copias de Entel |
|---|---|---|---|
| 20-10 | 0,19 % (23 539 de 12,65 M, VPS) | 60 % | 26 % |
| 21-10 | 0,29 % (26 675 de 9,2 M) | 36 % | 36 % |
| 22-10, desde 08:31 | 0,0017 % (352 de 21,1 M) | ~90 % | 91–100 % |

- Con 60 ms, un enlace solo lleva copia si llegaría a menos de 60 ms de la
  primera. WOM ya va 20–40 ms atrás y su capacidad estimada (1,6–2,7 Mb/s) estaba
  muy bajo su iperf3 (20,7): se quedaba sin copias y lo que Starlink perdía no lo
  tenía nadie.
- Laboratorio antes del despliegue (`~/campo/2026-10-20/peers/val/P`, n = 8):
  150–500 ms bajan las retransmisiones de `shallowqd` ~60 % y suben el caudal
  4 %, con el p99 +1–2 ms; `starsat` sin diferencia.
- El 22-10, el único episodio grande (10:30, 218 paquetes) fue con los tres
  enlaces mal a la vez: Starlink perdiendo, Entel todavía en B2 a 1,6 s y WOM con
  8 s de cola. Desde B28, casi cero.

### Starlink pierde aunque vaya liviano

- Pérdida propia de subida por día: 0,61 % (20-10), 0,62 % (22-10); por hora,
  0,1–0,9 %, subiendo en la tarde; 0,4 % aun con 1–2 Mb/s.
- Firma (`curvas.py`, 20–21-10): cola de 2–5 ms a cualquier carga y pérdida de
  0,3–1,3 %: pierde sin hacer cola. WOM encola (codo ~5 Mb/s) y pierde ~0,15 %.
- Microcortes (12:59, 16:12, 16:14, 18:17 del 20-10): 20–40 s con 50–77 % de la
  subida perdida. El plato no registró caídas; entregó lo que el NUC mandó y su
  ping al PoP siguió limpio (salvo 16:14, con latencia a 67 ms y pings perdidos).
- Pings por Starlink el 22-10 (22 946 por destino): router 0, plato 0, VPS 27
  (0,12 %), 1.1.1.1 12 (0,05 %).

### El rescate y las medidas

- El rescate pidió 112 reenvíos en un día de 25 000 pérdidas: el reordenador casi
  no retiene huecos para pedirlos.
- `rx_missed` por enlace cuenta como perdidas las copias que el emisor no mandó
  (la bajada de WOM salía en 4,33 %).
- `lost_all` se anota 2–5 min tarde: cuando la secuencia sale de la ventana de
  32 768.

## Qué hicimos

- `pace_peers_ms` (rama `claude/pace-peers-knob`, sobre `0d20169`): perilla del
  cliente sobre el 60 del preset; desplegada con `deploy.sh 250` (respaldo y
  `rollback`). Falta llevarla a `master` con su nota del mapa.
- Entel a B28 por AT desde el NUC: VLAN 99 temporal (`mgmt99`) y el candado de
  `cell-monitor` para el puerto AT. Configuración original en el cpe1,
  `/root/entel-band-before.txt`: revertir en el banco.
- Informes para el cliente (artifacts) de la jornada del 22-10.

## Pendiente

- La perilla en el servidor para la bajada, y su despliegue en el VPS.
- Copias parciales o paridad por el enlace ahogado (WOM sobre su codo).
- ~~El rescate: por qué el reordenador no retiene los huecos.~~ Hecho en la
  historia 023: la pista `CG_F_PREV` se escribía antes de quitar copias y en
  redundancia nada se retenía; falta probarlo aquí.
- IPv6 por Starlink: el NUC tiene v6 nativo; el VPS no. Comparar un día v4 y v6
  con el registrador de pings antes de usarlo.
- `lost_all` con la hora del hueco; `rx_missed` contra lo que el emisor mandó.

## Cambios

- 2026-10-23: creada.
- 2026-10-24: el rescate casi mudo, explicado y arreglado (historia 023: la
  pista antes de las copias, nada retenido en redundancia, lo pedido sin
  espera al tope); en el laboratorio, `protect` con copias de 250 ms pasa de
  0,275 a 0,005 % perdido y el TCP de 7,6 a 14,4 Mbit/s.
