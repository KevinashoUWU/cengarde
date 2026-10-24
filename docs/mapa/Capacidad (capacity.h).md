---
tags: [mapa, pieza]
archivos:
  - engine/src/capacity.h
  - engine/tests/test_capacity.c
sello: 1c21235f79bdca5f
---
# Capacidad (capacity.h)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
Estima cuánto lleva cada enlace en el sentido que envía este extremo. Entre dos informes del otro extremo: lo entregado (el cambio de `rx` por el tamaño medio enviado), sobre el tiempo entre informes, suavizado 1/4, en muestras de al menos 500 ms (`CG_CAP_MIN_MS`). La capacidad es la mayor tasa vista; **solo baja cuando el enlace está lleno**: cola sobre `sat_us` 2 muestras seguidas cargado (4 sin carga), entrega bajo la mitad de lo enviado 2 muestras (`short`), o pérdida sobre `loss_pct` cargado. El **arranque rápido** devuelve un enlace que lleva 10 s sin llenarse al menos a la mitad de su mejor capacidad de 5 min. Con `capacity_expire_ms` (experimental, apagado), un enlace fuera del envío ese tiempo sin una muestra cargada **caduca**: su capacidad baja a la mitad, y otra vez cada tanto, hasta 1/16 de su mejor (`cg_cap_expire`); cada mitad cuenta como muestra llena para el arranque rápido y como medida para `cg_cap_stale`. Con `capacity_loss_queue_ms` (experimental, apagado), un enlace que pierde ≥ 1 %, recibe más de lo que entrega y tiene cola sobre el mínimo de 60 s, dos muestras, está **lleno a cualquier carga** (`cg_cap_lossq`; con el preset de Starlink, solo cargado). Con `capacity_stand`, una cola permanente (`ql_us` sobre `sat_us`, 4 muestras) frena el arranque rápido (`cg_cap_stand`), y en modo 2 el arranque rápido va antes si el enlace entrega 9/8 de su capacidad sin pérdida ni cola (`cg_cap_room`).

## Lee
- `rx`, `rx_top` de [[Sondas y cola de bonding (proto.h)]]; lo enviado por enlace (paquetes, bytes, secuencia por tick).
- La cola de [[Cola por enlace (cg_gov_base)]] y el `owd` de [[Salud y silenciado (health.h)]].
- Por enlace, su [[Modelo de enlace (linkmodel.h)]]: `lossy` (pierde > 0,3 % en 5 s), `lossy_noprobe`, `loss_pct`, `rfull_hyst`, `g4_episode`, `slot_clock`.
- [[Ranuras de Starlink (slot.h)]]: una muestra enviada en la ventana de un borde no cuenta pérdida.
- [[G4 guarda de dispersión (split.h)]]: con `cap_g4_episode`, una salida por cola cuenta como episodio (`cg_cap_episode`).
- Con `capacity_loss_queue_ms` y `capacity_stand`, su `ql_us` (la cola sobre el mínimo de 60 s, [[Cola por enlace (cg_gov_base)]]) y, para Starlink, si su modelo sigue la pérdida (`cg_link_tracks_loss`).
- Con `capacity_expire_ms`, si el enlace está en el envío (`out`): vivo y portador según [[Salud y silenciado (health.h)]] y, con reparto, en él o llevando copia de todo ([[Reparto (split.h)]], [[G4 guarda de dispersión (split.h)]]).

## Decide / escribe
- `loss_pm` (la pérdida de la última muestra con lo enviado conocido, por mil) y su hora → [[Lo que sobra (spare.h)]] (la mezcla vuelve a copias en un microcorte).
- La tasa por enlace → [[ETA (eta.h)]] (`cg_cap_eta`), [[Dosificación (pace.h)]], [[Enlace dominante (dominant.h)]], [[Cosquilla (tickle.h)]] (`cg_cap_tickle`).
- Con `capacity_weights`, los pesos del reparto (`cg_cap_weights`) → [[Reparto (split.h)]]: su capacidad, 5/4 (y 1/16 del mayor) bajo media `sat_us`; el doble bajo la mitad de su mejor (arranque rápido); escalado por el [[Gobernador (govern.h)]] si está.
- El peso del rescate (`cg_cap_rescue_w`) y su bit `full` → [[Rescate (rescue.h)]].
- La tasa para `cake` → [[Tope para cake (capacity.h)]].
- Su `ql_us` → `mute_queue` de [[Salud y silenciado (health.h)]].
- Cortes y restauraciones → [[Bitácora de decisiones (decision.h)]].

