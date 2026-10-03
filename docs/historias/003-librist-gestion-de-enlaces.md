# 003 — libRIST: silenciado de enlaces, reparto WRR y recuperación

- **Fecha:** 2026-10-03
- **Estado:** vigente
- **Fuentes:** <https://code.videolan.org/rist/librist>, v0.2.20 (commit
  4f45ef8): `NEWS` (0.2.17–0.2.20), `src/`, `include/librist/`, `tools/`,
  `test/rist/unit/`. Licencia BSD-2-Clause, compatible con GPLv2: se puede
  reutilizar código conservando su aviso de copyright.

## TL;DR

- **Qué es libRIST:** implementa RIST (VSF TR-06-1/2/3). Es un protocolo
  emisor → receptor con ARQ (NACKs), un buffer de latencia en el receptor
  (1000 ms por defecto) y multicamino por peso. Peso 0 significa que ese
  enlace lleva copia de todo (redundancia); peso > 0 significa una parte de un
  reparto WRR (bonding). Las dos cosas se pueden mezclar.
- **Separa el reparto de la vida de la sesión.**
  - Un enlace sale rápido de la rotación: tras ~300 ms en silencio
    ("stall"), o con `rtt-drop` si su RTT suavizado pasa el techo durante 2 s.
  - Su sesión y su socket se mantienen mucho más: sin re-handshakes ni
    sockets destruidos. Es justo lo contrario que engarde Go (historia 001).
- **`rtt-drop` lleva histéresis completa:**
  - umbral de vuelta al 80 % del de caída; esperas asimétricas de 2 s para
    salir y 4 s para volver;
  - nunca silencia al último que transporta, y si todos están mal, el elegido
    es "pegajoso";
  - goteo de 1 de cada 100 paquetes para seguir midiendo, que se corta si la
    cola supera el buffer del receptor;
  - al volver, el peso sube en rampa;
  - red de seguridad: nunca se pierde un paquete si queda algún enlace usable.
- **El NEWS de 0.2.19 es un catálogo de fallos reales de bonding sobre
  celular** y de cómo se arreglaron: justo el I+D que cuesta hacer que el
  bonding "funcione bien".
- **Para cengarde:** casi todo sirve ya para la redundancia pura (Fase 2):
  silenciar o gotear enlaces que llegan siempre tarde o cuya cola crece, y no
  destruir sockets. El WRR, la reordenación y el ARQ quedan para la Fase 5.
- **`risttunnel`** existe (túnel IP sobre RIST con ARQ), pero en v0.2.20 usa
  un solo peer por sentido: no hace multienlace. El bonding está en
  `ristsender` y `rist2rist`.

## Contexto

El usuario pidió revisar libRIST como inspiración: `rtt-drop` y
`rtt-trickle`, los mecanismos que descartan o silencian enlaces malos (los que
ponen en riesgo a los sanos o los que van con retraso y desbordan buffers), y
la redundancia y el bonding WRR nativos. El bonding se deja para más adelante:
implementarlo es fácil, pero que funcione bien es una larga historia de
ensayos (Peplink, Mushroom Networks, Speedify…).

## Hallazgos

### Reparto: WRR con legs duplicados

- **Pesos:** `?weight=` en cada URL; `RIST_PEER_WEIGHT_DUPLICATE` = 0 hace que
  ese enlace reciba copia de todo (`include/librist/peer.h:51-54`).
- **Elección:** `rist_sender_send_data_balanced` (`src/udp.c:962`). Cada
  enlace tiene un crédito (`w_count`); en cada paquete se elige el que más
  crédito tiene y los créditos se recargan cuando el contador global llega a
  0. Los enlaces con peso 0 envían todos los paquetes en el mismo recorrido.
- **Enlaces silenciados o mudos:** se saltan, pero su peso se sigue
  descontando del ciclo para que los demás mantengan su proporción. Los
  silenciados reciben el goteo.
- **Red de seguridad** (`src/udp.c:1113`): si ningún enlace transportó el
  paquete, se envía por el mejor de los saltados. Uno silenciado (tarde pero
  entregable) gana a uno mudo (sin camino de vuelta), y a igualdad decide el
  menor RTT.
- `ristsender -o` acepta varias URLs separadas por comas
  (`tools/ristsender.c:665-691`); cada una es un enlace del bond.

### Señales de salud de un enlace

- **RTT:** sale de RTCP (RR/DLRR y echo) y se suaviza con una EWMA de 1/8,
  como el SRTT de TCP (`rist_peer_rtt_update`, `src/rist-private.h:913-923`).
  Arranca en `rtt-min`.
- **Silencio ("stall"):** el último paquete recibido por el enlace, de datos o
  RTCP, es más viejo que 3 intervalos de ping (100 ms por defecto, ~300 ms en
  total) (`src/rist-private.h:105-113`, `src/rist-common.c:4688-4734`). Se
  recalcula en cada tick, así que el enlace vuelve en cuanto llega algo.
  Nunca deja mudo al único enlace, y si todos están en silencio no apaga
  ninguno.
