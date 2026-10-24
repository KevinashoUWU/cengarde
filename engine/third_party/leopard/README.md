# Leopard-RS (vendorizado)

Código de terceros: **Leopard-RS** de Christopher A. Taylor,
<https://github.com/catid/leopard>, licencia **BSD-3-Clause** (`License.md`,
compatible con la GPL-3.0-or-later del motor). Lo usa `redund_spare = rs`
(historia 021) a través de `engine/src/rs.h` y `rs.c`.

- **Origen:** commit `6e5725ebdf9da4370b0bcc4f70fa8eb66f4e6198` (2024-03-23,
  «Merge pull request #23», el último de la versión clásica). Los commits de
  2026 reescriben la biblioteca («leopard2», ~1 MB de C++ con hilos y la
  biblioteca estándar) y la API vieja depende de ella: no se tomaron.
- **Archivos:** `leopard.h`, `leopard.cpp`, `LeopardCommon.{h,cpp}`,
  `LeopardFF8.{h,cpp}` y `License.md`, tal cual salvo los cambios de abajo.
  Solo el cuerpo de 8 bits (original + recuperación ≤ 256); sin `LeopardFF16`
  ni `sse2neon` (submódulo del original).
- **`cg_sse_shim.h`** es de cengarde (GPL-3.0-or-later): las siete
  intrínsecas SSE2/SSSE3 que usa el cuerpo de 8 bits, en NEON para aarch64 y
  en C portable para el resto (armhf, MIPS).

## Cambios (marcados `cengarde:` en el código)

1. `LeopardCommon.h`: sin `<vector>`, `<atomic>`, `<memory>`, `<mutex>` ni
   `<condition_variable>` (no se usaban; con `<stddef.h>` y `<stdlib.h>` en
   su lugar), y `LeopardCommon.cpp` sin `<thread>`: los objetos no necesitan
   libstdc++ en tiempo de ejecución, solo `malloc`, `calloc`, `memcpy` y
   `memset` de la libc.
2. `LeopardFF8.cpp`: `new ffe_t[256 * 256]` → `malloc` (lo mismo), y la
   inicialización falla si no hay memoria.
3. `LeopardCommon.h`: sin `LEO_HAS_FF16`.
4. `LeopardCommon.h`: fuera de x86 y aarch64 (o con `-DCG_LEO_PORTABLE`, para
   probarlo en x86), `LEO_USE_SSE_SCALAR`: el camino de referencia byte a byte
   (`LEO_TARGET_MOBILE`, `CpuHasSSSE3` constante falsa), que no depende del
   orden de los bytes; aarch64 usa `cg_sse_shim.h` en lugar de `sse2neon`.
5. **Error del original corregido** (`LeopardFF8.cpp`, `RefMul` en
   `LEO_TARGET_MOBILE`): la multiplicación `x = y · m` hacía `x ^= y · m`.
   El camino portable codificaba y decodificaba mal: `make
   LEO_PORTABLE=-DCG_LEO_PORTABLE test` fallaba en `test_rs` (0 de 1455
   patrones), y `fecbench` recuperaba 90 de 1908. Con la corrección pasan.

## Compilación

`engine/Makefile` compila estos `.cpp` con `$(CXX)` (`-std=c++11
-fno-exceptions -fno-rtti -fno-threadsafe-statics`; en x86, `-mssse3`) y
enlaza con `$(CC)`. `make LEOPARD=0` compila sin Leopard (sin compilador de
C++): `redund_spare = rs` queda rechazado y `rlc` funciona igual.
