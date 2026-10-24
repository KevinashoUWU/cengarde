# 022 — Varios Starlink: correlación entre platos, el motor con varios `starlink` y la topología de campo

- **Fecha:** 2026-10-24
- **Estado:** vigente
- **Fuentes:** `bench/multisl.py`, `bench/multisl_test.py`, casos `msl1`,
  `msl2`, `msl3`, `msl2c`, `msl3c` de `bench/lab.d/bond.sh`; motor
  `engine/src/dominant.h`, `slot.h`, `spare.h` (`cg_spare_mask`),
  `client.c`, `server.c`; tests `engine/tests/test_dominant.c`,
  `test_slot.c`, `test_spare.c`. Corridas con `bench/labq.py --lab-offset 8
  -j 6` en el PC del banco, planes, salidas y análisis en
  `~/.claude/jobs/1c68aec3/tmp/msl-lab/` (`plans/`, `q1`, `dom`, `q3s`,
  `q3sf`, `q3c`, `q3t`, `keep-*`) y `~/.claude/jobs/1c68aec3/tmp/msl-an/an.py`
  (scratch de la sesión; se pierde al reiniciar). Binarios: `cg-base`
  (master `6544c40`), `cg-fix1` (pares y bordes) y `cg-fix2` (más
  `cg_spare_mask`). Calibración: historia 017 (pérdida de Starlink 0,3–1,3 %,
  0,62 % en el día, microcortes de 20–40 s al 50–77 %), historia 015 (las
  ranuras), `~/campo/2026-10-22/nos-tarde/rec` (la pérdida por segundo).

## TL;DR

- **Un plato más solo tapa la pérdida que no comparte con los otros**
  [laboratorio, n = 6 por celda, 60 s, flujo de 9 Mbit/s]. Con la pérdida de
  cada plato independiente (0,6 %), dos platos en redundancia dejan el flujo
  en **0,001–0,004 %** (uno solo: 0,54–0,59 %, p < 0,01) y tres en 0,000 %.
  Con una parte ρ compartida (mismo PoP, mismo camino al VPS), el flujo
  pierde **≈ ρ × la pérdida de un plato** con dos o con tres: ρ = 0,5 →
  0,25 % (2) y 0,23 % (3); ρ = 1 → 0,55 % y 0,48 %, igual que un plato
  (p 0,42 y 1,0). **El tercer plato no suma nada frente al segundo** salvo
  en pérdida independiente alta (bordes con probabilidad 0,5: 0,063 → 0,000 %;
  microcorte en todos: 15,5 → 10,3 %).
- **Los bordes de ranura** (:12/:27/:42/:57) son de todos los platos a la
  vez: si cada plato se para en cada borde, dos o tres platos pierden lo
  mismo que uno (0,17 % contra 0,19 %). Un microcorte en un plato solo no
  cuesta nada con dos (0,000 %); en todos a la vez, lo mismo que la pérdida
  independiente dentro del corte.
- **Con celulares** (perfil de campo: 0,25 % propia + 0,25 % en apagones con
  ρ = 0,5 + bordes alineados), la redundancia pasa de **0,23–0,29 %** (2–3
  platos) a **0,10 %** con dos celulares (p < 0,01): lo compartido entre
  platos solo lo tapa un enlace de otro tipo.
- **Tres errores del motor con varios `starlink`, corregidos con tests:**
  1. `redund_spare` (`mix`, `rlc`) **perdía 4–7 % del flujo** con dos platos:
     en los primeros segundos la ventana de copias dejaba fuera al primer
     enlace (su capacidad, `lossy` y sin sondeo, arranca baja) y la
     asignación sacaba la copia del otro; el paquete no salía por ningún
     enlace. Ahora el primero lleva siempre (`cg_spare_mask`): 3,7–5,8 % →
     0,1–0,3 %, como las copias completas (p 0,1–0,9).
  2. **El enlace dominante iba y venía** entre platos: el que iba adelante en
     retardo base sumaba al otro como «más lento» y no dominaba; el de atrás
     sí. Los otros `starlink` son ahora **pares** y el candidato tiene
     histéresis (5/4). Bajada con reparto, 2 platos + 2 celulares: **28,7 →
     82,1 Mbit/s** (p 0,007), 3 platos: 38,1 → 103,4 (p < 0,001), como la
     redundancia (75,5 y 107,3).
  3. Con `starlink_edge_copies` y rescate, fuera del borde todo iba como
     DATA repartido por el primer plato y los demás quedaban ociosos: ahora
     una copia por cada plato apto (`cg_slot_pick`, `cg_slot_copy_mask`).
- **Lo que no es un error:** `pace_peers_ms` hace que cada `starlink` dosifique
  las copias del otro plato, pero con retardos parecidos no recorta nada (el
  segundo plato lleva el 100 % de las copias con 60 y con 250 ms).
  `starlink_edge_copies` empeora con varios platos (0,43–0,85 % contra
  0,10–0,29 %): copia solo por los platos «aptos» (sin pérdida en 2 s), y un
  Starlink casi nunca lo está; no usarlo.
- **Para Manto Verde:** dos platos y al menos un celular, en redundancia
  (`pace_peers_ms = 250`, lo probado en campo); un tercer plato solo si la
  medición de dos platos en el banco da ρ bajo. Antes de ir, medir ρ y la
  alineación de bordes con dos platos (plan al final). `rlc` iguala a las
  copias completas en pérdida con varios platos (con el arreglo 1) y gasta
  menos celular solo cuando hay un plato: con varios platos, el segundo
  plato ya lleva copias completas y no hay nada que ahorrar.

## Contexto

Para la visita a Minera Manto Verde el usuario piensa llevar 2, 3 o más
platos de Starlink, con o sin celulares. Hasta ahora todo el modelo de
enlace (historia 015) y la redundancia de campo (historia 017) suponían un
Starlink y celulares. Preguntas: ¿cuánto se parecen las pérdidas de dos
platos en el mismo lugar? ¿qué hace el motor con varios enlaces `type =
starlink` (preset, dominante, ranuras, `redund_spare`)? ¿cuántos platos
ayudan? ¿cómo se monta?

