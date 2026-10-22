---
tags: [mapa, pieza]
archivos:
  - engine/src/capacity.h
  - engine/tests/test_capacity.c
sello: 0fd08a4ba35b28b1
---
# Capacidad (capacity.h)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
Estima cuánto lleva cada enlace en el sentido que envía este extremo. Entre dos informes del otro extremo: lo entregado (el cambio de `rx` por el tamaño medio enviado), sobre el tiempo entre informes, suavizado 1/4, en muestras de al menos 500 ms (`CG_CAP_MIN_MS`). La capacidad es la mayor tasa vista; **solo baja cuando el enlace está lleno**: cola sobre `sat_us` 2 muestras seguidas cargado (4 sin carga), entrega bajo la mitad de lo enviado 2 muestras (`short`), o pérdida sobre `loss_pct` cargado. El **arranque rápido** devuelve un enlace que lleva 10 s sin llenarse al menos a la mitad de su mejor capacidad de 5 min. Con `capacity_expire_ms` (experimental, apagado), un enlace fuera del envío ese tiempo sin una muestra cargada **caduca**: su capacidad baja a la mitad, y otra vez cada tanto, hasta 1/16 de su mejor (`cg_cap_expire`); cada mitad cuenta como muestra llena para el arranque rápido y como medida para `cg_cap_stale`.

## Lee
- `rx`, `rx_top` de [[Sondas y cola de bonding (proto.h)]]; lo enviado por enlace (paquetes, bytes, secuencia por tick).
- La cola de [[Cola por enlace (cg_gov_base)]] y el `owd` de [[Salud y silenciado (health.h)]].
- Por enlace, su [[Modelo de enlace (linkmodel.h)]]: `lossy` (pierde > 0,3 % en 5 s), `lossy_noprobe`, `loss_pct`, `rfull_hyst`, `g4_episode`, `slot_clock`.
- [[Ranuras de Starlink (slot.h)]]: una muestra enviada en la ventana de un borde no cuenta pérdida.
- [[G4 guarda de dispersión (split.h)]]: con `cap_g4_episode`, una salida por cola cuenta como episodio (`cg_cap_episode`).
- Con `capacity_expire_ms`, si el enlace está en el envío (`out`): vivo y portador según [[Salud y silenciado (health.h)]] y, con reparto, en él o llevando copia de todo ([[Reparto (split.h)]], [[G4 guarda de dispersión (split.h)]]).

## Decide / escribe
- La tasa por enlace → [[ETA (eta.h)]] (`cg_cap_eta`), [[Dosificación (pace.h)]], [[Enlace dominante (dominant.h)]], [[Cosquilla (tickle.h)]] (`cg_cap_tickle`).
- Con `capacity_weights`, los pesos del reparto (`cg_cap_weights`) → [[Reparto (split.h)]]: su capacidad, 5/4 (y 1/16 del mayor) bajo media `sat_us`; el doble bajo la mitad de su mejor (arranque rápido); escalado por el [[Gobernador (govern.h)]] si está.
- El peso del rescate (`cg_cap_rescue_w`) y su bit `full` → [[Rescate (rescue.h)]].
- La tasa para `cake` → [[Tope para cake (capacity.h)]].
- Su `ql_us` → `mute_queue` de [[Salud y silenciado (health.h)]].
- Cortes y restauraciones → [[Bitácora de decisiones (decision.h)]].

## Perillas
- [[Mapa/Perillas/capacity_weights|capacity_weights]]: no. [[Mapa/Perillas/capacity_sat_ms|capacity_sat_ms]]: 0 (= `governor_target_ms`, 20).
- [[Mapa/Perillas/capacity_seq_lag|capacity_seq_lag]]: sí. [[Mapa/Perillas/capacity_loss_pct|capacity_loss_pct]]: 0; 0–50.
- [[Mapa/Perillas/cap_g4_episode|cap_g4_episode]]: 0; 0–3. [[Mapa/Perillas/capacity_expire_ms|capacity_expire_ms]]: 0; 0–600000. [[Mapa/Perillas/split_stale_max|split_stale_max]]: 1; 1–64.
- [[Mapa/Perillas/governor_window_ms|governor_window_ms]], [[Mapa/Perillas/rescue_target_ms|rescue_target_ms]], [[Mapa/Perillas/rescue_full_hyst|rescue_full_hyst]], [[Mapa/Perillas/eta_buffer|eta_buffer]], [[Mapa/Perillas/starlink_slot_clock|starlink_slot_clock]].

## Dónde se midió
- [[013-capacidad-y-dosificacion]]: muestras de 250 ms oscilaban hasta 1,5–2× lo real; con el decaimiento tipo BBR, dos o tres de cuatro enlaces quedaban en el piso y el túnel en 40–50 %; el arranque rápido fue neutro en laboratorio.
- [[016-celda-virtual]]: un LTE fuera por el acantilado del SNR guardaba 8–21 Mbit/s; con `capacity_expire_ms = 2000` vuelve con 0,3–1,3. Al volver la señal entera eso costó (bonding 23,0 → 20,8 Mbit/s en los 10 s siguientes); al volver débil, los cortes de siempre bajaron la vieja en 1,5–2,5 s y no hubo diferencia. Queda apagada.
- [[015-modelo-de-enlace]] §9: con la pérdida por secuencia y el reloj de ranuras, Starlink marcado `lossy` pasa del 93 % del tiempo al 17 %, caudal igual.

## Choques conocidos
- C5: arranque rápido y envejecimiento suben; solo «lleno» baja, y G4 saca al enlace antes de las 2 muestras: la sobreestimación no se corrige ([[audit-pending]]). `cap_g4_episode` quedó como perilla apagada ([[015-modelo-de-enlace]] §6).
- C12: `governor_target_ms` movía cuatro lazos; ahora `capacity_sat_ms` es propio (PR #86).
- Un enlace sin carga no se vuelve a medir: lo resuelven [[Cosquilla (tickle.h)]] y `split_stale_max`.
- `capacity_expire_ms` baja lo que el arranque rápido sube: por eso cada mitad marca `full_ms` (el arranque espera 10 s después de la vuelta).

## Archivos
- `capacity.h`; tests `engine/tests/test_capacity.c`.
- `client.c` y `server.c`: `cap_tick` (después de `gov_tick`), y `cg_cap_rescue_w` en el envío.
