# 016 — Celda LTE virtual: el acantilado del SNR

- **Fecha:** 2026-10-22
- **Estado:** vigente
- **Fuentes:** `bench/celda/` (srsRAN 4G `bef8680` con ZMQ, GNU Radio 3.10.12),
  resultados en `~/sdr/resultados/{calibracion,escenario}.jsonl` del PC del
  banco; terreno: nostalgica 20–22-10 (`cengarde-rec`, `cell-monitor` del cpe1).

## TL;DR

- Una celda LTE completa corre en el PC (núcleo, estación base, canal de
  GNU Radio y varios teléfonos, cada uno en su netns): `bench/celda/`.
- **El SNR tiene un acantilado.** Con un flujo chico (1 Mb/s, como las copias
  de cengarde), el RTT se mantiene en 20–50 ms hasta ~4 dB; con ~2 dB sube a
  44–84 ms con 8 % de pérdida; con ~1 dB el BLER llega a 64 % y el RTT a
  2,7–4,4 s, aunque no haya nadie más en la celda.
- **La carga de otros usuarios, sola, casi no daña un flujo chico** en srsRAN:
  con la celda pidiendo 45 Mb/s sobre ~25 de capacidad, a 4 dB nuestro RTT
  sigue en 20–50 ms. Cerca del acantilado, la carga sí lo empuja al lado malo.
- Encaja con Entel en B2 (terreno: RSRQ −18, SINR −6 a −2 por antena, RTT
  1–1,5 s, 5–19 % de pérdida): vivía en el borde. En B28 (SINR +13) quedó a
  42–112 ms, ~20 veces mejor (22-10, 11:39).
- Límites: el scheduler de srsRAN es más amable que el de un operador (sin
  colas hondas), su SNR no es el SINR del módem (1 antena frente a 4) y no hay
  4×4 ni 256QAM en subida.

## Contexto

En terreno (historia 015 y nostalgica 20–22-10) los enlaces tienen firmas
distintas: Starlink pierde sin hacer cola, WOM encola sin perder (codo cerca
de 5 Mb/s) y Entel en B2 encolaba con nuestra carga casi en cero y además
perdía. El netem del laboratorio no reproduce el porqué: SNR, MCS, HARQ y un
scheduler compartido. Se armó una celda virtual para eso.

## Hallazgos

### La celda

- srsRAN 4G compilado con ZMQ (Debian no lo empaqueta; gcc 14 obliga a
  `-DENABLE_WERROR=OFF`). Sin ruido, el ping del teléfono al núcleo da
  19–37 ms, como un LTE real.
- Las muestras de srsRAN por ZMQ son grandes (potencia media 6·10⁴–2·10⁵):
  los ruidos que importan van de ~300 a ~5000. Los primeros barridos (hasta
  1000) no movían nada por eso.
- **Modelo de subida:** el primero sumaba el ruido de cada teléfono antes de
  la estación base, y uno débil empeoraba a todos (el otro usuario bajó de
  24 a 6 dB). Corregido: ganancia por teléfono y un piso de ruido común.

### Calibración, un teléfono, 10 MHz (`calibrar.py`)

| Ruido | SNR | MCS | BLER | Capacidad | RTT con 1 Mb/s (p50/p90/máx) | RTT al 80 % |
|---|---|---|---|---|---|---|
| 0 | 48,7 dB | 23 | 0 | 21,4 Mb/s | 16 / 21 / 31 ms | 17 / 18 / 21 ms |
| 400 | 15,3 | 19 | 0 | 16,5 | 17 / 25 / 28 | 17 / 19 / 26 |
| 800 | 9,6 | 14 | 0 | 10,8 | 22 / 35 / 40 | 16 / 22 / 35 |
| 1600 | 4,8 | 7 | 0 | 5,4 | 19 / 33 / 57 | 19 / 184 / 307 |
| 3000 | 2,1 | 0 | 5 % | 1,45 | 20 / 25 / 31 | 328 / 466 / 482 (44 % perdido) |
| 4000 | 1,3 | 3 | 50 % | 0,84 | 2,5 s / 4,4 s / 4,6 s | 0,8 / 1,8 / 2,0 s |

Un solo barrido, n = 1 por punto: hay ruido entre puntos vecinos (2000 y
2500 salieron mejor que 1600 con carga). Hay que repetir cada punto.

### B2 de Entel, 15 MHz, dos teléfonos (`escenario.py`)

| Nuestro SNR | Otros | Nuestro RTT p50/p90/máx | Pérdida (ping / UDP) | Nuestro MCS / BLER |
|---|---|---|---|---|
| ~28 dB | 0–25 Mb/s | 18 / 27–35 / 32–43 ms | 0 / 0 % | 20 / 0 % |
| ~17 dB | 25 Mb/s | 23 / 48 / 69 ms | 6 / 4 % | 16 / 7 % |
| ~4 dB | 45 Mb/s | 20 / 34 / 52 ms | 2 / 3 % | 4 / 5 % |
| ~2 dB | 45 Mb/s | 44 / 78 / 84 ms | 8 % / — | sale del reporte |
| ~1 dB | 0 | 2,7 s / 4,3 s / 4,4 s | 40 / 25 % | 1 / 64 % |
| ~1 dB | 25–45 Mb/s | — | 88–98 % | se desconecta |

## Qué hacemos con esto

- Para un enlace celular, lo que decide si sus copias sirven es la distancia
  al acantilado del SNR, más que la carga de la celda: un módem cerca del
  borde (B2 de Entel en la tarde) se fuerza a una banda baja (B28) o se deja
  de esperar por él. WOM queda en automático, como seguro si Entel pierde su
  B28.
- La celda virtual complementa a las trazas de terreno: srsRAN explica los
  mecanismos; las trazas de nostalgica reproducen nuestras celdas tal como
  son. Para escala (cientos de usuarios, 4×4, 256QAM) queda ns-3 con LENA
  (tarea #54); para fidelidad total, un comercial como Amarisoft.

## Pendiente

- Repetir la calibración con n ≥ 3 por punto y los dos tamaños de celda.
- Una cola honda frente a la celda (bufferbloat de operador) para reproducir
  la cola de WOM con buen SNR.
- Los enlaces de cengarde encima de la celda (un teléfono por enlace) y
  medir el reparto, la capacidad y el rescate contra el acantilado.
- 64QAM en subida, 2×2 en bajada y agregación de portadoras en srsRAN, para
  acercarse a una macro de Entel.

## Cambios

- 2026-10-22: creada.