### Lo que dice el campo para calibrar

- Starlink pierde **sin hacer cola** 0,3–1,3 % (0,62 % el 22-10), y
  microcortes de 20–40 s al 50–77 % (historia 017). La pérdida aparece
  **después del plato**, más hacia Vultr (ping 0,12 %) que hacia Cloudflare
  (0,05 %): una parte vive en el camino PoP → VPS, que dos platos del mismo
  lugar comparten casi seguro; otra en la celda de Starlink (los platos de un
  mismo sitio están en la misma celda y el mismo haz), y otra en cada plato
  (obstrucción, su propio enlace). Cuánto pesa cada una no se puede saber
  con un plato: es ρ, y el plan de medición la busca.
- Por segundo (registrador de «nostalgica», 22-10, 4,6 h de subida con
  carga): 37 % de los segundos sin pérdida, la pérdida en ráfagas cortas
  (el 97 % de lo perdido en segundos con más de 1 %), autocorrelación de un
  segundo al siguiente 0,2 [la tasa absoluta de ese cálculo, con el
  contador `server_view` del VPS, da 3 % y no cuadra con el 0,62 % de la
  historia 017: solo se usa la forma].
- Los bordes de ranura: los mismos segundos (:12/:27/:42/:57) en todos los
  platos publicados, así que de la constelación, no del sitio (historia
  015); ~45 ms parados por borde (25 perdidos).

## El laboratorio: `multisl.py` y los casos `msl*`

Cada plato es un enlace como `starsat` en el tiempo (su propio sorteo de
nivel, deriva y tasa por ranura: 20/30/40 Mbit/s de subida y 100/150/200 de
bajada, 15 ms de cola, 15/16/17 ms de un sentido), y la pérdida se arma con
cuatro piezas (`bench/multisl.py`, «Varios platos» en `bench/README.md`):

| Pieza | Variable | Modelo |
|---|---|---|
| (a) propia | `MSL_IND` | pérdida aleatoria por paquete en cada plato (netem) |
| (b) compartida | `MSL_EP`, `MSL_RHO` | apagones de 10–60 ms que suman `MSL_EP` % del tiempo de cada plato; una parte ρ cae en todos a la vez |
| (c) bordes | `MSL_EDGE` | en cada borde del reloj de pared, cada plato se para (25 ms perdidos, 20 retenidos 55 ms) con esa probabilidad, sorteada por plato |
| (d) microcorte | `MSL_CUT` | 60 % perdido el tercio del medio, en el primer plato o en todos |

Los apagones compartidos empiezan a la vez en todos los platos (dos `tc
-batch`, uno por netns): las copias de un paquete los cruzan juntas, como
cruzarían un salto congestionado del camino al VPS. Cada plato pierde lo
mismo con cualquier ρ; solo cambia el solapamiento (`multisl.py joint` lo
calcula sin correr nada, y es lo que se mide abajo). Los celulares de
`msl2c` y `msl3c`: 10 Mbit/s, 30 ms, 1 s de cola y 0,1 % de pérdida; 5
Mbit/s, 35 ms, 2 s de cola (WOM sobre su codo).

Carga: subida, un flujo de 9 Mbit/s en datagramas de 1200 B (`udpgen`,
940 paquetes/s) más un TCP cubic de 1 Mbit/s (`BOND_RATE=1M`) o, en la
tanda de TCP, cubic sin tope. 60 s por corrida, `BOND_NETEM_INFLIGHT=1`,
`BOND_SAMPLE=1`. «Flujo perdido» es el del `udpgen` (enviados contra
recibidos únicos): la pérdida en todos los enlaces. `lost_all` del motor se
anota cuando la secuencia sale de la ventana de 32 768 (~30 s a este ritmo,
historia 017): en corridas de 60 s llega tarde, por eso no se usa para la
tabla. p: Mann-Whitney a dos colas.

## Hallazgos

### 1. Cuántos platos ayudan, según cuánto comparten

Redundancia de campo (`pace_peers_ms = 250`), solo platos, n = 6 por celda
(4–5 en `msl1` con microcorte: con un solo enlace al 60 % el TCP o el ping
fallan y la corrida no cuenta). Mediana del flujo perdido (%); p contra un
plato; «Programa» es lo que pierde el programa de `multisl.py` en todos los
platos a la vez (`joint`, 1 h), lo esperado si el motor no pierde nada
propio.

| Montaje | Un plato | Dos platos | Tres platos | Programa (2 / 3) |
|---|---:|---:|---:|---:|
| (a) 0,6 % propia | 0,590 | **0,004** (p 0,005) | **0,000** (p 0,004) | 0,004 / 0,000 |
| (b) 0,6 % en apagones, ρ = 0 | 0,543 | **0,001** (p 0,004) | **0,000** (p 0,003) | 0,002 / 0,000 |
| (b) ρ = 0,5 | 0,610 | 0,247 (p 0,08) | 0,226 (p 0,08) | 0,317 / 0,317 |
| (b) ρ = 1 | 0,508 | 0,546 (p 0,42) | 0,479 (p 1,0) | 0,622 / 0,622 |
| (c) bordes, cada plato en cada borde | 0,188 | 0,165 (p 0,04) | 0,167 (p 0,02) | 0,167 / 0,167 |
| (c) bordes, cada plato con p = 0,5 | 0,112 | 0,063 (p 0,02) | **0,000** (p 0,007) | 0,039 / 0,017 |
| (d) microcorte en el primer plato | 20,2 | **0,000** (p 0,009) | **0,000** (p 0,009) | 0 / 0 |
| (d) microcorte en todos | 20,0 | 15,5 (p 0,01) | 10,3 (p 0,01) | — |

Tres platos contra dos (p): (a) 0,007; ρ = 0,5: 1,0; ρ = 1: 0,49; bordes
alineados: 1,0; bordes con p = 0,5: 0,15; microcorte en todos: 0,002.

