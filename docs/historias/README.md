# Historias

Cada historia guarda una investigación o una decisión con su evidencia, para
no tener que repetirla ni cargarla entera en el contexto. El índice activo
está en [`CLAUDE.md`](../../CLAUDE.md); aquí van la plantilla y el archivo de
historias cerradas.

## Reglas

- Una historia por tema, con nombre `NNN-tema.md` (número correlativo).
- El TL;DR tiene que bastar para decidir si hace falta leer el resto.
- Cita las fuentes con precisión (archivo:línea, versión o commit, comando
  que reproduce el dato) y separa lo verificado de lo que es estimación o
  recuerdo.
- Si los hechos cambian, se actualiza la historia: nueva fecha en la cabecera
  y una línea en "Cambios".

## Plantilla

```markdown
# NNN — Título

- **Fecha:** AAAA-MM-DD
- **Estado:** vigente | cerrada | sustituida por NNN
- **Fuentes:** enlaces, versiones o commits, archivo:línea, comandos

## TL;DR

Tres a seis viñetas con lo que hay que saber aunque no se lea el resto.

## Contexto

## Hallazgos

## Qué hacemos con esto

## Pendiente

## Cambios

- AAAA-MM-DD: creada.
```

## Archivo

Historias cerradas o sustituidas:

| # | Tema | Léela cuando… |
| --- | --- | --- |
| [001](001-diagnostico-engarde-go.md) | Diagnóstico medido del engarde Go (cerrada el 2026-10-23: el motor Go salió del árbol en `3492df9`) | quieras saber qué no repetir o comparar con la línea base |
| [005](005-motor-c-v1.md) | Motor C v1: protocolo, arquitectura y medidas (cerrada el 2026-10-24: el protocolo va por la v9 y la política de envío la sustituyó la 006; lo pendiente sigue en 011 y 007) | quieras ver la arquitectura y las medidas de la primera versión del motor |
