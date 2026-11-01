# Desván

Bloques de código que salieron del motor porque perdieron, o empataron, en el
laboratorio o en terreno, pero que pueden servir más adelante en otro
escenario. Cada archivo está desacoplado: trae sus constantes, sus funciones
tal como estaban, dónde se enganchaban (los llamados, como comentarios), su
test y lo que se midió. **No se compilan.**

Para volver a usar uno:

1. Lee el bloque y la historia que lo midió.
2. Copia sus funciones a la cabecera de donde salió y sus campos a la
   estructura (el bloque dice cuáles); los llamados van donde dicen los
   comentarios. Si el motor cambió mucho, `git show <commit>:<archivo>`
   da el archivo entero de entonces.
3. Ponle una perilla o un bit nuevo, apagado por omisión, y mídelo contra
   la base (`bench/labq.py`, luego terreno).

| Bloque | Qué hacía | Perilla de entonces | Por qué salió | Último commit | Historia |
| --- | --- | --- | --- | --- | --- |
| [linkcc-mtpt.h](linkcc-mtpt.h) | MTPT: la cola objetivo de cada enlace por perturbar y observar, como el MPPT de un cargador solar, con techo por el presupuesto de latencia menos el atraso del enlace | `link_cc_bits` 128, `link_cc_budget_ms` | v1 −22 % de bajada, v2 −12 % / −6 % en terreno | `3e35f6f` | 029 |
| [linkcc-cola-reparto.h](linkcc-cola-reparto.h) | La cola que ve el reparto por el retardo de ida al ritmo del enlace (QETA), o el mayor entre eso y lo que va en vuelo (QMAX) | `link_cc_bits` 1 y 16 | QETA −20 a −53 % en el laboratorio: su retardo es el del paquete más nuevo con acuse y deja fuera lo enviado después | `3e35f6f` | 029 |
| [linkcc-ventana.h](linkcc-ventana.h) | El reparto le da datos a un enlace solo mientras su ventana tiene espacio, como el planificador MPTCP de OMR (incluye la parte de `eta.h`) | `link_cc_bits` 64 | neutro en el laboratorio (bajada −6 a 0 %, subida 0 a +4 %) | `3e35f6f` | 029 |
| [linkcc-solo.c](linkcc-solo.c) | El ritmo propio decide solo: entra al reparto sin esperar a la capacidad, sin peso 0, lleno para el rescate por su propia cola | `link_cc_bits` 512 | laboratorio +3,4 %, terreno subida −8 % con ±30 % entre rondas | `3e35f6f` | 029 |
| [linkcc-apagados.h](linkcc-apagados.h) | Lo que hacían los bits ganadores cuando estaban apagados: la semilla de la capacidad, STARTUP a ×2, los modos sin la cola, `bw` por la mayor muestra | `link_cc_bits` sin 2, 8, 256, 1024 | ganaron sus contrarios | `3e35f6f` | 029 |

Un bloque nuevo: archivo con la cabecera de los demás (de dónde salió, el
commit, el porqué) y una fila aquí.
