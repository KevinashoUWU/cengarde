---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `link_cc_budget_ms`

El techo del MTPT (*Maximum Transfer Point Tracking*, `link_cc_bits` 128): la cola que un enlace puede llegar a tener, menos cuánto más lento es que el más rápido (su `rtprop` sobre el menor), para que lo que lleva llegue dentro de la ventana del grupo. El MTPT mueve la cola objetivo de cada enlace un paso de 20 ms por segundo, entre `governor_target_ms` y este techo: sigue mientras lo entregado crece un 3 % o más, vuelve si cae, baja dos pasos si pierde más de 5 % y se queda quieto si queda plano o si el enlace no llegó a la mitad de su objetivo. La primera versión bajaba hasta 20 ms y con lo plano: en terreno perdió 22 % de la bajada (`field-m1`). Nace de la prueba de techo con captura (`bench/techo.c`, `curva1`): el codo de cada módem es abrupto y se mueve (WOM en bajada, limpio a 52 Mbit/s con p95 175 ms y 1,2 s de cola a 60). 300 ms por omisión: el presupuesto de latencia del usuario (~300, hasta 500 para explorar y picos de 700).

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `300`
- **Rango:** 20–1000
- **Pieza:** [[Ritmo por enlace (linkcc.h)]]
- **Historia:** [[029-mpgrit]]

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
