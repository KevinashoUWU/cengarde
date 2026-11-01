---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_cc_bits`

Experimento del paso 2b: qué usos de la cola de cada enlace (su retardo de ida) están encendidos en el [[Ritmo por enlace (linkcc.h)]], sumando: 1, la cola que ve el reparto; 2, los modos por la cola (STARTUP termina, DRAIN termina, PROBE drena o sondea); 4, el tope de `bw` por lo que entregó con cola; 8, STARTUP a ×3/2 (si no, ×2). En el laboratorio (n = 2, 7 casos × 2 sentidos), 2 + 8 ganó: +8,7 % de media geométrica sobre ninguno y 3–10 veces menos pérdidas por enlace; 1 perdió 20–53 % (su retardo es el del paquete más nuevo con acuse y deja fuera lo enviado después: el reparto llenó de más los enlaces); 4, algo peor en netem, que no tiene ráfagas de concesiones; en terreno subió la subida de 78 / 59 a 96 / 101 Mbit/s (OMR 93–95) y no la bajada: por eso el router lo trae y el VPS no. 16: la cola del reparto es la mayor entre lo que va en vuelo y la de su retardo; 32: el tope de 4 solo mientras el enlace tiene cola (en la bajada, sin el tope dos enlaces llegaron a 513–587 ms de cola, y con él el enlace rápido quedó en 60 Mbit/s donde daba 176); 46 (2 + 4 + 8 + 32) ganó en la bajada: +23 % en el `fade` del laboratorio, +11 % en cuatro rondas de terreno: es lo del VPS. 64: el reparto le da datos a un enlace solo mientras su ventana (`cg_lc_cwnd`) tiene espacio, como el planificador MPTCP de OMR; neutro en el laboratorio (n = 4, 6 casos: bajada −6 a 0 %, subida 0 a +4 %), apagado. 128: el MTPT, la cola objetivo de cada enlace por perturbar y observar, con techo `link_cc_budget_ms`. 256: pasado el arranque, `bw` es la media de lo entregado en 0,5 s (con cola, su capacidad; sin cola solo sube): el máximo de las muestras leía 1,7–5,2 veces la capacidad de un módem saturado por sus ráfagas (`bench/rateeval.py`); en terreno (270 / 302) la bajada +21 % y la subida +6 %; de madrugada (2026-11-01, 3 + 3 rondas) la bajada +25 % en iperf3 y +12 % en Speedtest, las tres rondas, el ping con carga de 95 a 71 ms, la subida igual: por omisión desde entonces (270 y 302). 512: el ritmo propio decide solo (entra al reparto sin esperar a la capacidad, sin peso 0 de la capacidad, lleno para el rescate por su propia cola): laboratorio +3,4 %; en terreno, 7 rondas: bajada igual y subida −8 %, con ±30 % entre rondas: apagado. 1024, 2048 y 4096: lo último que decidía la capacidad vieja, por el ritmo propio de cada enlace: 1024 sin la semilla de la capacidad ni su respaldo en la pista a GRIT, 2048 el enlace dominante por el `bw` de cada enlace, 4096 la guarda y la pérdida de la FEC v3 por la cola propia y el subflujo; en el laboratorio (12 casos × 2 sentidos, 7 corridas) 4096 +4,4 %, los tres +4,1 %, 1024 y 2048 parejos: por omisión los tres (7438 / 7470, 2026-11-01).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `7438` en el router, `7470` en el VPS
- **Rango:** 0–8191
- **Pieza:** [[Ritmo por enlace (linkcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
