---
tags: [mapa, perilla]
archivos:
  - engine/src/config.c
  - engine/src/config.h
---

# `protocontroller`

El protocontrolador (solo con `tunnel = native`), encendido por omisión desde la 0.17: en el router el motor toma el TCP que el firewall redirige a `protocontroller_port` y lo lleva al VPS como GRIT, su flujo fiable; el VPS lo abre al destino. Con `no`, el TCP cruza el túnel como paquetes (una salida de emergencia; los dos extremos tienen que decir lo mismo, o el router avisa que el VPS rechaza) y un mensaje de flujo que llega se cuenta y se descarta. La variante `a` (un TCP simple en el medio) salió en la 0.17 (`docs/desvan/protoctl-variante-a.c`): un archivo que la dice corre como `b`, con un aviso, como `protocontroller_mid_cc` y `protocontroller_vps`.

- **Sección:** global
- **Extremo:** los dos
- **Por omisión:** `b`
- **Rango:** `b` o `no`
- **Pieza:** [[Protocontrolador (protoctl.c, stream.h, streamcc.h)]]
- **Historia:** [[025-protocontrolador]], [[029-mpgrit]]
- **Recarga:** cambiarla reinicia el proceso (`cg_config_restart_needed`).

Índice: [[Perillas]] · Mapa: [[Mapa del motor]]