- **El motor no agrega pérdida**: el flujo queda en lo que el programa pierde
  en todos los platos a la vez (algo por debajo con ρ ≥ 0,5: las copias por
  los platos llegan con 1–2 ms de diferencia y una fracción esquiva el borde
  de un apagón).
- **Cuántos platos sirven:** con lo independiente, dos ya bajan la pérdida
  a ~p² (de 0,6 % a 0,004 %), y un tercero solo se nota cuando lo
  independiente es grande (bordes al 50 %, microcorte simultáneo con 60 %
  de pérdida aleatoria dentro). Con lo compartido, **ningún número de platos
  baja de ρ·p**: con ρ = 0,5, el segundo plato saca la mitad independiente
  y el tercero no saca nada (0,247 → 0,226, p 1,0).
- **Los bordes alineados** son lo compartido por excelencia: si todos los
  platos se paran en cada borde, la redundancia entre platos no gana nada
  (0,188 → 0,165). Que en campo se alineen o no (la constelación cambia los
  satélites de todos a la vez, pero cada plato puede o no perder en ese
  cambio) es lo segundo que hay que medir.
- La pérdida propia de cada plato sale 0,1–0,3 puntos sobre lo programado:
  desbordes de la cola de 15 ms en las ranuras de 20 Mbit/s con 10,7 Mbit/s
  de carga (lo mismo con uno o tres platos).

### 2. El motor con varios enlaces `starlink`

Revisado pieza por pieza con dos y tres platos (unidad y laboratorio):

**`redund_spare` perdía paquetes enteros (corregido).** En `send_links`,
`spare_masks` partía de la ventana de copias de `eta.h`, que toma como
primero al enlace que *según su propia cuenta* llega antes. Con dos platos
parecidos, el primero de `spare.h` (el de menor base) arrancaba con la
capacidad casi en cero (un `starlink` con pérdida no se sondea, `lossy`) y
su cola estimada lo dejaba fuera de la ventana; el otro plato estaba dentro,
pero su asignación (que arranca en el mínimo) le sacaba la copia. El
paquete no salía por ningún enlace: en los primeros ~4 s, l1 mandó 1 672 de
3 688 paquetes y l2 60 (muestras de `msl2`, `rlc`), ~2 000 paquetes de
cada corrida. Con un solo Starlink y un celular ahogado (los casos
`protect` de las historias 020 y 021) no pasa: el celular nunca parece
llegar antes. Arreglo: `cg_spare_mask` (`spare.h`) pone siempre al primero
en la máscara (y suma el paquete a su cola en `eta.h`); la ventana solo
recorta copias.

| Caso (n = 6) | copias 250 ms | `mix` antes | `mix` después | `rlc` antes | `rlc` después |
|---|---:|---:|---:|---:|---:|
| `msl2` | 0,227 | 3,680 (p 0,002) | 0,300 (p 0,31) | 4,012 (p 0,002) | 0,324 (p 0,09) |
| `msl3` | 0,291 | 3,775 (p 0,002) | 0,270 (p 0,87) | 3,762 (p 0,002) | 0,295 (p 0,59) |
| `msl2c` | 0,104 | 5,787 (p 0,002) | 0,105 (p 0,87) | 5,534 (p 0,002) | 0,317 (p 0,04) |
| `msl3c` | 0,105 | 5,848 (p 0,002) | 0,217 (p 0,11) | 5,303 (p 0,002) | 0,094 (p 1,0) |

(flujo perdido %, perfil de campo; p contra las copias de 250 ms).

**El enlace dominante iba y venía (corregido).** `cg_dom_tick` elegía como
candidato al enlace con `dominant_pct` que más entregaba y sumaba como «más
lento» a todo enlace con 2 ms o más de base detrás, **otro plato
incluido**. Con dos platos, cada ranura sortea su base (±2,5 ms de un
sentido): cuando el candidato iba adelante, sumaba al otro plato y no
dominaba; cuando iba atrás, el otro no contaba y dominaba. Y cada cambio de
candidato borraba la ventana de 10 s y la histéresis. En la bajada con
reparto (`msl2c`, el VPS reparte), el dominante cambió 11 veces por minuto
y estuvo a un lado el 23 % del tiempo; el 61 % de las muestras decía
`rate` sin nada fuera (un plato sumado). Ahora los otros enlaces con
`dominant_pct` son **pares** (ni fuera ni en la suma) y el candidato cambia
solo por un par que entrega más de 5/4 de lo suyo (`CG_DOM_SWITCH_NUM/DEN`):

| Bajada, reparto, TCP cubic (n = 8) | antes | después | redundancia |
|---|---:|---:|---:|
| `msl2c`, Mbit/s | 28,7 | **82,1** (p 0,007) | 75,5 |
| `msl3c`, Mbit/s | 38,1 | **103,4** (p < 0,001) | 107,3 |
| cambios del candidato por minuto (`msl2c` / `msl3c`) | 11 / 7 | 0 / 1 | — |
| dominante a un lado, % del tiempo | 23 / 32 | 100 / 100 | — |

Queda: con el dominante a un lado el reparto se aparta **entero**, así que
tampoco reparte entre los platos (la bajada da lo de un plato, como la
redundancia). Repartir solo entre los pares, con los celulares a copias a
tiempo, es lo siguiente para la bajada; la subida de campo va en
redundancia y no lo usa. El ping de 1200 B en el reparto con el dominante a
un lado queda en ~330 ms de p99 contra 53 de la redundancia (la variante
`split` del laboratorio: `pace_peers_ms` 60 y `reorder_max_wait_ms` 400):
sin mirar todavía.

**Las copias en el borde con rescate dejaban platos ociosos (corregido,
sin medir en laboratorio).** Con `starlink_edge_copies` y un par que
reordena y guarda (`rescue_kb`), fuera de la ventana del borde un paquete
iba como DATA repartido por el **primer** plato apto (`cg_slot_pick`,
`__builtin_ctz`): con dos platos, el segundo no llevaba nada y lo que
perdía el primero dependía del rescate. Ahora, con dos platos aptos o
más, una copia en cada uno (`cg_slot_pick` da −1 y `cg_slot_copy_mask`
recorta las copias a esos platos, en el cliente y el servidor); con uno,
como antes. Sin `rescue_kb` (lo de los casos de abajo) ya copiaba por
todos los platos aptos.