- **Vida de la sesión, separada y más larga:** el timeout es el mayor de
  `session-timeout` (2000 ms) y 2× el buffer. Además, a un peer "muerto" se le
  sigue enviando durante una ventana de buffer, porque un camino de vuelta
  callado no dice nada del de ida (`src/rist-send-grace.h`).

### `rtt-drop`: silenciado automático por RTT

| Parámetro | Por defecto | Significado |
| --- | --- | --- |
| `rtt-drop` | 0 (apagado) | techo de RTT suavizado, en ms |
| `rtt-restore` | 0 → 80 % del techo | umbral para volver |
| `rtt-drop-settle` | 2000 ms | espera antes de silenciar; la de vuelta es el doble |
| `rtt-drop-trickle` | 100 | duplicar 1 de cada N paquetes en el enlace silenciado; 0 = silencio total |

Parámetros en `include/librist/urlparam.h:62-65` y valores por defecto en
`include/librist/peer.h:45-49`.

Cómo funciona (`src/rist-rtt-mute.h`, orquestado en
`src/rist-common.c:4746-4858` con un tick de 250 ms en `:4907`):

1. **Histéresis** (`rist_rtt_mute_step`): la condición (si está silenciado,
   RTT < vuelta; si no, RTT > techo) tiene que mantenerse sin interrupción
   durante toda la espera; si se rompe una vez, el contador vuelve a cero.
2. **Estado deseado frente a aplicado:** primero corre el chequeo de silencio.
   Un enlace solo se silencia de verdad si queda otro que transporte (ni
   silenciado ni mudo).
3. **Todos mal:** el enlace que se queda transportando es pegajoso. Solo cede
   el papel si otro mide al menos 2× mejor y el actual lo ha tenido como
   mínimo una espera de caída (`rist_rtt_sole_carrier_handover`,
   `RIST_SOLE_CARRIER_MARGIN` = 2).
4. **El goteo se corta** cuando RTT/2 (estimación de un sentido) supera el
   buffer del receptor (`rist_rtt_trickle_useful`): esos paquetes llegarían
   tarde y harían daño.
5. **Vuelta en rampa:** el peso sube linealmente de ~0 al total durante la
   espera de vuelta, con un mínimo de 1 (`rist_rtt_ramped_weight`,
   `src/udp.c:954`).
6. **Visibilidad:** `rtt_muted` y `rtt_mute_events` por enlace en el JSON de
   estadísticas y en Prometheus (`src/stats.c:169-170`), y logs explícitos al
   silenciar, al volver y al elegir el último transportador.

### Recuperación (ARQ) y control de retransmisiones

