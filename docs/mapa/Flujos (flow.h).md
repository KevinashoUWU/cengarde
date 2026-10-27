---
tags: [mapa, pieza]
archivos:
  - engine/src/flow.h
sello: b3518362434750fc
---
# Flujos (flow.h)

**Grupo:** Medir — [[Mapa del motor]]

## Qué hace
Da el flujo de un paquete IP del túnel: un hash de 32 bits de su 5-tupla (direcciones, protocolo, puertos) y si es TCP. Recorre hasta `CG_FLOW_EXT_MAX` cabeceras de extensión de IPv6; los fragmentos se hashean sin puertos para que compartan flujo. `cg_flow_of_dir` con `rev` da el hash de la dirección contraria (un ACK encuentra el flujo de sus datos, [[023-reparacion-coherente]]). Es el gancho para un reordenamiento por flujo que todavía no existe.

## Lee
- El paquete IP leído de la TUN.

## Decide / escribe
- `struct cg_flow` → [[Liberación TCP (tcpfree.h)]]. El reordenamiento por flujo del [[Reordenador (reorder.h)]] es una fase posterior (no usado aún).

## Perillas
- Ninguna propia; solo actúa con [[Mapa/Perillas/tcp_aware|tcp_aware]].

## Dónde se midió
- [[014-tunel-nativo]]: `flow.h` deja listo el siguiente paso (el plan por flujo).

## Choques conocidos
- Ninguno registrado.

## Archivos
- `flow.h` (sin test propio; `cg_flow_of` se prueba en `engine/tests/test_tcpfree.c` y `test_tun.c`).
- `client.c`: `tun_read` (`cg_flow_of`). `server.c`: `down_prepare`.