**Lo que se revisó y no es un error:**

- `cg_link_pace_windows`: un `starlink` dosifica las copias de **todos** los
  demás enlaces, otro `starlink` incluido (ventana de 60 ms, o
  `pace_peers_ms`). Con platos al mismo PoP las llegadas esperadas están a
  pocos ms, y el segundo plato llevó el 100 % de las copias tanto con 60
  como con 250 ms (bytes de respaldo por byte útil 1,00 en `msl2` y 2,00 en
  `msl3`). Con un plato a otro PoP (decenas de ms detrás) sí recortaría;
  queda anotado.
- `slot.h` y la pérdida por secuencia van por enlace: cada plato cuenta sus
  bordes sin mirar a los otros. Todos tienen los mismos bordes, que es lo
  que el reloj espera.
- El primer enlace de `redund_spare` no se dio vuelta entre platos (0
  cambios por minuto en todas las corridas): los 5 ms de histéresis
  alcanzan para las bases por ranura.
- `starlink_edge_copies` **empeora** con varios platos (abajo): copia solo
  por los platos «aptos» (con reporte en 300 ms y **sin pérdida en 2 s**), y
  un Starlink que pierde 0,6 % casi nunca pasa 2 s sin perder: queda uno, o
  ninguno y todo va por todos. No es un error del reloj; es una perilla
  pensada para un Starlink y celulares caros.

### 3. Escenarios: flujo de 9 Mbit/s, TCP, microcorte

Perfil de campo en todos: cada plato 0,25 % de pérdida propia, 0,25 % en
apagones con ρ = 0,5, y todos los platos parados en cada borde (lo que
se espera de un mismo sitio y PoP, sin medir: ver el plan). Brazos (binario
`cg-fix2`, salvo los de la sección 2): `full` = redundancia con
`pace_peers_ms = 250` (lo de campo); `full60` = redundancia con el preset
(60 ms); `mix` y `rlc` = `redund_spare` con el servidor en `fec = yes` (y
`pace_peers_ms = 250`); `split` = la variante `split` de `bond.sh` (reparto
forzado, sin rescate); `edge` = `full` con `starlink_edge_copies`.
«Respaldo/útil»: lo que mandaron todos los enlaces menos el que más mandó,
sobre lo de ese (en `split` mide el reparto, no un respaldo); «Celular/útil»,
lo de los celulares. p contra `full`.

**Flujo de 9 Mbit/s + TCP de 1 Mbit/s, subida** (60 s):

| Caso | Brazo | n | Flujo perdido % | p99 flujo ms | Ping 1200 B p99 ms | Respaldo/útil | Celular/útil |
|---|---|---:|---:|---:|---:|---:|---:|
| `msl2` | `full` | 6 | 0,227 | 25 | 40 | 1,00 | 0,00 |
| `msl2` | `full60` | 6 | 0,255 (p 0,937) | 26 (p 0,818) | 42 (p 0,873) | 1,00 (p 0,003) | 0,00 |
| `msl2` | `mix` | 6 | 0,300 (p 0,310) | 24 (p 1,000) | 41 (p 0,937) | 0,91 (p 0,003) | 0,00 |
| `msl2` | `rlc` | 6 | 0,324 (p 0,092) | 25 (p 0,937) | 39 (p 0,818) | 0,78 (p 0,003) | 0,00 |
| `msl2` | `split` | 6 | 0,569 (p 0,002) | 19 (p 0,009) | 38 (p 0,485) | 1,00 (p 0,003) | 0,00 |
| `msl2` | `edge` | 6 | 0,434 (p 0,009) | 23 (p 0,589) | 39 (p 0,937) | 0,91 (p 0,003) | 0,00 |
| `msl3` | `full` | 6 | 0,291 | 22 | 37 | 2,00 | 0,00 |
| `msl3` | `full60` | 6 | 0,261 (p 0,748) | 22 (p 0,699) | 38 (p 1,000) | 2,00 (p 0,005) | 0,00 |
| `msl3` | `mix` | 6 | 0,270 (p 0,873) | 23 (p 0,818) | 39 (p 0,589) | 1,73 (p 0,005) | 0,00 |
| `msl3` | `rlc` | 6 | 0,295 (p 0,589) | 26 (p 0,394) | 40 (p 0,240) | 1,48 (p 0,005) | 0,00 |
| `msl3` | `split` | 6 | 0,618 (p 0,002) | 20 (p 0,065) | 45 (p 0,026) | 2,00 (p 0,044) | 0,00 |
| `msl3` | `edge` | 6 | 0,580 (p 0,002) | 23 (p 0,699) | 40 (p 0,699) | 1,84 (p 0,005) | 0,00 |
| `msl2c` | `full` | 6 | 0,104 | 45 | 48 | 1,77 | 0,77 |
| `msl2c` | `full60` | 6 | 0,038 (p 0,026) | 76 (p 0,004) | 51 (p 0,589) | 2,24 (p 0,002) | 1,24 |
| `msl2c` | `mix` | 6 | 0,105 (p 0,872) | 29 (p 0,002) | 44 (p 0,009) | 1,66 (p 0,394) | 0,94 |
| `msl2c` | `rlc` | 6 | 0,317 (p 0,041) | 29 (p 0,002) | 48 (p 0,520) | 0,14 (p 0,026) | 0,14 |
| `msl2c` | `split` | 6 | 0,309 (p 0,002) | 45 (p 1,000) | 81 (p 0,378) | 3,00 (p 0,002) | 2,00 |
| `msl2c` | `edge` | 6 | 0,770 (p 0,002) | 26 (p 0,002) | 45 (p 0,132) | 1,93 (p 0,065) | 0,97 |
| `msl3c` | `full` | 6 | 0,105 | 39 | 54 | 2,68 | 0,69 |
| `msl3c` | `full60` | 6 | 0,013 (p 0,041) | 53 (p 0,093) | 57 (p 0,937) | 3,21 (p 0,002) | 1,23 |
| `msl3c` | `mix` | 6 | 0,217 (p 0,109) | 28 (p 0,002) | 44 (p 0,065) | 2,30 (p 0,002) | 0,92 |
| `msl3c` | `rlc` | 6 | 0,094 (p 1,000) | 28 (p 0,002) | 42 (p 0,002) | 2,39 (p 0,240) | 0,93 |
| `msl3c` | `split` | 6 | 0,403 (p 0,002) | 42 (p 0,065) | 77 (p 0,015) | 4,00 (p 0,002) | 2,00 |
| `msl3c` | `edge` | 6 | 0,848 (p 0,002) | 24 (p 0,002) | 41 (p 0,009) | 2,85 (p 0,132) | 0,93 |