- **NACKs del receptor:** se repiten cada ~1,1 × RTT (RTT acotado a
  [`rtt-min` 5 ms, `rtt-max` 500 ms]) hasta `max-retries` (20) o hasta que el
  paquete es más viejo que 1,1 × buffer. Códigos de descarte 8 ("demasiados
  reintentos") y 9 ("demasiado tarde") en `rist_process_nack`
  (`src/rist-common.c:1013-1044`).
- **A qué enlace va el NACK:** al de mayor `recovery-priority`; a igualdad, al
  de menor RTT (`src/rist-nack-select.h`).
- **Espaciado de retransmisiones en el emisor** (`rist_retry_enqueue`,
  `src/udp.c:1329`; cuentan como `bloat_skip`):
  - `congestion-control` 0: duplicados sin límite;
  - 1 (por defecto): como mucho una retransmisión del mismo número de
    secuencia por RTT;
  - 2: una cada 2 RTT.
- **Techo de bitrate:** `?bandwidth=` limita carga útil y retransmisiones
  juntas. Si la carga útil llega al techo no hay recuperación posible, y se
  avisa cada 30 s (`src/rist-bandwidth-guard.h`).
- **Límite en el receptor:** si la cola de paquetes perdidos supera
  `missing_counter_max` (derivado del ancho de banda), deja de pedir NACKs
  (`src/rist-common.c:698-709`).
- **Resto antiguo:** `buffer_bloat_active` todavía se consulta
  (`rist-common.c:698`, `:705` y `:1559`; vaciado de NACKs con códigos 5 y 6),
  pero en 0.2.20 nada le asigna valor. El antiguo "bloat-mode" es código
  muerto.

### Lecciones del NEWS 0.2.19 (bonding sobre celular)

- **Ping-pong entre dos enlaces malos:** elegir cada tick por el RTT
  instantáneo cambiaba de enlace ~1 vez por segundo, peor que quedarse con
  cualquiera de los dos. Solución: elección pegajosa.
- **Oscilación al volver:** un enlace silenciado se vacía y mide bien hasta
  que vuelve a cargar; devolverle todo su peso de golpe lo re-silenciaba
  segundos después. Solución: rampa.
- **Paquetes rancios:** un enlace con segundos de cola entrega paquetes tan
  viejos que, tras vaciarse el buffer del receptor, uno de ellos podía
  convertirse en la nueva referencia. Rebobinó 12.000 números de secuencia
  (15 s) de un golpe.
  - Solución: no anclarse a un paquete más viejo que (máximo visto − buffer);
    esperar como mucho una ventana (`src/rist-reanchor.h`).
  - Por la misma razón, el goteo se corta en enlaces con la cola más larga
    que el buffer.
- **Timeout fijo de 250 ms con re-autenticación** en cada pico de RTT de un
  enlace inestable. Solución: separar el reparto (rápido) de la vida de la
  sesión (lenta) y reanudar sin handshake.
- **Estadísticas de pérdida falsas:** contaban huecos del anillo y mostraban
  40.600 paquetes perdidos en 1 s sin ningún salto de secuencia. Solución:
  calcular la pérdida a partir de los saltos de secuencia.
- **Buffers de socket:** se piden hasta 8 MB (el kernel los limita con
  `rmem_max`) para evitar `RcvbufErrors` mientras un único hilo vacía el
  socket. Es el mismo hallazgo que en la historia 001.
- **SDES al reconectar:** se envía en cuanto se autentica el enlace; si no, el
  receptor descartaba el primer intervalo RTCP de datos en cada reconexión de
  un enlace inestable.

### Ingeniería

- **Lógica de decisión aislada:** funciones `static inline` puras en
  cabeceras mínimas (`rist-rtt-mute.h`, `rist-send-grace.h`,
  `rist-reanchor.h`, `rist-nack-select.h`, `rist-bandwidth-guard.h`), con
  tests unitarios que no necesitan sockets ni hilos (`test/rist/unit/`:
  `test_rtt_mute.c`, `test_send_grace.c`, `test_reanchor.c`,
  `test_bandwidth_guard.c`, `test_url_rtt_drop_parse.c`). Se adopta como regla
  en `CLAUDE.md`.

### risttunnel

- **Qué es** (`tools/risttunnel.c`): túnel IP sobre RIST con ARQ. Pasa el fd
  del TUN a la librería (`data_fd`).
- **Modo de un solo puerto:** ida por RTP con ARQ, vuelta por OOB sin ARQ.
- **MTU** de 1400 por defecto; activa split/merge si es menor.
- **Sin multienlace:** crea un único peer emisor y uno receptor
  (`risttunnel.c:398-470`).
- **Frente a cengarde:** RIST cambia latencia (el buffer del receptor) por
  recuperación; la redundancia de cengarde entrega sin esperar. Son
  compromisos distintos.

## Qué hacemos con esto

| Idea de libRIST | Fase en cengarde | Señal sin cabecera propia (protocolo v0) | ¿Necesita v1? |
| --- | --- | --- | --- |
| Separar reparto y vida de la sesión: no destruir sockets por errores transitorios | 1 | errores de envío, EAGAIN | no |
| Enlace mudo → fuera de la rotación, vuelve en cuanto llega algo | 2 | el receptor no recibe nada por ese enlace mientras sí recibe por otros | no |
| Silenciar enlaces que llegan tarde (equivalente a `rtt-drop`) | 2 | retardo relativo y "wins" de la tabla de dedup (receptor); profundidad de la cola local (`SIOCOUTQ`, tasa de EAGAIN) en el emisor | para medir el RTT real por enlace y avisar al otro extremo |
| Goteo 1/N en el enlace silenciado | 2 | lo decide el emisor | para medir en remoto |
| Nunca silenciar al último, elección pegajosa, red de seguridad | 2 | — | no |
| Vuelta en rampa | 2 (redundancia: reanudar la duplicación por escalones) / 5 (WRR) | — | no |
| Peso 0 = duplicar, > 0 = WRR | definir ya la configuración; WRR en la 5 | — | no para repartir; sí para reordenar bien |
| Regla anti-rancios (reanchor) | 2 (anillo de dedup) / 5 (buffer de reordenación) | contador de WireGuard (historia 002) | no |
| ARQ con NACK y buffer de latencia | 5, como modo opcional ("redundancia barata": 1–2 enlaces + recuperación) | — | sí |
| Funciones puras + tests unitarios | desde la 1 | — | no |

**Estados propuestos para un enlace en cengarde:** ACTIVO ⇄ MUDO (silencio);
ACTIVO → SILENCIADO (llega tarde o su cola crece; con goteo) → EN RAMPA →
ACTIVO. Nunca se deja sin transportador. Nombres de parámetros alineados con
libRIST para que sean familiares: `drop`, `restore`, `settle` y `trickle`.

## Pendiente

- Leer los tests unitarios de libRIST y reutilizar sus casos para las
  funciones equivalentes de cengarde.
- Definir con precisión las señales en protocolo v0 y medirlas en el
  laboratorio (hace falta `netem` para retardo y pérdida).
- Decidir si la Fase 5 ofrece un modo ARQ al estilo RIST además de bonding y
  FEC.

## Cambios

- 2026-10-03: creada (libRIST v0.2.20, commit 4f45ef8).