## Perillas
- [[Mapa/Perillas/capacity_weights|capacity_weights]]: no. [[Mapa/Perillas/capacity_sat_ms|capacity_sat_ms]]: 0 (= `governor_target_ms`, 20).
- [[Mapa/Perillas/capacity_seq_lag|capacity_seq_lag]]: sí. [[Mapa/Perillas/capacity_loss_pct|capacity_loss_pct]]: 0; 0–50.
- [[Mapa/Perillas/capacity_loss_queue_ms|capacity_loss_queue_ms]]: 0; 0–1000 (sugerido 10). [[Mapa/Perillas/capacity_stand|capacity_stand]]: 0; 0–2.
- [[Mapa/Perillas/cap_g4_episode|cap_g4_episode]]: 0; 0–3. [[Mapa/Perillas/capacity_expire_ms|capacity_expire_ms]]: 0; 0–600000. [[Mapa/Perillas/split_stale_max|split_stale_max]]: 1; 1–64.
- [[Mapa/Perillas/governor_window_ms|governor_window_ms]], [[Mapa/Perillas/rescue_target_ms|rescue_target_ms]], [[Mapa/Perillas/rescue_full_hyst|rescue_full_hyst]], [[Mapa/Perillas/eta_buffer|eta_buffer]], [[Mapa/Perillas/starlink_slot_clock|starlink_slot_clock]].

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: muestras de 250 ms oscilaban hasta 1,5–2× lo real; con el decaimiento tipo BBR, dos o tres de cuatro enlaces quedaban en el piso y el túnel en 40–50 %; el arranque rápido fue neutro en laboratorio.
- [[016-celda-virtual]]: un LTE fuera por el acantilado del SNR guardaba 8–21 Mbit/s; con `capacity_expire_ms = 2000` vuelve con 0,3–1,3. Al volver la señal entera eso costó (bonding 23,0 → 20,8 Mbit/s en los 10 s siguientes); al volver débil, los cortes de siempre bajaron la vieja en 1,5–2,5 s y no hubo diferencia. Queda apagada.
- [[019-perdida-con-cola]]: en trazas de ns-3 con verdad, un búfer corto con TCP dejaba la capacidad 3–6 veces arriba 40 s y `capacity_loss_queue_ms = 10` la corrige en 1,0–1,4 s (sin cortes en pérdida aleatoria sin cola); una cola permanente devolvía +523 % por el arranque rápido y `capacity_stand` lo evita. En la celda calibrada corrige antes, pero ni ahí ni en el laboratorio cambia el TCP: quedan apagadas.
- [[020-copias-parciales-y-paridad]]: un enlace que solo llevaba paridad la contaba como enviada y no recibida (el servidor cuenta DATA por camino): su capacidad caía a la mitad cada muestra. Corregido en el cliente (`tx_fec_*` aparte).
- [[015-modelo-de-enlace]] §9: con la pérdida por secuencia y el reloj de ranuras, Starlink marcado `lossy` pasa del 93 % del tiempo al 17 %, caudal igual.

## Choques conocidos
- C5: arranque rápido y envejecimiento suben; solo «lleno» baja, y G4 saca al enlace antes de las 2 muestras: la sobreestimación no se corrige ([[audit-pending]]). `cap_g4_episode` quedó como perilla apagada ([[015-modelo-de-enlace]] §6). La cola permanente que se vuelve base y el arranque rápido que la deshace: `capacity_stand` ([[019-perdida-con-cola]]).
- `capacity_loss_queue_ms` frente a Starlink: su retardo salta por ranura (la cola sobre 60 s pasa 10 ms sin estar lleno) y pierde sin cola; por eso con el preset solo cuenta cargado.
- C12: `governor_target_ms` movía cuatro lazos; ahora `capacity_sat_ms` es propio (PR #86).
- Un enlace sin carga no se vuelve a medir: lo resuelven [[Cosquilla (tickle.h)]] y `split_stale_max`.
- `capacity_expire_ms` baja lo que el arranque rápido sube: por eso cada mitad marca `full_ms` (el arranque espera 10 s después de la vuelta).

## Archivos
- `capacity.h`; tests `engine/tests/test_capacity.c`.
- `client.c` y `server.c`: `cap_tick` (después de `gov_tick`), y `cg_cap_rescue_w` en el envío.