**Lo mismo con un microcorte en el primer plato** (60 % perdido 20 s):

| Caso | Brazo | n | Flujo perdido % | p99 flujo ms | Ping 1200 B p99 ms | Respaldo/útil | Celular/útil |
|---|---|---:|---:|---:|---:|---:|---:|
| `msl2` | `full` | 6 | 0,331 | 22 | 38 | 0,94 | 0,00 |
| `msl2` | `mix` | 6 | 0,350 (p 0,485) | 23 (p 0,699) | 38 (p 0,748) | 0,86 (p 0,026) | 0,00 |
| `msl2` | `rlc` | 6 | 0,596 (p 0,132) | 23 (p 0,589) | 40 (p 0,818) | 0,95 (p 0,699) | 0,00 |
| `msl2` | `split` | 6 | 3,933 (p 0,002) | 23 (p 0,818) | 44 (p 0,092) | 0,82 (p 0,002) | 0,00 |
| `msl2` | `edge` | 6 | 0,963 (p 0,002) | 23 (p 0,818) | 39 (p 0,630) | 0,87 (p 0,132) | 0,00 |
| `msl3` | `full` | 6 | 0,259 | 22 | 38 | 1,93 | 0,00 |
| `msl3` | `mix` | 6 | 0,314 (p 0,310) | 24 (p 0,180) | 41 (p 0,149) | 1,73 (p 0,002) | 0,00 |
| `msl3` | `rlc` | 6 | 0,302 (p 0,394) | 26 (p 0,132) | 40 (p 0,199) | 1,73 (p 0,002) | 0,00 |
| `msl3` | `split` | 6 | 2,760 (p 0,002) | 27 (p 0,002) | 50 (p 0,002) | 1,77 (p 0,002) | 0,00 |
| `msl3` | `edge` | 6 | 1,020 (p 0,002) | 24 (p 0,818) | 40 (p 0,180) | 1,88 (p 0,002) | 0,00 |
| `msl2c` | `full` | 6 | 0,093 | 48 | 59 | 1,72 | 0,81 |
| `msl2c` | `mix` | 6 | 1,281 (p 0,002) | 33 (p 0,002) | 62 (p 0,630) | 1,50 (p 0,394) | 0,83 |
| `msl2c` | `rlc` | 6 | 0,194 (p 0,240) | 30 (p 0,002) | 46 (p 0,128) | 1,74 (p 1,000) | 0,98 |
| `msl2c` | `split` | 6 | 1,890 (p 0,002) | 46 (p 0,240) | 86 (p 0,005) | 2,79 (p 0,002) | 2,00 |
| `msl2c` | `edge` | 6 | 1,615 (p 0,002) | 25 (p 0,002) | 46 (p 0,132) | 1,83 (p 0,310) | 0,91 |
| `msl3c` | `full` | 6 | 0,110 | 41 | 58 | 2,63 | 0,73 |
| `msl3c` | `mix` | 6 | 0,555 (p 0,229) | 29 (p 0,002) | 46 (p 0,015) | 2,68 (p 0,818) | 1,01 |
| `msl3c` | `rlc` | 6 | 0,723 (p 0,077) | 31 (p 0,002) | 46 (p 0,423) | 2,61 (p 1,000) | 0,98 |
| `msl3c` | `split` | 6 | 1,555 (p 0,005) | 43 (p 0,394) | 81 (p 0,002) | 3,81 (p 0,002) | 2,00 |
| `msl3c` | `edge` | 6 | 1,623 (p 0,005) | 23 (p 0,002) | 44 (p 0,002) | 2,68 (p 0,394) | 0,86 |

**TCP cubic sin tope, subida** (sin flujo):

| Caso | Brazo | n | TCP Mbit/s | Retrans. | Ping 1200 B p99 ms | Respaldo/útil | Celular/útil |
|---|---|---:|---:|---:|---:|---:|---:|
| `msl2` | `full` | 6 | 24,6 | 265 | 49 | 1,00 | 0,00 |
| `msl2` | `mix` | 6 | 24,2 (p 0,589) | 273 (p 0,699) | 49 (p 0,937) | 0,94 (p 0,002) | 0,00 |
| `msl2` | `rlc` | 6 | 24,2 (p 0,818) | 361 (p 0,065) | 50 (p 1,000) | 0,90 (p 0,002) | 0,00 |
| `msl2` | `split` | 6 | 7,9 (p 0,002) | 212 (p 0,132) | 41 (p 0,015) | 1,00 (p 0,065) | 0,00 |
| `msl3` | `full` | 6 | 28,2 | 298 | 49 | 2,00 | 0,00 |
| `msl3` | `mix` | 6 | 26,8 (p 0,485) | 304 (p 0,936) | 51 (p 0,747) | 1,91 (p 0,002) | 0,00 |
| `msl3` | `rlc` | 6 | 26,8 (p 0,310) | 304 (p 0,936) | 49 (p 0,810) | 1,88 (p 0,002) | 0,00 |
| `msl3` | `split` | 6 | 7,8 (p 0,002) | 252 (p 0,470) | 48 (p 1,000) | 2,00 (p 0,002) | 0,00 |
| `msl2c` | `full` | 6 | 24,4 | 429 | 55 | 1,36 | 0,36 |
| `msl2c` | `mix` | 6 | 24,8 (p 1,000) | 304 (p 0,180) | 50 (p 0,065) | 1,10 (p 0,041) | 0,35 |
| `msl2c` | `rlc` | 6 | 24,1 (p 1,000) | 305 (p 0,394) | 51 (p 0,310) | 1,30 (p 0,132) | 0,39 |
| `msl2c` | `split` | 6 | 6,2 (p 0,005) | 92 (p 0,002) | 262 (p 0,002) | 2,44 (p 0,394) | 1,91 |
| `msl3c` | `full` | 6 | 26,4 | 413 | 53 | 2,38 | 0,38 |
| `msl3c` | `mix` | 6 | 24,6 (p 0,699) | 294 (p 0,093) | 51 (p 0,199) | 1,68 (p 0,002) | 0,35 |
| `msl3c` | `rlc` | 6 | 26,9 (p 1,000) | 344 (p 0,078) | 52 (p 0,310) | 2,12 (p 0,004) | 0,39 |
| `msl3c` | `split` | 6 | 5,8 (p 0,002) | 120 (p 0,002) | 77 (p 0,002) | 4,00 (p 0,002) | 2,00 |

- **Redundancia completa** es lo mejor en pérdida en todos los montajes: con
  platos solos queda en lo compartido (0,23–0,29 %); con celulares, en
  0,10 %, y el microcorte de un plato no se nota (0,09–0,33 %).
- `full60` pierde algo menos que `full` con celulares (0,038 contra 0,104 %,
  p 0,03; 0,013 contra 0,105, p 0,04) a costa de más bytes por celular y del
  doble de p99 del flujo; en el campo pasó lo contrario (historia 017: con
  60 ms WOM se quedaba sin copias porque su capacidad estimada era baja). No
  cambiar lo de campo por esto.
- `mix` y `rlc` (con el arreglo) empatan con `full` en pérdida sin
  microcorte (p ≥ 0,1, salvo `rlc` en `msl2c`, 0,317 %, p 0,04) y bajan el
  p99 del flujo (28–29 contra 39–45 ms con celulares). **Ahorran poco:** el
  segundo plato no tiene cola y su asignación crece hasta copiar todo. Y
  `rlc` tiene una trampa: en `msl2c` el segundo plato quedó **ocioso** en 4
  de 6 corridas (0–1 % de los bytes del primero: solo reparaciones), porque
  su capacidad nunca se mide si no lleva nada y su asignación no crece sin
  uso; ahí la pérdida sube a 0,3 %. En el microcorte, peor que `full`
  (`mix` 1,28 % en `msl2c`, p 0,002; `rlc` 0,72 % en `msl3c`, p 0,08): el
  primero se queda de primero durante el microcorte (`cg_spare_keep`, que con
  un solo Starlink evita pasar al celular ahogado) en vez de pasar al otro
  plato, limpio.
- **Bonding (`split`) no sirve para la subida con platos que pierden:** cada
  plato pierde lo suyo sin copia, y TCP cae a 6–8 Mbit/s contra 24–28 de la
  redundancia (p ≤ 0,005); el flujo pierde 0,3–0,6 % (sin rescate, que esta
  variante no trae). Para la bajada con reparto, ver el dominante (sección 2).
- **`edge`**: 0,43–0,85 % contra 0,10–0,29 % (p ≤ 0,01): no usarlo con
  varios platos.
- El TCP sin tope va a ~25–28 Mbit/s en redundancia, con o sin celulares:
  lo limita el plato más rápido de cada ranura (20–40 Mbit/s); la
  redundancia no suma capacidad.

## Qué hacemos con esto

- **Para Manto Verde:** redundancia con `pace_peers_ms = 250` (lo probado en
  campo), todos los platos `type = starlink`, **dos platos y al menos un
  celular**. Un tercer plato solo si la medición en el banco da ρ bajo (y
  bordes no alineados): con lo compartido, el tercero no suma. Sin
  `starlink_edge_copies` ni `split` en la subida.
- `rlc` con varios platos: solo con el arreglo de este PR (`cg_spare_mask`;
  el binario de campo `7474be9` no tiene `redund_spare`) y sabiendo que con
  varios platos ahorra poco y que en un microcorte de un plato rinde menos
  que las copias. Para el evento, las copias completas.
- Los arreglos del motor (sección 2) entran en este PR con sus tests y las
  notas del mapa (`Enlace dominante`, `Ranuras de Starlink`, `Lo que
  sobra`).

## La topología de campo: receta

Lo que el usuario planea: un switch L3 gestionable, la LAN de cada plato sin
etiqueta en su propia VLAN (el router del plato da DHCP en su LAN; el switch
solo etiqueta), todas las VLAN troncalizadas al NUC. Es lo mismo que hoy con
el cpe1 (Entel y WOM en VLAN 10 y 20, historia 017), con platos en vez de
módems.

### El switch

- Un puerto de acceso por plato (sin etiqueta) en su VLAN: por ejemplo
  101, 102 y 103 para los platos 1, 2 y 3 (el número de VLAN = el número del
  plato, y la misma etiqueta en el cable, el puerto y el nombre del enlace).
  Los celulares como hoy (10, 20...).
- El puerto del NUC en troncal, **todas las VLAN etiquetadas** y la nativa
  sin uso (que nada sin etiqueta llegue al NUC por error).
- El switch **no enruta** esas VLAN: sin interfaz IP (SVI) en ellas. Si el
  switch tuviera una dirección en la VLAN de un plato, el DHCP del plato le
  daría una y el switch podría responder ARP por la puerta de enlace
  equivocada. Su gestión, en otra VLAN.
- Puertos de borde con *portfast*/*edge* (sin esperar al STP al enchufar un
  plato) y sin bucles: cada router de plato en un solo puerto.

### Los platos

- **Modo normal** (el router de Starlink hace NAT): cada plato da
  `192.168.1.0/24` con puerta `192.168.1.1`, **la misma red en todas las
  VLAN**. **Modo bypass** (el router no hace NAT; en los Gen2/Gen3 se activa
  en la app): el NUC recibe directo la dirección CGNAT del plato
  (`100.64.0.0/10`, puerta `100.64.0.1`), también la misma red en todos.
  Bypass quita una capa de NAT y el wifi del router; cualquiera de los dos
  funciona con lo de abajo [sin probar con dos platos: es lo primero del
  plan de medición].
- La gestión del plato (`192.168.100.1`, la que lee `dish-rec`) es la misma
  dirección en todos: ver «pitfalls».
- Wifi de los routers de los platos apagado si no se usa (en modo normal
  compiten por canal y no aportan).

### OpenWrt en el NUC

Un dispositivo 802.1q por VLAN, cada uno con su cliente DHCP y una MAC
propia (las subinterfaces VLAN heredan la MAC del puerto: con la misma MAC,
dos routers de plato le pueden dar al NUC **la misma IP** en las dos VLAN, y
el NUC queda con dos interfaces de igual dirección):

```sh
# eth1 = el puerto del troncal; platos en 101..103 (ajusta nombres e IDs)
n=1
for spec in sl1:101 sl2:102 sl3:103; do                 # nombre:vlan
	name=${spec%%:*} vid=${spec#*:}
	uci set network.${name}_dev=device
	uci set network.${name}_dev.type='8021q'
	uci set network.${name}_dev.ifname='eth1'
	uci set network.${name}_dev.vid="$vid"
	uci set network.${name}_dev.name="eth1.$vid"
	uci set network.${name}_dev.macaddr="02:00:5e:10:01:0$n"   # localmente administrada, una por VLAN
	uci set network.$name=interface
	uci set network.$name.proto='dhcp'
	uci set network.$name.device="eth1.$vid"
	uci add_list cengarde.main.uplink="$name"
	uci add_list cengarde.main.uplink_type="$name=starlink"
	n=$((n + 1))
done
uci commit network && uci commit cengarde
service network reload && /etc/init.d/cengarde reload
```


- Los celulares igual, con `uplink_type='wom=cellular'`.
- `cengarde-setup` hace el resto, como con un solo plato: **una métrica
  propia por enlace** (dos rutas por defecto con la misma métrica se pisan),
  `peerdns 0`, la zona `wan` del firewall (masquerade, entrada rechazada) y la
  compañera DHCPv6 `<enlace>6` (`delegate 0`, `peerdns 0`).
- El motor abre un socket por enlace atado a su dispositivo
  (`SO_BINDTODEVICE`, `engine/src/sock.c`) y a su dirección: la búsqueda de
  ruta se hace con ese dispositivo, así que **tres redes iguales no se
  pisan para el túnel** sin reglas de política. Lo que sí se pisa es lo que
  no va atado a un dispositivo (ver abajo).
- **Cómo los distingue el NUC:** por el dispositivo (`eth1.101`), que es el
  nombre del enlace en el motor; el nombre UCI va como `label` (`[link
  eth1.101]` con `label = sl1`) y es el que muestran LuCI, `cengarde ctl
  links` y el registrador. Conviene que el nombre diga dónde está el plato
  (`sl_norte`, `sl_sur`) y que la VLAN sea su número.
- **IPv6:** cada plato da IPv6 nativa (en modo normal un /56 por DHCPv6-PD);
  la compañera `<enlace>6` la toma sin delegarla a la LAN. El VPS de hoy no
  tiene IPv6 (historia 017): el motor llega por IPv4. Si se le pone IPv6 al
  VPS, comparar un día v4 contra v6 con el registrador de pings antes de
  usarla.
- **CGNAT:** la IPv4 de Starlink va detrás de un CGNAT. El túnel es UDP
  saliente desde el NUC, así que pasa; las sondas mantienen viva la
  asignación. Dos platos del mismo PoP pueden salir con **la misma IP
  pública** (puertos distintos): el VPS distingue los caminos por el
  enlace del encabezado y la cookie, no por la dirección (historia 010).

### Pitfalls

- **Misma red en todas las VLAN.** Para el túnel no importa (sockets atados),
  pero el tráfico propio del NUC que no va atado sale por la ruta de menor
  métrica: `ping 192.168.1.1` o la página del router del plato llegan solo al
  plato de la métrica más baja. Para llegar a uno en particular: `ping -I
  eth1.102 192.168.1.1`, `curl --interface eth1.102 ...`.
- **La gestión del plato (`192.168.100.1`) es la misma en todos.** Con un
  plato se llega por la ruta por defecto; con varios hay que atar el cliente
  al dispositivo. El API gRPC-web del plato
  (`http://192.168.100.1:9201/SpaceX.API.Device.Device/Handle`) acepta `curl
  --interface eth1.10N` [sin probar; si `dish-rec` no puede atarse a un
  dispositivo, una tabla por VLAN: `ip route add 192.168.100.1/32 dev
  eth1.10N table 10N` y `ip rule add oif eth1.10N lookup 10N`, que vale para
  los sockets atados]. En modo bypass hace falta además una ruta a
  `192.168.100.1` por el dispositivo (el plato no la anuncia).
- **`rp_filter` estricto** descartaría las respuestas que llegan por una VLAN
  que no es la de la mejor ruta al VPS. Revisar `sysctl
  net.ipv4.conf.all.rp_filter` y el de cada `eth1.10N`: 0 o 2 (suelto), no 1.
- **NTP**: el reloj de ranuras (`slot.h`) usa la hora de pared; el NUC
  necesita NTP (con `route_all`, por el túnel). Sin hora, el reloj se apaga
  solo (`CG_WALL_SET_MS`).
- **Energía y montaje:** cada plato con su router consume decenas de vatios
  (más con calefacción de nieve); dimensionar la UPS por plato. Separar los
  platos unos metros para que no se obstruyan entre sí, con cielo despejado
  en los dos (la app muestra el mapa de obstrucción de cada uno).
- **Mismo PoP:** varios platos en el mismo lugar casi seguro salen por el
  mismo PoP y el mismo camino a Vultr: la parte de la pérdida que ocurre
  después del plato (historia 017: más hacia Vultr que hacia Cloudflare) es
  la que no se reparte (ver resultados). Un segundo VPS en otro proveedor o
  región no lo cambia el motor hoy (un servidor por router).

## Plan de medición: dos platos en el banco antes de ir

Lo que el laboratorio no puede decir es **ρ**: cuánto de la pérdida de un
plato comparte otro plato del mismo lugar. Eso decide si un segundo plato
vale (ρ bajo) o si hace falta un celular (ρ alto). Se mide con dos platos en
el banco, con la topología de campo (switch, VLAN, NUC), antes de llevarla.

**Montaje.** Dos platos separados unos metros, cada uno en su VLAN, el NUC con
el motor de campo (`7474be9` o el que se lleve), redundancia, `pace_peers_ms
= 250`, los dos `type = starlink`, sin celulares (o con un celular, apartado,
para tener una referencia de otro tipo). NTP en el NUC y el VPS.

**Qué grabar.**

1. `cengarde-rec` en el NUC (estado cada segundo; las 16 últimas sondas de
   cada enlace a ~10 Hz van dentro) y **`cengarde-rec -s` en el VPS**: el
   estado del servidor trae, por sesión, la pérdida en todos los enlaces y
   `joint` de la subida (`arrival.h`): para cada par de enlaces, la
   probabilidad de que los dos lleguen tarde o se pierdan (a 50, 100 y
   150 ms) contra el producto de cada uno solo, y el *lift*. Eso es la
   pérdida conjunta medida por el motor, paquete a paquete.
2. Carga: un flujo fijo por el túnel como el del evento, 9 Mbit/s de
   datagramas de 1200 B (`udpgen`, o el codificador real) más algo de TCP;
   el flujo con números de secuencia da su propia pérdida (`small` en
   `bondtab.py`) para cruzar con `lost_all`.
3. Pings por cada plato atados a su dispositivo (`ping -I eth1.10N`, 10 Hz)
   al VPS, a 1.1.1.1 y al router del plato: separa lo que se pierde antes
   del plato de lo que se pierde en el camino al VPS (historia 017).
4. `dish-rec` de cada plato (historial y cortes, con la causa), atado a su
   VLAN (ver «pitfalls»).

**Cuánto.** Al menos 6 horas con carga, que incluyan la tarde (la pérdida de
Starlink sube en la tarde, historia 017), y mejor 24 h: con ~0,6 % por
plato y 9 Mbit/s (≈940 paquetes/s), una hora da ~20 000 pérdidas por plato,
de sobra para estimar ρ por hora; los microcortes (1–4 por día en
«nostalgica») piden el día entero.

**Cómo calcularlo.**

- **ρ por hora:** con `fieldrec.py` sobre lo del VPS (sección *joint
  lateness*): `P(los dos tarde o perdidos)` contra `P(A)·P(B)`. Con la
  pérdida del flujo: si cada plato pierde `p` y el flujo en redundancia
  pierde `q`, la parte compartida es `ρ ≈ q / p` (en este laboratorio, con
  ρ = 0,5 y `p` = 0,6 %, el flujo perdió 0,25 %: `q/p` = 0,4–0,5).
- **Alineación de bordes:** `starfield.py` sobre las sondas de cada enlace
  (pérdida y retardo por fase de la ranura de 15 s); para cada borde
  (:12/:27/:42/:57), si el plato A se paró (sonda perdida o +40 ms en los
  45 ms antes del borde) y si se paró B: `P(B | A)` en el borde. En el
  laboratorio el caso malo es `P(B | A)` = 1 (todos en todos los bordes):
  la redundancia entre platos no tapa nada ahí.
- **Microcortes:** de `fieldrec.py` (cortes por enlace y «cuántos enlaces
  fuera a la vez»): ¿coinciden los microcortes de los dos platos? Uno solo
  con el otro limpio no cuesta nada con dos platos (`D1`); los dos a la vez
  cuestan como uno solo (`DA`).
- **Cuántos platos:** con ρ medido, la tabla de abajo da lo que se gana: con
  ρ ≈ 0, dos platos bastan (la pérdida conjunta baja a ~p²); con ρ alto, el
  tercer plato no suma y hace falta un enlace que no comparta camino
  (celular, otra operadora).

## Pendiente

- **Medir ρ y la alineación de bordes con dos platos** (plan de arriba)
  antes de decidir el tercero.
- `redund_spare`: en un microcorte del primero, pasar a otro plato limpio en
  vez de retenerlo (`cg_spare_keep` pensaba en un Starlink y celulares
  ahogados); y que la asignación de un enlace sin cola (otro plato) crezca
  aunque no se use, para que `rlc` no deje al segundo plato ocioso.
- El dominante a un lado aparta el reparto entero: repartir solo entre los
  pares (los platos) con los celulares a copias a tiempo, para sumar la
  bajada de varios platos; y el ping grande en ese modo (~330 ms de p99).
- `cg_dom_evidence` (con `hold`) todavía suma a los pares.
- `pace_peers_ms` entre platos de PoP distintos (decenas de ms): recortaría
  las copias del plato lejano; sin caso todavía.
- `lost_all` llega ~30 s tarde en el laboratorio (la ventana de 32 768):
  para corridas cortas, el flujo de `udpgen` es la medida.
- La tasa absoluta de pérdida por segundo de Starlink desde `server_view`
  (3 %) no cuadra con el 0,62 % de la historia 017: revisar el contador.
- En este PC, `gcc -Werror` del binario de tests falla en `test_fwdtable.c`
  y `split.h` (avisos de `array-bounds` y `uninitialized` de su gcc); pasa
  igual en `master` (`6544c40`) y no lo toca este PR.

## Cambios

- 2026-10-24: creada.
